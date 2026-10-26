#include "video/decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

#include "app/log.h"

namespace pm {
namespace {

void lock_device(void* ctx) { static_cast<std::recursive_mutex*>(ctx)->lock(); }
void unlock_device(void* ctx) { static_cast<std::recursive_mutex*>(ctx)->unlock(); }

std::string av_error(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

AVPixelFormat pick_d3d11(AVCodecContext* ctx, const AVPixelFormat* formats) {
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f != AV_PIX_FMT_D3D11) continue;
        AVBufferRef* frames = nullptr;
        int err = avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, AV_PIX_FMT_D3D11, &frames);
        if (err < 0) {
            log::error("avcodec_get_hw_frames_parameters: {}", av_error(err));
            return AV_PIX_FMT_NONE;
        }
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        auto* d3d = static_cast<AVD3D11VAFramesContext*>(fc->hwctx);
        d3d->BindFlags |= D3D11_BIND_SHADER_RESOURCE;  // sample decoder output directly
        err = av_hwframe_ctx_init(frames);
        if (err < 0) {
            log::error("D3D11VA frame pool init: {}", av_error(err));
            av_buffer_unref(&frames);
            return AV_PIX_FMT_NONE;
        }
        av_buffer_unref(&ctx->hw_frames_ctx);
        ctx->hw_frames_ctx = frames;
        log::info("D3D11VA surfaces: {}x{} {}, pool {}", fc->width, fc->height, av_get_pix_fmt_name(fc->sw_format),
                  fc->initial_pool_size);
        return AV_PIX_FMT_D3D11;
    }
    log::error("Decoder offered no D3D11 output format");
    return AV_PIX_FMT_NONE;
}

}  // namespace

bool VideoDecoder::open(ID3D11Device* device, std::recursive_mutex& device_lock, bool h265) {
    close();
    h265_ = h265;

    device_ref_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    auto* hw = reinterpret_cast<AVHWDeviceContext*>(device_ref_->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(hw->hwctx);
    device->AddRef();
    d3d->device = device;
    d3d->lock = &lock_device;
    d3d->unlock = &unlock_device;
    d3d->lock_ctx = &device_lock;
    int err = av_hwdevice_ctx_init(device_ref_);
    if (err < 0) {
        log::error("D3D11VA device init: {}", av_error(err));
        close();
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    ctx_ = avcodec_alloc_context3(codec);
    ctx_->hw_device_ctx = av_buffer_ref(device_ref_);
    ctx_->get_format = &pick_d3d11;
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx_->thread_count = 1;
    // One surface is held by the renderer while the next decodes; give the pool slack for that.
    ctx_->extra_hw_frames = 3;
    err = avcodec_open2(ctx_, codec, nullptr);
    if (err < 0) {
        log::error("avcodec_open2 ({}): {}", h265 ? "hevc" : "h264", av_error(err));
        close();
        return false;
    }
    packet_ = av_packet_alloc();
    scratch_ = av_frame_alloc();
    log::info("Decoder open: {} (D3D11VA, low-delay)", h265 ? "H.265" : "H.264");
    return true;
}

void VideoDecoder::close() {
    av_frame_free(&scratch_);
    av_packet_free(&packet_);
    avcodec_free_context(&ctx_);
    av_buffer_unref(&device_ref_);
}

bool VideoDecoder::decode(const uint8_t* data, size_t size, AVFrame* out) {
    if (!ctx_) return false;
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    int err = avcodec_send_packet(ctx_, packet_);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        log::error("avcodec_send_packet: {}", av_error(err));
        return false;
    }
    bool got = false;
    while (avcodec_receive_frame(ctx_, scratch_) >= 0) {
        av_frame_unref(out);  // only the newest picture of this input matters
        av_frame_move_ref(out, scratch_);
        got = true;
    }
    return got;
}

}  // namespace pm
