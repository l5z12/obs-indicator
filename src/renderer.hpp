// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace indicator {

using Microsoft::WRL::ComPtr;

enum class Icon {
	Record,
	Pause,
	Stop,
	Saved,
	Stream,
	Replay,
	Screenshot,
	VirtualCam,
	Mic,
	MicMuted,
	Info,
};

struct IndicatorItem {
	Icon icon;
	uint32_t color;    // 0xRRGGBB
	std::wstring text; // optional label drawn after the icon (e.g. elapsed time)
};

struct ToastContent {
	Icon icon = Icon::Info;
	std::wstring title;
	std::wstring subtitle;
};

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.0f);

// Stateless drawing of the overlay artwork into any D2D render target
// (a DC render target for the layered windows, a WIC target for previews).
// All sizes are in physical pixels: `s` is the combined DPI x user scale.
// Render targets must use 96 DPI so that one DIP equals one pixel.
class Renderer {
public:
	bool Init();
	ID2D1Factory *Factory() const { return d2d_.Get(); }

	D2D1_SIZE_F MeasureIndicator(const std::vector<IndicatorItem> &items, float s);
	void DrawIndicator(ID2D1RenderTarget *rt, D2D1_POINT_2F origin, const std::vector<IndicatorItem> &items,
			   float s);

	float ToastWidth(float s) const;
	float ToastHeight(const ToastContent &t, float s) const;
	void DrawToast(ID2D1RenderTarget *rt, D2D1_POINT_2F origin, const ToastContent &t, uint32_t accent, float s);

	// Draws `icon` from its 24x24 design grid into the square at (x, y) with side `size`.
	void DrawIcon(ID2D1RenderTarget *rt, Icon icon, float x, float y, float size, D2D1_COLOR_F color);

private:
	struct Fonts {
		float scale = 0;
		ComPtr<IDWriteTextFormat> label, title, subtitle;
		ComPtr<IDWriteInlineObject> title_ellipsis, subtitle_ellipsis;
	};
	bool EnsureFonts(float s);
	ComPtr<IDWriteTextLayout> LabelLayout(const std::wstring &text, float s, float height);

	ComPtr<ID2D1Factory> d2d_;
	ComPtr<IDWriteFactory> dwrite_;
	ComPtr<ID2D1StrokeStyle> round_;
	ComPtr<IDWriteTypography> tabular_;
	Fonts fonts_;
};

} // namespace indicator
