#pragma once

#include <d3d11.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace pm {

// Hardware (D3D11VA) H.264/H.265 decoder configured for zero added latency: no frame threading,
// no reorder delay, output surfaces created shader-readable so the renderer samples them in place.
class VideoDecoder {
public:
    VideoDecoder() = default;
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    ~VideoDecoder() { close(); }

    bool open(ID3D11Device* device, std::recursive_mutex& device_lock, bool h265);
    void close();
    bool is_open() const { return ctx_ != nullptr; }
    bool is_h265() const { return h265_; }

    // Feeds one Annex-B access unit. Returns the newest decoded picture (caller owns it, av_frame_free),
    // or nullptr when the decoder produced nothing for this input.
    AVFrame* decode(const uint8_t* data, size_t size);

private:
    AVBufferRef* device_ref_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVPacket* packet_ = nullptr;
    bool h265_ = false;
};

}  // namespace pm
