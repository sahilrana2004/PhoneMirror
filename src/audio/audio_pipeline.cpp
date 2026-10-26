#include "audio/audio_pipeline.h"

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "app/log.h"

namespace pm {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kRate = 44100;  // every AirPlay audio format is 44.1 kHz stereo
constexpr int kChannels = 2;
constexpr size_t kRingFrames = kRate;           // 1 s of headroom
constexpr size_t kStartFrames = kRate / 50;     // 20 ms queued before (re)starting playback
constexpr size_t kTargetFrames = kRate / 40;    // trim back to 25 ms ...
constexpr size_t kCeilingFrames = kRate * 3 / 50;  // ... once more than 60 ms is queued

// Codec configuration exactly as UxPlay feeds GStreamer (renderers/audio_renderer.c).
constexpr uint8_t kAacEld[] = {0xf8, 0xe8, 0x50, 0x00};  // ct 8: AAC-ELD 44100/2, 480 spf
constexpr uint8_t kAacLc[] = {0x12, 0x10};               // ct 4: AAC-LC 44100/2, 1024 spf
constexpr uint8_t kAlac[] = {                            // ct 2: ALAC 44100/16/2, 352 spf
    0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x60, 0x00, 0x10,
    0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};

unsigned hex(HRESULT hr) { return static_cast<unsigned>(hr); }

float sample(const AVFrame* f, int channel, int i) {
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP:
        return reinterpret_cast<const float*>(f->extended_data[channel])[i];
    case AV_SAMPLE_FMT_FLT:
        return reinterpret_cast<const float*>(f->extended_data[0])[i * f->ch_layout.nb_channels + channel];
    case AV_SAMPLE_FMT_S16P:
        return reinterpret_cast<const int16_t*>(f->extended_data[channel])[i] / 32768.0f;
    case AV_SAMPLE_FMT_S16:
        return reinterpret_cast<const int16_t*>(f->extended_data[0])[i * f->ch_layout.nb_channels + channel] / 32768.0f;
    case AV_SAMPLE_FMT_S32P:
        return reinterpret_cast<const int32_t*>(f->extended_data[channel])[i] / 2147483648.0f;
    case AV_SAMPLE_FMT_S32:
        return reinterpret_cast<const int32_t*>(f->extended_data[0])[i * f->ch_layout.nb_channels + channel] /
               2147483648.0f;
    default:
        return 0.0f;
    }
}

}  // namespace

AudioPipeline::AudioPipeline()
    : ring_(kRingFrames * kChannels), wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    render_thread_ = std::thread([this] { render_loop(); });
}

AudioPipeline::~AudioPipeline() {
    running_ = false;
    SetEvent(wake_);
    if (render_thread_.joinable()) render_thread_.join();
    close_decoder();
    CloseHandle(wake_);
}

HRESULT AudioPipeline::QueryInterface(REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
        *out = static_cast<IMMNotificationClient*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

HRESULT AudioPipeline::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) {
    if (flow == eRender && role == eConsole) {
        device_changed_ = true;
        SetEvent(wake_);
    }
    return S_OK;
}

void AudioPipeline::on_audio_volume(float db) {
    float gain = db <= -30.0f ? 0.0f : std::pow(10.0f, std::min(db, 0.0f) / 20.0f);
    gain_ = gain;
}

void AudioPipeline::on_audio_flush() {
    flush_ = true;
    SetEvent(wake_);
}

bool AudioPipeline::open_decoder(int ct) {
    close_decoder();
    const uint8_t* config = nullptr;
    size_t config_size = 0;
    AVCodecID id = AV_CODEC_ID_AAC;
    const char* name = "AAC-ELD";
    switch (ct) {
    case 8:
        config = kAacEld;
        config_size = sizeof(kAacEld);
        break;
    case 4:
        config = kAacLc;
        config_size = sizeof(kAacLc);
        name = "AAC-LC";
        break;
    case 2:
        id = AV_CODEC_ID_ALAC;
        config = kAlac;
        config_size = sizeof(kAlac);
        name = "ALAC";
        break;
    default:
        log::error("Audio: unsupported compression type {}", ct);
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(id);
    decoder_ = avcodec_alloc_context3(codec);
    decoder_->extradata = static_cast<uint8_t*>(av_mallocz(config_size + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(decoder_->extradata, config, config_size);
    decoder_->extradata_size = static_cast<int>(config_size);
    decoder_->sample_rate = kRate;
    av_channel_layout_default(&decoder_->ch_layout, kChannels);
    decoder_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (int err = avcodec_open2(decoder_, codec, nullptr); err < 0) {
        log::error("Audio: cannot open {} decoder ({})", name, err);
        close_decoder();
        return false;
    }
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    compression_type_ = ct;
    log::info("Audio: {} 44100 Hz stereo", name);
    return true;
}

void AudioPipeline::close_decoder() {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&decoder_);
    compression_type_ = -1;
}

void AudioPipeline::on_audio(const uint8_t* data, size_t size, int ct) {
    std::lock_guard lock(decoder_mutex_);
    if (ct != compression_type_ && !open_decoder(ct)) return;
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    if (avcodec_send_packet(decoder_, packet_) < 0) return;  // corrupt/lost frame: skip it, keep going
    while (avcodec_receive_frame(decoder_, frame_) >= 0) {
        push(frame_);
        av_frame_unref(frame_);
    }
}

void AudioPipeline::push(const AVFrame* f) {
    int channels = f->ch_layout.nb_channels;
    size_t write = write_pos_.load(std::memory_order_relaxed);
    size_t read = read_pos_.load(std::memory_order_acquire);
    size_t space = kRingFrames - (write - read);
    size_t n = std::min(static_cast<size_t>(f->nb_samples), space);  // full ring: newest audio is dropped
    for (size_t i = 0; i < n; ++i) {
        size_t slot = ((write + i) % kRingFrames) * kChannels;
        float l = sample(f, 0, static_cast<int>(i));
        ring_[slot] = l;
        ring_[slot + 1] = channels > 1 ? sample(f, 1, static_cast<int>(i)) : l;
    }
    write_pos_.store(write + n, std::memory_order_release);
    SetEvent(wake_);
}

void AudioPipeline::render_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (enumerator) enumerator->RegisterEndpointNotificationCallback(this);
    HANDLE buffer_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    while (running_ && enumerator) {
        device_changed_ = false;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);

        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = kChannels;
        fmt.Format.nSamplesPerSec = kRate;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = kChannels * 4;
        fmt.Format.nAvgBytesPerSec = kRate * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(fmt) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        // Shared mode with the engine's own sample-rate conversion (44.1 kHz -> device rate); buffer size 0
        // lets the engine pick its minimum for event-driven streams.
        if (SUCCEEDED(hr)) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    0, 0, &fmt.Format, nullptr);
        }
        UINT32 buffer_frames = 0;
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&buffer_frames);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(buffer_event);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(hr)) {
            log::error("Audio: WASAPI init failed 0x{:08X}; retrying", hex(hr));
            WaitForSingleObject(wake_, 1000);
            continue;
        }
        REFERENCE_TIME period = 0;
        client->GetDevicePeriod(&period, nullptr);
        log::info("Audio: WASAPI shared, buffer {:.1f} ms, period {:.1f} ms", buffer_frames * 1000.0 / kRate,
                  period / 10000.0);

        bool playing = false;
        HANDLE waits[2] = {wake_, buffer_event};
        while (running_ && !device_changed_) {
            WaitForMultipleObjects(2, waits, FALSE, 100);
            if (flush_.exchange(false)) {
                read_pos_.store(write_pos_.load(std::memory_order_acquire), std::memory_order_release);
                if (playing) {
                    client->Stop();
                    client->Reset();
                    playing = false;
                }
            }
            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;  // device invalidated: reopen the (new) default device

            size_t read = read_pos_.load(std::memory_order_relaxed);
            size_t available = write_pos_.load(std::memory_order_acquire) - read;
            if (!playing && available < kStartFrames) continue;
            if (available + padding > kCeilingFrames) {
                size_t excess = std::min(available, available + padding - kTargetFrames);
                read += excess;
                available -= excess;
            }
            size_t n = std::min<size_t>(available, buffer_frames - padding);
            if (n) {
                BYTE* out = nullptr;
                if (FAILED(render->GetBuffer(static_cast<UINT32>(n), &out))) break;
                float gain = gain_.load(std::memory_order_relaxed);
                auto* dst = reinterpret_cast<float*>(out);
                for (size_t i = 0; i < n; ++i) {
                    size_t slot = ((read + i) % kRingFrames) * kChannels;
                    dst[i * 2] = ring_[slot] * gain;
                    dst[i * 2 + 1] = ring_[slot + 1] * gain;
                }
                render->ReleaseBuffer(static_cast<UINT32>(n), 0);
                read += n;
            }
            read_pos_.store(read, std::memory_order_release);
            if (!playing && n) {
                client->Start();
                playing = true;
            } else if (playing && !n && !padding) {
                client->Stop();  // underrun: re-prime to 20 ms instead of stuttering on every packet
                client->Reset();
                playing = false;
            }
        }
        if (playing) client->Stop();
        if (running_) log::info("Audio: output device changed, reopening");
    }

    if (enumerator) enumerator->UnregisterEndpointNotificationCallback(this);
    CloseHandle(buffer_event);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

}  // namespace pm
