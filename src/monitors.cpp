// SPDX-License-Identifier: GPL-2.0-or-later
#include "monitors.hpp"

#include <shellscalingapi.h>

namespace indicator {

namespace {

// Maps GDI device names (\\.\DISPLAYn) to the monitor's EDID friendly name.
std::vector<std::pair<std::wstring, std::wstring>> FriendlyNames()
{
	std::vector<std::pair<std::wstring, std::wstring>> out;
	UINT32 num_paths = 0, num_modes = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes) != ERROR_SUCCESS)
		return out;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(num_paths);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(num_modes);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths.data(), &num_modes, modes.data(), nullptr) !=
	    ERROR_SUCCESS)
		return out;
	paths.resize(num_paths);

	for (const auto &p : paths) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof(src);
		src.header.adapterId = p.sourceInfo.adapterId;
		src.header.id = p.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS)
			continue;

		DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
		tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tgt.header.size = sizeof(tgt);
		tgt.header.adapterId = p.targetInfo.adapterId;
		tgt.header.id = p.targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS)
			continue;

		out.emplace_back(src.viewGdiDeviceName, tgt.monitorFriendlyDeviceName);
	}
	return out;
}

BOOL CALLBACK EnumProc(HMONITOR mon, HDC, LPRECT, LPARAM param)
{
	auto *list = reinterpret_cast<std::vector<MonitorDesc> *>(param);
	MONITORINFOEXW mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, &mi))
		return TRUE;
	MonitorDesc d;
	d.handle = mon;
	d.device = mi.szDevice;
	d.rect = mi.rcMonitor;
	d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
	list->push_back(std::move(d));
	return TRUE;
}

} // namespace

std::vector<MonitorDesc> EnumerateMonitors()
{
	std::vector<MonitorDesc> list;
	EnumDisplayMonitors(nullptr, nullptr, EnumProc, reinterpret_cast<LPARAM>(&list));
	const auto names = FriendlyNames();
	for (auto &m : list) {
		for (const auto &[dev, name] : names) {
			if (_wcsicmp(dev.c_str(), m.device.c_str()) == 0) {
				m.friendly_name = name;
				break;
			}
		}
	}
	return list;
}

HMONITOR FindMonitorByDevice(const std::wstring &device)
{
	if (device.empty())
		return nullptr;
	for (const auto &m : EnumerateMonitors()) {
		if (_wcsicmp(m.device.c_str(), device.c_str()) == 0)
			return m.handle;
	}
	return nullptr;
}

float MonitorScale(HMONITOR mon)
{
	UINT dx = 96, dy = 96;
	if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx > 0)
		return dx / 96.0f;
	return 1.0f;
}

} // namespace indicator
