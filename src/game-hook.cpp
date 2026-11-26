// SPDX-License-Identifier: GPL-3.0-or-later
#include "game-hook.hpp"
#include "game-overlay-shared.hpp"
#include "module-path.hpp"

#include <psapi.h>

#include <algorithm>
#include <cstring>

namespace indicator {

using namespace shared;

namespace {

constexpr ULONGLONG kDrawnRecentlyMs = 250; // a game drawing at 4+ fps counts as drawing
constexpr DWORD kLoadTimeoutMs = 5000;
constexpr wchar_t kHookDll[] = L"obs-indicator-hook.dll";

Header *Hdr(void *view)
{
	return static_cast<Header *>(view);
}

std::string Utf8(const std::wstring &w)
{
	if (w.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::string ExeName(HANDLE process)
{
	wchar_t path[MAX_PATH];
	DWORD len = MAX_PATH;
	if (!QueryFullProcessImageNameW(process, 0, path, &len))
		return "?";
	const wchar_t *base = wcsrchr(path, L'\\');
	return Utf8(base ? base + 1 : path);
}

// True if `hwnd` covers its whole monitor (the overlay thread is per-monitor DPI aware, so
// these are physical pixels, as the game's fullscreen window is).
bool CoversMonitor(HWND hwnd)
{
	RECT r;
	MONITORINFO mi{sizeof(mi)};
	if (!GetWindowRect(hwnd, &r) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL), &mi))
		return false;
	const RECT &m = mi.rcMonitor;
	return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
}

// Whether `process` has OpenGL loaded, and whether it already has our hook (from an earlier OBS).
void ScanModules(HANDLE process, bool &opengl, bool &hooked)
{
	opengl = hooked = false;
	HMODULE mods[1024];
	DWORD needed = 0;
	if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
		return;
	const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
	for (DWORD i = 0; i < count; ++i) {
		wchar_t name[MAX_PATH];
		if (!GetModuleBaseNameW(process, mods[i], name, MAX_PATH))
			continue;
		opengl = opengl || _wcsicmp(name, L"opengl32.dll") == 0;
		hooked = hooked || _wcsicmp(name, kHookDll) == 0;
	}
}

} // namespace

GameHook::~GameHook()
{
	if (view_) {
		Hide(0);
		Hide(1);
		InterlockedExchange(&Hdr(view_)->flags, 0);
		UnmapViewOfFile(view_);
	}
	if (map_)
		CloseHandle(map_);
	for (const auto *list : {&hooked_, &skipped_})
		for (const Known &k : *list)
			if (k.process)
				CloseHandle(k.process);
}

bool GameHook::Open()
{
	if (view_)
		return true;
	// Reserved, not committed: only the pages Publish() writes take memory. A game still holding
	// the hook from an earlier OBS keeps this mapping alive, in which case it is reused.
	map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
				  (DWORD)((uint64_t)kMappingSize >> 32), (DWORD)kMappingSize, kMappingName);
	if (map_)
		view_ = MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingSize);
	if (!view_ || !VirtualAlloc(view_, kPixelsOffset, MEM_COMMIT, PAGE_READWRITE)) {
		log_(true, "Draw inside OpenGL games: can't create the shared memory (error %lu)", GetLastError());
		if (view_)
			UnmapViewOfFile(view_);
		if (map_)
			CloseHandle(map_);
		view_ = map_ = nullptr;
		return false;
	}
	Header *h = Hdr(view_);
	h->flags = 0;
	h->magic = kMagic;
	h->version = kVersion;
	h->owner_pid = GetCurrentProcessId();
	for (Layer &l : h->layers) {
		if (l.seq & 1) // left mid-write by an OBS that crashed
			InterlockedIncrement(&l.seq);
		l.visible = 0;
	}
	last_msg_seq_ = h->hook_msg_seq;
	return true;
}

void GameHook::SetEnabled(HMODULE module, bool on)
{
	if (on == enabled_)
		return;
	if (on) {
		dll_path_ = SiblingPath(module, kHookDll);
		if (dll_path_.empty() || GetFileAttributesW(dll_path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
			log_(true, "Draw inside OpenGL games: %ls is missing from the plugin's folder", kHookDll);
			return;
		}
		if (!Open())
			return;
	} else {
		Hide(0);
		Hide(1);
		// Look at those again next time: the reason may have gone.
		for (const Known &k : skipped_)
			if (k.process)
				CloseHandle(k.process);
		skipped_.clear();
	}
	enabled_ = on;
	if (view_)
		InterlockedExchange(&Hdr(view_)->flags, on ? (LONG)kFlagEnabled : 0);
}

void GameHook::Publish(int layer, const void *bits, int w, int h, int x, int y, BYTE alpha)
{
	if (!enabled_ || !view_ || layer < 0 || layer >= kLayers || !bits)
		return;
	const size_t bytes = (size_t)std::max(w, 0) * (size_t)std::max(h, 0) * 4;
	if (!bytes || bytes > kMaxLayerBytes) {
		Hide(layer);
		return;
	}
	uint8_t *dst = LayerPixels(view_, layer);
	if (bytes > committed_[layer]) {
		if (!VirtualAlloc(dst, bytes, MEM_COMMIT, PAGE_READWRITE)) {
			Hide(layer);
			return;
		}
		committed_[layer] = bytes;
	}
	Layer &l = Hdr(view_)->layers[layer];
	InterlockedIncrement(&l.seq); // odd: being written
	l.x = x;
	l.y = y;
	l.w = w;
	l.h = h;
	l.alpha = alpha;
	memcpy(dst, bits, bytes);
	l.visible = 1;
	InterlockedIncrement(&l.seq); // even: complete
}

void GameHook::Hide(int layer)
{
	if (!view_ || layer < 0 || layer >= kLayers)
		return;
	Layer &l = Hdr(view_)->layers[layer];
	if (!l.visible)
		return;
	InterlockedIncrement(&l.seq);
	l.visible = 0;
	InterlockedIncrement(&l.seq);
}

bool GameHook::DrawnInGame(int layer) const
{
	if (!enabled_ || !view_ || layer < 0 || layer >= kLayers)
		return false;
	const Layer &l = Hdr(view_)->layers[layer];
	const ULONGLONG drawn = (ULONGLONG)l.drawn_tick, now = GetTickCount64();
	return l.visible && drawn && now >= drawn && now - drawn < kDrawnRecentlyMs;
}

bool GameHook::Has(std::vector<Known> &list, DWORD pid)
{
	// Forget processes that have exited, so a reused pid is looked at afresh. Without a process
	// handle, the process counts as gone once its window is.
	std::erase_if(list, [](const Known &k) {
		if (!k.process) {
			DWORD owner = 0;
			return !IsWindow(k.hwnd) || !GetWindowThreadProcessId(k.hwnd, &owner) || owner != k.pid;
		}
		if (WaitForSingleObject(k.process, 0) == WAIT_TIMEOUT)
			return false;
		CloseHandle(k.process);
		return true;
	});
	return std::any_of(list.begin(), list.end(), [pid](const Known &k) { return k.pid == pid; });
}

void GameHook::Poll()
{
	if (!enabled_ || !view_)
		return;
	Header *h = Hdr(view_);
	const LONG seq = h->hook_msg_seq;
	if (seq - last_msg_seq_ > kHookMessages) // more than the ring holds: the oldest are gone
		last_msg_seq_ = seq - kHookMessages;
	for (; last_msg_seq_ < seq; ++last_msg_seq_) {
		const HookMessage &m = h->hook_msgs[(ULONG)last_msg_seq_ % kHookMessages];
		char text[sizeof(m.text) + 1];
		memcpy(text, m.text, sizeof(m.text));
		text[sizeof(m.text)] = '\0';
		log_(false, "Draw inside OpenGL games (pid %u): %s", m.pid, text);
	}

	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid || pid == GetCurrentProcessId())
		return;
	DWORD shell_pid = 0; // the desktop covers the monitor too; never load into Explorer
	GetWindowThreadProcessId(GetShellWindow(), &shell_pid);
	if (pid == shell_pid)
		return;
	if (Has(hooked_, pid) || Has(skipped_, pid))
		return;
	if (CoversMonitor(fg)) // not fullscreen (yet): look again later
		Inject(fg, pid);
}

void GameHook::Inject(HWND hwnd, DWORD pid)
{
	HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
				       PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
			       FALSE, pid);
	if (!p) {
		// Remember it, so we don't try (and log) again every 100 ms. A process whose name we can't
		// even read is part of Windows: DWM briefly covers the screen while a game switches to
		// exclusive fullscreen. One we can name is usually running as administrator, or protected.
		skipped_.push_back({pid, OpenProcess(SYNCHRONIZE, FALSE, pid), hwnd});
		if (HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
			log_(false,
			     "Draw inside OpenGL games: can't load into %s (pid %lu, access denied). If it runs as "
			     "administrator, OBS has to as well",
			     ExeName(q).c_str(), pid);
			CloseHandle(q);
		}
		return;
	}
	const std::string exe = ExeName(p);
	bool opengl = false, already = false;
	ScanModules(p, opengl, already);
	BOOL wow64 = FALSE;
	IsWow64Process(p, &wow64);
	if (!opengl || wow64) {
		if (opengl)
			log_(false, "Draw inside OpenGL games: %s is a 32-bit game, which isn't supported", exe.c_str());
		skipped_.push_back({pid, p, hwnd}); // not an OpenGL game: nothing to do
		return;
	}
	if (already) {
		hooked_.push_back({pid, p, hwnd});
		log_(false, "Draw inside OpenGL games: %s (pid %lu) still has the hook from before", exe.c_str(), pid);
		return;
	}

	// LoadLibraryW(path) on a thread in the game. kernel32 sits at the same address in every
	// process of a session, so our LoadLibraryW address is valid there too.
	const size_t bytes = (dll_path_.size() + 1) * sizeof(wchar_t);
	void *mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
	HANDLE t = nullptr;
	if (mem && load && WriteProcessMemory(p, mem, dll_path_.c_str(), bytes, nullptr))
		t = CreateRemoteThread(p, nullptr, 0, load, mem, 0, nullptr);
	const DWORD err = GetLastError();
	DWORD wait = WAIT_FAILED;
	if (t) {
		wait = WaitForSingleObject(t, kLoadTimeoutMs);
		CloseHandle(t);
	}
	if (mem && (wait == WAIT_OBJECT_0 || !t)) // after a timeout the thread may still read it
		VirtualFreeEx(p, mem, 0, MEM_RELEASE);
	ScanModules(p, opengl, already);
	if (!already) {
		log_(true, "Draw inside OpenGL games: couldn't load the hook into %s (pid %lu): %s (error %lu)", exe.c_str(),
		     pid, !t ? "can't start a thread in it" : wait != WAIT_OBJECT_0 ? "timed out" : "LoadLibrary failed",
		     err);
		skipped_.push_back({pid, p, hwnd});
		return;
	}
	hooked_.push_back({pid, p, hwnd});
	log_(false, "Draw inside OpenGL games: loaded the hook into %s (pid %lu)", exe.c_str(), pid);
}

} // namespace indicator
