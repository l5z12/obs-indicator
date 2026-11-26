// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

namespace indicator {

// Raises overlay windows into a high window-manager z-order band so they appear over fullscreen
// games that out-rank a normal top-most window. Implemented with the bundled WindowTopMost DLL,
// which injects a helper into Explorer.exe; loading and init are lazy and happen only when the
// feature is actually used. Every method is a safe no-op (returning false) when the WindowTopMost
// DLLs are missing or initialisation fails, so callers can use it unconditionally.
//
// Not thread-safe: construct and use from a single thread (the overlay thread).
class TopMost {
public:
	TopMost() = default;
	~TopMost();
	TopMost(const TopMost &) = delete;
	TopMost &operator=(const TopMost &) = delete;

	// Raise `hwnd` into z-order band `band` (a ZBID value) so it shows above fullscreen games. On
	// first use, loads WindowTopMost.dll from `module`'s own directory and runs one-time init
	// (which injects the Explorer.exe helper). Returns true if the band was set.
	bool Raise(HMODULE module, HWND hwnd, unsigned long band);

	// Drop `hwnd` back to the normal desktop band. No-op if the feature was never initialised.
	void Lower(HWND hwnd);

	// Remove the Explorer.exe helper and unload the DLL. Lower() the windows first if they should
	// stay. A later Raise() starts the helper again. Also runs on destruction.
	void Release();

	// Whether init has been attempted and failed (so the UI/log can explain the no-op).
	bool Failed() const { return init_failed_; }

private:
	bool EnsureInit(HMODULE module);
	void UninitAndUnload();

	HMODULE dll_ = nullptr;
	void *init_ = nullptr;     // WTMInit
	void *uninit_ = nullptr;   // WTMUninit
	void *set_band_ = nullptr; // WTMSetWindowBand
	bool init_done_ = false;
	bool init_failed_ = false;
};

} // namespace indicator
