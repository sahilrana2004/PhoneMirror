#include "net/firewall.h"

#include <netfw.h>
#include <netlistmgr.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <format>
#include <string>

#include "app/log.h"
#include "app/settings.h"

namespace pm {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring exe_path() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// True when any connected network uses the Public category.
bool on_public_network() {
    ComPtr<INetworkListManager> nlm;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm)))) return false;
    ComPtr<IEnumNetworks> networks;
    if (FAILED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks))) return false;
    ComPtr<INetwork> network;
    while (networks->Next(1, &network, nullptr) == S_OK) {
        NLM_NETWORK_CATEGORY category;
        if (SUCCEEDED(network->GetCategory(&category)) && category == NLM_NETWORK_CATEGORY_PUBLIC) return true;
        network.Reset();
    }
    return false;
}

// Bitmask of NET_FW_PROFILE2_* profiles in which an enabled inbound allow rule exists for this exe.
long allowed_profiles(long* current_profiles) {
    *current_profiles = 0;
    ComPtr<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) return -1;
    policy->get_CurrentProfileTypes(current_profiles);
    VARIANT_BOOL enabled = VARIANT_FALSE;
    policy->get_FirewallEnabled(static_cast<NET_FW_PROFILE_TYPE2>(*current_profiles & NET_FW_PROFILE2_PUBLIC
                                                                     ? NET_FW_PROFILE2_PUBLIC
                                                                     : NET_FW_PROFILE2_PRIVATE),
                                &enabled);
    if (!enabled) return NET_FW_PROFILE2_ALL;  // firewall off: nothing to allow
    ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules))) return -1;
    ComPtr<IUnknown> unknown;
    rules->get__NewEnum(&unknown);
    ComPtr<IEnumVARIANT> it;
    if (!unknown || FAILED(unknown.As(&it))) return -1;
    std::wstring me = exe_path();
    long profiles = 0;
    VARIANT v;
    VariantInit(&v);
    while (it->Next(1, &v, nullptr) == S_OK) {
        ComPtr<INetFwRule> rule;
        if (v.vt == VT_DISPATCH && v.pdispVal && SUCCEEDED(v.pdispVal->QueryInterface(IID_PPV_ARGS(&rule)))) {
            BSTR app = nullptr;
            NET_FW_RULE_DIRECTION dir;
            NET_FW_ACTION action;
            VARIANT_BOOL on = VARIANT_FALSE;
            long p = 0;
            rule->get_ApplicationName(&app);
            rule->get_Direction(&dir);
            rule->get_Action(&action);
            rule->get_Enabled(&on);
            rule->get_Profiles(&p);
            if (app && _wcsicmp(app, me.c_str()) == 0 && dir == NET_FW_RULE_DIR_IN && action == NET_FW_ACTION_ALLOW && on) {
                profiles |= p;
            }
            SysFreeString(app);
        }
        VariantClear(&v);
    }
    return profiles;
}

bool add_rules_elevated(HWND owner) {
    std::wstring params = std::format(
        L"advfirewall firewall add rule name=\"PhoneMirror\" dir=in action=allow program=\"{}\" enable=yes "
        L"profile=private,domain",
        exe_path());
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = L"netsh.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;  // UAC declined
    WaitForSingleObject(sei.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

}  // namespace

void check_network_setup(HWND owner, bool always_report) {
    Settings& settings = Settings::instance();
    long current = 0;
    long allowed = allowed_profiles(&current);
    bool firewall_ok = allowed < 0 || (allowed & current) != 0;
    bool is_public = on_public_network();
    log::info("Network: firewall rule covers current profile: {}, public network: {}", firewall_ok, is_public);

    if (!firewall_ok && (always_report || !settings.get_bool("setup_prompted.firewall", false))) {
        settings.set_bool("setup_prompted.firewall", true);
        settings.save();
        int answer = MessageBoxW(owner,
                                 L"Windows Firewall has no rule letting iPhones connect to PhoneMirror on this network, "
                                 L"so it will not show up in Screen Mirroring.\n\n"
                                 L"Add an inbound rule for PhoneMirror on private networks now? Windows will ask for "
                                 L"administrator permission.",
                                 L"PhoneMirror — firewall", MB_YESNO | MB_ICONWARNING);
        if (answer == IDYES) {
            bool ok = add_rules_elevated(owner);
            log::info("Firewall rule add: {}", ok ? "ok" : "failed/declined");
            if (ok) allowed = allowed_profiles(&current);
        }
    }
    if (is_public && (always_report || !settings.get_bool("setup_prompted.public", false))) {
        settings.set_bool("setup_prompted.public", true);
        settings.save();
        int answer = MessageBoxW(owner,
                                 L"Your current network is set to Public. AirPlay discovery and the firewall work best "
                                 L"on a Private network (your home Wi-Fi).\n\n"
                                 L"Open network settings to change the profile type to Private?",
                                 L"PhoneMirror — network", MB_YESNO | MB_ICONINFORMATION);
        if (answer == IDYES) ShellExecuteW(owner, L"open", L"ms-settings:network-status", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (always_report && firewall_ok) {
        MessageBoxW(owner, L"Network and firewall are set up for PhoneMirror.", L"PhoneMirror", MB_OK | MB_ICONINFORMATION);
    }
}

}  // namespace pm
