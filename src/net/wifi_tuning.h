#pragma once

#include <windows.h>

#include <mutex>

namespace pm {

// While held, asks the WLAN service for streaming behaviour on every connected Wi-Fi adapter: media
// streaming mode (no power-save dozing between packets) and no background scans (the periodic scan is
// a classic source of 50-200 ms stalls). Both are per-client settings the WLAN service reverts as soon
// as this process closes its handle, so nothing persists on the machine.
class WifiLowLatency {
public:
    ~WifiLowLatency() { release(); }
    void acquire();  // ref-counted: one per active mirroring session
    void release_one();

private:
    void release();

    std::mutex mutex_;
    int users_ = 0;
    HANDLE wlan_ = nullptr;
};

WifiLowLatency& wifi_low_latency();

}  // namespace pm
