// SPDX-License-Identifier: GPL-3.0-or-later
#include "topmost.hpp"

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

// Full path to a file sitting next to `module` (the plugin DLL).
std::wstring SiblingPath(HMODULE module, const wchar_t *name)
{
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(module, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return {};
	std::wstring path(buf, n);
	const size_t slash = path.find_last_of(L"\\/");
	path.resize(slash == std::wstring::npos ? 0 : slash + 1);
	path += name;
	return path;
}

} // namespace

TopMost::~TopMost()
{
	Shutdown();
}

bool TopMost::EnsureInit(HMODULE module)
{
	if (init_done_)
		return true;
	if (init_failed_)
		return false;

	// From here on, any failure is permanent for this session: do not keep retrying the heavy
	// Explorer.exe injection on every settings change.
	init_failed_ = true;

	const std::wstring dll = SiblingPath(module, L"WindowTopMost.dll");
	if (dll.empty())
		return false;
	dll_ = LoadLibraryW(dll.c_str());
	if (!dll_)
		return false;

	init_ = reinterpret_cast<void *>(GetProcAddress(dll_, "WTMInit"));
	uninit_ = reinterpret_cast<void *>(GetProcAddress(dll_, "WTMUninit"));
	set_band_ = reinterpret_cast<void *>(GetProcAddress(dll_, "WTMSetWindowBand"));
	if (!init_ || !uninit_ || !set_band_) {
		FreeLibrary(dll_);
		dll_ = nullptr;
		return false;
	}

	if (!reinterpret_cast<WTMInitFn>(init_)()) {
		UninitAndUnload();
		return false;
	}

	init_done_ = true;
	init_failed_ = false;
	return true;
}

bool TopMost::Raise(HMODULE module, HWND hwnd, unsigned long band)
{
	if (!hwnd || !EnsureInit(module))
		return false;
	// hWndInsertAfter must be a real window or NULL: the underlying SetWindowBand rejects the
	// HWND_TOPMOST pseudo-handle with ERROR_INVALID_PARAMETER. The band alone keeps us on top.
	return reinterpret_cast<WTMSetWindowBandFn>(set_band_)(hwnd, nullptr, band) != FALSE;
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

void TopMost::Shutdown()
{
	if (dll_)
		UninitAndUnload();
	dll_ = nullptr;
	init_ = uninit_ = set_band_ = nullptr;
	init_done_ = false;
	init_failed_ = false;
}

} // namespace indicator
