#include "airplay/receiver.h"

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <filesystem>
#include <format>

#include "app/log.h"

extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"
}

namespace pm {
namespace {

constexpr uint64_t kMissedFeedbackLimitMs = 15000;  // UxPlay MISSED_FEEDBACK_LIMIT

uint64_t now_ms() { return GetTickCount64(); }

// Stable per-machine, per-slot AirPlay device id (iOS remembers receivers by it). Locally administered MAC.
std::array<uint8_t, 6> device_id(int slot) {
    char guid[64] = {};
    DWORD size = sizeof(guid);
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                 RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, guid, &size);
    uint64_t h = 1469598103934665603ull;
    for (const char* p = guid; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 1099511628211ull;
    h = (h ^ static_cast<uint64_t>(slot)) * 1099511628211ull;
    std::array<uint8_t, 6> mac;
    for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(h >> (8 * i));
    mac[0] = static_cast<uint8_t>((mac[0] & 0xFC) | 0x02);
    return mac;
}

std::string keyfile_path(int slot) {
    PWSTR appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) return {};
    std::filesystem::path p = std::filesystem::path(appdata) / L"PhoneMirror" / std::format(L"pairing-{}.pem", slot);
    CoTaskMemFree(appdata);
    return log::utf8(p.wstring());
}

void raop_log(void*, int level, const char* msg) {
    if (level <= LOGGER_ERR) {
        log::error("raop: {}", msg);
    } else {
        log::info("raop: {}", msg);
    }
}

}  // namespace

ReceiverPorts receiver_ports(int slot) {
    auto s = static_cast<unsigned short>(slot * 10);
    return {static_cast<unsigned short>(7000 + s), static_cast<unsigned short>(7100 + s),
            static_cast<unsigned short>(7011 + s), static_cast<unsigned short>(6001 + s),
            static_cast<unsigned short>(6000 + s)};
}

// Static trampolines from UxPlay's C callback table into the Receiver.
struct ReceiverCallbacks {
    static Receiver& self(void* cls) { return *static_cast<Receiver*>(cls); }

    static void conn_init(void* cls) {
        self(cls).connections_++;
        self(cls).last_feedback_ms_ = now_ms();
    }
    static void conn_destroy(void* cls) {
        Receiver& r = self(cls);
        if (--r.connections_ == 0 && r.mirroring_.exchange(false)) r.sink_.on_mirror_stop();
    }
    static void conn_feedback(void* cls) { self(cls).last_feedback_ms_ = now_ms(); }
    static void conn_reset(void* cls, int reason) {
        self(cls).request_reset(reason == 1 ? "lost connection with client" : "unsupported stream");
    }
    static void video_reset(void* cls, reset_type_t type) {
        Receiver& r = self(cls);
        log::info("AirPlay: video reset (type {})", static_cast<int>(type));
        if (r.mirroring_.exchange(false)) r.sink_.on_mirror_stop();
    }
    static void mirror_video_running(void* cls, bool running) {
        Receiver& r = self(cls);
        if (running) {
            if (!r.mirroring_.exchange(true)) r.sink_.on_mirror_start();
        } else if (r.mirroring_.exchange(false)) {
            r.sink_.on_mirror_stop();
        }
    }
    static int video_set_codec(void* cls, video_codec_t codec) {
        return self(cls).sink_.on_codec(codec == VIDEO_CODEC_H265) ? 0 : -1;
    }
    static void video_report_size(void* cls, float* ws, float* hs, float* w, float* h) {
        log::info("AirPlay: source {}x{}, stream {}x{}", *ws, *hs, *w, *h);
        self(cls).sink_.on_video_size(static_cast<int>(*w), static_cast<int>(*h));
    }
    static void video_process(void* cls, raop_ntp_t*, video_decode_struct* data) {
        self(cls).sink_.on_video(data->data, static_cast<size_t>(data->data_len), data->is_h265);
    }
    static void report_client_request(void* cls, char* deviceid, char* model, char* name, bool* admit) {
        log::info("AirPlay: connection from \"{}\" ({}) id {}", name ? name : "", model ? model : "",
                  deviceid ? deviceid : "");
        *admit = true;
        self(cls).sink_.on_client(name ? name : "", model ? model : "");
    }
    static void audio_process(void*, raop_ntp_t*, audio_decode_struct*) {}
    static void audio_flush(void*) {}
    static void video_flush(void*) {}
    static void video_pause(void*) {}
    static void video_resume(void*) {}
    static double audio_set_client_volume(void*) { return 0.0; }
    static void audio_set_volume(void*, float) {}
    static void audio_get_format(void*, unsigned char*, unsigned short*, bool*, bool*, uint64_t*) {}
    static void audio_set_metadata(void*, const void*, int) {}
    static void audio_set_coverart(void*, const void*, int) {}
    static void audio_stop_coverart_rendering(void*) {}
    static void audio_set_progress(void*, uint32_t*, uint32_t*, uint32_t*) {}
    static void export_dacp(void*, const char*, const char*) {}
};

Receiver::~Receiver() { stop(); }

bool Receiver::start(const ReceiverConfig& config) {
    config_ = config;
    std::array<uint8_t, 6> mac = device_id(config.slot);
    std::string mac_str = std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3],
                                      mac[4], mac[5]);

    int error = 0;
    dnssd_ = dnssd_init(config.name.c_str(), static_cast<int>(config.name.size()),
                        reinterpret_cast<const char*>(mac.data()), static_cast<int>(mac.size()), &error, 0);
    if (!dnssd_ || error) {
        log::error("dnssd_init failed: {}", error);
        return false;
    }
    // Feature bits as set by UxPlay's start_dnssd() with HLS off and modern (non-legacy) pairing.
    static constexpr int kOn[] = {1, 2, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 25, 28, 30};
    static constexpr int kOff[] = {0, 3, 4, 8, 23, 24, 26, 27, 29, 31};
    for (int bit : kOn) dnssd_set_airplay_features(dnssd_, bit, 1);
    for (int bit : kOff) dnssd_set_airplay_features(dnssd_, bit, 0);
    dnssd_set_airplay_features(dnssd_, 42, config.h265 ? 1 : 0);

    raop_callbacks_t cbs{};
    cbs.cls = this;
    cbs.conn_init = &ReceiverCallbacks::conn_init;
    cbs.conn_destroy = &ReceiverCallbacks::conn_destroy;
    cbs.conn_feedback = &ReceiverCallbacks::conn_feedback;
    cbs.conn_reset = &ReceiverCallbacks::conn_reset;
    cbs.video_reset = &ReceiverCallbacks::video_reset;
    cbs.mirror_video_running = &ReceiverCallbacks::mirror_video_running;
    cbs.video_set_codec = &ReceiverCallbacks::video_set_codec;
    cbs.video_report_size = &ReceiverCallbacks::video_report_size;
    cbs.video_process = &ReceiverCallbacks::video_process;
    cbs.report_client_request = &ReceiverCallbacks::report_client_request;
    cbs.audio_process = &ReceiverCallbacks::audio_process;
    cbs.audio_flush = &ReceiverCallbacks::audio_flush;
    cbs.video_flush = &ReceiverCallbacks::video_flush;
    cbs.video_pause = &ReceiverCallbacks::video_pause;
    cbs.video_resume = &ReceiverCallbacks::video_resume;
    cbs.audio_set_client_volume = &ReceiverCallbacks::audio_set_client_volume;
    cbs.audio_set_volume = &ReceiverCallbacks::audio_set_volume;
    cbs.audio_get_format = &ReceiverCallbacks::audio_get_format;
    cbs.audio_set_metadata = &ReceiverCallbacks::audio_set_metadata;
    cbs.audio_set_coverart = &ReceiverCallbacks::audio_set_coverart;
    cbs.audio_stop_coverart_rendering = &ReceiverCallbacks::audio_stop_coverart_rendering;
    cbs.audio_set_progress = &ReceiverCallbacks::audio_set_progress;
    cbs.export_dacp = &ReceiverCallbacks::export_dacp;

    raop_ = raop_init(&cbs);
    if (!raop_) {
        log::error("raop_init failed");
        return false;
    }
    raop_set_log_callback(raop_, &raop_log, nullptr);
    raop_set_log_level(raop_, LOGGER_INFO);
    std::string keyfile = keyfile_path(config.slot);
    if (raop_init2(raop_, 1, mac_str.c_str(), keyfile.c_str())) {
        log::error("raop_init2 failed (keyfile {})", keyfile);
        raop_destroy(raop_);
        raop_ = nullptr;
        return false;
    }
    raop_set_plist(raop_, "width", config.width);
    raop_set_plist(raop_, "height", config.height);
    raop_set_plist(raop_, "refreshRate", config.refresh_rate);
    raop_set_plist(raop_, "maxFPS", config.max_fps);

    ReceiverPorts ports = receiver_ports(config.slot);
    unsigned short tcp[2] = {ports.mirror, ports.http};
    unsigned short udp[3] = {ports.timing, ports.control, ports.data};
    raop_set_tcp_ports(raop_, tcp);
    raop_set_udp_ports(raop_, udp);
    port_ = raop_get_port(raop_);
    if (raop_start_httpd(raop_, &port_) < 0) {
        log::error("AirPlay: cannot listen on TCP {} (in use by another AirPlay receiver?)", port_);
        return false;
    }
    raop_set_port(raop_, port_);
    raop_set_dnssd(raop_, dnssd_);

    if (dnssd_register_raop(dnssd_, port_) || dnssd_register_airplay(dnssd_, port_)) {
        log::error("AirPlay: service registration failed");
        return false;
    }
    log::info("AirPlay receiver \"{}\" on TCP {} (device id {}, {})", config.name, port_, mac_str,
              config.h265 ? "H.264+H.265" : "H.264");

    stopping_ = false;
    supervisor_ = std::thread([this] { supervise(); });
    return true;
}

void Receiver::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (supervisor_.joinable()) supervisor_.join();
    if (dnssd_) {
        dnssd_unregister_raop(dnssd_);
        dnssd_unregister_airplay(dnssd_);
    }
    if (raop_) {
        raop_destroy(raop_);
        raop_ = nullptr;
    }
    if (dnssd_) {
        dnssd_destroy(dnssd_);
        dnssd_ = nullptr;
    }
}

void Receiver::request_reset(const char* reason) {
    log::info("AirPlay: resetting server ({})", reason);
    {
        std::lock_guard lock(mutex_);
        reset_pending_ = true;
    }
    wake_.notify_all();
}

// Restarting the HTTP server must happen off UxPlay's own threads, as UxPlay's main loop does.
void Receiver::supervise() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        wake_.wait_for(lock, std::chrono::seconds(1));
        if (stopping_) break;
        if (connections_ > 0 && mirroring_ && now_ms() - last_feedback_ms_ > kMissedFeedbackLimitMs) {
            log::error("AirPlay: no feedback from client for {} s", kMissedFeedbackLimitMs / 1000);
            reset_pending_ = true;
        }
        if (!reset_pending_) continue;
        reset_pending_ = false;
        lock.unlock();
        if (mirroring_.exchange(false)) sink_.on_mirror_stop();
        raop_stop_httpd(raop_);
        raop_remove_known_connections(raop_);
        unsigned short port = raop_get_port(raop_);
        raop_start_httpd(raop_, &port);
        raop_set_port(raop_, port);
        lock.lock();
    }
}

}  // namespace pm
