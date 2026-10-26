#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace pm {

class AudioSink;
class ReceiverSink;

// Network impairments applied to file playback (lab). The mirror stream is TCP, so Wi-Fi trouble shows up
// as delay, not loss: random extra delay per frame (jitter) and stalls after which several frames arrive
// back to back (bursts, as after a TCP retransmit).
struct Impairment {
    int jitter_ms = 0;
    int burst_every = 0;  // every N frames, hold `burst_size` frames and release them together
    int burst_size = 0;
};

// Dev/benchmark inputs that drive the live pipelines without a phone:
//  --play <file.h264|h265>   Annex-B video, one access unit per call at 60 fps, as the mirror stream delivers it.
//  --play-audio <file.m4a>   AAC-LC (44.1 kHz stereo) or ALAC packets in real time, as AirPlay audio delivers them.
// The lab also switches clips mid-stream (rotation / resolution change) and applies impairments.
class FileSource {
public:
    ~FileSource() { stop(); }
    bool start(const std::filesystem::path& path, ReceiverSink& sink);
    bool start_audio(const std::filesystem::path& path, AudioSink& sink);
    void stop();
    void stop_video();
    void stop_audio();
    void set_impairment(const Impairment& impairment);

private:
    std::thread thread_;
    std::thread audio_thread_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> video_running_ = false;
    std::atomic<bool> audio_running_ = false;
    bool mirror_started_ = false;
    std::mutex impairment_mutex_;
    Impairment impairment_;
};

}  // namespace pm
