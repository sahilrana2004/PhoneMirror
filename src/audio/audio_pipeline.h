#pragma once

#include <windows.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "airplay/receiver.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace pm {

class Recorder;

// AirPlay audio (AAC-ELD while mirroring, ALAC/AAC-LC for audio-only AirPlay) -> FFmpeg decode on the
// network thread -> lock-free ring -> event-driven WASAPI render thread. Audio is played as soon as it
// arrives (never delayed to match video); queued audio beyond a small ceiling is discarded so clock
// drift between the phone and the sound card cannot build up latency.
class AudioPipeline final : public AudioSink, public IMMNotificationClient {
public:
    AudioPipeline();
    ~AudioPipeline();
    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    void on_audio(const uint8_t* data, size_t size, int compression_type) override;
    void on_audio_volume(float db) override;
    void on_audio_flush() override;

    void set_recorder(Recorder* recorder) { recorder_ = recorder; }
    void set_muted(bool muted) { muted_ = muted; }
    void set_focus_muted(bool muted) { focus_muted_ = muted; }  // another session has the audio focus
    bool muted() const { return muted_; }
    void set_local_volume(float volume) { local_volume_ = volume; }  // 0..1, on top of the phone's volume
    float local_volume() const { return local_volume_; }

    // IMMNotificationClient: follow the Windows default output device.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

private:
    bool open_decoder(int compression_type);
    void close_decoder();
    void push(const AVFrame* frame);
    void render_loop();

    // Single-producer (network thread) / single-consumer (render thread) ring of interleaved stereo floats.
    std::vector<float> ring_;
    std::atomic<size_t> write_pos_ = 0;  // in frames, monotonically increasing
    std::atomic<size_t> read_pos_ = 0;
    std::atomic<bool> flush_ = false;

    std::mutex decoder_mutex_;
    AVCodecContext* decoder_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int compression_type_ = -1;
    std::vector<float> scratch_;

    std::atomic<float> gain_ = 1.0f;
    std::atomic<float> local_volume_ = 1.0f;
    std::atomic<bool> muted_ = false;
    std::atomic<bool> focus_muted_ = false;
    std::atomic<Recorder*> recorder_ = nullptr;
    std::atomic<bool> running_ = true;
    std::atomic<bool> device_changed_ = false;
    HANDLE wake_ = nullptr;
    std::thread render_thread_;
};

}  // namespace pm
