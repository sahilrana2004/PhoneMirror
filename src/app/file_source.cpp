#include "app/file_source.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <random>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "airplay/receiver.h"
#include "app/log.h"

namespace pm {

bool FileSource::start(const std::filesystem::path& path, ReceiverSink& sink) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        log::error("--play: cannot open {}", log::utf8(path.wstring()));
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring ext = path.extension().wstring();
    bool h265 = ext == L".h265" || ext == L".hevc" || ext == L".265";
    log::info("--play: {} ({} bytes, {})", log::utf8(path.wstring()), data.size(), h265 ? "H.265" : "H.264");

    stop_video();  // switching clips mid-stream is how the lab simulates rotation
    running_ = true;
    video_running_ = true;
    bool first = !mirror_started_;
    mirror_started_ = true;
    thread_ = std::thread([this, data = std::move(data), h265, first, &sink] {
        AVCodecParserContext* parser = av_parser_init(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        AVCodecContext* ctx = avcodec_alloc_context3(nullptr);
        if (first) sink.on_mirror_start();
        if (!sink.on_codec(h265)) log::error("--play: codec rejected");
        std::minstd_rand rng(12345);
        int reported_w = 0, reported_h = 0;
        std::vector<std::vector<uint8_t>> held;  // frames stuck behind a simulated stall
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        uint64_t total = 0;
        while (video_running_) {
            QueryPerformanceCounter(&start);
            uint64_t frames = 0;
            const uint8_t* p = data.data();
            int left = static_cast<int>(data.size());
            while (video_running_ && left > 0) {
                uint8_t* out = nullptr;
                int out_size = 0;
                int used = av_parser_parse2(parser, ctx, &out, &out_size, p, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                p += used;
                left -= used;
                if (!out_size) continue;
                if (parser->width > 0 && (parser->width != reported_w || parser->height != reported_h)) {
                    reported_w = parser->width;
                    reported_h = parser->height;
                    sink.on_video_size(reported_w, reported_h);  // as AirPlay reports a rotation
                }
                Impairment imp;
                {
                    std::lock_guard lock(impairment_mutex_);
                    imp = impairment_;
                }
                ++total;
                if (imp.burst_every > 0 && imp.burst_size > 0 && (total % imp.burst_every) < static_cast<uint64_t>(imp.burst_size)) {
                    held.emplace_back(out, out + out_size);
                } else {
                    if (imp.jitter_ms > 0) Sleep(static_cast<DWORD>(rng() % (imp.jitter_ms + 1)));
                    for (auto& f : held) sink.on_video(f.data(), f.size(), h265);
                    held.clear();
                    sink.on_video(out, static_cast<size_t>(out_size), h265);
                }
                ++frames;
                // Pace at 60 fps against the clock, not by sleeping a fixed amount per frame.
                for (;;) {
                    QueryPerformanceCounter(&now);
                    double due = static_cast<double>(frames) / 60.0;
                    double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
                    if (elapsed >= due) break;
                    Sleep(static_cast<DWORD>((due - elapsed) * 1000.0) > 1 ? 1 : 0);
                }
            }
            av_parser_close(parser);
            parser = av_parser_init(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        }
        av_parser_close(parser);
        avcodec_free_context(&ctx);
    });
    return true;
}

void FileSource::stop_video() {
    video_running_ = false;
    if (thread_.joinable()) thread_.join();
}

void FileSource::set_impairment(const Impairment& impairment) {
    std::lock_guard lock(impairment_mutex_);
    impairment_ = impairment;
    log::info("Lab: impairment jitter {} ms, burst {} of every {} frames", impairment.jitter_ms, impairment.burst_size,
              impairment.burst_every);
}

bool FileSource::start_audio(const std::filesystem::path& path, AudioSink& sink) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, log::utf8(path.wstring()).c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(fmt, nullptr) < 0) {
        log::error("--play-audio: cannot open {}", log::utf8(path.wstring()));
        avformat_close_input(&fmt);
        return false;
    }
    int index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVCodecID codec = index >= 0 ? fmt->streams[index]->codecpar->codec_id : AV_CODEC_ID_NONE;
    int ct = codec == AV_CODEC_ID_ALAC ? 2 : codec == AV_CODEC_ID_AAC ? 4 : 0;
    if (!ct) {
        log::error("--play-audio: needs an AAC-LC or ALAC stream");
        avformat_close_input(&fmt);
        return false;
    }
    log::info("--play-audio: {} (ct {})", log::utf8(path.wstring()), ct);
    running_ = true;
    audio_thread_ = std::thread([this, fmt, index, ct, &sink]() mutable {
        AVRational tb = fmt->streams[index]->time_base;
        AVPacket* pkt = av_packet_alloc();
        ULONGLONG start = GetTickCount64();
        while (running_ && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == index) {
                if (pkt->pts != AV_NOPTS_VALUE) {  // AAC priming makes the first pts negative
                    int64_t due_ms = pkt->pts * 1000 * tb.num / tb.den;
                    auto now = static_cast<int64_t>(GetTickCount64() - start);
                    if (due_ms > now) Sleep(static_cast<DWORD>(due_ms - now));
                }
                sink.on_audio(pkt->data, static_cast<size_t>(pkt->size), ct);
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        avformat_close_input(&fmt);
    });
    return true;
}

void FileSource::stop() {
    running_ = false;
    stop_video();
    if (audio_thread_.joinable()) audio_thread_.join();
}

}  // namespace pm
