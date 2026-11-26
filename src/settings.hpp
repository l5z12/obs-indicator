// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace indicator {

enum class Corner : int { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

enum class MonitorMode : int {
	Primary = 0,          // primary display
	FollowForeground = 1, // display that holds the foreground (game) window
	Specific = 2,         // display named by Settings::monitor_device
};

enum class MicMode : int { Off = 0, WhenMuted = 1, Always = 2 };

struct Settings {
	bool enabled = true;

	// Status indicator (the persistent icons in a corner)
	bool show_indicator = true;
	Corner indicator_corner = Corner::TopRight;
	bool ind_recording = true;
	bool ind_streaming = true;
	bool ind_replay = true;
	bool ind_virtualcam = false;
	bool show_timer = false;
	MicMode mic_mode = MicMode::Off;
	std::string mic_source; // UTF-8 OBS source name; empty = the global Mic/Aux channel

	// Notifications (toasts that slide in and fade out)
	bool notifications = true;
	Corner toast_corner = Corner::TopRight;
	int toast_seconds = 4;
	bool notify_recording = true;
	bool notify_streaming = true;
	bool notify_replay = true;
	bool notify_screenshot = true;
	bool notify_virtualcam = false;
	bool notify_mic = false;

	// Appearance / placement
	int scale_percent = 100;   // multiplied with the display's DPI scale
	int opacity_percent = 100; // 20..100
	int margin = 20;           // distance from the screen edge, in 100%-scale pixels
	uint32_t accent = 0x76B900; // 0xRRGGBB
	MonitorMode monitor_mode = MonitorMode::Primary;
	std::wstring monitor_device; // e.g. L"\\\\.\\DISPLAY2" for MonitorMode::Specific
	bool hide_from_capture = true;
	// Advanced, off by default: raise the overlay into a high window-manager z-order band so it
	// shows over fullscreen games that out-rank a normal top-most window. Uses WindowTopMost,
	// which injects a helper into Explorer.exe, so it is opt-in. No effect on true exclusive
	// fullscreen (that bypasses the compositor).
	bool show_over_fullscreen = false;
	// Which z-order band to use when show_over_fullscreen is on, as a ZBID value. Only the two
	// bands an ordinary desktop window can actually use are allowed (see kFullscreenBands in
	// settings-dialog.cpp): ZBID_UIACCESS (2, the top) and ZBID_SYSTEM_TOOLS (16). Default UIAccess.
	int fullscreen_band = 2; // ZBID_UIACCESS

	void Clamp()
	{
		auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
		toast_seconds = clampi(toast_seconds, 1, 30);
		scale_percent = clampi(scale_percent, 50, 300);
		opacity_percent = clampi(opacity_percent, 20, 100);
		margin = clampi(margin, 0, 400);
		accent &= 0xFFFFFF;
		auto corner_ok = [](Corner c) { return (int)c >= 0 && (int)c <= 3; };
		if (!corner_ok(indicator_corner))
			indicator_corner = Corner::TopRight;
		if (!corner_ok(toast_corner))
			toast_corner = Corner::TopRight;
		if ((int)monitor_mode < 0 || (int)monitor_mode > 2)
			monitor_mode = MonitorMode::Primary;
		if ((int)mic_mode < 0 || (int)mic_mode > 2)
			mic_mode = MicMode::Off;
		// win32k treats only ZBID_DESKTOP (1), ZBID_UIACCESS (2) and ZBID_SYSTEM_TOOLS (16) as
		// non-immersive; a plain window placed in any other band drops out of the desktop z-order.
		// Of those three, 1 is "no raising", so only 2 and 16 are offered. Anything else (including
		// values saved by older builds that listed more bands) falls back to UIAccess (2, the top).
		if (fullscreen_band != 2 && fullscreen_band != 16)
			fullscreen_band = 2;
	}
};

} // namespace indicator
