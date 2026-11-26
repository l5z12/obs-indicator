// SPDX-License-Identifier: GPL-3.0-or-later
#include "game-hook.hpp"
#include "game-overlay-shared.hpp"
#include "module-path.hpp"

#include <psapi.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace indicator {

using namespace shared;

namespace {

constexpr wchar_t kHookDll[] = L"obs-indicator-hook.dll";
constexpr ULONGLONG kLoadTimeoutMs = 10000;
// A fullscreen process without an OpenGL driver yet (a loading screen, say) is looked at again
// this often, this many times, before it counts as not an OpenGL game.
constexpr ULONGLONG kLookAgainMs = 2000;
constexpr int kMaxLooks = 15;

Header *Hdr(void *view)
{
	return static_cast<Header *>(view);
}

LONG64 Age(LONG64 tick)
{
	return (LONG64)GetTickCount64() - tick;
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

// True if `hwnd`'s client area covers its whole monitor, as a fullscreen game's does (a
// maximized window's frame overhangs the monitor, but its client area doesn't). The overlay
// thread is per-monitor DPI aware, so these are physical pixels.
bool CoversMonitor(HWND hwnd)
{
	RECT r;
	MONITORINFO mi{sizeof(mi)};
	if (!GetClientRect(hwnd, &r) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL), &mi))
		return false;
	MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT *>(&r), 2);
	const RECT &m = mi.rcMonitor;
	return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
}

// File names of the OpenGL drivers (ICDs) the display adapters register, plus the well-known
// ones in case the registry can't be read.
std::vector<std::wstring> InstalledIcds()
{
	std::vector<std::wstring> names = {L"nvoglv64.dll", L"atio6axx.dll"};
	HKEY cls;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
			  L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0,
			  KEY_READ, &cls) != ERROR_SUCCESS)
		return names;
	wchar_t sub[16];
	for (DWORD i = 0;; ++i) {
		DWORD sub_len = 16;
		const LONG r = RegEnumKeyExW(cls, i, sub, &sub_len, nullptr, nullptr, nullptr, nullptr);
		if (r == ERROR_NO_MORE_ITEMS)
			break;
		if (r != ERROR_SUCCESS)
			continue;
		wchar_t value[2048];
		DWORD bytes = sizeof(value) - 2 * sizeof(wchar_t), type = 0;
		if (RegGetValueW(cls, sub, L"OpenGLDriverName", RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, &type, value,
				 &bytes) != ERROR_SUCCESS)
			continue;
		value[bytes / sizeof(wchar_t)] = value[bytes / sizeof(wchar_t) + 1] = L'\0';
		for (const wchar_t *p = value; *p; p += wcslen(p) + 1) { // REG_SZ reads as a one-entry list
			const wchar_t *base = wcsrchr(p, L'\\');
			names.emplace_back(base ? base + 1 : p);
		}
	}
	RegCloseKey(cls);
	return names;
}

struct Modules {
	bool read = false;   // the list could be read at all
	bool opengl = false; // opengl32.dll and an OpenGL driver are loaded
	bool vulkan = false; // vulkan-1.dll is loaded
	bool hooked = false; // our hook is already loaded (from an earlier OBS)
};

Modules ScanModules(HANDLE process, const std::vector<std::wstring> &icds)
{
	Modules m;
	HMODULE mods[1024];
	DWORD needed = 0;
	if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
		return m;
	m.read = true;
	bool opengl32 = false, icd = false;
	const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
	for (DWORD i = 0; i < count; ++i) {
		wchar_t name[MAX_PATH];
		if (!GetModuleBaseNameW(process, mods[i], name, MAX_PATH))
			continue;
		opengl32 = opengl32 || _wcsicmp(name, L"opengl32.dll") == 0;
		m.vulkan = m.vulkan || _wcsicmp(name, L"vulkan-1.dll") == 0;
		m.hooked = m.hooked || _wcsicmp(name, kHookDll) == 0;
		for (const std::wstring &d : icds)
			icd = icd || _wcsicmp(name, d.c_str()) == 0;
	}
	m.opengl = opengl32 && icd;
	return m;
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
	for (const Loading &l : loading_) {
		CloseHandle(l.thread);
		CloseHandle(l.process);
	}
}

bool GameHook::Open()
{
	if (view_)
		return true;
	// Open to the interactive user's processes at medium integrity and up. Without this, an OBS
	// running as administrator would create it with a DACL and integrity label that shut a
	// normal game out.
	PSECURITY_DESCRIPTOR sd = nullptr;
	ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)S:(ML;;NW;;;ME)",
							     SDDL_REVISION_1, &sd, nullptr);
	SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
	// Reserved, not committed: only the pages Publish() writes take memory. A game still holding
	// the hook from an earlier OBS keeps this mapping alive, in which case it is reused.
	map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE | SEC_RESERVE,
				  (DWORD)((uint64_t)kMappingSize >> 32), (DWORD)kMappingSize, kMappingName);
	const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
	if (sd)
		LocalFree(sd);
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
	const bool same_version = existed && h->magic == kMagic && h->version == kVersion;
	if (same_version && h->owner_pid != GetCurrentProcessId() && (h->flags & kFlagEnabled) &&
	    Age(h->owner_tick) < (LONG64)kOwnerTimeoutMs) {
		log_(true, "Draw inside OpenGL games: another OBS (pid %u) is already drawing inside games",
		     h->owner_pid);
		UnmapViewOfFile(view_);
		CloseHandle(map_);
		view_ = map_ = nullptr;
		return false;
	}
	if (!same_version) {
		// New, or left by a different build: start clean. (Same-version leftovers keep their
		// layer seqs, so a hook still running from before never mistakes new pixels for old.)
		ZeroMemory(h, sizeof(Header));
	}
	h->flags = 0;
	h->owner_pid = GetCurrentProcessId();
	h->owner_tick = (LONG64)GetTickCount64();
	for (Layer &l : h->layers) {
		if (l.seq & 1) // left mid-write by an OBS that crashed
			InterlockedIncrement(&l.seq);
		l.visible = 0;
	}
	h->version = kVersion;
	InterlockedExchange(reinterpret_cast<volatile LONG *>(&h->magic), (LONG)kMagic);
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
		if (icds_.empty())
			icds_ = InstalledIcds();
		Beat();
	} else {
		Hide(0);
		Hide(1);
		// Look at those again next time: the reason may have gone. Loads still running are
		// left to finish on their own (the hook stays idle while the option is off).
		for (const Known &k : skipped_)
			if (k.process)
				CloseHandle(k.process);
		skipped_.clear();
		for (const Loading &l : loading_) {
			CloseHandle(l.thread);
			CloseHandle(l.process);
		}
		loading_.clear();
	}
	enabled_ = on;
	if (view_)
		InterlockedExchange(&Hdr(view_)->flags, on ? (LONG)kFlagEnabled : 0);
}

void GameHook::Beat()
{
	if (view_)
		InterlockedExchange64(&Hdr(view_)->owner_tick, (LONG64)GetTickCount64());
}

void GameHook::Publish(int layer, const void *bits, int w, int h, int x, int y, BYTE alpha)
{
	if (!enabled_ || !view_ || layer < 0 || layer >= kLayers || !bits)
		return;
	Beat();
	const size_t bytes = (size_t)std::max(w, 0) * (size_t)std::max(h, 0) * 4;
	if (!bytes || bytes > kMaxLayerBytes) {
		Hide(layer);
		return;
	}
	// Commit the pages before publishing a size that needs them, so a hook reading the new size
	// never reads uncommitted memory. They stay committed.
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

bool GameHook::GameDraws(int layer, const RECT &rect) const
{
	if (!enabled_ || !view_ || layer < 0 || layer >= kLayers)
		return false;
	const Header *h = Hdr(view_);
	const LONG64 tick = h->game_tick;
	if (!h->layers[layer].visible || !tick || Age(tick) >= (LONG64)kGameActiveMs)
		return false;
	const RECT monitor = h->game_monitor;
	RECT both;
	return IntersectRect(&both, &rect, &monitor) != FALSE;
}

void GameHook::Prune(std::vector<Known> &list)
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
}

void GameHook::LogHookMessages()
{
	Header *h = Hdr(view_);
	const LONG seq = h->hook_msg_seq;
	if (seq - last_msg_seq_ > kHookMessages) // more than the ring holds: the oldest are gone
		last_msg_seq_ = seq - kHookMessages;
	while (last_msg_seq_ < seq) {
		const HookMessage &m = h->hook_msgs[(ULONG)last_msg_seq_ % kHookMessages];
		const LONG ms = ReadAcquire(&m.seq);
		if (ms > last_msg_seq_ + 1) { // already overwritten by a newer one
			++last_msg_seq_;
			continue;
		}
		if (ms != last_msg_seq_ + 1)
			break; // still being written: next time
		char text[sizeof(m.text) + 1];
		memcpy(text, m.text, sizeof(m.text));
		text[sizeof(m.text)] = '\0';
		const DWORD pid = m.pid;
		std::atomic_thread_fence(std::memory_order_acquire);
		if (ReadAcquire(&m.seq) != ms)
			continue; // overwritten while we copied it: the loop skips it
		log_(false, "Draw inside OpenGL games (pid %lu): %s", pid, text);
		++last_msg_seq_;
	}
}

void GameHook::Poll()
{
	if (!enabled_ || !view_)
		return;
	Beat();
	FinishLoads(); // first, so "loaded the hook" comes before what the hook then says
	LogHookMessages();

	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid || pid == GetCurrentProcessId())
		return;
	DWORD shell_pid = 0; // the desktop covers the monitor too; never load into Explorer
	GetWindowThreadProcessId(GetShellWindow(), &shell_pid);
	if (pid == shell_pid)
		return;
	Prune(hooked_);
	Prune(skipped_);
	const auto is = [pid](const auto &x) { return x.pid == pid; };
	if (std::any_of(hooked_.begin(), hooked_.end(), is) || std::any_of(loading_.begin(), loading_.end(), is))
		return;
	const auto known = std::find_if(skipped_.begin(), skipped_.end(), is);
	Known *again = known == skipped_.end() ? nullptr : &*known;
	if (again && (!again->retry_at || GetTickCount64() < again->retry_at))
		return;
	// Only fullscreen windows matter; anything else is looked at once it goes fullscreen.
	if (CoversMonitor(fg))
		Look(fg, pid, again);
}

void GameHook::Skip(HWND hwnd, DWORD pid, Known *again, bool look_again)
{
	if (!again) {
		skipped_.push_back({pid, OpenProcess(SYNCHRONIZE, FALSE, pid), hwnd, 0, 0});
		again = &skipped_.back();
	}
	again->hwnd = hwnd;
	++again->looks;
	again->retry_at = look_again && again->looks < kMaxLooks ? GetTickCount64() + kLookAgainMs : 0;
}

void GameHook::Look(HWND hwnd, DWORD pid, Known *again)
{
	// Read-only first: most fullscreen processes are not OpenGL games and get nothing more.
	HANDLE scan = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!scan) {
		// A process whose name we can't even read is part of Windows: DWM briefly covers the
		// screen while a game switches to exclusive fullscreen. One we can name usually runs as
		// administrator, or is protected.
		if (!again)
			if (HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
				log_(false,
				     "Draw inside OpenGL games: can't look into %s (pid %lu, access denied). If it "
				     "runs as administrator, OBS has to as well",
				     ExeName(q).c_str(), pid);
				CloseHandle(q);
			}
		Skip(hwnd, pid, again, false);
		return;
	}
	const std::string exe = ExeName(scan);
	const Modules mods = ScanModules(scan, icds_);
	BOOL wow64 = FALSE;
	IsWow64Process(scan, &wow64);
	CloseHandle(scan);

	if (mods.hooked) {
		hooked_.push_back({pid, OpenProcess(SYNCHRONIZE, FALSE, pid), hwnd, 0, 0});
		if (again) {
			if (again->process)
				CloseHandle(again->process);
			skipped_.erase(skipped_.begin() + (again - skipped_.data()));
		}
		log_(false, "Draw inside OpenGL games: %s (pid %lu) already has the hook", exe.c_str(), pid);
		return;
	}
	if (mods.opengl && mods.vulkan) {
		// NVIDIA's OpenGL driver is its Vulkan driver too, and pulls in opengl32.dll, so every
		// Vulkan game looks like this. Stay out: the overlay's windows show over Vulkan games.
		log_(false, "Draw inside OpenGL games: %s (pid %lu) uses Vulkan, so the hook stays out of it",
		     exe.c_str(), pid);
		Skip(hwnd, pid, again, false);
		return;
	}
	if (!mods.read || !mods.opengl) {
		// Can't tell yet, or no OpenGL driver loaded (yet): a game may show its window before
		// creating its OpenGL context. Look again in a moment, for a while.
		Skip(hwnd, pid, again, true);
		return;
	}
	if (wow64) {
		log_(false, "Draw inside OpenGL games: %s is a 32-bit game, which isn't supported", exe.c_str());
		Skip(hwnd, pid, again, false);
		return;
	}

	// LoadLibraryW(path) on a thread in the game. kernel32 sits at the same address in every
	// process of a session, so our LoadLibraryW address is valid there too. The thread is
	// checked from later polls, so a slow or stuck game never stalls the overlay.
	HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
				       PROCESS_VM_WRITE | PROCESS_VM_READ,
			       FALSE, pid);
	const size_t bytes = (dll_path_.size() + 1) * sizeof(wchar_t);
	void *mem = p ? VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : nullptr;
	const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
	HANDLE t = nullptr;
	if (mem && load && WriteProcessMemory(p, mem, dll_path_.c_str(), bytes, nullptr))
		t = CreateRemoteThread(p, nullptr, 0, load, mem, 0, nullptr);
	if (!t) {
		log_(true, "Draw inside OpenGL games: couldn't start loading the hook into %s (pid %lu, error %lu)",
		     exe.c_str(), pid, GetLastError());
		if (mem)
			VirtualFreeEx(p, mem, 0, MEM_RELEASE);
		if (p)
			CloseHandle(p);
		Skip(hwnd, pid, again, false);
		return;
	}
	if (again) {
		if (again->process)
			CloseHandle(again->process);
		skipped_.erase(skipped_.begin() + (again - skipped_.data()));
	}
	loading_.push_back({pid, hwnd, p, t, mem, GetTickCount64(), exe});
}

void GameHook::FinishLoads()
{
	for (size_t i = 0; i < loading_.size();) {
		Loading &l = loading_[i];
		const bool done = WaitForSingleObject(l.thread, 0) == WAIT_OBJECT_0;
		const bool timed_out = !done && GetTickCount64() - l.started > kLoadTimeoutMs;
		if (!done && !timed_out) {
			++i;
			continue;
		}
		DWORD code = 0;
		if (done) {
			GetExitCodeThread(l.thread, &code); // LoadLibraryW's result, cut to 32 bits
			VirtualFreeEx(l.process, l.mem, 0, MEM_RELEASE);
		} // else the thread may still read the path: leave those few bytes
		const Modules mods = ScanModules(l.process, icds_);
		if (mods.hooked) {
			hooked_.push_back({l.pid, OpenProcess(SYNCHRONIZE, FALSE, l.pid), l.hwnd, 0, 0});
			log_(false, "Draw inside OpenGL games: loaded the hook into %s (pid %lu)", l.exe.c_str(),
			     l.pid);
		} else if (timed_out) {
			// It may still load; the next look finds it if it does.
			log_(true, "Draw inside OpenGL games: loading the hook into %s (pid %lu) timed out",
			     l.exe.c_str(), l.pid);
			Skip(l.hwnd, l.pid, nullptr, true);
		} else {
			log_(true, "Draw inside OpenGL games: %s (pid %lu) refused to load the hook%s", l.exe.c_str(),
			     l.pid, code ? "" : " (LoadLibrary failed)");
			Skip(l.hwnd, l.pid, nullptr, false);
		}
		CloseHandle(l.thread);
		CloseHandle(l.process);
		loading_.erase(loading_.begin() + (ptrdiff_t)i);
	}
}

} // namespace indicator
