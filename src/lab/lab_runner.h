#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "video/renderer.h"

namespace pm {

class LabPhone;
class Session;

inline constexpr UINT kMsgLabQuit = WM_APP + 30;

// Runs a .lab scenario: a timeline of steps executed against the live app with real Windows input
// (SendInput), so the same code paths run as with a person at the keyboard. Format, one step per line:
//
//   <ms> video <clip>            switch/start the video clip (clips resolve against the scenario folder)
//   <ms> audio <clip>            start audio playback (restarts it if already playing)
//   <ms> audio-stop              stop sending audio, as a phone does when nothing is playing
//   <ms> impair jitter=<ms> burst=<n>/<every>
//   <ms> focus                   bring the mirror window to the front
//   <ms> key <combo>             e.g. ctrl+shift+r, f3, ctrl+1, ctrl+alt, esc
//   <ms> type <text...>          typed key by key (scan codes, like a physical keyboard)
//   <ms> click <x> <y>           click at a phone-screen position given as 0..1 fractions
//   <ms> click-pointer           click where the virtual iPhone pointer currently is (calibration)
//   <ms> move <dx> <dy>          relative mouse motion in counts
//   <ms> wheel <notches>
//   <ms> connect | disconnect    virtual iPhone keyboard/mouse link
//   <ms> session <n>             later steps act on mirror window / phone n (1-based; see "phones")
//   <ms> expect-pointer <x> <y> <tolerance>
//   <ms> expect-text <text...>
//   <ms> log <text...>
//   <ms> quit
//
// "phones <n>" (no timestamp) runs n mirror sessions, phone k named "Lab iPhone k".
// Lines starting with "expect " / "expect-not " are log assertions evaluated by lab/run-lab.ps1; '#' comments.
class LabRunner {
public:
    LabRunner(LabPhone& phone, std::vector<Session*> sessions, HWND app_window)
        : phone_(phone), sessions_(std::move(sessions)), app_(app_window) {}
    ~LabRunner();

    static int phones_in(const std::filesystem::path& scenario);
    static std::string phone_name(int slot) { return "Lab iPhone " + std::to_string(slot + 1); }

    bool load(const std::filesystem::path& scenario);
    void start();

    // Draws the named virtual iPhone (pointer, taps, typed text, last event) over its mirror window.
    static Renderer::Painter painter(LabPhone& phone, std::string phone_name);

private:
    struct Step {
        int at_ms = 0;
        std::string verb;
        std::vector<std::string> args;
        std::string rest;  // everything after the verb, for text arguments
    };

    void run();
    void execute(const Step& step);
    bool phone_to_screen(float x, float y, POINT& out);
    bool ensure_foreground();
    void click_at(POINT screen);

    Session& session() { return *sessions_[current_]; }

    LabPhone& phone_;
    std::vector<Session*> sessions_;
    size_t current_ = 0;
    HWND app_;
    std::filesystem::path dir_;
    std::vector<Step> steps_;
    std::thread thread_;
    std::atomic<bool> running_ = false;
};

}  // namespace pm
