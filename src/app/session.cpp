#include "app/session.h"

#include "app/log.h"
#include "app/settings.h"

namespace pm {

bool Session::create(HINSTANCE instance) {
    if (!window_.create(instance, name_, slot_)) return false;
    video_ = std::make_unique<VideoPipeline>(window_.renderer(), window_.hwnd());
    audio_ = std::make_unique<AudioPipeline>();
    video_->set_recorder(&recorder_);
    audio_->set_recorder(&recorder_);
    audio_->set_delay_ms(static_cast<int>(Settings::instance().get_number("audio_delay_ms", 0)));
    window_.attach({slot_, video_.get(), audio_.get(), &recorder_, control_});
    return true;
}

bool Session::start_receiver() {
    Settings& s = Settings::instance();
    ReceiverConfig config;
    config.name = log::utf8(name_);
    config.slot = slot_;
    // iOS sizes the stream from the requested height and only exceeds 1080 lines over H.265.
    std::string codec = s.get_string("codec", "auto");
    config.h265 = codec == "h264" ? false : window_.renderer().supports_hevc();
    // The height sets portrait size (2160 -> the phone's native 994x2160); the width caps landscape. 2560 keeps
    // landscape near 3 MP instead of a 4K60 encode that makes the iPhone heat up and throttle after a few
    // minutes. H.264 tops out at 1080 lines.
    config.width = static_cast<int>(s.get_number("max_width", config.h265 ? 2560 : 1920));
    config.height = static_cast<int>(s.get_number("max_height", config.h265 ? 2160 : 1080));
    config.refresh_rate = static_cast<int>(s.get_number("refresh_rate", 60));
    config.max_fps = static_cast<int>(s.get_number("max_fps", 60));
    receiver_ = std::make_unique<Receiver>(*video_, *audio_);
    if (receiver_->start(config)) return true;
    receiver_.reset();
    ReceiverPorts ports = receiver_ports(slot_);
    window_.set_status(L"AirPlay receiver failed to start",
                       std::format(L"Another AirPlay receiver may be using TCP port {}. Details in "
                                   L"%APPDATA%\\PhoneMirror\\phonemirror.log",
                                   ports.http));
    return false;
}

void Session::start_files(const std::filesystem::path& video, const std::filesystem::path& audio) {
    if (!video.empty()) files_.start(video, *video_);
    if (!audio.empty()) files_.start_audio(audio, *audio_);
}

}  // namespace pm
