// SPDX-License-Identifier: GPL-2.0-or-later
#include "renderer.hpp"

#include <cmath>
#include <numbers>

namespace indicator {

namespace {

// Indicator ("status pill") metrics at scale 1.0
constexpr float kPillHeight = 32.0f;
constexpr float kPillPadX = 10.0f;
constexpr float kPillIcon = 18.0f;
constexpr float kPillGap = 10.0f;
constexpr float kPillLabelGap = 5.0f;
constexpr float kPillRadius = 7.0f;
constexpr float kPillLabelSize = 13.0f;

// Notification ("toast") metrics at scale 1.0
constexpr float kToastWidth = 340.0f;
constexpr float kToastAccent = 4.0f;
constexpr float kToastIconCol = 54.0f;
constexpr float kToastIcon = 24.0f;
constexpr float kToastTextPadR = 16.0f;
constexpr float kToastRadius = 3.0f;
constexpr float kToastPadY = 13.0f;
constexpr float kToastTitleLine = 20.0f;
constexpr float kToastSubLine = 17.0f;
constexpr float kToastSingleHeight = 48.0f;
constexpr float kToastTitleSize = 14.5f;
constexpr float kToastSubSize = 12.5f;

constexpr wchar_t kFont[] = L"Segoe UI";

D2D1_POINT_2F OnCircle(float cx, float cy, float r, float deg)
{
	const float a = deg * std::numbers::pi_v<float> / 180.0f;
	return D2D1::Point2F(cx + r * std::cos(a), cy + r * std::sin(a));
}

// Open arc from angle a0 to a1 (degrees, 0 = 3 o'clock, clockwise on screen).
void AddArcFigure(ID2D1GeometrySink *sink, float cx, float cy, float r, float a0, float a1)
{
	sink->BeginFigure(OnCircle(cx, cy, r, a0), D2D1_FIGURE_BEGIN_HOLLOW);
	const float sweep = a1 - a0;
	sink->AddArc(D2D1::ArcSegment(OnCircle(cx, cy, r, a1), D2D1::SizeF(r, r), 0.0f,
				      sweep >= 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE
						 : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
				      std::fabs(sweep) > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
}

void AddPolyline(ID2D1GeometrySink *sink, std::initializer_list<D2D1_POINT_2F> pts, bool closed)
{
	auto it = pts.begin();
	sink->BeginFigure(*it, closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
	for (++it; it != pts.end(); ++it)
		sink->AddLine(*it);
	sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
}

template<typename Fn> ComPtr<ID2D1PathGeometry> MakePath(ID2D1Factory *f, Fn &&build)
{
	ComPtr<ID2D1PathGeometry> geo;
	if (FAILED(f->CreatePathGeometry(&geo)))
		return nullptr;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink)))
		return nullptr;
	build(sink.Get());
	if (FAILED(sink->Close()))
		return nullptr;
	return geo;
}

ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory *dw, DWRITE_FONT_WEIGHT weight, float size)
{
	ComPtr<IDWriteTextFormat> fmt;
	if (FAILED(dw->CreateTextFormat(kFont, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
					size, L"", &fmt)))
		return nullptr;
	fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
	fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	return fmt;
}

void SetEllipsis(IDWriteFactory *dw, IDWriteTextFormat *fmt, ComPtr<IDWriteInlineObject> &sign)
{
	if (FAILED(dw->CreateEllipsisTrimmingSign(fmt, &sign)))
		return;
	DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
	fmt->SetTrimming(&trim, sign.Get());
}

} // namespace

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha)
{
	return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

bool Renderer::Init()
{
	if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf())))
		return false;
	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
				       reinterpret_cast<IUnknown **>(dwrite_.GetAddressOf()))))
		return false;
	const auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
						       D2D1_LINE_JOIN_ROUND);
	if (FAILED(d2d_->CreateStrokeStyle(props, nullptr, 0, &round_)))
		return false;
	if (SUCCEEDED(dwrite_->CreateTypography(&tabular_)))
		tabular_->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
	return true;
}

bool Renderer::EnsureFonts(float s)
{
	if (fonts_.scale == s && fonts_.label)
		return true;
	Fonts f;
	f.scale = s;
	f.label = MakeFormat(dwrite_.Get(), DWRITE_FONT_WEIGHT_SEMI_BOLD, kPillLabelSize * s);
	f.title = MakeFormat(dwrite_.Get(), DWRITE_FONT_WEIGHT_SEMI_BOLD, kToastTitleSize * s);
	f.subtitle = MakeFormat(dwrite_.Get(), DWRITE_FONT_WEIGHT_NORMAL, kToastSubSize * s);
	if (!f.label || !f.title || !f.subtitle)
		return false;
	SetEllipsis(dwrite_.Get(), f.title.Get(), f.title_ellipsis);
	SetEllipsis(dwrite_.Get(), f.subtitle.Get(), f.subtitle_ellipsis);
	fonts_ = std::move(f);
	return true;
}

ComPtr<IDWriteTextLayout> Renderer::LabelLayout(const std::wstring &text, float s, float height)
{
	if (!EnsureFonts(s))
		return nullptr;
	ComPtr<IDWriteTextLayout> layout;
	if (FAILED(dwrite_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fonts_.label.Get(), 10000.0f, height,
					     &layout)))
		return nullptr;
	if (tabular_)
		layout->SetTypography(tabular_.Get(), {0, (UINT32)text.size()});
	return layout;
}

// ---------------------------------------------------------------------------
// Status indicator

D2D1_SIZE_F Renderer::MeasureIndicator(const std::vector<IndicatorItem> &items, float s)
{
	if (items.empty())
		return D2D1::SizeF(0, 0);
	float w = kPillPadX * 2 * s;
	for (size_t i = 0; i < items.size(); i++) {
		if (i)
			w += kPillGap * s;
		w += kPillIcon * s;
		if (!items[i].text.empty()) {
			DWRITE_TEXT_METRICS m{};
			auto layout = LabelLayout(items[i].text, s, kPillHeight * s);
			if (layout && SUCCEEDED(layout->GetMetrics(&m)))
				w += kPillLabelGap * s + std::ceil(m.widthIncludingTrailingWhitespace);
		}
	}
	return D2D1::SizeF(std::ceil(w), std::ceil(kPillHeight * s));
}

void Renderer::DrawIndicator(ID2D1RenderTarget *rt, D2D1_POINT_2F o, const std::vector<IndicatorItem> &items, float s)
{
	if (items.empty())
		return;
	const D2D1_SIZE_F size = MeasureIndicator(items, s);
	const float h = kPillHeight * s;

	ComPtr<ID2D1SolidColorBrush> brush;
	if (FAILED(rt->CreateSolidColorBrush(Rgb(0x000000, 0.55f), &brush)))
		return;

	const float inset = 0.5f;
	const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
		D2D1::RectF(o.x + inset, o.y + inset, o.x + size.width - inset, o.y + h - inset), kPillRadius * s,
		kPillRadius * s);
	rt->FillRoundedRectangle(pill, brush.Get());
	brush->SetColor(Rgb(0xFFFFFF, 0.10f));
	rt->DrawRoundedRectangle(pill, brush.Get(), 1.0f);

	float x = o.x + kPillPadX * s;
	const float icon = kPillIcon * s;
	for (size_t i = 0; i < items.size(); i++) {
		if (i)
			x += kPillGap * s;
		const auto &it = items[i];
		DrawIcon(rt, it.icon, x, o.y + (h - icon) / 2, icon, Rgb(it.color));
		x += icon;
		if (!it.text.empty()) {
			x += kPillLabelGap * s;
			auto layout = LabelLayout(it.text, s, h);
			if (layout) {
				DWRITE_TEXT_METRICS m{};
				layout->GetMetrics(&m);
				brush->SetColor(Rgb(0xFFFFFF, 0.95f));
				rt->DrawTextLayout(D2D1::Point2F(x, o.y), layout.Get(), brush.Get());
				x += std::ceil(m.widthIncludingTrailingWhitespace);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Notifications

float Renderer::ToastWidth(float s) const
{
	return std::ceil(kToastWidth * s);
}

float Renderer::ToastHeight(const ToastContent &t, float s) const
{
	const float h = t.subtitle.empty() ? kToastSingleHeight : kToastPadY * 2 + kToastTitleLine + kToastSubLine;
	return std::ceil(h * s);
}

void Renderer::DrawToast(ID2D1RenderTarget *rt, D2D1_POINT_2F o, const ToastContent &t, uint32_t accent, float s)
{
	if (!EnsureFonts(s))
		return;
	const float w = ToastWidth(s);
	const float h = ToastHeight(t, s);

	ComPtr<ID2D1SolidColorBrush> brush;
	if (FAILED(rt->CreateSolidColorBrush(Rgb(0x1B1B1B, 0.95f), &brush)))
		return;

	const D2D1_RECT_F box = D2D1::RectF(o.x, o.y, o.x + w, o.y + h);
	const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(box, kToastRadius * s, kToastRadius * s);
	rt->FillRoundedRectangle(rr, brush.Get());

	// Accent bar on the leading edge, clipped to the rounded background.
	ComPtr<ID2D1RoundedRectangleGeometry> clip;
	if (SUCCEEDED(Factory()->CreateRoundedRectangleGeometry(rr, &clip))) {
		ComPtr<ID2D1Layer> layer;
		if (SUCCEEDED(rt->CreateLayer(&layer))) {
			rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
			brush->SetColor(Rgb(accent));
			rt->FillRectangle(D2D1::RectF(o.x, o.y, o.x + kToastAccent * s, o.y + h), brush.Get());
			rt->PopLayer();
		}
	}

	const D2D1_ROUNDED_RECT border = D2D1::RoundedRect(D2D1::RectF(box.left + 0.5f, box.top + 0.5f,
								       box.right - 0.5f, box.bottom - 0.5f),
							   kToastRadius * s, kToastRadius * s);
	brush->SetColor(Rgb(0xFFFFFF, 0.08f));
	rt->DrawRoundedRectangle(border, brush.Get(), 1.0f);

	const float icon = kToastIcon * s;
	const float icon_x = o.x + kToastAccent * s + (kToastIconCol * s - icon) / 2;
	DrawIcon(rt, t.icon, icon_x, o.y + (h - icon) / 2, icon, Rgb(accent));

	const float text_x = o.x + (kToastAccent + kToastIconCol) * s;
	const float text_r = o.x + w - kToastTextPadR * s;
	brush->SetColor(Rgb(0xFFFFFF));
	if (t.subtitle.empty()) {
		rt->DrawText(t.title.c_str(), (UINT32)t.title.size(), fonts_.title.Get(),
			     D2D1::RectF(text_x, o.y, text_r, o.y + h), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
		return;
	}
	const float title_y = o.y + kToastPadY * s;
	const float sub_y = title_y + kToastTitleLine * s;
	rt->DrawText(t.title.c_str(), (UINT32)t.title.size(), fonts_.title.Get(),
		     D2D1::RectF(text_x, title_y, text_r, sub_y), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
	brush->SetColor(Rgb(0xB4B4B4));
	rt->DrawText(t.subtitle.c_str(), (UINT32)t.subtitle.size(), fonts_.subtitle.Get(),
		     D2D1::RectF(text_x, sub_y, text_r, sub_y + kToastSubLine * s), brush.Get(),
		     D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// ---------------------------------------------------------------------------
// Icons, drawn on a 24x24 grid

void Renderer::DrawIcon(ID2D1RenderTarget *rt, Icon icon, float x, float y, float size, D2D1_COLOR_F color)
{
	ComPtr<ID2D1SolidColorBrush> b;
	if (FAILED(rt->CreateSolidColorBrush(color, &b)))
		return;

	D2D1_MATRIX_3X2_F saved;
	rt->GetTransform(&saved);
	rt->SetTransform(D2D1::Matrix3x2F::Scale(size / 24.0f, size / 24.0f) * D2D1::Matrix3x2F::Translation(x, y) *
			 saved);

	ID2D1Factory *f = Factory();
	ID2D1StrokeStyle *rs = round_.Get();
	const float sw = 2.0f; // stroke width in grid units

	auto stroke = [&](ComPtr<ID2D1PathGeometry> g, float width) {
		if (g)
			rt->DrawGeometry(g.Get(), b.Get(), width, rs);
	};
	auto mic = [&]() {
		rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(9.0f, 2.5f, 15.0f, 14.5f), 3.0f, 3.0f), b.Get());
		stroke(MakePath(f,
				[](ID2D1GeometrySink *k) {
					AddArcFigure(k, 12, 11, 6.0f, 180.0f, 90.0f);
					AddArcFigure(k, 12, 11, 6.0f, 90.0f, 0.0f);
					AddPolyline(k, {{12, 17}, {12, 21}}, false);
					AddPolyline(k, {{8.5f, 21}, {15.5f, 21}}, false);
				}),
		       1.9f);
	};

	switch (icon) {
	case Icon::Record:
		rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12, 12), 9.0f, 9.0f), b.Get(), sw);
		rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(12, 12), 5.0f, 5.0f), b.Get());
		break;

	case Icon::Pause:
		rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(6.5f, 5, 10.0f, 19), 1, 1), b.Get());
		rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(14.0f, 5, 17.5f, 19), 1, 1), b.Get());
		break;

	case Icon::Stop:
		rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(6, 6, 18, 18), 2, 2), b.Get());
		break;

	case Icon::Saved:
		rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12, 12), 9.5f, 9.5f), b.Get(), sw);
		stroke(MakePath(f,
				[](ID2D1GeometrySink *k) {
					AddPolyline(k, {{7.6f, 12.4f}, {10.6f, 15.3f}, {16.4f, 9.3f}}, false);
				}),
		       2.2f);
		break;

	case Icon::Stream:
		rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(12, 12), 2.4f, 2.4f), b.Get());
		stroke(MakePath(f,
				[](ID2D1GeometrySink *k) {
					AddArcFigure(k, 12, 12, 5.6f, -42, 42);
					AddArcFigure(k, 12, 12, 5.6f, 138, 222);
					AddArcFigure(k, 12, 12, 9.6f, -44, 44);
					AddArcFigure(k, 12, 12, 9.6f, 136, 224);
				}),
		       sw);
		break;

	case Icon::Replay: {
		// Counter-clockwise arrow around clock hands ("rewind time").
		constexpr float r = 8.6f, a0 = 213.0f, a1 = 500.0f;
		stroke(MakePath(f,
				[](ID2D1GeometrySink *k) {
					AddArcFigure(k, 12, 12.4f, r, a0, 360.0f);
					AddArcFigure(k, 12, 12.4f, r, 0.0f, a1 - 360.0f);
					AddPolyline(k, {{12, 8.0f}, {12, 12.4f}, {15.0f, 14.4f}}, false);
				}),
		       sw);
		// Arrow head at the start of the arc, pointing along the counter-clockwise tangent.
		const float rad = a0 * std::numbers::pi_v<float> / 180.0f;
		const D2D1_POINT_2F p = OnCircle(12, 12.4f, r, a0);
		const float tx = std::sin(rad), ty = -std::cos(rad); // ccw tangent
		const float nx = std::cos(rad), ny = std::sin(rad);  // radial
		auto head = MakePath(f, [&](ID2D1GeometrySink *k) {
			AddPolyline(k,
				    {{p.x + tx * 3.6f, p.y + ty * 3.6f},
				     {p.x + nx * 3.4f - tx * 0.6f, p.y + ny * 3.4f - ty * 0.6f},
				     {p.x - nx * 3.4f - tx * 0.6f, p.y - ny * 3.4f - ty * 0.6f}},
				    true);
		});
		if (head) {
			rt->FillGeometry(head.Get(), b.Get());
			rt->DrawGeometry(head.Get(), b.Get(), 0.8f, rs);
		}
		break;
	}

	case Icon::Screenshot:
		rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3, 7.5f, 21, 19.5f), 2.5f, 2.5f), b.Get(), 1.9f,
					 rs);
		stroke(MakePath(f,
				[](ID2D1GeometrySink *k) {
					AddPolyline(k, {{8.4f, 7.5f}, {9.8f, 4.8f}, {14.2f, 4.8f}, {15.6f, 7.5f}},
						    false);
				}),
		       1.9f);
		rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12, 13.4f), 3.5f, 3.5f), b.Get(), 1.9f);
		break;

	case Icon::VirtualCam:
		rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2.5f, 6.5f, 15.5f, 17.5f), 2.5f, 2.5f), b.Get(),
					 1.9f, rs);
		stroke(MakePath(f,
				[](ID2D1GeometrySink *k) {
					AddPolyline(k, {{15.5f, 10.5f}, {21.0f, 7.5f}, {21.0f, 16.5f}, {15.5f, 13.5f}},
						    true);
				}),
		       1.9f);
		break;

	case Icon::Mic:
		mic();
		break;

	case Icon::MicMuted: {
		mic();
		ComPtr<ID2D1SolidColorBrush> red;
		if (SUCCEEDED(rt->CreateSolidColorBrush(Rgb(0xFF4A4A), &red))) {
			rt->DrawLine(D2D1::Point2F(4.0f, 3.5f), D2D1::Point2F(20.0f, 20.5f), red.Get(), 2.4f, rs);
		}
		break;
	}

	case Icon::Info:
		rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12, 12), 9.5f, 9.5f), b.Get(), sw);
		rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(12, 7.6f), 1.35f, 1.35f), b.Get());
		rt->DrawLine(D2D1::Point2F(12, 11), D2D1::Point2F(12, 16.8f), b.Get(), 2.2f, rs);
		break;
	}

	rt->SetTransform(saved);
}

} // namespace indicator
