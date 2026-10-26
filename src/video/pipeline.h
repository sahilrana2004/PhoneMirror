#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "airplay/receiver.h"
#include "video/decoder.h"

struct AVFrame;

namespace pm {

class Renderer;

// Window messages posted from the pipeline to the mirror window.
inline constexpr UINT kMsgMirrorStarted = WM_APP + 10;
inline constexpr UINT kMsgMirrorStopped = WM_APP + 11;
inline constexpr UINT kMsgVideoSize = WM_APP + 12;  // wParam = width, lParam = height

// Receive -> decode inline on UxPlay's mirror thread -> newest-frame slot -> dedicated present thread.
// No jitter buffer and no clock: a frame is shown the moment the swapchain can take it, and a frame
// that is overtaken before that is dropped.
class VideoPipeline final : public ReceiverSink {
public:
    VideoPipeline(Renderer& renderer, HWND window);
    ~VideoPipeline() override;

    void on_client(const std::string& name, const std::string& model) override;
    void on_mirror_start() override;
    void on_mirror_stop() override;
    bool on_codec(bool h265) override;
    void on_video_size(int width, int height) override;
    void on_video(const uint8_t* data, size_t size, bool h265) override;

private:
    void present_loop();
    void drop_frames();
    void record_presented(uint64_t ticks);

    Renderer& renderer_;
    HWND window_;

    std::mutex decoder_mutex_;
    VideoDecoder decoder_;

    std::mutex slot_mutex_;
    AVFrame* pending_ = nullptr;
    AVFrame* shown_ = nullptr;  // keeps the displayed surface out of the decoder pool
    HANDLE frame_ready_ = nullptr;

    std::thread present_thread_;
    std::atomic<bool> running_ = true;
    std::atomic<uint64_t> decoded_ = 0;
    std::atomic<uint64_t> dropped_ = 0;
    std::atomic<uint64_t> decode_ticks_ = 0;

    // Present-thread-only statistics window.
    double qpc_freq_ = 1;
    uint64_t stats_start_ = 0;
    uint64_t stats_frames_ = 0;
    uint64_t stats_total_ticks_ = 0;
    uint64_t stats_max_ticks_ = 0;
    uint64_t stats_decode_base_ = 0;
    uint64_t stats_decoded_base_ = 0;
};

}  // namespace pm
