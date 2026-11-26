// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace indicator {

struct MonitorDesc {
	HMONITOR handle = nullptr;
	std::wstring device;        // GDI device name, e.g. \\.\DISPLAY1 (stable across sessions)
	std::wstring friendly_name; // e.g. "DELL U2720Q" (may be empty)
	RECT rect{};
	bool primary = false;
};

std::vector<MonitorDesc> EnumerateMonitors();

// Returns the monitor handle for a GDI device name, or nullptr if not connected.
HMONITOR FindMonitorByDevice(const std::wstring &device);

// Effective DPI scale of a monitor (1.0 = 96 DPI).
float MonitorScale(HMONITOR mon);

} // namespace indicator
