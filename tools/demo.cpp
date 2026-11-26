// SPDX-License-Identifier: GPL-3.0-or-later
// Drives the real overlay with a scripted sequence of fake OBS events, or
// opens the settings dialog, without OBS running.
//
//   overlay-demo.exe                       play the demo (about 15 s)
//   overlay-demo.exe --corner bl           ...with the overlay bottom-left (tl|tr|bl|br)
//   overlay-demo.exe --dialog              open the settings dialog (Preview works)
//   overlay-demo.exe --dialog --snap x.png screenshot the dialog and close it
//   overlay-demo.exe --snap x.png          screenshot the overlay corner mid-demo and exit
//   overlay-demo.exe --capturable          don't hide the overlay from screen capture
//   overlay-demo.exe --locale de-DE.ini    use a translation from data/locale
//   overlay-demo.exe --dialog --check-layout   list dialog text that doesn't fit its control, then close

#include "overlay.hpp"
#include "settings-dialog.hpp"
#include "png.hpp"

#include <commctrl.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace indicator;

namespace {

std::wstring g_snap;
bool g_check_layout = false;
int g_layout_problems = 0;
std::map<std::string, std::string> g_locale;

// Reads key="value" lines the way OBS does (\" \n \t escapes, # comments).
bool LoadLocale(const std::wstring &path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t eq = line.find('=');
		if (line.empty() || line[0] == '#' || eq == std::string::npos)
			continue;
		std::string value = line.substr(eq + 1);
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
			value = value.substr(1, value.size() - 2);
		std::string out;
		for (size_t i = 0; i < value.size(); i++) {
			if (value[i] == '\\' && i + 1 < value.size()) {
				const char c = value[++i];
				out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
			} else {
				out += value[i];
			}
		}
		g_locale[line.substr(0, eq)] = out;
	}
	return true;
}

std::wstring T(const char *key, const wchar_t *english)
{
	const auto it = g_locale.find(key);
	return it == g_locale.end() ? english : Utf8ToWide(it->second.c_str());
}

std::wstring Replace(std::wstring s, const wchar_t *what, const std::wstring &with)
{
	if (const size_t at = s.find(what); at != std::wstring::npos)
		s.replace(at, wcslen(what), with);
	return s;
}

void ReportClipped(HWND h, const wchar_t *what, const std::wstring &text, int need, int have)
{
	wprintf(L"  control %d (%ls): needs %d px, has %d: %ls\n", GetDlgCtrlID(h), what, need, have, text.c_str());
	g_layout_problems++;
}

// Measures every piece of dialog text against the control showing it, using the control's own font.
void CheckLayout(HWND dlg)
{
	EnumChildWindows(
		dlg,
		[](HWND h, LPARAM) -> BOOL {
			wchar_t cls[32];
			GetClassNameW(h, cls, 32);
			wchar_t buf[512];
			GetWindowTextW(h, buf, 512);
			const std::wstring text = buf;
			RECT rc;
			GetClientRect(h, &rc);
			HDC dc = GetDC(h);
			HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
			const UINT dpi = GetDpiForWindow(h);
			if (_wcsicmp(cls, L"Button") == 0 && !text.empty()) {
				if ((GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX) {
					RECT t{};
					DrawTextW(dc, text.c_str(), -1, &t, DT_CALCRECT | DT_SINGLELINE);
					const int need = t.right + MulDiv(16, dpi, 96);
					if (need > rc.right)
						ReportClipped(h, L"group", text, need, rc.right);
				} else {
					SIZE ideal{};
					SendMessageW(h, BCM_GETIDEALSIZE, 0, reinterpret_cast<LPARAM>(&ideal));
					if (ideal.cx > rc.right)
						ReportClipped(h, L"button", text, ideal.cx, rc.right);
				}
			} else if (_wcsicmp(cls, L"Static") == 0 && !text.empty()) {
				RECT t{0, 0, rc.right, 0};
				DrawTextW(dc, text.c_str(), -1, &t, DT_CALCRECT | DT_WORDBREAK);
				if (t.right > rc.right)
					ReportClipped(h, L"label", text, t.right, rc.right);
				else if (t.bottom > rc.bottom)
					ReportClipped(h, L"label wraps", text, t.bottom, rc.bottom);
			} else if (_wcsicmp(cls, L"ComboBox") == 0) {
				const int room = rc.right - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - MulDiv(8, dpi, 96);
				const int n = (int)SendMessageW(h, CB_GETCOUNT, 0, 0);
				for (int i = 0; i < n; i++) {
					SendMessageW(h, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
					RECT t{};
					DrawTextW(dc, buf, -1, &t, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
					if (t.right > room)
						ReportClipped(h, L"list item", buf, t.right, room);
				}
			}
			SelectObject(dc, old);
			ReleaseDC(h, dc);
			return TRUE;
		},
		0);
	wprintf(L"layout: %d problem(s)\n", g_layout_problems);
}

void Log(bool error, const char *msg)
{
	fprintf(error ? stderr : stdout, "[overlay] %s\n", msg);
}

void CALLBACK SnapDialog(HWND, UINT, UINT_PTR id, DWORD)
{
	KillTimer(nullptr, id);
	HWND dlg = nullptr;
	EnumThreadWindows(
		GetCurrentThreadId(),
		[](HWND h, LPARAM p) {
			wchar_t cls[16];
			GetClassNameW(h, cls, 16);
			if (wcscmp(cls, L"#32770") == 0 && IsWindowVisible(h)) {
				*reinterpret_cast<HWND *>(p) = h;
				return FALSE;
			}
			return TRUE;
		},
		reinterpret_cast<LPARAM>(&dlg));
	if (dlg) {
		if (g_check_layout)
			CheckLayout(dlg);
		if (!g_snap.empty()) {
			const bool ok = tools::SaveWindow(dlg, g_snap);
			wprintf(L"%ls %ls\n", ok ? L"wrote" : L"FAILED to write", g_snap.c_str());
		}
		PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
	}
}

// Opaque stand-in for "the game" behind the overlay, so screenshots only contain our pixels.
HWND CreateBackdrop(const RECT &r)
{
	WNDCLASSW wc{};
	wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
		if (m == WM_PAINT) {
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(h, &ps);
			RECT rc;
			GetClientRect(h, &rc);
			TRIVERTEX v[2] = {{0, 0, 0xB000, 0xC800, 0xD800, 0xFF00},
					  {rc.right, rc.bottom, 0x1800, 0x1C00, 0x2400, 0xFF00}};
			GRADIENT_RECT g{0, 1};
			GradientFill(dc, v, 2, &g, 1, GRADIENT_FILL_RECT_H);
			EndPaint(h, &ps);
			return 0;
		}
		return DefWindowProcW(h, m, w, l);
	};
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"OverlayDemoBackdrop";
	RegisterClassW(&wc);
	HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
				 r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance,
				 nullptr);
	ShowWindow(h, SW_SHOWNOACTIVATE);
	UpdateWindow(h);
	return h;
}

void Pump(DWORD ms)
{
	const ULONGLONG end = GetTickCount64() + ms;
	while (GetTickCount64() < end) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
	}
}

Toast MakeToast(ToastCategory c, Icon i, const std::wstring &title, const std::wstring &sub = {})
{
	Toast t;
	t.category = c;
	t.content = {i, title, sub};
	return t;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	bool dialog = false, capturable = false;
	int scale = 100;
	Corner corner = Corner::TopRight;
	for (int i = 1; i < argc; i++) {
		const std::wstring a = argv[i];
		if (a == L"--dialog")
			dialog = true;
		else if (a == L"--capturable")
			capturable = true;
		else if (a == L"--snap" && i + 1 < argc)
			g_snap = argv[++i];
		else if (a == L"--check-layout")
			g_check_layout = true;
		else if (a == L"--locale" && i + 1 < argc) {
			if (!LoadLocale(argv[++i])) {
				fwprintf(stderr, L"can't read %ls\n", argv[i]);
				return 1;
			}
		}
		else if (a == L"--scale" && i + 1 < argc)
			scale = _wtoi(argv[++i]);
		else if (a == L"--corner" && i + 1 < argc) {
			const std::wstring c = argv[++i];
			corner = c == L"tl" ? Corner::TopLeft
				 : c == L"bl" ? Corner::BottomLeft
				 : c == L"br" ? Corner::BottomRight
					      : Corner::TopRight;
		}
	}

	Overlay::SetLogger(Log);
	Overlay overlay;
	Settings s;
	s.indicator_corner = s.toast_corner = corner;
	s.show_timer = true;
	s.mic_mode = MicMode::WhenMuted;
	s.notify_mic = true;
	s.hide_from_capture = !capturable;
	s.scale_percent = scale;
	overlay.SetSettings(s);
	if (!overlay.Start(GetModuleHandleW(nullptr))) {
		fprintf(stderr, "overlay failed to start\n");
		return 1;
	}

	if (dialog) {
		DialogContext ctx;
		ctx.audio_sources = {{"Mic/Aux"}, {"Headset Microphone"}, {"Elgato Wave:3"}};
		ctx.default_mic_label = T("Settings.DefaultMic", L"Default (Mic/Aux)");
		ctx.preview = [&](const Settings &p) {
			overlay.SetSettings(p);
			overlay.Preview(T("Toast.ReplaySaved", L"Replay saved"));
		};
		if (!g_locale.empty()) {
			ctx.text = [](const char *key) {
				const auto it = g_locale.find(key);
				return it == g_locale.end() ? std::wstring() : Utf8ToWide(it->second.c_str());
			};
		}
		if (!g_snap.empty() || g_check_layout)
			SetTimer(nullptr, 1, 1500, SnapDialog); // the first CJK font fallback can take a moment
		const bool ok = ShowSettingsDialog(nullptr, GetModuleHandleW(nullptr), s, ctx);
		printf("dialog %s\n", ok ? "accepted" : "cancelled");
		overlay.Stop();
		return g_layout_problems ? 2 : 0;
	}

	HWND backdrop = nullptr;
	RECT snap_rect{};
	if (!g_snap.empty()) {
		MONITORINFO mi{sizeof(mi)};
		GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
		const RECT m = mi.rcMonitor;
		const int w = 640 * scale / 100, h = 380 * scale / 100;
		const bool right = corner == Corner::TopRight || corner == Corner::BottomRight;
		const bool top = corner == Corner::TopLeft || corner == Corner::TopRight;
		snap_rect.left = right ? m.right - w : m.left;
		snap_rect.top = top ? m.top : m.bottom - h;
		snap_rect.right = snap_rect.left + w;
		snap_rect.bottom = snap_rect.top + h;
		backdrop = CreateBackdrop(snap_rect);
		Pump(300);
	}

	Status st;
	auto now = [] { return GetTickCount64(); };

	st.replay = true;
	overlay.SetStatus(st);
	const std::wstring five_minutes = Replace(T("Duration.Minutes", L"%1 minutes"), L"%1", L"5");
	overlay.ShowToast(MakeToast(ToastCategory::Replay, Icon::Replay, T("Toast.ReplayOn", L"Replay buffer is on"),
				    Replace(Replace(T("Toast.ReplayHintKey", L"Press %1 to save the last %2"), L"%1",
						    L"Alt + F10"),
					    L"%2", five_minutes)));
	Pump(900);

	st.recording = true;
	st.rec_start = now() - 61000;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Recording, Icon::Record, T("Toast.RecordingStarted", L"Recording started")));
	Pump(700);

	st.mic_present = st.mic_muted = true;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Mic, Icon::MicMuted, T("Toast.MicMuted", L"Microphone muted")));
	Pump(1000);

	if (!g_snap.empty()) {
		const bool ok = tools::SaveScreenRect(snap_rect, g_snap);
		wprintf(L"%ls %ls\n", ok ? L"wrote" : L"FAILED to write", g_snap.c_str());
		overlay.Stop();
		DestroyWindow(backdrop);
		return ok ? 0 : 1;
	}

	Pump(900);
	overlay.ShowToast(MakeToast(ToastCategory::Replay, Icon::Saved, T("Toast.ReplaySaved", L"Replay saved"),
				    L"Replay 2026-11-25 21-14-03.mkv"));
	Pump(1500);
	overlay.ShowToast(MakeToast(ToastCategory::Screenshot, Icon::Screenshot, T("Toast.ScreenshotSaved", L"Screenshot saved"),
				    L"Screenshot 2026-11-25 21-15-44.png"));
	Pump(1000);

	st.streaming = true;
	st.stream_start = now();
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Streaming, Icon::Stream, T("Toast.StreamStarted", L"You're live"),
				    T("Toast.StreamStartedSub", L"Stream started")));
	Pump(1500);

	st.rec_paused = true;
	st.rec_pause_start = now();
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Recording, Icon::Pause, T("Toast.RecordingPaused", L"Recording paused")));
	Pump(1500);

	st.rec_paused_total += now() - st.rec_pause_start;
	st.rec_pause_start = 0;
	st.rec_paused = false;
	st.mic_muted = false;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Recording, Icon::Record, T("Toast.RecordingResumed", L"Recording resumed")));
	Pump(1500);

	st.recording = false;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Recording, Icon::Saved, T("Toast.RecordingSaved", L"Recording saved"),
				    L"2026-11-25 21-13-02.mkv"));
	Pump(1500);

	st.streaming = false;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Streaming, Icon::Stop, T("Toast.StreamEnded", L"Stream ended"),
				    Replace(T("Toast.StreamDuration", L"Streamed for %1"), L"%1", L"0:06")));
	Pump(1200);

	st.replay = false;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Replay, Icon::Replay, T("Toast.ReplayOff", L"Replay buffer is off")));
	Pump(5000);

	overlay.Stop();
	return 0;
}
