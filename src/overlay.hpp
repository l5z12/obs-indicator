// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "renderer.hpp"
#include "settings.hpp"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace indicator {

enum class ToastCategory { General, Recording, Streaming, Replay, Screenshot, VirtualCam, Mic };

struct Toast {
	ToastCategory category = ToastCategory::General;
	ToastContent content;
};

// Snapshot of what OBS is doing. Times are GetTickCount64() values.
struct Status {
	bool recording = false;
	bool rec_paused = false;
	bool streaming = false;
	bool replay = false;
	bool virtualcam = false;
	bool mic_present = false;
	bool mic_muted = false;

	uint64_t rec_start = 0;
	uint64_t rec_paused_total = 0; // ms spent in completed pauses
	uint64_t rec_pause_start = 0;  // start of the current pause, 0 when not paused
	uint64_t stream_start = 0;

	uint64_t RecordingElapsed(uint64_t now) const;
	uint64_t StreamingElapsed(uint64_t now) const;
};

// "4:05", "1:02:03"
std::wstring FormatDuration(uint64_t ms);

// The on-screen overlay: a status indicator and a stack of notifications,
// each drawn into its own click-through, always-on-top layered window.
// All windows live on a private thread with its own message loop, so the
// overlay never blocks (or is blocked by) the OBS UI thread.
// Every public method is thread-safe.
class Overlay {
public:
	Overlay();
	~Overlay();
	Overlay(const Overlay &) = delete;
	Overlay &operator=(const Overlay &) = delete;

	using LogFn = void (*)(bool error, const char *message);
	static void SetLogger(LogFn fn);

	// `module` owns the window classes (the plugin DLL or the host exe).
	bool Start(HINSTANCE module);
	void Stop();
	bool Running() const;

	void SetSettings(const Settings &settings);
	void SetStatus(const Status &status);
	void ShowToast(const Toast &toast); // dropped if the category is disabled
	void Preview(std::wstring sample_title = L"Replay saved", uint32_t duration_ms = 6000);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace indicator
