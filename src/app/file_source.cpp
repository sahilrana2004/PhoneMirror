#include "app/file_source.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
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

void FileSource::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

}  // namespace pm
