#include "lab/lab_runner.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>

#include "app/log.h"
#include "app/session.h"
#include "lab/lab_phone.h"
#include "ui/phone_geometry.h"

namespace pm {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

INPUT key_input(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    // Extended keys need the flag so the scan code matches a physical keyboard's.
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    return in;
}

WORD vk_for(const std::string& name) {
    static const std::pair<const char*, WORD> named[] = {
        {"ctrl", VK_CONTROL}, {"shift", VK_SHIFT}, {"alt", VK_MENU}, {"win", VK_LWIN}, {"esc", VK_ESCAPE},
        {"enter", VK_RETURN}, {"tab", VK_TAB}, {"space", VK_SPACE}, {"backspace", VK_BACK}, {"left", VK_LEFT},
        {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN}, {"volup", VK_VOLUME_UP}, {"voldown", VK_VOLUME_DOWN},
    };
    for (auto& [n, vk] : named) {
        if (name == n) return vk;
    }
    if (name.size() >= 2 && name[0] == 'f' && isdigit(static_cast<unsigned char>(name[1]))) {
        return static_cast<WORD>(VK_F1 + std::stoi(name.substr(1)) - 1);
    }
    if (name.size() == 1) return static_cast<WORD>(LOBYTE(VkKeyScanW(static_cast<wchar_t>(name[0]))));
    return 0;
}

// Presses a combo like "ctrl+shift+r": modifiers down in order, key tap, modifiers up in reverse.
void send_combo(const std::string& combo) {
    std::vector<WORD> keys;
    std::stringstream ss(lower(combo));
    std::string part;
    while (std::getline(ss, part, '+')) {
        if (WORD vk = vk_for(part)) keys.push_back(vk);
    }
    std::vector<INPUT> inputs;
    for (WORD vk : keys) inputs.push_back(key_input(vk, false));
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) inputs.push_back(key_input(*it, true));
    // Hold the chord briefly the way a person does, so hooks see the modifiers down.
    size_t half = keys.size();
    SendInput(static_cast<UINT>(half), inputs.data(), sizeof(INPUT));
    Sleep(30);
    SendInput(static_cast<UINT>(inputs.size() - half), inputs.data() + half, sizeof(INPUT));
}

void type_text(const std::wstring& text) {
    for (wchar_t c : text) {
        SHORT vk = VkKeyScanW(c);
        if (vk == -1) continue;
        bool shift = (vk >> 8) & 1;
        std::vector<INPUT> in;
        if (shift) in.push_back(key_input(VK_SHIFT, false));
        in.push_back(key_input(LOBYTE(vk), false));
        in.push_back(key_input(LOBYTE(vk), true));
        if (shift) in.push_back(key_input(VK_SHIFT, true));
        SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
        Sleep(25);
    }
}

}  // namespace

LabRunner::~LabRunner() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool LabRunner::load(const std::filesystem::path& scenario) {
    std::ifstream in(scenario);
    if (!in) {
        log::error("Lab: cannot open scenario {}", log::utf8(scenario.wstring()));
        return false;
    }
    dir_ = scenario.parent_path();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#' || !isdigit(static_cast<unsigned char>(line[start]))) {
            continue;  // comments and runner-side "expect" lines
        }
        std::stringstream ss(line.substr(start));
        Step step;
        ss >> step.at_ms >> step.verb;
        std::getline(ss, step.rest);
        step.rest.erase(0, step.rest.find_first_not_of(" \t"));
        std::stringstream args(step.rest);
        std::string a;
        while (args >> a) step.args.push_back(a);
        steps_.push_back(std::move(step));
    }
    std::stable_sort(steps_.begin(), steps_.end(), [](const Step& a, const Step& b) { return a.at_ms < b.at_ms; });
    log::info("Lab: scenario {} ({} steps)", log::utf8(scenario.filename().wstring()), steps_.size());
    return true;
}

int LabRunner::phones_in(const std::filesystem::path& scenario) {
    std::ifstream in(scenario);
    std::string word;
    int n = 1;
    while (in >> word) {
        if (word == "phones" && in >> n) break;
    }
    return std::clamp(n, 1, 4);
}

void LabRunner::start() {
    // Each session's phone introduces itself the way AirPlay reports a client name; control routing
    // matches on these names.
    for (size_t i = 0; i < sessions_.size(); ++i) sessions_[i]->video().on_client(phone_name(static_cast<int>(i)), "iPhone18,1");
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void LabRunner::run() {
    ULONGLONG t0 = GetTickCount64();
    for (const Step& step : steps_) {
        while (running_ && GetTickCount64() - t0 < static_cast<ULONGLONG>(step.at_ms)) Sleep(5);
        if (!running_) return;
        log::info("Lab: step {} ms: {} {}", step.at_ms, step.verb, step.rest);
        execute(step);
    }
}

bool LabRunner::phone_to_screen(float x, float y, POINT& out) {
    HWND hwnd = session().window().hwnd();
    RECT rc;
    GetClientRect(hwnd, &rc);
    geometry::Rect s = geometry::screen_rect(static_cast<float>(rc.right), static_cast<float>(rc.bottom),
                                             session().window().renderer().content_aspect(),
                                             session().window().renderer().margin_px());
    out = {static_cast<LONG>(std::lround(s.x + x * s.w)), static_cast<LONG>(std::lround(s.y + y * s.h))};
    return ClientToScreen(hwnd, &out) != FALSE;
}

void LabRunner::click_at(POINT p) {
    SetCursorPos(p.x, p.y);
    Sleep(30);
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(60);
    SendInput(1, &in[1], sizeof(INPUT));
}

// SendInput goes to whatever window is in front: never inject unless that is the lab's mirror window.
bool LabRunner::ensure_foreground() {
    HWND hwnd = session().window().hwnd();
    for (int attempt = 0; attempt < 3 && GetForegroundWindow() != hwnd; ++attempt) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        Sleep(100);
    }
    GUITHREADINFO info{sizeof(info)};
    GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info);
    if (GetForegroundWindow() == hwnd && info.hwndFocus == hwnd) return true;
    log::error("Lab: FAIL mirror window is not in front (foreground {}, focus {}); input step skipped",
               static_cast<void*>(GetForegroundWindow()), static_cast<void*>(info.hwndFocus));
    return false;
}

void LabRunner::execute(const Step& s) {
    auto arg = [&](size_t i) { return i < s.args.size() ? s.args[i] : std::string(); };
    auto num = [&](size_t i) { return i < s.args.size() ? std::stof(s.args[i]) : 0.0f; };
    const std::string& v = s.verb;
    bool input = v == "key" || v == "type" || v == "click" || v == "click-pointer" || v == "move" || v == "wheel";
    if (input && !ensure_foreground()) return;
    if (v == "video") {
        session().files().start(dir_ / arg(0), session().video());
    } else if (v == "audio") {
        session().files().start_audio(dir_ / arg(0), session().audio());
    } else if (v == "audio-stop") {
        session().files().stop_audio();
    } else if (v == "impair") {
        Impairment imp;
        for (const std::string& a : s.args) {
            if (a.rfind("jitter=", 0) == 0) imp.jitter_ms = std::stoi(a.substr(7));
            if (a.rfind("burst=", 0) == 0) {
                size_t slash = a.find('/');
                imp.burst_size = std::stoi(a.substr(6, slash - 6));
                imp.burst_every = slash == std::string::npos ? 60 : std::stoi(a.substr(slash + 1));
            }
        }
        session().files().set_impairment(imp);
    } else if (v == "focus") {
        HWND hwnd = session().window().hwnd();
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
    } else if (v == "key") {
        send_combo(arg(0));
    } else if (v == "type") {
        type_text(widen(s.rest));
    } else if (v == "click") {
        POINT p;
        if (phone_to_screen(num(0), num(1), p)) click_at(p);
    } else if (v == "click-pointer") {
        LabPhoneView view = phone_.view(phone_name(static_cast<int>(current_)));
        POINT p;
        if (phone_to_screen(view.pointer_x, view.pointer_y, p)) click_at(p);
    } else if (v == "move") {
        int dx = static_cast<int>(num(0)), dy = static_cast<int>(num(1));
        int steps = std::max(1, std::max(std::abs(dx), std::abs(dy)) / 10);
        for (int i = 0; i < steps; ++i) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            in.mi.dx = dx / steps;
            in.mi.dy = dy / steps;
            SendInput(1, &in, sizeof(INPUT));
            Sleep(8);
        }
    } else if (v == "wheel") {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = static_cast<DWORD>(static_cast<int>(num(0)) * WHEEL_DELTA);
        SendInput(1, &in, sizeof(INPUT));
    } else if (v == "session") {
        size_t n = static_cast<size_t>(std::max(1, static_cast<int>(num(0))));
        if (n <= sessions_.size()) {
            current_ = n - 1;
        } else {
            log::error("Lab: FAIL session {} does not exist (scenario has {} phones)", n, sessions_.size());
        }
    } else if (v == "connect" || v == "disconnect") {
        phone_.set_connected(v == "connect");
    } else if (v == "expect-pointer") {
        phone_.expect_pointer(phone_name(static_cast<int>(current_)), num(0), num(1), num(2));
    } else if (v == "expect-text") {
        phone_.expect_text(phone_name(static_cast<int>(current_)), widen(s.rest));
    } else if (v == "log") {
        log::info("Lab: note {}", s.rest);
    } else if (v == "quit") {
        PostMessageW(app_, kMsgLabQuit, 0, 0);
        running_ = false;
    } else {
        log::error("Lab: FAIL unknown step verb '{}'", v);
    }
}

Renderer::Painter LabRunner::painter(LabPhone& phone, std::string name) {
    struct Resources {
        ComPtr<ID2D1SolidColorBrush> fill, ring, text, panel, badge;
        ComPtr<IDWriteTextFormat> format;
    };
    auto res = std::make_shared<Resources>();
    return [&phone, res, name](ID2D1DeviceContext2* ctx, const D2D1_RECT_F& screen) {
        if (!res->fill) {
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.15f, 0.55f), &res->fill);
            ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &res->ring);
            ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &res->text);
            ctx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &res->panel);
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.55f, 0.1f, 1), &res->badge);
            ComPtr<IDWriteFactory> dw;
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
            dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &res->format);
            res->format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        LabPhoneView v = phone.view(name);
        float w = screen.right - screen.left;
        float h = screen.bottom - screen.top;
        auto at = [&](float x, float y) { return D2D1::Point2F(screen.left + x * w, screen.top + y * h); };

        // "LAB" badge so a lab window is never mistaken for a real phone.
        D2D1_RECT_F badge = D2D1::RectF(screen.left + w * 0.06f, screen.top + h * 0.05f, screen.left + w * 0.06f + 64,
                                        screen.top + h * 0.05f + 22);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(badge, 6, 6), res->badge.Get());
        ctx->DrawText(v.connected ? L"LAB" : L"LAB · off", v.connected ? 3 : 9, res->format.Get(),
                      D2D1::RectF(badge.left, badge.top + 2, badge.right, badge.bottom), res->text.Get());

        if (v.tap_age < 0.45f) {  // expanding ripple where the last tap landed
            float r = 10 + v.tap_age * 80;
            res->ring->SetOpacity(1.0f - v.tap_age / 0.45f);
            ctx->DrawEllipse(D2D1::Ellipse(at(v.tap_x, v.tap_y), r, r), res->ring.Get(), 2.0f);
            res->ring->SetOpacity(0.9f);
        }
        float pr = v.pressed ? 9.0f : 12.0f;  // AssistiveTouch-style pointer
        ctx->FillEllipse(D2D1::Ellipse(at(v.pointer_x, v.pointer_y), pr, pr), res->fill.Get());
        ctx->DrawEllipse(D2D1::Ellipse(at(v.pointer_x, v.pointer_y), pr, pr), res->ring.Get(), 1.5f);

        auto panel = [&](const std::wstring& text, float y) {
            if (text.empty()) return;
            D2D1_RECT_F r = D2D1::RectF(screen.left + w * 0.08f, screen.top + y * h, screen.right - w * 0.08f,
                                        screen.top + y * h + 30);
            ctx->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), res->panel.Get());
            ctx->DrawText(text.c_str(), static_cast<UINT32>(text.size()), res->format.Get(),
                          D2D1::RectF(r.left, r.top + 6, r.right, r.bottom), res->text.Get());
        };
        panel(v.event, 0.12f);
        panel(v.text.empty() ? std::wstring() : L"⌨ " + v.text, 0.80f);
    };
}

}  // namespace pm
