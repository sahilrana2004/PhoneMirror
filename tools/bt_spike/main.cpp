// M7 spike: can this PC act as a BLE HID (HOGP) keyboard/mouse for an iPhone using only Windows APIs?
// Tries to publish GATT HID (0x1812) next to Battery (0x180F) and Device Information (0x180A) as
// controls, then advertises connectable if HID was accepted. Prints every result/HRESULT.

#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

namespace {

const char* error_name(BluetoothError e) {
    switch (e) {
    case BluetoothError::Success: return "Success";
    case BluetoothError::RadioNotAvailable: return "RadioNotAvailable";
    case BluetoothError::ResourceInUse: return "ResourceInUse";
    case BluetoothError::DeviceNotConnected: return "DeviceNotConnected";
    case BluetoothError::OtherError: return "OtherError";
    case BluetoothError::DisabledByPolicy: return "DisabledByPolicy";
    case BluetoothError::NotSupported: return "NotSupported";
    case BluetoothError::DisabledByUser: return "DisabledByUser";
    case BluetoothError::ConsentRequired: return "ConsentRequired";
    case BluetoothError::TransportNotSupported: return "TransportNotSupported";
    default: return "?";
    }
}

guid uuid16(uint16_t id) { return guid{0x00000000u + id, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}}; }

IBuffer bytes(std::initializer_list<uint8_t> data) {
    DataWriter w;
    for (uint8_t b : data) w.WriteByte(b);
    return w.DetachBuffer();
}

// Keyboard (report 1) + mouse (report 2) descriptor, the shape the real backend would publish.
IBuffer report_map() {
    return bytes({0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
                  0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
                  0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0, 0x05, 0x01, 0x09,
                  0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15,
                  0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05,
                  0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81,
                  0x06, 0xC0, 0xC0});
}

GattServiceProvider try_service(const char* label, guid uuid) {
    std::printf("\n[%s] GattServiceProvider::CreateAsync(%s)\n", label, to_string(to_hstring(uuid)).c_str());
    try {
        GattServiceProviderResult r = GattServiceProvider::CreateAsync(uuid).get();
        std::printf("  result: BluetoothError::%s (%d)\n", error_name(r.Error()), static_cast<int>(r.Error()));
        return r.Error() == BluetoothError::Success ? r.ServiceProvider() : nullptr;
    } catch (const hresult_error& e) {
        std::printf("  threw HRESULT 0x%08X: %ls\n", static_cast<unsigned>(e.code()), e.message().c_str());
        return nullptr;
    }
}

GattLocalCharacteristic add_characteristic(GattServiceProvider const& p, const char* label, uint16_t id,
                                           GattCharacteristicProperties props, IBuffer value) {
    GattLocalCharacteristicParameters params;
    params.CharacteristicProperties(props);
    params.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
    params.WriteProtectionLevel(GattProtectionLevel::EncryptionRequired);
    if (value) params.StaticValue(value);
    try {
        GattLocalCharacteristicResult r = p.Service().CreateCharacteristicAsync(uuid16(id), params).get();
        std::printf("  characteristic %s (0x%04X): BluetoothError::%s\n", label, id, error_name(r.Error()));
        return r.Error() == BluetoothError::Success ? r.Characteristic() : nullptr;
    } catch (const hresult_error& e) {
        std::printf("  characteristic %s (0x%04X) threw HRESULT 0x%08X: %ls\n", label, id,
                    static_cast<unsigned>(e.code()), e.message().c_str());
        return nullptr;
    }
}

// Report Reference descriptor (0x2908): which report id / type an input characteristic carries.
void add_report_reference(GattLocalCharacteristic const& c, const char* label, uint8_t report_id) {
    GattLocalDescriptorParameters params;
    params.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
    params.StaticValue(bytes({report_id, 0x01}));
    try {
        GattLocalDescriptorResult r = c.CreateDescriptorAsync(uuid16(0x2908), params).get();
        std::printf("  descriptor Report Reference for %s: BluetoothError::%s\n", label, error_name(r.Error()));
    } catch (const hresult_error& e) {
        std::printf("  descriptor Report Reference for %s threw HRESULT 0x%08X: %ls\n", label,
                    static_cast<unsigned>(e.code()), e.message().c_str());
    }
}

void log_access(GattLocalCharacteristic const& c, const char* label) {
    c.ReadRequested([label](auto&&, GattReadRequestedEventArgs const& args) {
        std::printf("  [central] read %s (session %ls)\n", label,
                    args.Session().DeviceId().Id().c_str());
    });
    c.WriteRequested([label](auto&&, GattWriteRequestedEventArgs const&) {
        std::printf("  [central] write %s\n", label);
    });
}

}  // namespace

int main() {
    init_apartment(apartment_type::multi_threaded);
    std::printf("PhoneMirror Bluetooth HID peripheral spike\n");

    BluetoothAdapter adapter = BluetoothAdapter::GetDefaultAsync().get();
    if (!adapter) {
        std::printf("No Bluetooth adapter\n");
        return 1;
    }
    std::printf("Adapter %012llX  LE:%d Classic:%d PeripheralRole:%d CentralRole:%d AdvertisementOffload:%d\n",
                static_cast<unsigned long long>(adapter.BluetoothAddress()), adapter.IsLowEnergySupported(),
                adapter.IsClassicSupported(), adapter.IsPeripheralRoleSupported(), adapter.IsCentralRoleSupported(),
                adapter.IsAdvertisementOffloadSupported());

    GattServiceProvider battery = try_service("control: Battery 0x180F", uuid16(0x180F));
    GattServiceProvider devinfo = try_service("control: Device Information 0x180A", uuid16(0x180A));
    GattServiceProvider hid = try_service("HID 0x1812", uuid16(0x1812));

    if (!hid) {
        std::printf("\nRESULT: Windows refuses to host the HID service; a PC-only BLE HID backend is not possible.\n");
        return 2;
    }

    auto rd = GattCharacteristicProperties::Read;
    auto rn = GattCharacteristicProperties::Read | GattCharacteristicProperties::Notify;
    add_characteristic(hid, "HID Information", 0x2A4A, rd, bytes({0x11, 0x01, 0x00, 0x03}));
    GattLocalCharacteristic map = add_characteristic(hid, "Report Map", 0x2A4B, rd, report_map());
    add_characteristic(hid, "HID Control Point", 0x2A4C, GattCharacteristicProperties::WriteWithoutResponse, nullptr);
    add_characteristic(hid, "Protocol Mode", 0x2A4E,
                       GattCharacteristicProperties::Read | GattCharacteristicProperties::WriteWithoutResponse,
                       bytes({0x01}));
    GattLocalCharacteristic keyboard = add_characteristic(hid, "Report (keyboard input)", 0x2A4D, rn, nullptr);
    GattLocalCharacteristic mouse = add_characteristic(hid, "Report (mouse input)", 0x2A4D, rn, nullptr);
    if (!map || !keyboard || !mouse) {
        std::printf("\nRESULT: HID characteristics rejected.\n");
        return 4;
    }
    add_report_reference(keyboard, "keyboard", 1);
    add_report_reference(mouse, "mouse", 2);
    log_access(map, "Report Map");

    hid.AdvertisementStatusChanged([](GattServiceProvider const&, GattServiceProviderAdvertisementStatusChangedEventArgs const& a) {
        std::printf("  [adv] status %d error %s\n", static_cast<int>(a.Status()), error_name(a.Error()));
    });
    std::atomic<bool> subscribed = false;
    for (auto* c : {&keyboard, &mouse}) {
        c->SubscribedClientsChanged([&subscribed, c, &mouse](GattLocalCharacteristic const& ch, auto&&) {
            auto clients = ch.SubscribedClients();
            std::printf("  [central] %s subscribers: %u\n", c == &mouse ? "mouse" : "keyboard", clients.Size());
            for (auto const& client : clients) {
                std::printf("    client %ls, max PDU %u\n", client.Session().DeviceId().Id().c_str(),
                            client.Session().MaxPduSize());
            }
            if (c == &mouse) subscribed = clients.Size() > 0;
        });
    }

    GattServiceProviderAdvertisingParameters adv;
    adv.IsConnectable(true);
    adv.IsDiscoverable(true);
    try {
        hid.StartAdvertising(adv);
        std::printf("\nStartAdvertising: status %d (0=Created,1=Stopped,2=Started,3=Aborted,4=StartedWithoutAllAdvertisementData)\n",
                    static_cast<int>(hid.AdvertisementStatus()));
    } catch (const hresult_error& e) {
        std::printf("\nStartAdvertising threw HRESULT 0x%08X: %ls\n", static_cast<unsigned>(e.code()), e.message().c_str());
        return 3;
    }
    std::printf("Advertising for 3 min. On the iPhone: Settings > Bluetooth, pair this PC; then turn on\n"
                "Settings > Accessibility > Touch > AssistiveTouch. The pointer should circle once subscribed.\n");
    // Mouse report id 2: buttons, dx, dy, wheel. A slow circle: 8 px steps at ~60 Hz.
    static constexpr int8_t dx[] = {8, 6, 0, -6, -8, -6, 0, 6};
    static constexpr int8_t dy[] = {0, 6, 8, 6, 0, -6, -8, -6};
    uint64_t sent = 0;
    uint64_t failed = 0;
    auto end = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    while (std::chrono::steady_clock::now() < end) {
        if (!subscribed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        size_t step = (sent / 8) % 8;
        try {
            auto results = mouse.NotifyValueAsync(bytes({0x00, static_cast<uint8_t>(dx[step]),
                                                         static_cast<uint8_t>(dy[step]), 0x00}))
                               .get();
            for (auto const& r : results) {
                if (r.Status() != GattCommunicationStatus::Success) ++failed;
            }
        } catch (const hresult_error& e) {
            ++failed;
            if (failed < 5) std::printf("  notify threw 0x%08X: %ls\n", static_cast<unsigned>(e.code()), e.message().c_str());
        }
        if (++sent % 300 == 0) std::printf("  mouse reports sent %llu, failed %llu\n", sent, failed);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    hid.StopAdvertising();
    std::printf("Done. reports sent %llu, failed %llu\n", sent, failed);
    return 0;
}
