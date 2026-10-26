#pragma once

#include <atomic>
#include <filesystem>
#include <thread>

namespace pm {

class ReceiverSink;

// Dev/benchmark input: replays an Annex-B .h264/.h265 file through the live pipeline at 60 fps,
// one access unit per call, exactly as the AirPlay mirror stream delivers it. Started by --play <file>.
class FileSource {
public:
    ~FileSource() { stop(); }
    bool start(const std::filesystem::path& path, ReceiverSink& sink);
    void stop();

private:
    std::thread thread_;
    std::atomic<bool> running_ = false;
};

}  // namespace pm
