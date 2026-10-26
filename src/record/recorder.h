#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

namespace pm {

// MP4 recorder. Video is the phone's compressed H.264/H.265 stream remuxed as-is (no re-encode, nothing
// added to the live path); audio is the decoded AirPlay audio re-encoded to AAC-LC for compatibility.
// Writing happens on its own thread. A resolution change (rotation) starts a new file segment at the
// next keyframe, because a single MP4 track cannot change its parameter sets reliably across players.
class Recorder {
public:
    Recorder() = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    ~Recorder() { stop(); }

    // Output goes to <dir>/<stem>.mp4, then <stem>-2.mp4, ... for later segments.
    bool start(const std::filesystem::path& dir, const std::wstring& stem);
    void stop();
    bool active() const { return active_; }
    double elapsed_seconds() const;

    // Any thread.
    void set_video_size(int width, int height);
    void push_video(const uint8_t* annexb, size_t size, bool h265);
    void push_audio(const float* interleaved_stereo, int frames);  // 44.1 kHz

private:
    struct Item {
        bool video = false;
        bool keyframe = false;
        bool h265 = false;
        int64_t time_us = 0;
        std::vector<uint8_t> data;  // annex-B for video, float32 interleaved stereo for audio
    };

    void writer_loop();
    bool open_segment(const Item& keyframe);
    void close_segment();
    void write_video(const Item& item);
    void write_audio(const Item& item);
    void encode_audio(bool flush);

    std::filesystem::path dir_;
    std::wstring stem_;
    int segment_ = 0;

    std::atomic<bool> active_ = false;
    std::atomic<int64_t> start_us_ = 0;
    std::atomic<int> width_ = 0;
    std::atomic<int> height_ = 0;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> queue_;
    bool stopping_ = false;
    std::thread writer_;

    // Writer thread only.
    AVFormatContext* out_ = nullptr;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    AVCodecContext* aac_ = nullptr;
    AVFrame* aac_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::vector<float> pcm_;  // pending interleaved samples not yet encoded
    int64_t audio_samples_ = 0;
    int64_t audio_origin_us_ = -1;
    int64_t last_video_pts_ = -1;
    int64_t segment_origin_us_ = 0;
    int seg_width_ = 0;
    int seg_height_ = 0;
    bool seg_h265_ = false;
    bool header_written_ = false;
};

}  // namespace pm
