#include "net/wifi_tuning.h"

#include <wlanapi.h>

#include "app/log.h"

namespace pm {

WifiLowLatency& wifi_low_latency() {
    static WifiLowLatency instance;
    return instance;
}

void WifiLowLatency::acquire() {
    std::lock_guard lock(mutex_);
    if (users_++ > 0) return;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &wlan_) != ERROR_SUCCESS) {
        wlan_ = nullptr;
        return;  // no WLAN service (wired-only PC)
    }
    WLAN_INTERFACE_INFO_LIST* list = nullptr;
    if (WlanEnumInterfaces(wlan_, nullptr, &list) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& itf = list->InterfaceInfo[i];
        if (itf.isState != wlan_interface_state_connected) continue;
        BOOL streaming = TRUE;
        BOOL background_scan = FALSE;
        DWORD r1 = WlanSetInterface(wlan_, &itf.InterfaceGuid, wlan_intf_opcode_media_streaming_mode, sizeof(streaming),
                                    &streaming, nullptr);
        DWORD r2 = WlanSetInterface(wlan_, &itf.InterfaceGuid, wlan_intf_opcode_background_scan_enabled,
                                    sizeof(background_scan), &background_scan, nullptr);
        log::info("Wi-Fi {}: media streaming mode {}, background scan off {}", log::utf8(itf.strInterfaceDescription),
                  r1 == ERROR_SUCCESS ? "on" : std::format("failed ({})", r1),
                  r2 == ERROR_SUCCESS ? "ok" : std::format("failed ({})", r2));
    }
    WlanFreeMemory(list);
}

void WifiLowLatency::release_one() {
    std::lock_guard lock(mutex_);
    if (users_ > 0 && --users_ == 0) release();
}

void WifiLowLatency::release() {
    if (wlan_) {
        WlanCloseHandle(wlan_, nullptr);  // the WLAN service restores normal power save and scanning
        wlan_ = nullptr;
        log::info("Wi-Fi: normal power save and scanning restored");
    }
}

}  // namespace pm
