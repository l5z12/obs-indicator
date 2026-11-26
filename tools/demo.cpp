// SPDX-License-Identifier: GPL-2.0-or-later
// Drives the real overlay with a scripted sequence of fake OBS events, or
// opens the settings dialog, without OBS running.
//
//   overlay-demo.exe                       play the demo (about 15 s)
//   overlay-demo.exe --corner bl           ...with the overlay bottom-left (tl|tr|bl|br)
//   overlay-demo.exe --dialog              open the settings dialog (Preview works)
//   overlay-demo.exe --dialog --snap x.png screenshot the dialog and close it
//   overlay-demo.exe --snap x.png          screenshot the overlay corner mid-demo and exit
//   overlay-demo.exe --capturable          don't hide the overlay from screen capture

#include "overlay.hpp"
#include "settings-dialog.hpp"
#include "png.hpp"

#include <cstdio>
#include <cwchar>
#include <string>

using namespace indicator;

namespace {

std::wstring g_snap;

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
		const bool ok = tools::SaveWindow(dlg, g_snap);
		wprintf(L"%ls %ls\n", ok ? L"wrote" : L"FAILED to write", g_snap.c_str());
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

Toast MakeToast(ToastCategory c, Icon i, const wchar_t *title, const wchar_t *sub = L"")
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
		ctx.preview = [&](const Settings &p) {
			overlay.SetSettings(p);
			overlay.Preview();
		};
		if (!g_snap.empty())
			SetTimer(nullptr, 1, 800, SnapDialog);
		const bool ok = ShowSettingsDialog(nullptr, GetModuleHandleW(nullptr), s, ctx);
		printf("dialog %s\n", ok ? "accepted" : "cancelled");
		overlay.Stop();
		return 0;
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
	overlay.ShowToast(MakeToast(ToastCategory::Replay, Icon::Replay, L"Replay buffer is on",
				    L"Press Alt + F10 to save the last 5 minutes"));
	Pump(900);

	st.recording = true;
	st.rec_start = now() - 61000;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Recording, Icon::Record, L"Recording started"));
	Pump(700);

	st.mic_present = st.mic_muted = true;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Mic, Icon::MicMuted, L"Microphone muted"));
	Pump(1000);

	if (!g_snap.empty()) {
		const bool ok = tools::SaveScreenRect(snap_rect, g_snap);
		wprintf(L"%ls %ls\n", ok ? L"wrote" : L"FAILED to write", g_snap.c_str());
		overlay.Stop();
		DestroyWindow(backdrop);
		return ok ? 0 : 1;
	}

	Pump(900);
	overlay.ShowToast(MakeToast(ToastCategory::Replay, Icon::Saved, L"Replay saved",
				    L"Replay 2026-11-25 21-14-03.mkv"));
	Pump(1500);
	overlay.ShowToast(MakeToast(ToastCategory::Screenshot, Icon::Screenshot, L"Screenshot saved",
				    L"Screenshot 2026-11-25 21-15-44.png"));
	Pump(1000);

	st.streaming = true;
	st.stream_start = now();
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Streaming, Icon::Stream, L"You're live", L"Stream started"));
	Pump(1500);

	st.rec_paused = true;
	st.rec_pause_start = now();
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Recording, Icon::Pause, L"Recording paused"));
	Pump(1500);

	st.rec_paused_total += now() - st.rec_pause_start;
	st.rec_pause_start = 0;
	st.rec_paused = false;
	st.mic_muted = false;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Recording, Icon::Record, L"Recording resumed"));
	Pump(1500);

	st.recording = false;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Recording, Icon::Saved, L"Recording saved",
				    L"2026-11-25 21-13-02.mkv"));
	Pump(1500);

	st.streaming = false;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Streaming, Icon::Stop, L"Stream ended", L"Streamed for 0:06"));
	Pump(1200);

	st.replay = false;
	overlay.SetStatus(st);
	overlay.ShowToast(MakeToast(ToastCategory::Replay, Icon::Replay, L"Replay buffer is off"));
	Pump(5000);

	overlay.Stop();
	return 0;
}
