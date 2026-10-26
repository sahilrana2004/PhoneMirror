#include "record/recorder.h"

#include <windows.h>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "app/log.h"

namespace pm {
namespace {

constexpr int kAudioRate = 44100;
constexpr AVRational kTimeBase{1, 1000000};

int64_t now_us() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<int64_t>(t.QuadPart / f.QuadPart * 1000000 + (t.QuadPart % f.QuadPart) * 1000000 / f.QuadPart);
}

// Calls fn(nal_type, nal_with_start_code, size) for every NAL unit of an annex-B buffer.
template <class Fn>
void for_each_nal(const uint8_t* p, size_t size, bool h265, Fn fn) {
    size_t i = 0;
    auto next_start = [&](size_t from) {
        for (size_t k = from; k + 3 <= size; ++k) {
            if (p[k] == 0 && p[k + 1] == 0 && (p[k + 2] == 1 || (k + 4 <= size && p[k + 2] == 0 && p[k + 3] == 1))) return k;
        }
        return size;
    };
    i = next_start(0);
    while (i < size) {
        size_t header = i + (p[i + 2] == 1 ? 3 : 4);
        size_t end = next_start(header);
        if (header < size) {
            int type = h265 ? (p[header] >> 1) & 0x3F : p[header] & 0x1F;
            fn(type, p + i, end - i);
        }
        i = end;
    }
}

bool is_keyframe(const uint8_t* p, size_t size, bool h265) {
    bool key = false;
    for_each_nal(p, size, h265, [&](int type, const uint8_t*, size_t) {
        if (h265 ? (type >= 16 && type <= 21) : type == 5) key = true;
    });
    return key;
}

std::vector<uint8_t> parameter_sets(const uint8_t* p, size_t size, bool h265) {
    std::vector<uint8_t> out;
    for_each_nal(p, size, h265, [&](int type, const uint8_t* nal, size_t n) {
        bool ps = h265 ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
        if (ps) out.insert(out.end(), nal, nal + n);
    });
    return out;
}

std::string av_error(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

bool Recorder::start(const std::filesystem::path& dir, const std::wstring& stem) {
    if (active_) return true;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    dir_ = dir;
    stem_ = stem;
    segment_ = 0;
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        stopping_ = false;
    }
    start_us_ = now_us();
    active_ = true;
    writer_ = std::thread([this] { writer_loop(); });
    log::info("Recording: waiting for a keyframe ({})", log::utf8((dir / stem).wstring()));
    return true;
}

void Recorder::stop() {
    if (!active_.exchange(false)) return;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (writer_.joinable()) writer_.join();
}

double Recorder::elapsed_seconds() const {
    return active_ ? static_cast<double>(now_us() - start_us_) / 1e6 : 0.0;
}

void Recorder::set_video_size(int width, int height) {
    width_ = width;
    height_ = height;
}

void Recorder::push_video(const uint8_t* annexb, size_t size, bool h265) {
    if (!active_) return;
    Item item;
    item.video = true;
    item.h265 = h265;
    item.keyframe = is_keyframe(annexb, size, h265);
    item.time_us = now_us();
    item.data.assign(annexb, annexb + size);
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(item));
    wake_.notify_one();
}

void Recorder::push_audio(const float* interleaved_stereo, int frames) {
    if (!active_) return;
    Item item;
    item.time_us = now_us();
    const auto* bytes = reinterpret_cast<const uint8_t*>(interleaved_stereo);
    item.data.assign(bytes, bytes + static_cast<size_t>(frames) * 2 * sizeof(float));
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(item));
    wake_.notify_one();
}

void Recorder::writer_loop() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty() && stopping_) break;
        Item item = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        if (item.video) {
            bool resized = out_ && (seg_width_ != width_ || seg_height_ != height_ || seg_h265_ != item.h265);
            if (item.keyframe && (!out_ || resized)) {
                close_segment();
                open_segment(item);
            }
            if (out_) write_video(item);
        } else if (out_) {
            write_audio(item);
        }
        lock.lock();
    }
    lock.unlock();
    close_segment();
}

bool Recorder::open_segment(const Item& key) {
    std::vector<uint8_t> extradata = parameter_sets(key.data.data(), key.data.size(), key.h265);
    if (extradata.empty() || width_ <= 0 || height_ <= 0) return false;  // need SPS/PPS and a known size

    std::filesystem::path path =
        dir_ / (segment_ == 0 ? stem_ + L".mp4" : std::format(L"{}-{}.mp4", stem_, segment_ + 1));
    std::string utf8_path = log::utf8(path.wstring());
    int err = avformat_alloc_output_context2(&out_, nullptr, "mp4", utf8_path.c_str());
    if (err < 0 || !out_) {
        log::error("Recording: cannot create {}: {}", utf8_path, av_error(err));
        out_ = nullptr;
        return false;
    }

    video_stream_ = avformat_new_stream(out_, nullptr);
    AVCodecParameters* vp = video_stream_->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = key.h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    vp->width = width_;
    vp->height = height_;
    vp->extradata = static_cast<uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(vp->extradata, extradata.data(), extradata.size());
    vp->extradata_size = static_cast<int>(extradata.size());
    if (key.h265) vp->codec_tag = MKTAG('h', 'v', 'c', '1');  // plays in QuickTime/Photos too
    video_stream_->time_base = kTimeBase;

    const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    aac_ = avcodec_alloc_context3(aac);
    aac_->sample_rate = kAudioRate;
    aac_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aac_->bit_rate = 160000;
    av_channel_layout_default(&aac_->ch_layout, 2);
    aac_->time_base = AVRational{1, kAudioRate};
    aac_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((err = avcodec_open2(aac_, aac, nullptr)) < 0) {
        log::error("Recording: AAC encoder: {}", av_error(err));
        avcodec_free_context(&aac_);
    } else {
        audio_stream_ = avformat_new_stream(out_, nullptr);
        avcodec_parameters_from_context(audio_stream_->codecpar, aac_);
        audio_stream_->time_base = aac_->time_base;
        aac_frame_ = av_frame_alloc();
        aac_frame_->nb_samples = aac_->frame_size;
        aac_frame_->format = aac_->sample_fmt;
        av_channel_layout_copy(&aac_frame_->ch_layout, &aac_->ch_layout);
        aac_frame_->sample_rate = kAudioRate;
        av_frame_get_buffer(aac_frame_, 0);
    }

    if ((err = avio_open(&out_->pb, utf8_path.c_str(), AVIO_FLAG_WRITE)) < 0) {
        log::error("Recording: cannot open {}: {}", utf8_path, av_error(err));
        close_segment();
        return false;
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    err = avformat_write_header(out_, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        log::error("Recording: header: {}", av_error(err));
        close_segment();
        return false;
    }
    header_written_ = true;
    packet_ = av_packet_alloc();
    segment_origin_us_ = key.time_us;
    last_video_pts_ = -1;
    audio_samples_ = 0;
    audio_origin_us_ = -1;
    pcm_.clear();
    seg_width_ = width_;
    seg_height_ = height_;
    seg_h265_ = key.h265;
    ++segment_;
    log::info("Recording: writing {} ({} {}x{})", utf8_path, key.h265 ? "H.265" : "H.264", seg_width_, seg_height_);
    return true;
}

void Recorder::close_segment() {
    if (!out_) return;
    if (header_written_) {
        if (aac_) encode_audio(true);
        av_write_trailer(out_);
    }
    header_written_ = false;
    if (out_->pb) avio_closep(&out_->pb);
    avformat_free_context(out_);
    out_ = nullptr;
    video_stream_ = nullptr;
    audio_stream_ = nullptr;
    av_frame_free(&aac_frame_);
    avcodec_free_context(&aac_);
    av_packet_free(&packet_);
    log::info("Recording: segment closed");
}

void Recorder::write_video(const Item& item) {
    int64_t pts = item.time_us - segment_origin_us_;
    if (pts <= last_video_pts_) pts = last_video_pts_ + 1;
    last_video_pts_ = pts;
    // The mov muxer converts annex-B to length-prefixed NAL units itself.
    av_packet_unref(packet_);
    packet_->data = const_cast<uint8_t*>(item.data.data());
    packet_->size = static_cast<int>(item.data.size());
    packet_->stream_index = video_stream_->index;
    packet_->pts = packet_->dts = av_rescale_q(pts, kTimeBase, video_stream_->time_base);
    if (item.keyframe) packet_->flags |= AV_PKT_FLAG_KEY;
    int err = av_interleaved_write_frame(out_, packet_);
    if (err < 0) log::error("Recording: video write: {}", av_error(err));
}

void Recorder::write_audio(const Item& item) {
    if (!aac_) return;
    if (audio_origin_us_ < 0) {
        int64_t offset = item.time_us - segment_origin_us_;
        if (offset < 0) return;  // audio from before the first keyframe of this segment
        audio_origin_us_ = offset;
    }
    const auto* samples = reinterpret_cast<const float*>(item.data.data());
    pcm_.insert(pcm_.end(), samples, samples + item.data.size() / sizeof(float));
    encode_audio(false);
}

void Recorder::encode_audio(bool flush) {
    int frame_size = aac_->frame_size;
    size_t offset = 0;
    auto submit = [&](AVFrame* frame) {
        if (avcodec_send_frame(aac_, frame) < 0) return;
        while (avcodec_receive_packet(aac_, packet_) >= 0) {
            packet_->stream_index = audio_stream_->index;
            int64_t origin = av_rescale_q(audio_origin_us_ < 0 ? 0 : audio_origin_us_, kTimeBase, aac_->time_base);
            packet_->pts += origin;
            packet_->dts += origin;
            av_packet_rescale_ts(packet_, aac_->time_base, audio_stream_->time_base);
            av_interleaved_write_frame(out_, packet_);
        }
    };
    while (pcm_.size() - offset >= static_cast<size_t>(frame_size) * 2) {
        av_frame_make_writable(aac_frame_);
        auto* left = reinterpret_cast<float*>(aac_frame_->data[0]);
        auto* right = reinterpret_cast<float*>(aac_frame_->data[1]);
        for (int i = 0; i < frame_size; ++i) {
            left[i] = pcm_[offset + i * 2];
            right[i] = pcm_[offset + i * 2 + 1];
        }
        aac_frame_->pts = audio_samples_;
        audio_samples_ += frame_size;
        offset += static_cast<size_t>(frame_size) * 2;
        submit(aac_frame_);
    }
    pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<ptrdiff_t>(offset));
    if (flush) {
        submit(nullptr);
        pcm_.clear();
    }
}

}  // namespace pm
