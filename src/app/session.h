#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>

#include "airplay/receiver.h"
#include "app/file_source.h"
#include "audio/audio_pipeline.h"
#include "record/recorder.h"
#include "ui/mirror_window.h"
#include "video/pipeline.h"

namespace pm {

// One receiver slot: its window, video/audio pipelines, recorder and AirPlay server. Several sessions run
// side by side for several phones; each advertises its own name, device id and port set.
class Session {
public:
    Session(int slot, std::wstring name, IControlBackend* control) : slot_(slot), name_(std::move(name)), control_(control) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool create(HINSTANCE instance);
    bool start_receiver();
    void start_files(const std::filesystem::path& video, const std::filesystem::path& audio);

    int slot() const { return slot_; }
    const std::wstring& name() const { return name_; }
    MirrorWindow& window() { return window_; }
    AudioPipeline& audio() { return *audio_; }
    VideoPipeline& video() { return *video_; }
    FileSource& files() { return files_; }

private:
    int slot_;
    std::wstring name_;
    IControlBackend* control_;

    // Destroyed bottom-up: inputs stop first, then pipelines, then the recorder and the window.
    MirrorWindow window_;
    Recorder recorder_;
    std::unique_ptr<VideoPipeline> video_;
    std::unique_ptr<AudioPipeline> audio_;
    std::unique_ptr<Receiver> receiver_;
    FileSource files_;
};

}  // namespace pm
