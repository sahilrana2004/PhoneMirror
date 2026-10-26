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
#include "record/recorder.h"

namespace pm {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kRate = 44100;  // every AirPlay audio format is 44.1 kHz stereo
constexpr int kChannels = 2;
constexpr size_t kRingFrames = kRate * 2;            // 2 s of headroom
constexpr size_t kMinTargetFrames = kRate * 50 / 1000;   // adaptive buffer: 50 ms on a clean link ...
constexpr size_t kStartTargetFrames = kRate * 80 / 1000; // first guess before the link is known
constexpr size_t kMaxTargetFrames = kRate * 300 / 1000;  // ... up to 300 ms on a bad one
constexpr size_t kGrowFrames = kRate * 20 / 1000;        // +20 ms per dropout
constexpr size_t kGapMarginFrames = kRate * 25 / 1000;   // margin over the longest recent arrival gap
constexpr size_t kShrinkFrames = kRate * 10 / 1000;      // -10 ms per 15 s without one
constexpr size_t kRateBandFrames = kRate * 40 / 1000;    // speed up beyond target + 40 ms
constexpr size_t kHardTrimFrames = kRate * 500 / 1000;   // cut only after a >0.5 s pile-up
constexpr double kPauseGapMs = 1000.0;                   // a longer silence is a pause, not jitter
constexpr ULONGLONG kIdleMs = 400;                       // nothing arrives this long after running dry: sound ended

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
    auto frames = static_cast<size_t>(f->nb_samples);
    scratch_.resize(frames * kChannels);
    for (size_t i = 0; i < frames; ++i) {
        float l = sample(f, 0, static_cast<int>(i));
        scratch_[i * 2] = l;
        scratch_[i * 2 + 1] = channels > 1 ? sample(f, 1, static_cast<int>(i)) : l;
    }
    if (Recorder* r = recorder_.load()) r->push_audio(scratch_.data(), static_cast<int>(frames));

    // Arrival-gap tracker: the longest recent silence between packets is how much audio the buffer must
    // hold to ride through the next one. The peak decays ~2 ms per second so the buffer can shrink again.
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (last_push_qpc_) {
        double gap_ms = static_cast<double>(now.QuadPart - last_push_qpc_) * 1000.0 / static_cast<double>(freq.QuadPart);
        if (gap_ms > kPauseGapMs) {
            peak_gap_ms_ = 0;  // the phone simply stopped sending (nothing playing): not network jitter
        } else {
            peak_gap_ms_ = std::max(gap_ms, peak_gap_ms_ - gap_ms * 0.005);  // decays ~5 ms per second
        }
        peak_gap_frames_ = static_cast<size_t>(peak_gap_ms_ * kRate / 1000.0);
    }
    last_push_qpc_ = now.QuadPart;
    last_push_tick_ = GetTickCount64();

    size_t write = write_pos_.load(std::memory_order_relaxed);
    size_t read = read_pos_.load(std::memory_order_acquire);
    size_t space = kRingFrames - (write - read);
    size_t n = std::min(frames, space);  // full ring: newest audio is dropped
    for (size_t i = 0; i < n; ++i) {
        size_t slot = ((write + i) % kRingFrames) * kChannels;
        ring_[slot] = scratch_[i * 2];
        ring_[slot + 1] = scratch_[i * 2 + 1];
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

        // Adaptive jitter buffer. AirPlay audio arrives over Wi-Fi in uneven clumps, so the amount queued
        // (ring + WASAPI padding) is steered toward a target that grows by 20 ms after every dropout and
        // relaxes slowly while the link is calm. Small deviations are absorbed by playing up to 1.5 %
        // faster/slower (linear-interpolation resampling, inaudible) instead of cutting or repeating audio.
        bool playing = false;
        size_t target = kStartTargetFrames;
        double frac = 0.0;   // fractional read offset for the rate-adjusted reader
        double rate = 1.0;
        uint64_t underruns = 0, total_underruns = 0, trims = 0;
        ULONGLONG last_underrun = GetTickCount64(), last_stats = GetTickCount64();
        ULONGLONG drained_at = 0;  // when the queue last ran dry (0 = not pending)
        HANDLE waits[2] = {wake_, buffer_event};
        while (running_ && !device_changed_) {
            WaitForMultipleObjects(2, waits, FALSE, 100);
            if (flush_.exchange(false)) {
                read_pos_.store(write_pos_.load(std::memory_order_acquire), std::memory_order_release);
                frac = 0.0;
                if (playing) {
                    client->Stop();
                    client->Reset();
                    playing = false;
                }
            }
            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;  // device invalidated: reopen the (new) default device

            ULONGLONG now = GetTickCount64();
            if (drained_at) {
                ULONGLONG last_push = last_push_tick_.load();
                if (last_push > drained_at) {
                    // Packets came back soon after running dry: the link stalled longer than the buffer
                    // covers. Grow the target (the queue re-primes to it before playing again).
                    ++underruns;
                    ++total_underruns;
                    last_underrun = now;
                    target = std::min(target + kGrowFrames, kMaxTargetFrames);
                    drained_at = 0;
                } else if (now - drained_at > kIdleMs) {
                    // Nothing more arrived: the sound simply ended. The next one starts from a fresh buffer
                    // instead of inheriting this one's history.
                    target = kStartTargetFrames;
                    drained_at = 0;
                }
            }
            const size_t delay = static_cast<size_t>(extra_delay_frames_.load());  // "Sync audio to video" offset
            // Hold at least the longest recent arrival gap plus a margin (predictive), never less.
            size_t needed = std::min(peak_gap_frames_.load() + kGapMarginFrames, kMaxTargetFrames);
            target = std::max(target, needed);
            const size_t goal = target + delay;
            size_t read = read_pos_.load(std::memory_order_relaxed);
            size_t available = write_pos_.load(std::memory_order_acquire) - read;
            size_t level = available + padding;

            if (!playing && available < goal) continue;  // (re)prime to the target before starting
            if (level > goal + kHardTrimFrames) {  // long stall then a flood: jump back to the target once
                size_t excess = std::min(available, level - goal);
                read += excess;
                available -= excess;
                level -= excess;
                ++trims;
            }
            if (level > goal + kRateBandFrames) {
                rate = 1.015;
            } else if (level + kRateBandFrames / 2 < goal) {
                rate = 0.985;
            } else if ((rate > 1.0 && level <= goal) || (rate < 1.0 && level >= goal)) {
                rate = 1.0;  // hysteresis: hold the correction until back on target
            }

            // Frames we can produce without reading past the newest sample (+1 for interpolation).
            size_t producible = available > 2 ? static_cast<size_t>((available - 2 - frac) / rate) : 0;
            size_t n = std::min<size_t>(producible, buffer_frames - padding);
            if (n) {
                BYTE* out = nullptr;
                if (FAILED(render->GetBuffer(static_cast<UINT32>(n), &out))) break;
                float gain = (muted_ || focus_muted_) ? 0.0f : gain_.load(std::memory_order_relaxed) * local_volume_.load(std::memory_order_relaxed);
                auto* dst = reinterpret_cast<float*>(out);
                double pos = frac;
                for (size_t i = 0; i < n; ++i) {
                    auto k = static_cast<size_t>(pos);
                    auto t = static_cast<float>(pos - static_cast<double>(k));
                    size_t a = ((read + k) % kRingFrames) * kChannels;
                    size_t b = ((read + k + 1) % kRingFrames) * kChannels;
                    dst[i * 2] = (ring_[a] + (ring_[b] - ring_[a]) * t) * gain;
                    dst[i * 2 + 1] = (ring_[a + 1] + (ring_[b + 1] - ring_[a + 1]) * t) * gain;
                    pos += rate;
                }
                render->ReleaseBuffer(static_cast<UINT32>(n), 0);
                auto whole = static_cast<size_t>(pos);
                read += whole;
                frac = pos - static_cast<double>(whole);
            }
            read_pos_.store(read, std::memory_order_release);
            if (!playing && n) {
                client->Start();
                playing = true;
            } else if (playing && !n && !padding) {
                client->Stop();
                client->Reset();
                playing = false;
                frac = 0.0;
                drained_at = now;  // stall or end of the sound? decided by what happens next
            }
            if (playing && now - last_underrun > 15000 && target > kMinTargetFrames) {
                target = std::max(target - kShrinkFrames, kMinTargetFrames);  // calm link: win latency back
                last_underrun = now;
            }
            if (now - last_stats >= 10000) {
                if (playing || underruns) {
                    log::info("Audio: buffer target {:.0f} ms (peak gap {:.0f} ms), queued {:.0f} ms, rate {:.3f}, underruns {} (total {}), trims {}",
                              goal * 1000.0 / kRate, peak_gap_frames_.load() * 1000.0 / kRate, level * 1000.0 / kRate, rate, underruns, total_underruns, trims);
                }
                underruns = trims = 0;
                last_stats = now;
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
