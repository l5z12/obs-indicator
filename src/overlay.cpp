// SPDX-License-Identifier: GPL-2.0-or-later
#include "overlay.hpp"
#include "monitors.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace indicator {

namespace {

constexpr UINT WM_APP_WAKE = WM_APP + 1;

constexpr UINT_PTR TIMER_FRAME = 1;   // animation frames
constexpr UINT_PTR TIMER_WAKE = 2;    // one-shot: next clock tick / toast expiry
constexpr UINT_PTR TIMER_TOPMOST = 3; // periodically re-assert z-order while visible

constexpr uint64_t kFrameMs = 15;
constexpr float kIndicatorFadeInMs = 160.0f;
constexpr float kIndicatorFadeOutMs = 240.0f;
constexpr float kToastInMs = 240.0f;
constexpr float kToastOutMs = 280.0f;
constexpr float kToastMoveTauMs = 55.0f;
constexpr float kToastSlide = 28.0f;
constexpr float kToastGap = 8.0f;
constexpr size_t kMaxToasts = 4;

constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kRed = 0xFF3B3B;
constexpr uint32_t kAmber = 0xFFB300;

constexpr wchar_t kOverlayClass[] = L"ObsIndicatorOverlayWindow";
constexpr wchar_t kControlClass[] = L"ObsIndicatorControlWindow";

std::atomic<Overlay::LogFn> g_log{nullptr};

void Log(bool error, const char *fmt, ...)
{
	auto fn = g_log.load();
	if (!fn)
		return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fn(error, buf);
}

float Clamp01(float t)
{
	return t < 0 ? 0 : (t > 1 ? 1 : t);
}
float EaseOutCubic(float t)
{
	const float u = 1 - Clamp01(t);
	return 1 - u * u * u;
}
float EaseInCubic(float t)
{
	t = Clamp01(t);
	return t * t * t;
}

bool IsRight(Corner c)
{
	return c == Corner::TopRight || c == Corner::BottomRight;
}
bool IsTop(Corner c)
{
	return c == Corner::TopLeft || c == Corner::TopRight;
}

// One click-through, top-most, per-pixel-alpha window plus the D2D target that paints it.
struct LayeredWindow {
	HWND hwnd = nullptr;
	HDC mem = nullptr;
	HBITMAP bmp = nullptr;
	HGDIOBJ old = nullptr;
	int w = 0, h = 0;
	bool shown = false;
	ComPtr<ID2D1DCRenderTarget> rt;
	ComPtr<ID2D1Layer> layer;
	ID2D1Factory *factory = nullptr;

	bool Create(HINSTANCE inst, ID2D1Factory *f)
	{
		factory = f;
		hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
					       WS_EX_NOACTIVATE,
				       kOverlayClass, L"OBS Indicator", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst,
				       nullptr);
		mem = CreateCompatibleDC(nullptr);
		return hwnd && mem && CreateTarget();
	}

	bool CreateTarget()
	{
		rt.Reset();
		layer.Reset();
		// Software rendering: these surfaces are tiny, and it keeps the overlay
		// entirely off the GPU the game is using.
		const auto props = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_SOFTWARE,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
		if (FAILED(factory->CreateDCRenderTarget(&props, &rt)))
			return false;
		rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
		rt->CreateLayer(&layer);
		return true;
	}

	bool EnsureSize(int nw, int nh)
	{
		nw = std::max(nw, 1);
		nh = std::max(nh, 1);
		if (bmp && nw == w && nh == h)
			return true;
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = nw;
		bi.bmiHeader.biHeight = -nh; // top-down
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		void *bits = nullptr;
		HBITMAP nb = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!nb)
			return false;
		HGDIOBJ prev = SelectObject(mem, nb);
		if (!old)
			old = prev;
		else
			DeleteObject(prev);
		bmp = nb;
		w = nw;
		h = nh;
		return true;
	}

	ID2D1RenderTarget *Begin(int nw, int nh)
	{
		if (!rt && !CreateTarget())
			return nullptr;
		if (!EnsureSize(nw, nh))
			return nullptr;
		const RECT rc{0, 0, w, h};
		if (FAILED(rt->BindDC(mem, &rc)))
			return nullptr;
		rt->BeginDraw();
		rt->SetTransform(D2D1::Matrix3x2F::Identity());
		rt->Clear(D2D1::ColorF(0, 0, 0, 0));
		return rt.Get();
	}

	bool End()
	{
		const HRESULT hr = rt->EndDraw();
		if (hr == D2DERR_RECREATE_TARGET) {
			CreateTarget();
			return false;
		}
		return SUCCEEDED(hr);
	}

	void Present(int x, int y, BYTE alpha)
	{
		POINT dst{x, y}, src{0, 0};
		SIZE size{w, h};
		BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
		if (!UpdateLayeredWindow(hwnd, nullptr, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA))
			return;
		if (!shown) {
			ShowWindow(hwnd, SW_SHOWNOACTIVATE);
			shown = true;
			BringToTop();
		}
	}

	void BringToTop() const
	{
		if (shown)
			SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
				     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	}

	void Hide()
	{
		if (shown) {
			ShowWindow(hwnd, SW_HIDE);
			shown = false;
		}
	}

	void Destroy()
	{
		rt.Reset();
		layer.Reset();
		if (mem) {
			if (old)
				SelectObject(mem, old);
			DeleteDC(mem);
			mem = nullptr;
		}
		if (bmp) {
			DeleteObject(bmp);
			bmp = nullptr;
		}
		if (hwnd) {
			DestroyWindow(hwnd);
			hwnd = nullptr;
		}
		shown = false;
		old = nullptr;
		w = h = 0;
	}
};

struct ToastAnim {
	ToastContent content;
	uint64_t born = 0;
	uint64_t expires = 0;
	uint64_t leave_at = 0; // 0 while not leaving
	float y = 0;           // animated offset from the anchor corner along the stack
	bool placed = false;
};

} // namespace

uint64_t Status::RecordingElapsed(uint64_t now) const
{
	if (!recording || !rec_start)
		return 0;
	const uint64_t end = rec_pause_start ? rec_pause_start : now;
	const uint64_t gross = end > rec_start ? end - rec_start : 0;
	return gross > rec_paused_total ? gross - rec_paused_total : 0;
}

uint64_t Status::StreamingElapsed(uint64_t now) const
{
	return streaming && stream_start && now > stream_start ? now - stream_start : 0;
}

std::wstring FormatDuration(uint64_t ms)
{
	const uint64_t t = ms / 1000;
	wchar_t buf[32];
	if (t >= 3600)
		swprintf(buf, 32, L"%llu:%02llu:%02llu", t / 3600, (t / 60) % 60, t % 60);
	else
		swprintf(buf, 32, L"%llu:%02llu", t / 60, t % 60);
	return buf;
}

// ---------------------------------------------------------------------------

struct Overlay::Impl {
	// --- shared with other threads (guarded by mtx) ---
	std::mutex mtx;
	struct Pending {
		bool has_settings = false;
		Settings settings;
		bool has_status = false;
		Status status;
		std::vector<Toast> toasts;
		uint32_t preview_ms = 0;
	} pending;
	std::atomic<HWND> ctrl{nullptr};
	std::thread thread;
	HINSTANCE inst = nullptr;

	// --- overlay thread only ---
	Renderer renderer;
	LayeredWindow ind_win, toast_win;
	HWINEVENTHOOK fg_hook = nullptr;
	Settings settings;
	Status status;
	std::deque<ToastAnim> toasts; // front = newest
	uint64_t last_tick = 0;
	uint64_t preview_until = 0;
	uint64_t preview_start = 0;
	bool frame_timer = false;
	bool topmost_timer = false;

	float ind_fade = 0; // 0..1
	std::vector<IndicatorItem> ind_items; // what is (or was last) drawn
	std::wstring ind_signature;
	bool toasts_dirty = false;

	HMONITOR specific_mon = nullptr;
	bool specific_mon_valid = false;

	static thread_local Impl *t_self;

	// ---------------------------------------------------------------- thread
	bool Run(HANDLE ready, bool *ok)
	{
		SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		SetThreadDescription(GetCurrentThread(), L"obs-indicator overlay");
		t_self = this;

		*ok = InitWindows();
		SetEvent(ready);
		if (!*ok) {
			Shutdown();
			return false;
		}

		// Apply anything queued before the thread was up.
		OnWake();

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		Shutdown();
		return true;
	}

	bool InitWindows()
	{
		if (!renderer.Init()) {
			Log(true, "Direct2D/DirectWrite initialisation failed");
			return false;
		}
		WNDCLASSEXW wc{sizeof(wc)};
		wc.lpfnWndProc = OverlayProc;
		wc.hInstance = inst;
		wc.lpszClassName = kOverlayClass;
		RegisterClassExW(&wc);
		wc.lpfnWndProc = ControlProc;
		wc.lpszClassName = kControlClass;
		RegisterClassExW(&wc);

		// A hidden top-level window (not message-only) so that broadcast
		// messages such as WM_DISPLAYCHANGE reach us.
		HWND c = CreateWindowExW(WS_EX_TOOLWINDOW, kControlClass, L"OBS Indicator Control", WS_POPUP, 0, 0, 0,
					 0, nullptr, nullptr, inst, nullptr);
		if (!c) {
			Log(true, "CreateWindowEx (control) failed: %lu", GetLastError());
			return false;
		}
		ctrl = c;
		if (!ind_win.Create(inst, renderer.Factory()) || !toast_win.Create(inst, renderer.Factory())) {
			Log(true, "Failed to create overlay windows: %lu", GetLastError());
			return false;
		}
		ApplyAffinity();

		fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundHook, 0,
					  0, WINEVENT_OUTOFCONTEXT);
		last_tick = GetTickCount64();
		return true;
	}

	void Shutdown()
	{
		if (fg_hook) {
			UnhookWinEvent(fg_hook);
			fg_hook = nullptr;
		}
		ind_win.Destroy();
		toast_win.Destroy();
		if (HWND c = ctrl.exchange(nullptr))
			DestroyWindow(c);
		UnregisterClassW(kOverlayClass, inst);
		UnregisterClassW(kControlClass, inst);
		t_self = nullptr;
	}

	static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg) {
		case WM_NCHITTEST:
			return HTTRANSPARENT;
		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE;
		case WM_DPICHANGED:
			// We size and place these windows ourselves from the target monitor's DPI.
			if (t_self)
				t_self->Invalidate();
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	static LRESULT CALLBACK ControlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		Impl *self = t_self;
		switch (msg) {
		case WM_APP_WAKE:
			if (self)
				self->OnWake();
			return 0;
		case WM_TIMER:
			if (self) {
				if (wp == TIMER_TOPMOST) {
					self->ind_win.BringToTop();
					self->toast_win.BringToTop();
				} else {
					if (wp == TIMER_WAKE)
						KillTimer(hwnd, TIMER_WAKE);
					self->Update();
				}
			}
			return 0;
		case WM_DISPLAYCHANGE:
		case WM_SETTINGCHANGE:
			if (self) {
				self->specific_mon_valid = false;
				self->Invalidate();
			}
			break;
		case WM_CLOSE:
			// Stop posting to this handle before it can be recycled.
			if (self)
				self->ctrl = nullptr;
			DestroyWindow(hwnd);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	static void CALLBACK ForegroundHook(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
	{
		Impl *self = t_self;
		if (!self)
			return;
		// A game going fullscreen may push itself above us; take the top spot back.
		self->ind_win.BringToTop();
		self->toast_win.BringToTop();
		if (self->settings.monitor_mode == MonitorMode::FollowForeground)
			self->Invalidate();
	}

	// Forces a redraw on the next pass. Deferred through the queue because it
	// can be triggered from inside UpdateLayeredWindow (WM_DPICHANGED when a
	// window moves to another monitor), which must not re-enter Update().
	void Invalidate()
	{
		ind_signature.clear();
		toasts_dirty = true;
		Wake();
	}

	// ------------------------------------------------------------ messages
	void OnWake()
	{
		Pending p;
		{
			std::lock_guard lock(mtx);
			p = std::move(pending);
			pending = Pending{};
		}
		if (p.has_settings)
			ApplySettings(p.settings);
		if (p.has_status)
			status = p.status;
		const uint64_t now = GetTickCount64();
		for (auto &t : p.toasts)
			AddToast(t, now);
		if (p.preview_ms)
			StartPreview(p.preview_ms, now);
		Update();
	}

	void ApplySettings(Settings s)
	{
		s.Clamp();
		const bool affinity_changed = s.hide_from_capture != settings.hide_from_capture;
		settings = std::move(s);
		specific_mon_valid = false;
		if (affinity_changed)
			ApplyAffinity();
		if (!settings.enabled || !settings.notifications)
			toasts.clear();
		ind_signature.clear();
		toasts_dirty = true;
	}

	void ApplyAffinity()
	{
		const DWORD mode = settings.hide_from_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
		for (HWND h : {ind_win.hwnd, toast_win.hwnd}) {
			if (h && !SetWindowDisplayAffinity(h, mode))
				Log(true, "SetWindowDisplayAffinity(%lu) failed: %lu (overlay may appear in captures)",
				    mode, GetLastError());
		}
	}

	bool CategoryEnabled(ToastCategory c) const
	{
		switch (c) {
		case ToastCategory::Recording:
			return settings.notify_recording;
		case ToastCategory::Streaming:
			return settings.notify_streaming;
		case ToastCategory::Replay:
			return settings.notify_replay;
		case ToastCategory::Screenshot:
			return settings.notify_screenshot;
		case ToastCategory::VirtualCam:
			return settings.notify_virtualcam;
		case ToastCategory::Mic:
			return settings.notify_mic;
		case ToastCategory::General:
			return true;
		}
		return true;
	}

	void AddToast(const Toast &t, uint64_t now, bool force = false)
	{
		if (!settings.enabled || !settings.notifications)
			return;
		if (!force && !CategoryEnabled(t.category))
			return;
		ToastAnim a;
		a.content = t.content;
		a.born = now;
		a.expires = now + (uint64_t)settings.toast_seconds * 1000;
		toasts.push_front(std::move(a));

		size_t live = 0;
		for (auto &x : toasts) {
			if (x.leave_at)
				continue;
			if (++live > kMaxToasts)
				x.leave_at = now;
		}
		toasts_dirty = true;
	}

	void StartPreview(uint32_t ms, uint64_t now)
	{
		preview_start = now;
		preview_until = now + ms;
		Toast t;
		t.content.icon = Icon::Saved;
		t.content.title = L"Replay saved";
		t.content.subtitle = L"Replay 2026-01-01 12-00-00.mkv";
		AddToast(t, now, true);
		ind_signature.clear();
	}

	// --------------------------------------------------------------- frame
	struct Geometry {
		RECT mon{};
		float s = 1;      // content scale (DPI x user)
		float dpi = 1;    // DPI scale only
		int margin = 0;
	};

	Geometry ComputeGeometry()
	{
		HMONITOR mon = nullptr;
		switch (settings.monitor_mode) {
		case MonitorMode::Specific:
			if (!specific_mon_valid) {
				specific_mon = FindMonitorByDevice(settings.monitor_device);
				specific_mon_valid = true;
			}
			mon = specific_mon;
			break;
		case MonitorMode::FollowForeground:
			if (HWND fg = GetForegroundWindow())
				mon = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
			break;
		case MonitorMode::Primary:
			break;
		}
		if (!mon)
			mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

		Geometry g;
		MONITORINFO mi{sizeof(mi)};
		GetMonitorInfoW(mon, &mi);
		g.mon = mi.rcMonitor;
		g.dpi = MonitorScale(mon);
		g.s = g.dpi * settings.scale_percent / 100.0f;
		g.margin = (int)std::lround(settings.margin * g.dpi);
		return g;
	}

	std::vector<IndicatorItem> BuildItems(uint64_t now) const
	{
		Status st = status;
		if (preview_until > now) {
			st = Status{};
			st.recording = st.streaming = st.replay = st.virtualcam = true;
			st.rec_start = preview_start - 754000;
			st.stream_start = preview_start - 3723000;
			st.mic_present = true;
			st.mic_muted = settings.mic_mode == MicMode::WhenMuted;
		}
		const Settings &s = settings;
		std::vector<IndicatorItem> items;
		const bool rec = st.recording && s.ind_recording;
		const bool stream = st.streaming && s.ind_streaming;
		const bool replay = st.replay && s.ind_replay;
		const bool vcam = st.virtualcam && s.ind_virtualcam;
		if (!rec && !stream && !replay && !vcam)
			return items;

		if (replay)
			items.push_back({Icon::Replay, kWhite, {}});
		if (rec) {
			std::wstring t = s.show_timer ? FormatDuration(st.RecordingElapsed(now)) : std::wstring();
			if (st.rec_paused)
				items.push_back({Icon::Pause, kAmber, std::move(t)});
			else
				items.push_back({Icon::Record, kRed, std::move(t)});
		}
		if (stream)
			items.push_back({Icon::Stream, kWhite,
					 s.show_timer ? FormatDuration(st.StreamingElapsed(now)) : std::wstring()});
		if (vcam)
			items.push_back({Icon::VirtualCam, kWhite, {}});
		if (s.mic_mode != MicMode::Off && st.mic_present) {
			if (st.mic_muted)
				items.push_back({Icon::MicMuted, kWhite, {}});
			else if (s.mic_mode == MicMode::Always)
				items.push_back({Icon::Mic, kWhite, {}});
		}
		return items;
	}

	// Returns the indicator's on-screen height (0 when hidden), used to push
	// notifications out of its way when both share a corner.
	int UpdateIndicator(uint64_t now, float dt, const Geometry &g, bool &animating)
	{
		std::vector<IndicatorItem> items =
			settings.enabled && settings.show_indicator ? BuildItems(now) : std::vector<IndicatorItem>{};
		const bool want = !items.empty();
		if (want)
			ind_items = std::move(items);

		const float target = want ? 1.0f : 0.0f;
		if (ind_fade != target) {
			const float step = dt / (want ? kIndicatorFadeInMs : kIndicatorFadeOutMs);
			ind_fade = want ? std::min(1.0f, ind_fade + step) : std::max(0.0f, ind_fade - step);
			animating = animating || ind_fade != target;
		}
		if (ind_fade <= 0.0f) {
			ind_win.Hide();
			ind_items.clear();
			ind_signature.clear();
			return 0;
		}

		const D2D1_SIZE_F sz = renderer.MeasureIndicator(ind_items, g.s);
		const int w = (int)sz.width, h = (int)sz.height;
		const Corner c = settings.indicator_corner;
		const int x = IsRight(c) ? g.mon.right - g.margin - w : g.mon.left + g.margin;
		const int y = IsTop(c) ? g.mon.top + g.margin : g.mon.bottom - g.margin - h;
		const float fade = want ? EaseOutCubic(ind_fade) : ind_fade * ind_fade;
		const BYTE alpha = (BYTE)std::lround(255.0f * fade * settings.opacity_percent / 100.0f);

		std::wstring sig;
		for (const auto &it : ind_items) {
			sig += std::to_wstring((int)it.icon) + L":" + std::to_wstring(it.color) + L":" + it.text + L"|";
		}
		sig += std::to_wstring(x) + L"," + std::to_wstring(y) + L"," + std::to_wstring(alpha) + L"," +
		       std::to_wstring(g.s);
		if (sig != ind_signature || !ind_win.shown) {
			if (ID2D1RenderTarget *rt = ind_win.Begin(w, h)) {
				renderer.DrawIndicator(rt, D2D1::Point2F(0, 0), ind_items, g.s);
				if (ind_win.End()) {
					ind_win.Present(x, y, alpha);
					ind_signature = std::move(sig);
				}
			}
		}
		return want ? h : 0;
	}

	void UpdateToasts(uint64_t now, float dt, const Geometry &g, int indicator_h, bool &animating)
	{
		// Expire and retire.
		for (auto &t : toasts) {
			if (!t.leave_at && now >= t.expires)
				t.leave_at = now;
		}
		std::erase_if(toasts, [&](const ToastAnim &t) { return t.leave_at && now - t.leave_at >= kToastOutMs; });
		if (toasts.empty()) {
			if (toast_win.shown)
				toast_win.Hide();
			toasts_dirty = false;
			return;
		}

		const float s = g.s;
		const float tw = renderer.ToastWidth(s);
		const float gap = kToastGap * s;
		const float slide = kToastSlide * s;
		const Corner c = settings.toast_corner;
		float offset = 0;
		if (indicator_h > 0 && c == settings.indicator_corner)
			offset = indicator_h + gap;

		// Stack layout (newest closest to the corner) with smooth repositioning.
		float target = offset;
		float extent = 0;
		bool moving = false;
		std::vector<float> heights;
		heights.reserve(toasts.size());
		for (auto &t : toasts) {
			const float th = renderer.ToastHeight(t.content, s);
			heights.push_back(th);
			if (!t.placed) {
				t.y = target;
				t.placed = true;
			} else if (std::fabs(t.y - target) > 0.5f) {
				t.y += (target - t.y) * (1.0f - std::exp(-dt / kToastMoveTauMs));
				moving = true;
			} else {
				t.y = target;
			}
			extent = std::max(extent, t.y + th);
			target += th + gap;
		}

		const int edge_room = std::min((int)std::ceil(slide), g.margin);
		const int win_w = (int)tw + edge_room;
		const int win_h = std::min((int)std::ceil(extent), (int)(g.mon.bottom - g.mon.top));
		const int win_x = IsRight(c) ? g.mon.right - g.margin - (int)tw : g.mon.left + g.margin - edge_room;
		const int win_y = IsTop(c) ? g.mon.top + g.margin : g.mon.bottom - g.margin - win_h;

		bool fading = false;
		ID2D1RenderTarget *rt = toast_win.Begin(win_w, win_h);
		if (!rt)
			return;
		size_t i = 0;
		for (auto &t : toasts) {
			const float th = heights[i++];
			const float tin = (now - t.born) / kToastInMs;
			float alpha = EaseOutCubic(tin);
			float dx = slide * (1.0f - EaseOutCubic(tin));
			if (tin < 1.0f)
				fading = true;
			if (t.leave_at) {
				const float tout = (now - t.leave_at) / kToastOutMs;
				alpha *= 1.0f - EaseInCubic(tout);
				dx += slide * 0.6f * EaseInCubic(tout);
				fading = true;
			}
			if (alpha <= 0.0f)
				continue;
			const float x = IsRight(c) ? dx : edge_room - dx;
			const float y = IsTop(c) ? t.y : win_h - t.y - th;
			const bool layered = alpha < 0.999f && toast_win.layer;
			if (layered)
				rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
								    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
								    D2D1::IdentityMatrix(), alpha),
					      toast_win.layer.Get());
			renderer.DrawToast(rt, D2D1::Point2F(std::round(x), std::round(y)), t.content,
					   settings.accent, s);
			if (layered)
				rt->PopLayer();
		}
		if (toast_win.End())
			toast_win.Present(win_x, win_y,
					  (BYTE)std::lround(255.0f * settings.opacity_percent / 100.0f));
		toasts_dirty = false;
		animating = animating || moving || fading;
	}

	void Update()
	{
		const uint64_t now = GetTickCount64();
		// When idle, the first frame of a new animation advances by one frame,
		// not by the time we spent asleep.
		const float dt = frame_timer ? (float)std::min<uint64_t>(now - last_tick, 100) : (float)kFrameMs;
		last_tick = now;

		if (preview_until && now >= preview_until) {
			preview_until = 0;
			ind_signature.clear();
		}

		const Geometry g = ComputeGeometry();
		bool animating = false;
		const int ind_h = UpdateIndicator(now, dt, g, animating);
		bool toast_anim = false;
		if (toasts_dirty || !toasts.empty())
			UpdateToasts(now, dt, g, ind_h, toast_anim);
		animating = animating || toast_anim;
		Schedule(now, animating);
	}

	void Schedule(uint64_t now, bool animating)
	{
		HWND c = ctrl.load();
		if (!c)
			return;
		if (animating != frame_timer) {
			if (animating)
				SetTimer(c, TIMER_FRAME, (UINT)kFrameMs, nullptr);
			else
				KillTimer(c, TIMER_FRAME);
			frame_timer = animating;
		}

		const bool visible = ind_win.shown || toast_win.shown;
		if (visible != topmost_timer) {
			if (visible)
				SetTimer(c, TIMER_TOPMOST, 2000, nullptr);
			else
				KillTimer(c, TIMER_TOPMOST);
			topmost_timer = visible;
		}

		if (animating) {
			KillTimer(c, TIMER_WAKE);
			return;
		}
		// Idle: sleep until the next thing that changes on its own.
		uint64_t next = UINT64_MAX;
		for (const auto &t : toasts)
			if (!t.leave_at)
				next = std::min(next, t.expires);
		if (preview_until)
			next = std::min(next, preview_until);
		bool has_clock = false;
		for (const auto &it : ind_items)
			has_clock = has_clock || !it.text.empty();
		if (has_clock && ind_fade > 0) {
			const Status &st = status;
			uint64_t el = UINT64_MAX;
			if (preview_until > now)
				el = now - preview_start;
			else if (st.recording && !st.rec_paused)
				el = st.RecordingElapsed(now);
			else if (st.streaming)
				el = st.StreamingElapsed(now);
			if (el != UINT64_MAX)
				next = std::min(next, now + (1000 - el % 1000) + 2);
		}
		if (next == UINT64_MAX) {
			KillTimer(c, TIMER_WAKE);
			return;
		}
		const uint64_t delay = next > now ? next - now : 1;
		SetTimer(c, TIMER_WAKE, (UINT)std::min<uint64_t>(delay, 60000), nullptr);
	}

	// ------------------------------------------------------------- posting
	void Wake()
	{
		if (HWND c = ctrl.load())
			PostMessageW(c, WM_APP_WAKE, 0, 0);
	}
};

thread_local Overlay::Impl *Overlay::Impl::t_self = nullptr;

// ---------------------------------------------------------------------------

Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}

Overlay::~Overlay()
{
	Stop();
}

void Overlay::SetLogger(LogFn fn)
{
	g_log = fn;
}

bool Overlay::Start(HINSTANCE module)
{
	if (impl_->thread.joinable())
		return true;
	impl_->inst = module;
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ready)
		return false;
	bool ok = false;
	Impl *impl = impl_.get();
	try {
		impl_->thread = std::thread([impl, ready, &ok] { impl->Run(ready, &ok); });
	} catch (const std::system_error &) {
		CloseHandle(ready);
		return false;
	}
	WaitForSingleObject(ready, INFINITE);
	CloseHandle(ready);
	if (!ok) {
		impl_->thread.join();
		return false;
	}
	return true;
}

void Overlay::Stop()
{
	if (!impl_->thread.joinable())
		return;
	if (HWND c = impl_->ctrl.load())
		PostMessageW(c, WM_CLOSE, 0, 0);
	impl_->thread.join();
}

bool Overlay::Running() const
{
	return impl_->thread.joinable();
}

void Overlay::SetSettings(const Settings &settings)
{
	{
		std::lock_guard lock(impl_->mtx);
		impl_->pending.settings = settings;
		impl_->pending.has_settings = true;
	}
	impl_->Wake();
}

void Overlay::SetStatus(const Status &status)
{
	{
		std::lock_guard lock(impl_->mtx);
		impl_->pending.status = status;
		impl_->pending.has_status = true;
	}
	impl_->Wake();
}

void Overlay::ShowToast(const Toast &toast)
{
	{
		std::lock_guard lock(impl_->mtx);
		impl_->pending.toasts.push_back(toast);
	}
	impl_->Wake();
}

void Overlay::Preview(uint32_t duration_ms)
{
	{
		std::lock_guard lock(impl_->mtx);
		impl_->pending.preview_ms = duration_ms;
	}
	impl_->Wake();
}

} // namespace indicator
