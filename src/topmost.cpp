// SPDX-License-Identifier: GPL-3.0-or-later
#include "topmost.hpp"
#include "module-path.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace indicator {
namespace {

// ZBID_DESKTOP is the normal band; Lower() returns a window to it. The raised band is chosen by
// the user (see kFullscreenBands in settings-dialog.cpp) and passed in to Raise().
constexpr DWORD kBandDesktop = 1; // ZBID_DESKTOP (normal)

// On x64 there is a single calling convention, so no __stdcall/__cdecl distinction is needed.
using WTMInitFn = BOOL (*)();
using WTMUninitFn = BOOL (*)();
using WTMSetWindowBandFn = BOOL (*)(HWND hWnd, HWND hWndInsertAfter, DWORD dwBand);

// WindowTopMost's error for "another process controls the Explorer helper" (third_party/
// WindowTopMost/Common Headers/ErrorCodes.h, an obs-indicator addition).
constexpr DWORD kErrorWtmInUse = 0x3F09;

} // namespace

TopMost::~TopMost()
{
	Release();
}

bool TopMost::Fail(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	failure_ = buf;
	return false;
}

bool TopMost::EnsureInit(HMODULE module)
{
	if (init_done_)
		return true;
	if (init_failed_)
		return false; // failure_ still says why

	// From here on, any failure is permanent until Release(): do not keep retrying the heavy
	// Explorer.exe injection on every settings change.
	init_failed_ = true;

	const std::wstring dll = SiblingPath(module, L"WindowTopMost.dll");
	if (dll.empty())
		return Fail("can't find the plugin's folder");
	dll_ = LoadLibraryW(dll.c_str());
	if (!dll_)
		return Fail("can't load WindowTopMost.dll from the plugin's folder (error %lu)", GetLastError());

	init_ = reinterpret_cast<void *>(GetProcAddress(dll_, "WTMInit"));
	uninit_ = reinterpret_cast<void *>(GetProcAddress(dll_, "WTMUninit"));
	set_band_ = reinterpret_cast<void *>(GetProcAddress(dll_, "WTMSetWindowBand"));
	if (!init_ || !uninit_ || !set_band_) {
		FreeLibrary(dll_);
		dll_ = nullptr;
		return Fail("WindowTopMost.dll is missing WTMInit, WTMUninit or WTMSetWindowBand");
	}

	if (!reinterpret_cast<WTMInitFn>(init_)()) {
		const DWORD err = GetLastError();
		UninitAndUnload();
		if (err == kErrorWtmInUse)
			return Fail("another program (or another OBS) is already using WindowTopMost");
		return Fail("WindowTopMost couldn't start its helper in Explorer (error 0x%lX)", err);
	}

	init_done_ = true;
	init_failed_ = false;
	return true;
}

bool TopMost::Raise(HMODULE module, HWND hwnd, unsigned long band)
{
	if (!hwnd)
		return Fail("the window doesn't exist");
	if (!EnsureInit(module))
		return false;
	// hWndInsertAfter must be a real window or NULL: the underlying SetWindowBand rejects the
	// HWND_TOPMOST pseudo-handle with ERROR_INVALID_PARAMETER. The band alone keeps us on top.
	if (!reinterpret_cast<WTMSetWindowBandFn>(set_band_)(hwnd, nullptr, band))
		return Fail("SetWindowBand(%lu) failed (error 0x%lX)", band, GetLastError());
	failure_.clear();
	return true;
}

void TopMost::Lower(HWND hwnd)
{
	if (!hwnd || !init_done_ || !set_band_)
		return;
	reinterpret_cast<WTMSetWindowBandFn>(set_band_)(hwnd, nullptr, kBandDesktop);
}

void TopMost::UninitAndUnload()
{
	// WTMUninit removes the Explorer helper and stops WindowTopMost's own thread. If it reports a
	// failure, that thread may still be running code from the DLL, so the DLL stays loaded (until
	// OBS exits) rather than being unloaded under it.
	if (reinterpret_cast<WTMUninitFn>(uninit_)())
		FreeLibrary(dll_);
	dll_ = nullptr;
}

void TopMost::Release()
{
	if (dll_)
		UninitAndUnload();
	dll_ = nullptr;
	init_ = uninit_ = set_band_ = nullptr;
	init_done_ = false;
	init_failed_ = false;
	failure_.clear();
}

} // namespace indicator
