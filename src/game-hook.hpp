// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace indicator {

// "Draw inside OpenGL games". In a fullscreen OpenGL game such as Minecraft the graphics driver
// sends frames straight to the display, past Windows' compositor, so no window can be seen over
// it, whatever its z-order band. This loads obs-indicator-hook.dll into such a game and hands it
// the overlay's pixels through shared memory; the hook draws them into the game's own frame (see
// src/hook/gl-hook.cpp) and reports which layers it drew, so their windows can step aside.
//
// Not thread-safe: use from the overlay thread.
class GameHook {
public:
	using LogFn = void (*)(bool error, const char *fmt, ...);

	explicit GameHook(LogFn log) : log_(log) {}
	~GameHook();
	GameHook(const GameHook &) = delete;
	GameHook &operator=(const GameHook &) = delete;

	// Turns drawing inside games on or off. The shared memory is created on first use; the hook
	// DLL `module`'s folder holds is loaded into games only while this is on. Turning it off
	// stops hooked games drawing; the DLL stays in them, idle, until they exit.
	void SetEnabled(HMODULE module, bool on);
	bool Enabled() const { return enabled_; }

	// Hands layer `layer` (0 = status indicator, 1 = notifications) to hooked games: top-down,
	// premultiplied BGRA pixels, w * 4 bytes per row, shown at x, y (physical screen pixels).
	void Publish(int layer, const void *bits, int w, int h, int x, int y, BYTE alpha);
	void Hide(int layer);

	// True if a game drew `layer` just now, so its window should stay hidden.
	bool DrawnInGame(int layer) const;

	// Loads the hook into the foreground window's process if that is a fullscreen OpenGL game,
	// and logs what hooked games report. Call on foreground changes and often (~10 Hz) while the
	// overlay is visible: games can go fullscreen without a foreground change.
	void Poll();

private:
	struct Known {
		DWORD pid;
		HANDLE process; // SYNCHRONIZE, to notice the exit (and pid reuse)
	};

	bool Open();
	void Inject(DWORD pid);
	static bool Has(std::vector<Known> &list, DWORD pid);

	LogFn log_;
	bool enabled_ = false;
	HANDLE map_ = nullptr;
	void *view_ = nullptr;
	size_t committed_[2] = {};
	std::wstring dll_path_;
	std::vector<Known> hooked_;  // processes the hook was loaded into
	std::vector<Known> skipped_; // fullscreen processes that aren't OpenGL games, or failed
	LONG last_msg_seq_ = 0;
};

} // namespace indicator
