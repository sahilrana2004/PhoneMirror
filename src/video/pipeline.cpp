#include "video/pipeline.h"

#include <avrt.h>

#include <cmath>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "app/log.h"
#include "net/wifi_tuning.h"
#include "record/recorder.h"
#include "video/renderer.h"

namespace pm {
namespace {

uint64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// Raises the calling thread into the MMCSS class once (UxPlay owns the network thread, so do it lazily).
void join_mmcss(const wchar_t* task) {
    thread_local bool joined = false;
    if (joined) return;
    joined = true;
    DWORD index = 0;
    if (HANDLE h = AvSetMmThreadCharacteristicsW(task, &index)) AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH);
}

}  // namespace

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
    for (AVFrame*& f : pool_) av_frame_free(&f);
    CloseHandle(frame_ready_);
    if (wifi_held_) wifi_low_latency().release_one();
}

void VideoPipeline::on_client(const std::string& name, const std::string& model) {
    log::info("Client: {} ({})", name, model);
    std::lock_guard lock(name_mutex_);
    client_name_ = name;
}

void VideoPipeline::on_mirror_start() {
    log::info("Mirroring started");
    if (!wifi_held_.exchange(true)) wifi_low_latency().acquire();
    PostMessageW(window_, kMsgMirrorStarted, 0, 0);
}

void VideoPipeline::on_mirror_stop() {
    log::info("Mirroring stopped (decoded {}, dropped {})", decoded_.load(), dropped_.load());
    if (wifi_held_.exchange(false)) wifi_low_latency().release_one();
    {
        std::lock_guard lock(decoder_mutex_);
        decoder_.close();
    }
    drop_frames();
    last_arrival_ = 0;
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

AVFrame* VideoPipeline::take_frame() {
    std::lock_guard lock(slot_mutex_);
    if (pool_.empty()) return av_frame_alloc();
    AVFrame* f = pool_.back();
    pool_.pop_back();
    return f;
}

// Caller holds slot_mutex_ (or is the sole owner during teardown).
void VideoPipeline::recycle(AVFrame*& frame) {
    if (!frame) return;
    av_frame_unref(frame);
    pool_.push_back(frame);
    frame = nullptr;
}

void VideoPipeline::on_video(const uint8_t* data, size_t size, bool h265) {
    join_mmcss(L"Playback");
    uint64_t received = qpc_now();
    if (last_arrival_) {
        double interval = static_cast<double>(received - last_arrival_) * 1000.0 / qpc_freq_;
        arrival_mean_ += (interval - arrival_mean_) / 32.0;
        jitter_ms_ = jitter_ms_ + (std::fabs(interval - arrival_mean_) - jitter_ms_) / 32.0;
    }
    last_arrival_ = received;
    bytes_ += size;
    h265_ = h265;
    if (recorder_) recorder_->push_video(data, size, h265);

    AVFrame* frame = take_frame();
    bool got = false;
    {
        std::lock_guard lock(decoder_mutex_);
        if (!decoder_.is_open() || decoder_.is_h265() != h265) {
            decoder_.open(renderer_.device(), renderer_.device_lock(), h265);
        }
        got = decoder_.decode(data, size, frame);
    }
    if (!got) {
        std::lock_guard lock(slot_mutex_);
        recycle(frame);
        return;
    }
    uint64_t decoded = qpc_now();
    frame->opaque = reinterpret_cast<void*>(received);
    // The decoded size is authoritative (also covers inputs that never report a size, like --play).
    if (frame->width != width_ || frame->height != height_) {
        width_ = frame->width;
        height_ = frame->height;
        if (recorder_) recorder_->set_video_size(frame->width, frame->height);
    }
    decode_ticks_ += decoded - received;
    ++decoded_;
    {
        std::lock_guard lock(slot_mutex_);
        if (pending_) {
            recycle(pending_);
            ++dropped_;
        }
        pending_ = frame;
    }
    SetEvent(frame_ready_);
}

void VideoPipeline::present_loop() {
    join_mmcss(L"Games");
    while (running_) {
        WaitForSingleObject(frame_ready_, 1000);
        AVFrame* frame = nullptr;
        {
            std::lock_guard lock(slot_mutex_);
            std::swap(frame, pending_);
        }
        if (!frame) {
            record_presented(0);  // keeps the stats line current while the screen is static
            continue;
        }

        VideoFrame vf;
        vf.texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        vf.slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
        vf.width = static_cast<UINT>(frame->width);
        vf.height = static_cast<UINT>(frame->height);
        vf.full_range = frame->color_range == AVCOL_RANGE_JPEG;
        vf.bt601 = frame->colorspace == AVCOL_SPC_SMPTE170M || frame->colorspace == AVCOL_SPC_BT470BG;
        renderer_.present_frame(vf);
        record_presented(qpc_now() - reinterpret_cast<uint64_t>(frame->opaque));

        std::lock_guard lock(slot_mutex_);
        recycle(shown_);
        shown_ = frame;
    }
}

// Receive -> Present() returned, i.e. everything on the PC before DWM composition and scanout.
void VideoPipeline::record_presented(uint64_t ticks) {
    uint64_t now = qpc_now();
    if (ticks) {
        ++stats_frames_;
        stats_total_ticks_ += ticks;
        stats_max_ticks_ = std::max(stats_max_ticks_, ticks);
    }
    if (!stats_start_) stats_start_ = now;
    double window_s = static_cast<double>(now - stats_start_) / qpc_freq_;
    if (window_s < 1.0) return;

    double ms = 1000.0 / qpc_freq_;
    uint64_t decoded = decoded_.load();
    uint64_t bytes = bytes_.load();
    uint64_t decoded_in_window = decoded - stats_decoded_base_;
    double fps = static_cast<double>(stats_frames_) / window_s;
    double decode_ms = decoded_in_window ? (decode_ticks_ - stats_decode_base_) * ms / decoded_in_window : 0;
    double present_ms = stats_frames_ ? stats_total_ticks_ * ms / stats_frames_ : 0;
    double max_ms = stats_max_ticks_ * ms;
    double mbps = static_cast<double>(bytes - stats_bytes_base_) * 8.0 / window_s / 1e6;

    renderer_.set_stats(std::format(L"{} {}×{}  {:.0f} fps\n"
                                    L"decode {:.2f} ms · to screen {:.2f} ms (max {:.1f})\n"
                                    L"{:.1f} Mbit/s · jitter {:.1f} ms · RTT {:.1f} ms · dropped {}",
                                    h265_ ? L"H.265" : L"H.264", width_.load(), height_.load(), fps, decode_ms,
                                    present_ms, max_ms, mbps, jitter_ms_.load(), rtt_ms_.load(), dropped_.load()));
    if (now >= stats_log_due_ && stats_frames_) {
        log::info("Video: {:.1f} fps, decode {:.2f} ms, receive->present {:.2f} ms max {:.2f}, {:.1f} Mbit/s, "
                  "jitter {:.1f} ms, RTT {:.1f} ms, dropped {}",
                  fps, decode_ms, present_ms, max_ms, mbps, jitter_ms_.load(), rtt_ms_.load(), dropped_.load());
        stats_log_due_ = now + static_cast<uint64_t>(qpc_freq_ * 10);
    }
    stats_start_ = now;
    stats_frames_ = 0;
    stats_total_ticks_ = 0;
    stats_max_ticks_ = 0;
    stats_decode_base_ = decode_ticks_.load();
    stats_decoded_base_ = decoded;
    stats_bytes_base_ = bytes;
}

void VideoPipeline::drop_frames() {
    renderer_.clear_frame();
    std::lock_guard lock(slot_mutex_);
    recycle(pending_);
    recycle(shown_);
}

}  // namespace pm
