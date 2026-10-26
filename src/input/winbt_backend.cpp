#include "input/winbt_backend.h"

#include <unknwn.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "app/log.h"

namespace pm {
namespace {

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;
using Clock = std::chrono::steady_clock;

constexpr uint8_t kReportKeyboard = 1;
constexpr uint8_t kReportMouse = 2;
constexpr uint8_t kReportConsumer = 3;

// Report 1: keyboard (8 modifier bits, reserved, 6 keys). Report 2: mouse, 3 buttons, 16-bit relative X/Y
// (large deltas fit one report), wheel, AC Pan. Report 3: one 16-bit consumer usage.
constexpr uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, kReportKeyboard,              //
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,             //
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,                                     // modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                     // reserved
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x19, 0x00, 0x29, 0x65, //
    0x81, 0x00,                                                             // keys
    0xC0,                                                                   //
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, kReportMouse, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,  // buttons
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,                                                              // pad
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02,
    0x81, 0x06,                                                                                      // X, Y
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,                          // wheel
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,        // AC Pan
    0xC0, 0xC0,                                                                                      //
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, kReportConsumer, 0x15, 0x00, 0x26, 0xFF, 0x03,
    0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0};

guid uuid16(uint16_t id) { return guid{0x00000000u + id, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}}; }

IBuffer to_buffer(const uint8_t* data, size_t size) {
    DataWriter w;
    w.WriteBytes(array_view<const uint8_t>(data, data + size));
    return w.DetachBuffer();
}

const char* error_name(BluetoothError e) {
    switch (e) {
    case BluetoothError::Success: return "Success";
    case BluetoothError::RadioNotAvailable: return "RadioNotAvailable";
    case BluetoothError::ResourceInUse: return "ResourceInUse";
    case BluetoothError::DisabledByPolicy: return "DisabledByPolicy";
    case BluetoothError::NotSupported: return "NotSupported";
    case BluetoothError::DisabledByUser: return "DisabledByUser";
    case BluetoothError::ConsentRequired: return "ConsentRequired";
    default: return "OtherError";
    }
}

}  // namespace

struct WinBtBackend::Impl {
    HWND notify_window = nullptr;
    std::thread thread;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::atomic<bool> connected = false;
    int dx = 0, dy = 0, wheel = 0, pan = 0;
    uint8_t buttons = 0;
    bool mouse_dirty = false;
    std::deque<KeyboardState> keys;
    std::deque<uint16_t> consumer;
    std::deque<std::pair<int, int>> steps;  // discrete (uncoalesced) motion reports

    // Minimum spacing of mouse reports: one per connection event, so motion is coalesced rather than queued.
    std::atomic<int> min_interval_us = 7500;

    // Every phone subscribed to the HID reports, and which one input currently goes to.
    struct Client {
        GattSubscribedClient client{nullptr};
        std::string id;
        std::string name;
        BluetoothLEDevice device{nullptr};
        BluetoothLEPreferredConnectionParametersRequest params{nullptr};  // kept alive: dropping it cancels the request
    };
    std::mutex clients_mutex;
    std::vector<Client> clients;
    std::string target;

    void run();
    void update_clients(GattLocalCharacteristic const& reports);
    void tune_connection(Client& client);
    GattSubscribedClient target_client();
};

WinBtBackend::WinBtBackend() : impl_(std::make_unique<Impl>()) {}

WinBtBackend::~WinBtBackend() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
}

void WinBtBackend::start(HWND notify_window) {
    impl_->notify_window = notify_window;
    impl_->thread = std::thread([this] { impl_->run(); });
}

bool WinBtBackend::connected() const { return impl_->connected; }

void WinBtBackend::mouse_move(int dx, int dy) {
    std::lock_guard lock(impl_->mutex);
    impl_->dx += dx;
    impl_->dy += dy;
    impl_->mouse_dirty = true;
    impl_->wake.notify_one();
}

void WinBtBackend::mouse_scroll(int wheel, int pan) {
    std::lock_guard lock(impl_->mutex);
    impl_->wheel += wheel;
    impl_->pan += pan;
    impl_->mouse_dirty = true;
    impl_->wake.notify_one();
}

void WinBtBackend::mouse_buttons(uint8_t buttons) {
    std::lock_guard lock(impl_->mutex);
    impl_->buttons = buttons;
    impl_->mouse_dirty = true;
    impl_->wake.notify_one();
}

void WinBtBackend::mouse_steps(int dx, int dy, int step) {
    step = std::clamp(step, 1, 32767);
    std::lock_guard lock(impl_->mutex);
    while (dx || dy) {
        int sx = std::clamp(dx, -step, step);
        int sy = std::clamp(dy, -step, step);
        impl_->steps.emplace_back(sx, sy);
        dx -= sx;
        dy -= sy;
    }
    impl_->wake.notify_one();
}

void WinBtBackend::keyboard(const KeyboardState& state) {
    std::lock_guard lock(impl_->mutex);
    impl_->keys.push_back(state);
    impl_->wake.notify_one();
}

void WinBtBackend::consumer_click(uint16_t usage) {
    std::lock_guard lock(impl_->mutex);
    impl_->consumer.push_back(usage);
    impl_->consumer.push_back(0);
    impl_->wake.notify_one();
}

void WinBtBackend::set_target(const std::string& phone_name) {
    std::lock_guard lock(impl_->clients_mutex);
    impl_->target = phone_name;
}

void WinBtBackend::Impl::tune_connection(Client& c) {
    try {
        BluetoothLEConnectionParameters p = c.device.GetConnectionParameters();
        log::info("Control: '{}' link interval {:.2f} ms, latency {}, timeout {} ms", c.name, p.ConnectionInterval() * 1.25,
                  p.ConnectionLatency(), p.LinkTimeout() * 10);
        min_interval_us = std::max(7500, static_cast<int>(p.ConnectionInterval() * 1250));
        c.device.ConnectionParametersChanged([this](BluetoothLEDevice const& d, auto&&) {
            BluetoothLEConnectionParameters np = d.GetConnectionParameters();
            min_interval_us = std::max(7500, static_cast<int>(np.ConnectionInterval() * 1250));
            log::info("Control: link interval now {:.2f} ms", np.ConnectionInterval() * 1.25);
        });
        c.params = c.device.RequestPreferredConnectionParameters(BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
        log::info("Control: requested low-latency link parameters (status {})", static_cast<int>(c.params.Status()));
    } catch (hresult_error const& e) {
        log::info("Control: link tuning unavailable (0x{:08X})", static_cast<unsigned>(e.code()));
    }
}

// Keeps `clients` in step with the phones subscribed to the reports; new phones get their name resolved
// (for routing) and their link tuned.
void WinBtBackend::Impl::update_clients(GattLocalCharacteristic const& reports) {
    std::vector<Client> next;
    for (GattSubscribedClient const& sub : reports.SubscribedClients()) {
        std::string id = to_string(sub.Session().DeviceId().Id());
        Client c;
        {
            std::lock_guard lock(clients_mutex);
            auto it = std::find_if(clients.begin(), clients.end(), [&](const Client& k) { return k.id == id; });
            if (it != clients.end()) c = *it;
        }
        if (!c.client) {
            c.id = id;
            try {
                c.device = BluetoothLEDevice::FromIdAsync(sub.Session().DeviceId().Id()).get();
                if (c.device) c.name = to_string(c.device.Name());
            } catch (hresult_error const&) {
            }
            log::info("Control: phone '{}' connected (subscribed to HID reports)", c.name);
            if (c.device) tune_connection(c);
        }
        c.client = sub;
        next.push_back(std::move(c));
    }
    {
        std::lock_guard lock(clients_mutex);
        for (const Client& old : clients) {
            if (std::none_of(next.begin(), next.end(), [&](const Client& n) { return n.id == old.id; })) {
                log::info("Control: phone '{}' disconnected", old.name);
            }
        }
        clients = std::move(next);
    }
    bool now = !clients.empty();
    if (now != connected.exchange(now)) PostMessageW(notify_window, kMsgControlState, now ? 1 : 0, 0);
}

// The subscribed phone whose Bluetooth name matches the target (exact, then containment, case-insensitive);
// null means "all phones" (one connected, no target, or no match).
GattSubscribedClient WinBtBackend::Impl::target_client() {
    std::lock_guard lock(clients_mutex);
    if (clients.size() < 2 || target.empty()) return nullptr;
    auto lower = [](std::string s) {
        for (char& ch : s) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    std::string want = lower(target);
    for (const Client& c : clients) {
        if (lower(c.name) == want) return c.client;
    }
    for (const Client& c : clients) {
        std::string have = lower(c.name);
        if (!have.empty() && (have.find(want) != std::string::npos || want.find(have) != std::string::npos)) return c.client;
    }
    return nullptr;
}

void WinBtBackend::Impl::run() {
    init_apartment(apartment_type::multi_threaded);
    try {
        BluetoothAdapter adapter = BluetoothAdapter::GetDefaultAsync().get();
        if (!adapter || !adapter.IsLowEnergySupported() || !adapter.IsPeripheralRoleSupported()) {
            log::error("Control: Bluetooth LE peripheral role unavailable on this PC");
            return;
        }
        GattServiceProviderResult created = GattServiceProvider::CreateAsync(uuid16(0x1812)).get();
        if (created.Error() != BluetoothError::Success) {
            log::error("Control: HID service rejected: {}", error_name(created.Error()));
            return;
        }
        GattServiceProvider hid = created.ServiceProvider();
        GattLocalService service = hid.Service();

        auto make = [&](uint16_t id, GattCharacteristicProperties props, IBuffer value) {
            GattLocalCharacteristicParameters params;
            params.CharacteristicProperties(props);
            params.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
            params.WriteProtectionLevel(GattProtectionLevel::EncryptionRequired);
            if (value) params.StaticValue(value);
            GattLocalCharacteristicResult r = service.CreateCharacteristicAsync(uuid16(id), params).get();
            if (r.Error() != BluetoothError::Success) {
                throw hresult_error(E_FAIL, to_hstring(std::format("characteristic 0x{:04X}: {}", id, error_name(r.Error()))));
            }
            return r.Characteristic();
        };
        auto read = GattCharacteristicProperties::Read;
        auto notify_props = GattCharacteristicProperties::Read | GattCharacteristicProperties::Notify;
        auto write_nr = GattCharacteristicProperties::WriteWithoutResponse;

        const uint8_t hid_info[] = {0x11, 0x01, 0x00, 0x03};  // HID 1.11, no country, remote wake + connectable
        const uint8_t protocol_report[] = {0x01};
        make(0x2A4A, read, to_buffer(hid_info, sizeof(hid_info)));
        make(0x2A4B, read, to_buffer(kReportMap, sizeof(kReportMap)));
        GattLocalCharacteristic control_point = make(0x2A4C, write_nr, nullptr);
        GattLocalCharacteristic protocol = make(0x2A4E, read | write_nr, to_buffer(protocol_report, sizeof(protocol_report)));
        GattLocalCharacteristic keyboard_report = make(0x2A4D, notify_props, nullptr);
        GattLocalCharacteristic mouse_report = make(0x2A4D, notify_props, nullptr);
        GattLocalCharacteristic consumer_report = make(0x2A4D, notify_props, nullptr);

        auto reference = [](GattLocalCharacteristic const& c, uint8_t id, size_t report_size) {
            const uint8_t ref[] = {id, 0x01};  // input report
            GattLocalDescriptorParameters params;
            params.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
            params.StaticValue(to_buffer(ref, sizeof(ref)));
            c.CreateDescriptorAsync(uuid16(0x2908), params).get();
            // Reads of an input report return its idle value.
            c.ReadRequested([report_size](auto&&, GattReadRequestedEventArgs const& args) {
                auto deferral = args.GetDeferral();
                GattReadRequest request = args.GetRequestAsync().get();
                if (request) {
                    uint8_t zeros[8] = {};
                    request.RespondWithValue(to_buffer(zeros, report_size));
                }
                deferral.Complete();
            });
        };
        reference(keyboard_report, kReportKeyboard, 8);
        reference(mouse_report, kReportMouse, 7);
        reference(consumer_report, kReportConsumer, 2);

        for (GattLocalCharacteristic const& c : {control_point, protocol}) {
            c.WriteRequested([](auto&&, GattWriteRequestedEventArgs const& args) {
                auto deferral = args.GetDeferral();
                GattWriteRequest request = args.GetRequestAsync().get();
                if (request && request.Option() == GattWriteOption::WriteWithResponse) request.Respond();
                deferral.Complete();
            });
        }

        mouse_report.SubscribedClientsChanged([this](GattLocalCharacteristic const& c, auto&&) { update_clients(c); });
        hid.AdvertisementStatusChanged([](GattServiceProvider const&, GattServiceProviderAdvertisementStatusChangedEventArgs const& a) {
            log::info("Control: advertising status {} ({})", static_cast<int>(a.Status()), error_name(a.Error()));
        });

        GattServiceProviderAdvertisingParameters adv;
        adv.IsConnectable(true);
        adv.IsDiscoverable(true);
        hid.StartAdvertising(adv);
        log::info("Control: Bluetooth HID keyboard/mouse published");

        uint64_t sent = 0, failed = 0, notify_us = 0;
        auto stats_start = Clock::now();
        auto last_mouse = Clock::now() - std::chrono::seconds(1);
        auto notify = [&](GattLocalCharacteristic const& c, const uint8_t* data, size_t size) {
            auto t0 = Clock::now();
            try {
                if (GattSubscribedClient only = target_client()) {  // several phones: just the focused one
                    if (c.NotifyValueAsync(to_buffer(data, size), only).get().Status() != GattCommunicationStatus::Success) {
                        ++failed;
                    }
                } else {
                    for (auto const& r : c.NotifyValueAsync(to_buffer(data, size)).get()) {
                        if (r.Status() != GattCommunicationStatus::Success) ++failed;
                    }
                }
            } catch (hresult_error const&) {
                ++failed;
            }
            notify_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
            ++sent;
        };

        std::unique_lock lock(mutex);
        while (!stopping) {
            auto mouse_due = last_mouse + std::chrono::microseconds(min_interval_us.load());
            bool mouse_ready = (mouse_dirty || !steps.empty()) && Clock::now() >= mouse_due;
            if (!keys.empty() || !consumer.empty() || mouse_ready) {
                if (!connected) {  // nothing is listening: drop input instead of replaying it later
                    keys.clear();
                    consumer.clear();
                    steps.clear();
                    mouse_dirty = false;
                    dx = dy = wheel = pan = 0;
                    continue;
                }
                if (!consumer.empty()) {
                    uint16_t usage = consumer.front();
                    consumer.pop_front();
                    lock.unlock();
                    const uint8_t r[2] = {static_cast<uint8_t>(usage), static_cast<uint8_t>(usage >> 8)};
                    notify(consumer_report, r, sizeof(r));
                } else if (!keys.empty()) {
                    KeyboardState k = keys.front();
                    keys.pop_front();
                    lock.unlock();
                    const uint8_t r[8] = {k.modifiers, 0, k.keys[0], k.keys[1], k.keys[2], k.keys[3], k.keys[4], k.keys[5]};
                    notify(keyboard_report, r, sizeof(r));
                } else if (!steps.empty()) {
                    auto [sx, sy] = steps.front();
                    steps.pop_front();
                    const uint8_t r[7] = {buttons, static_cast<uint8_t>(sx), static_cast<uint8_t>(sx >> 8),
                                          static_cast<uint8_t>(sy), static_cast<uint8_t>(sy >> 8), 0, 0};
                    last_mouse = Clock::now();
                    lock.unlock();
                    notify(mouse_report, r, sizeof(r));
                } else {
                    int sx = std::clamp(dx, -32767, 32767);
                    int sy = std::clamp(dy, -32767, 32767);
                    int sw = std::clamp(wheel, -127, 127);
                    int sp = std::clamp(pan, -127, 127);
                    dx -= sx;
                    dy -= sy;
                    wheel -= sw;
                    pan -= sp;
                    mouse_dirty = dx || dy || wheel || pan;
                    const uint8_t r[7] = {buttons,
                                          static_cast<uint8_t>(sx),
                                          static_cast<uint8_t>(sx >> 8),
                                          static_cast<uint8_t>(sy),
                                          static_cast<uint8_t>(sy >> 8),
                                          static_cast<uint8_t>(static_cast<int8_t>(sw)),
                                          static_cast<uint8_t>(static_cast<int8_t>(sp))};
                    last_mouse = Clock::now();
                    lock.unlock();
                    notify(mouse_report, r, sizeof(r));
                }
                lock.lock();
                if (Clock::now() - stats_start > std::chrono::seconds(10) && sent) {
                    log::info("Control: {} reports, notify avg {:.2f} ms, failed {}", sent,
                              notify_us / 1000.0 / static_cast<double>(sent), failed);
                    stats_start = Clock::now();
                    sent = failed = notify_us = 0;
                }
                continue;
            }
            if (mouse_dirty || !steps.empty()) {
                wake.wait_until(lock, mouse_due);
            } else {
                wake.wait(lock);
            }
        }
        lock.unlock();
        hid.StopAdvertising();
    } catch (hresult_error const& e) {
        log::error("Control: Bluetooth HID failed: 0x{:08X} {}", static_cast<unsigned>(e.code()), to_string(e.message()));
    }
    connected = false;
}

}  // namespace pm
