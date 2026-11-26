// SPDX-License-Identifier: GPL-3.0-or-later
// Renders every indicator state, notification and icon into one PNG so the
// artwork can be reviewed without OBS or a live overlay.
//
//   overlay-preview.exe [out.png] [scale]

#include "overlay.hpp"
#include "renderer.hpp"
#include "png.hpp"

#include <algorithm>
#include <cstdio>
#include <cwchar>

using namespace indicator;

int wmain(int argc, wchar_t **argv)
{
	const std::wstring out = argc > 1 ? argv[1] : L"overlay-preview.png";
	const float s = argc > 2 ? (float)_wtof(argv[2]) : 1.0f;

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	Renderer r;
	if (!r.Init()) {
		fprintf(stderr, "renderer init failed\n");
		return 1;
	}
	auto wic = tools::Wic();
	const UINT W = (UINT)(980 * s), H = (UINT)(760 * s);
	ComPtr<IWICBitmap> bmp;
	if (!wic || FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) {
		fprintf(stderr, "WIC bitmap failed\n");
		return 1;
	}
	ComPtr<ID2D1RenderTarget> rt;
	const auto props = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_SOFTWARE,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
	if (FAILED(r.Factory()->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) {
		fprintf(stderr, "render target failed\n");
		return 1;
	}
	rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
	rt->BeginDraw();

	// Backdrop: a bright "game" half and a dark half, so contrast shows on both.
	ComPtr<ID2D1GradientStopCollection> stops;
	D2D1_GRADIENT_STOP gs[] = {{0.0f, Rgb(0xDDE8F0)}, {0.45f, Rgb(0x9DB6C8)}, {0.55f, Rgb(0x2A2F36)},
				   {1.0f, Rgb(0x0C0E10)}};
	rt->CreateGradientStopCollection(gs, 4, &stops);
	ComPtr<ID2D1LinearGradientBrush> bg;
	rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F((float)W, 0)),
				      stops.Get(), &bg);
	rt->FillRectangle(D2D1::RectF(0, 0, (float)W, (float)H), bg.Get());

	const float pad = 24 * s;
	float y = pad;

	// Status indicator variants
	const std::vector<std::vector<IndicatorItem>> rows = {
		{{Icon::Replay, 0xFFFFFF, {}}},
		{{Icon::Replay, 0xFFFFFF, {}}, {Icon::Record, 0xFF3B3B, {}}},
		{{Icon::Record, 0xFF3B3B, FormatDuration(754000)}, {Icon::Stream, 0xFFFFFF, FormatDuration(3723000)}},
		{{Icon::Replay, 0xFFFFFF, {}},
		 {Icon::Pause, 0xFFB300, FormatDuration(754000)},
		 {Icon::VirtualCam, 0xFFFFFF, {}},
		 {Icon::MicMuted, 0xFFFFFF, {}}},
		{{Icon::Stream, 0xFFFFFF, {}}, {Icon::Mic, 0xFFFFFF, {}}},
	};
	for (const auto &items : rows) {
		const auto sz = r.MeasureIndicator(items, s);
		r.DrawIndicator(rt.Get(), D2D1::Point2F(pad, y), items, s);
		r.DrawIndicator(rt.Get(), D2D1::Point2F(W - pad - sz.width, y), items, s);
		y += sz.height + 10 * s;
	}

	// Notifications
	const ToastContent toasts[] = {
		{Icon::Replay, L"Replay buffer is on", L"Press Alt + F10 to save the last 5 minutes"},
		{Icon::Record, L"Recording started", L""},
		{Icon::Saved, L"Replay saved", L"Replay 2026-11-25 21-14-03.mkv"},
		{Icon::Saved, L"Recording saved",
		 L"A very long recording file name that will not fit on one line 2026-11-25 21-14-03.mkv"},
		{Icon::Stream, L"You're live", L"Stream started"},
		{Icon::Screenshot, L"Screenshot saved", L"Screenshot 2026-11-25 21-15-44.png"},
	};
	y += 8 * s;
	const float tw = r.ToastWidth(s);
	float ly = y, ry = y;
	int i = 0;
	for (const auto &t : toasts) {
		const uint32_t accent = i == 4 ? 0x0078D4 : 0x76B900;
		if (i % 2 == 0) {
			r.DrawToast(rt.Get(), D2D1::Point2F(pad, ly), t, accent, s);
			ly += r.ToastHeight(t, s) + 8 * s;
		} else {
			r.DrawToast(rt.Get(), D2D1::Point2F(W - pad - tw, ry), t, accent, s);
			ry += r.ToastHeight(t, s) + 8 * s;
		}
		i++;
	}
	y = std::max(ly, ry) + 8 * s;

	// Icon sheet at a large size
	const Icon icons[] = {Icon::Record, Icon::Pause,      Icon::Stop,       Icon::Saved, Icon::Stream, Icon::Replay,
			      Icon::Screenshot, Icon::VirtualCam, Icon::Mic, Icon::MicMuted, Icon::Info};
	float x = pad;
	for (Icon ic : icons) {
		r.DrawIcon(rt.Get(), ic, x, y, 48 * s, Rgb(0xFFFFFF));
		r.DrawIcon(rt.Get(), ic, x, y + 56 * s, 24 * s, Rgb(0x111111));
		x += 64 * s;
	}

	if (FAILED(rt->EndDraw())) {
		fprintf(stderr, "EndDraw failed\n");
		return 1;
	}
	if (!tools::SavePng(bmp.Get(), out)) {
		fprintf(stderr, "saving PNG failed\n");
		return 1;
	}
	wprintf(L"wrote %ls (%ux%u)\n", out.c_str(), W, H);
	return 0;
}
