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
// src/hook/gl-hook.cpp) and reports the monitor it draws on, so the windows there step aside.
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
	// DLL in `module`'s folder is loaded into games only while this is on. Turning it off stops
	// hooked games drawing; the DLL stays in them, idle, until they exit.
	void SetEnabled(HMODULE module, bool on);
	bool Enabled() const { return enabled_; }

	// Hands layer `layer` (0 = status indicator, 1 = notifications) to hooked games: top-down,
	// premultiplied BGRA pixels, w * 4 bytes per row, shown at x, y (physical screen pixels).
	void Publish(int layer, const void *bits, int w, int h, int x, int y, BYTE alpha);
	void Hide(int layer);

	// True if a hooked game, fullscreen and in front, is drawing published layer `layer` on the
	// monitor where `rect` (physical pixels) lies, so the window there should stay hidden.
	bool GameDraws(int layer, const RECT &rect) const;

	// Loads the hook into the foreground window's process if that is a fullscreen OpenGL game,
	// logs what hooked games report, and keeps OBS's heartbeat going. Call on foreground changes
	// and about 10 times a second while the overlay is visible: games can go fullscreen without
	// a foreground change, and hooks stop drawing when the heartbeat stops.
	void Poll();

private:
	// A process looked at already, so it isn't opened every 100 ms.
	struct Known {
		DWORD pid;
		HANDLE process;     // SYNCHRONIZE only, to notice the exit (and pid reuse); may be null
		HWND hwnd;          // its fullscreen window; stands in for the process when that is null
		ULONGLONG retry_at; // when to look again; 0 = never
		int looks;
	};
	// A hook load still running in a game (the remote LoadLibraryW thread).
	struct Loading {
		DWORD pid;
		HWND hwnd;
		HANDLE process; // the rights to inject
		HANDLE thread;
		void *mem; // the DLL path inside the game
		ULONGLONG started;
		std::string exe;
	};

	bool Open();
	void Beat();
	void LogHookMessages();
	void Look(HWND hwnd, DWORD pid, Known *again);
	void Skip(HWND hwnd, DWORD pid, Known *again, bool look_again);
	void FinishLoads();
	static void Prune(std::vector<Known> &list);

	LogFn log_;
	bool enabled_ = false;
	HANDLE map_ = nullptr;
	void *view_ = nullptr;
	size_t committed_[2] = {};
	std::wstring dll_path_;
	std::vector<std::wstring> icds_; // file names of the installed OpenGL drivers
	std::vector<Known> hooked_;      // processes the hook is loaded in
	std::vector<Known> skipped_;     // fullscreen processes it isn't (yet) loaded in
	std::vector<Loading> loading_;
	LONG last_msg_seq_ = 0;
};

} // namespace indicator
