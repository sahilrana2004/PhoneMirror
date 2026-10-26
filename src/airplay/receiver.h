#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct raop_s;
struct dnssd_s;

namespace pm {

// Callbacks arrive on UxPlay's network threads; implementations must not block for long.
class ReceiverSink {
public:
    virtual ~ReceiverSink() = default;
    virtual void on_client(const std::string& name, const std::string& model) = 0;
    virtual void on_mirror_start() = 0;
    virtual void on_mirror_stop() = 0;
    virtual bool on_codec(bool h265) = 0;
    virtual void on_video_size(int width, int height) = 0;
    // Annex-B access unit (start-code prefixed NALs, parameter sets inline). Valid only during the call.
    virtual void on_video(const uint8_t* data, size_t size, bool h265) = 0;
    // Round trip of AirPlay's timing channel, refreshed every couple of seconds while mirroring.
    virtual void on_link_rtt(double ms) = 0;
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // One compressed frame; compression_type is AirPlay's ct (2 ALAC, 4 AAC-LC, 8 AAC-ELD).
    virtual void on_audio(const uint8_t* data, size_t size, int compression_type) = 0;
    virtual void on_audio_volume(float db) = 0;  // -30..0 dB, -144 = mute
    virtual void on_audio_flush() = 0;
};

struct ReceiverConfig {
    std::string name = "PhoneMirror";
    int slot = 0;                    // distinguishes device ids / keys when several receivers run
    bool h265 = false;               // advertise ScreenMultiCodec (bit 42)
    int width = 1920;                // the height is what iOS uses to size the stream
    int height = 1080;
    int refresh_rate = 60;
    int max_fps = 60;
};

// Fixed ports (UxPlay's "-p" set, shifted by 10 per slot) so firewall rules stay stable.
struct ReceiverPorts {
    unsigned short http, mirror, timing, control, data;
};
ReceiverPorts receiver_ports(int slot);

class Receiver {
public:
    Receiver(ReceiverSink& sink, AudioSink& audio) : sink_(sink), audio_(audio) {}
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    ~Receiver();

    bool start(const ReceiverConfig& config);
    void stop();

private:
    friend struct ReceiverCallbacks;
    void request_reset(const char* reason);
    void supervise();

    ReceiverSink& sink_;
    AudioSink& audio_;
    ReceiverConfig config_;
    raop_s* raop_ = nullptr;
    dnssd_s* dnssd_ = nullptr;
    unsigned short port_ = 0;

    std::thread supervisor_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool reset_pending_ = false;
    std::atomic<int> connections_ = 0;
    std::atomic<uint64_t> last_feedback_ms_ = 0;
    std::atomic<bool> mirroring_ = false;
    int frames_since_rtt_ = 0;  // mirror thread only
};

}  // namespace pm
