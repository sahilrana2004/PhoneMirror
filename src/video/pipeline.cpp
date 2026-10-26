#include "video/pipeline.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "app/log.h"
#include "video/renderer.h"

namespace pm {

VideoPipeline::VideoPipeline(Renderer& renderer, HWND window)
    : renderer_(renderer), window_(window), frame_ready_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpc_freq_ = static_cast<double>(freq.QuadPart);
    present_thread_ = std::thread([this] { present_loop(); });
}

VideoPipeline::~VideoPipeline() {
    running_ = false;
    SetEvent(frame_ready_);
    if (present_thread_.joinable()) present_thread_.join();
    drop_frames();
    CloseHandle(frame_ready_);
}

void VideoPipeline::on_client(const std::string& name, const std::string& model) {
    log::info("Client: {} ({})", name, model);
}

void VideoPipeline::on_mirror_start() {
    log::info("Mirroring started");
    PostMessageW(window_, kMsgMirrorStarted, 0, 0);
}

void VideoPipeline::on_mirror_stop() {
    log::info("Mirroring stopped (decoded {}, dropped {})", decoded_.load(), dropped_.load());
    {
        std::lock_guard lock(decoder_mutex_);
        decoder_.close();
    }
    drop_frames();
    PostMessageW(window_, kMsgMirrorStopped, 0, 0);
}

bool VideoPipeline::on_codec(bool h265) {
    if (h265 && !renderer_.supports_hevc()) return false;
    std::lock_guard lock(decoder_mutex_);
    if (decoder_.is_open() && decoder_.is_h265() == h265) return true;
    return decoder_.open(renderer_.device(), renderer_.device_lock(), h265);
}

void VideoPipeline::on_video_size(int width, int height) {
    PostMessageW(window_, kMsgVideoSize, static_cast<WPARAM>(width), static_cast<LPARAM>(height));
}

void VideoPipeline::on_video(const uint8_t* data, size_t size, bool h265) {
    LARGE_INTEGER received, decoded;
    QueryPerformanceCounter(&received);
    AVFrame* frame = nullptr;
    {
        std::lock_guard lock(decoder_mutex_);
        if (!decoder_.is_open() || decoder_.is_h265() != h265) {
            if (!decoder_.open(renderer_.device(), renderer_.device_lock(), h265)) return;
        }
        frame = decoder_.decode(data, size);
    }
    if (!frame) return;
    QueryPerformanceCounter(&decoded);
    frame->opaque = reinterpret_cast<void*>(received.QuadPart);
    decode_ticks_ += static_cast<uint64_t>(decoded.QuadPart - received.QuadPart);
    ++decoded_;
    {
        std::lock_guard lock(slot_mutex_);
        if (pending_) {
            av_frame_free(&pending_);
            ++dropped_;
        }
        pending_ = frame;
    }
    SetEvent(frame_ready_);
}

void VideoPipeline::present_loop() {
    while (running_) {
        WaitForSingleObject(frame_ready_, INFINITE);
        AVFrame* frame = nullptr;
        {
            std::lock_guard lock(slot_mutex_);
            std::swap(frame, pending_);
        }
        if (!frame) continue;

        VideoFrame vf;
        vf.texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        vf.slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
        vf.width = static_cast<UINT>(frame->width);
        vf.height = static_cast<UINT>(frame->height);
        vf.full_range = frame->color_range == AVCOL_RANGE_JPEG;
        vf.bt601 = frame->colorspace == AVCOL_SPC_SMPTE170M || frame->colorspace == AVCOL_SPC_BT470BG;
        renderer_.present_frame(vf);
        LARGE_INTEGER presented;
        QueryPerformanceCounter(&presented);
        record_presented(static_cast<uint64_t>(presented.QuadPart - reinterpret_cast<intptr_t>(frame->opaque)));

        std::lock_guard lock(slot_mutex_);
        av_frame_free(&shown_);
        shown_ = frame;
    }
}

// Receive -> Present() returned, i.e. everything on the PC before DWM composition and scanout.
void VideoPipeline::record_presented(uint64_t ticks) {
    ++stats_frames_;
    stats_total_ticks_ += ticks;
    stats_max_ticks_ = std::max(stats_max_ticks_, ticks);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!stats_start_) stats_start_ = static_cast<uint64_t>(now.QuadPart);
    double window_s = static_cast<double>(static_cast<uint64_t>(now.QuadPart) - stats_start_) / qpc_freq_;
    if (window_s < 5.0) return;
    uint64_t decoded = decoded_.load();
    double ms = 1000.0 / qpc_freq_;
    log::info("Video: {:.1f} fps, decode avg {:.2f} ms, receive->present avg {:.2f} ms max {:.2f} ms, dropped {}",
              stats_frames_ / window_s, (decode_ticks_ - stats_decode_base_) * ms / (decoded - stats_decoded_base_),
              stats_total_ticks_ * ms / stats_frames_, stats_max_ticks_ * ms, dropped_.load());
    stats_start_ = static_cast<uint64_t>(now.QuadPart);
    stats_frames_ = 0;
    stats_total_ticks_ = 0;
    stats_max_ticks_ = 0;
    stats_decode_base_ = decode_ticks_.load();
    stats_decoded_base_ = decoded;
}

void VideoPipeline::drop_frames() {
    renderer_.clear_frame();
    std::lock_guard lock(slot_mutex_);
    av_frame_free(&pending_);
    av_frame_free(&shown_);
}

}  // namespace pm
