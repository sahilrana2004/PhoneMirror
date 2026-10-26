#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "airplay/receiver.h"
#include "video/decoder.h"

struct AVFrame;

namespace pm {

class Renderer;
class Recorder;

// Window messages posted from the pipeline to the mirror window.
inline constexpr UINT kMsgMirrorStarted = WM_APP + 10;
inline constexpr UINT kMsgMirrorStopped = WM_APP + 11;
inline constexpr UINT kMsgVideoSize = WM_APP + 12;  // wParam = width, lParam = height

// Receive -> decode inline on UxPlay's mirror thread -> newest-frame slot -> dedicated present thread.
// No jitter buffer and no clock: a frame is shown the moment the swapchain can take it, and a frame
// that is overtaken before that is dropped. Decoded frames come from a small pool (no per-frame allocation).
class VideoPipeline final : public ReceiverSink {
public:
    VideoPipeline(Renderer& renderer, HWND window);
    ~VideoPipeline() override;

    void set_recorder(Recorder* recorder) { recorder_ = recorder; }

    void on_client(const std::string& name, const std::string& model) override;
    void on_mirror_start() override;
    void on_mirror_stop() override;
    bool on_codec(bool h265) override;
    void on_video_size(int width, int height) override;
    void on_video(const uint8_t* data, size_t size, bool h265) override;
    void on_link_rtt(double ms) override { rtt_ms_ = ms; }

    std::string client_name() const {
        std::lock_guard lock(name_mutex_);
        return client_name_;
    }

private:
    void present_loop();
    void drop_frames();
    AVFrame* take_frame();
    void recycle(AVFrame*& frame);
    void record_presented(uint64_t ticks);

    Renderer& renderer_;
    HWND window_;
    Recorder* recorder_ = nullptr;
    mutable std::mutex name_mutex_;
    std::string client_name_;

    std::mutex decoder_mutex_;
    VideoDecoder decoder_;

    std::mutex slot_mutex_;
    AVFrame* pending_ = nullptr;
    AVFrame* shown_ = nullptr;  // keeps the displayed surface out of the decoder pool
    std::vector<AVFrame*> pool_;
    HANDLE frame_ready_ = nullptr;

    std::thread present_thread_;
    std::atomic<bool> running_ = true;
    std::atomic<bool> wifi_held_ = false;
    std::atomic<uint64_t> decoded_ = 0;
    std::atomic<uint64_t> dropped_ = 0;
    std::atomic<uint64_t> decode_ticks_ = 0;
    std::atomic<uint64_t> bytes_ = 0;
    std::atomic<double> rtt_ms_ = 0;
    std::atomic<bool> h265_ = false;
    std::atomic<int> width_ = 0;
    std::atomic<int> height_ = 0;

    // Arrival jitter, network thread only: mean absolute deviation of packet inter-arrival time.
    uint64_t last_arrival_ = 0;
    double arrival_mean_ = 0;
    std::atomic<double> jitter_ms_ = 0;

    // Present-thread-only statistics window.
    double qpc_freq_ = 1;
    uint64_t stats_start_ = 0;
    uint64_t stats_frames_ = 0;
    uint64_t stats_total_ticks_ = 0;
    uint64_t stats_max_ticks_ = 0;
    uint64_t stats_decode_base_ = 0;
    uint64_t stats_decoded_base_ = 0;
    uint64_t stats_bytes_base_ = 0;
    uint64_t stats_log_due_ = 0;
};

}  // namespace pm
