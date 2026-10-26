#pragma once

#include <windows.h>

namespace pm {

// Checks what an AirPlay receiver needs from Windows: an inbound firewall allow rule for this exe that
// covers the active network profile, and ideally a Private network profile. Offers the fix (an elevated
// netsh call the user approves via UAC, or the network settings page). At startup it asks only once per
// problem; `always_report` (tray menu) reports the state even when everything is fine.
void check_network_setup(HWND owner, bool always_report = false);

}  // namespace pm
