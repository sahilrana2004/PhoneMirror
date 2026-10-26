#pragma once

#include <atomic>
#include <filesystem>
#include <thread>

namespace pm {

class AudioSink;
class ReceiverSink;

// Dev/benchmark inputs that drive the live pipelines without a phone:
//  --play <file.h264|h265>   Annex-B video, one access unit per call at 60 fps, as the mirror stream delivers it.
//  --play-audio <file.m4a>   AAC-LC (44.1 kHz stereo) or ALAC packets in real time, as AirPlay audio delivers them.
class FileSource {
public:
    ~FileSource() { stop(); }
    bool start(const std::filesystem::path& path, ReceiverSink& sink);
    bool start_audio(const std::filesystem::path& path, AudioSink& sink);
    void stop();

private:
    std::thread thread_;
    std::thread audio_thread_;
    std::atomic<bool> running_ = false;
};

}  // namespace pm
