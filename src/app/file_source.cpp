#include "app/file_source.h"

#include <windows.h>

#include <fstream>
#include <iterator>
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

    running_ = true;
    thread_ = std::thread([this, data = std::move(data), h265, &sink] {
        AVCodecParserContext* parser = av_parser_init(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        AVCodecContext* ctx = avcodec_alloc_context3(nullptr);
        sink.on_mirror_start();
        if (!sink.on_codec(h265)) log::error("--play: codec rejected");
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        while (running_) {
            QueryPerformanceCounter(&start);
            uint64_t frames = 0;
            const uint8_t* p = data.data();
            int left = static_cast<int>(data.size());
            while (running_ && left > 0) {
                uint8_t* out = nullptr;
                int out_size = 0;
                int used = av_parser_parse2(parser, ctx, &out, &out_size, p, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                p += used;
                left -= used;
                if (!out_size) continue;
                sink.on_video(out, static_cast<size_t>(out_size), h265);
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
    if (thread_.joinable()) thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
}

}  // namespace pm
