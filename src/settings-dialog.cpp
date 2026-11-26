// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings-dialog.hpp"
#include "monitors.hpp"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>

#include <cwchar>

namespace indicator {

std::wstring Utf8ToWide(const char *s)
{
	if (!s || !*s)
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
	if (n <= 1)
		return {};
	std::wstring out(n - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
	return out;
}

std::string WideToUtf8(const std::wstring &s)
{
	if (s.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
	return out;
}

namespace {

struct DlgState {
	Settings original;
	const DialogContext *ctx = nullptr;
	std::vector<std::string> mic_names;        // combo index -> Settings::mic_source
	std::vector<std::wstring> monitor_devices; // (combo index - 2) -> device name
	uint32_t accent = 0;
	HBRUSH swatch = nullptr;
	Settings result;
};

constexpr const wchar_t *kCorners[] = {L"Top left", L"Top right", L"Bottom left", L"Bottom right"};
constexpr const wchar_t *kMicModes[] = {L"Off", L"Show when muted", L"Always show"};

// Activates the Common Controls v6 manifest embedded in `module` (resource #2,
// as for any DLL) so the dialog gets visual styles even if the host exe does
// not opt in. Harmless no-op for an exe, whose manifest is already active.
class ActivationScope {
public:
	explicit ActivationScope(HINSTANCE module)
	{
		ACTCTXW ac{sizeof(ac)};
		ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
		ac.hModule = module;
		ac.lpResourceName = MAKEINTRESOURCEW(2);
		ctx_ = CreateActCtxW(&ac);
		if (ctx_ != INVALID_HANDLE_VALUE)
			active_ = ActivateActCtx(ctx_, &cookie_) != FALSE;
	}
	~ActivationScope()
	{
		if (active_)
			DeactivateActCtx(0, cookie_);
		if (ctx_ != INVALID_HANDLE_VALUE)
			ReleaseActCtx(ctx_);
	}
	ActivationScope(const ActivationScope &) = delete;
	ActivationScope &operator=(const ActivationScope &) = delete;

private:
	HANDLE ctx_ = INVALID_HANDLE_VALUE;
	ULONG_PTR cookie_ = 0;
	bool active_ = false;
};

void Check(HWND d, int id, bool v)
{
	CheckDlgButton(d, id, v ? BST_CHECKED : BST_UNCHECKED);
}

bool Checked(HWND d, int id)
{
	return IsDlgButtonChecked(d, id) == BST_CHECKED;
}

void AddItem(HWND d, int id, const wchar_t *text)
{
	SendDlgItemMessageW(d, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

int Selection(HWND d, int id)
{
	return (int)SendDlgItemMessageW(d, id, CB_GETCURSEL, 0, 0);
}

void Select(HWND d, int id, int index)
{
	SendDlgItemMessageW(d, id, CB_SETCURSEL, (WPARAM)index, 0);
}

void SetSpin(HWND d, int spin, int lo, int hi, int value)
{
	SendDlgItemMessageW(d, spin, UDM_SETRANGE32, (WPARAM)lo, (LPARAM)hi);
	SendDlgItemMessageW(d, spin, UDM_SETPOS32, 0, (LPARAM)value);
}

int ReadInt(HWND d, int edit, int fallback)
{
	BOOL ok = FALSE;
	const UINT v = GetDlgItemInt(d, edit, &ok, FALSE);
	return ok ? (int)v : fallback;
}

COLORREF ToColorRef(uint32_t rgb)
{
	return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

uint32_t FromColorRef(COLORREF c)
{
	return ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c);
}

void SetAccent(HWND d, DlgState &st, uint32_t rgb)
{
	st.accent = rgb;
	if (st.swatch)
		DeleteObject(st.swatch);
	st.swatch = CreateSolidBrush(ToColorRef(rgb));
	InvalidateRect(GetDlgItem(d, IDC_ACCENT_SWATCH), nullptr, TRUE);
}

void UpdateEnabled(HWND d)
{
	const bool en = Checked(d, IDC_ENABLED);
	const bool ind = en && Checked(d, IDC_IND_SHOW);
	const bool toast = en && Checked(d, IDC_TOAST_SHOW);
	const bool mic_ind = ind && Selection(d, IDC_MIC_MODE) > 0;
	const bool mic_toast = toast && Checked(d, IDC_N_MIC);

	auto enable = [d](std::initializer_list<int> ids, bool on) {
		for (int id : ids)
			EnableWindow(GetDlgItem(d, id), on);
	};
	enable({IDC_IND_SHOW, IDC_TOAST_SHOW, IDC_MONITOR, IDC_SCALE, IDC_SCALE_SPIN, IDC_OPACITY, IDC_OPACITY_SPIN,
		IDC_MARGIN, IDC_MARGIN_SPIN, IDC_ACCENT_PICK, IDC_HIDE_CAPTURE, IDC_PREVIEW},
	       en);
	enable({IDC_IND_CORNER, IDC_IND_CORNER_LABEL, IDC_IND_LABEL_ICONS, IDC_IND_REC, IDC_IND_STREAM,
		IDC_IND_REPLAY, IDC_IND_VCAM, IDC_IND_TIMER, IDC_MIC_LABEL, IDC_MIC_MODE},
	       ind);
	enable({IDC_MIC_SOURCE, IDC_MIC_SOURCE_LABEL}, mic_ind || mic_toast);
	enable({IDC_TOAST_CORNER, IDC_TOAST_CORNER_LABEL, IDC_TOAST_LABEL_ABOUT, IDC_N_REC, IDC_N_STREAM,
		IDC_N_REPLAY, IDC_N_SHOT, IDC_N_VCAM, IDC_N_MIC, IDC_TOAST_LABEL_FOR, IDC_TOAST_SECS,
		IDC_TOAST_SECS_SPIN, IDC_TOAST_LABEL_SECS},
	       toast);
}

void InitControls(HWND d, DlgState &st)
{
	const Settings &s = st.original;
	Check(d, IDC_ENABLED, s.enabled);

	// Status indicator
	Check(d, IDC_IND_SHOW, s.show_indicator);
	for (auto *c : kCorners)
		AddItem(d, IDC_IND_CORNER, c);
	Select(d, IDC_IND_CORNER, (int)s.indicator_corner);
	Check(d, IDC_IND_REC, s.ind_recording);
	Check(d, IDC_IND_STREAM, s.ind_streaming);
	Check(d, IDC_IND_REPLAY, s.ind_replay);
	Check(d, IDC_IND_VCAM, s.ind_virtualcam);
	Check(d, IDC_IND_TIMER, s.show_timer);
	for (auto *m : kMicModes)
		AddItem(d, IDC_MIC_MODE, m);
	Select(d, IDC_MIC_MODE, (int)s.mic_mode);

	st.mic_names.clear();
	st.mic_names.push_back({});
	AddItem(d, IDC_MIC_SOURCE, st.ctx->default_mic_label.c_str());
	int mic_sel = 0;
	for (const auto &src : st.ctx->audio_sources) {
		if (src.name.empty())
			continue;
		if (src.name == s.mic_source)
			mic_sel = (int)st.mic_names.size();
		st.mic_names.push_back(src.name);
		AddItem(d, IDC_MIC_SOURCE, Utf8ToWide(src.name.c_str()).c_str());
	}
	if (!s.mic_source.empty() && mic_sel == 0) {
		// Keep a configured source that doesn't exist right now (other scene collection).
		mic_sel = (int)st.mic_names.size();
		st.mic_names.push_back(s.mic_source);
		AddItem(d, IDC_MIC_SOURCE, (Utf8ToWide(s.mic_source.c_str()) + L" (not found)").c_str());
	}
	Select(d, IDC_MIC_SOURCE, mic_sel);

	// Notifications
	Check(d, IDC_TOAST_SHOW, s.notifications);
	for (auto *c : kCorners)
		AddItem(d, IDC_TOAST_CORNER, c);
	Select(d, IDC_TOAST_CORNER, (int)s.toast_corner);
	Check(d, IDC_N_REC, s.notify_recording);
	Check(d, IDC_N_STREAM, s.notify_streaming);
	Check(d, IDC_N_REPLAY, s.notify_replay);
	Check(d, IDC_N_SHOT, s.notify_screenshot);
	Check(d, IDC_N_VCAM, s.notify_virtualcam);
	Check(d, IDC_N_MIC, s.notify_mic);
	SetSpin(d, IDC_TOAST_SECS_SPIN, 1, 30, s.toast_seconds);

	// Appearance
	AddItem(d, IDC_MONITOR, L"Primary display");
	AddItem(d, IDC_MONITOR, L"Display with the active window (follows your game)");
	int mon_sel = s.monitor_mode == MonitorMode::FollowForeground ? 1 : 0;
	for (const auto &m : EnumerateMonitors()) {
		std::wstring label = m.device;
		if (label.rfind(L"\\\\.\\", 0) == 0)
			label.erase(0, 4);
		if (!m.friendly_name.empty())
			label += L": " + m.friendly_name;
		wchar_t res[64];
		swprintf(res, 64, L" (%ld×%ld%s)", m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
			 m.primary ? L", primary" : L"");
		label += res;
		if (s.monitor_mode == MonitorMode::Specific && _wcsicmp(m.device.c_str(), s.monitor_device.c_str()) == 0)
			mon_sel = 2 + (int)st.monitor_devices.size();
		st.monitor_devices.push_back(m.device);
		AddItem(d, IDC_MONITOR, label.c_str());
	}
	if (s.monitor_mode == MonitorMode::Specific && mon_sel == 0 && !s.monitor_device.empty()) {
		mon_sel = 2 + (int)st.monitor_devices.size();
		st.monitor_devices.push_back(s.monitor_device);
		AddItem(d, IDC_MONITOR, (s.monitor_device + L" (disconnected)").c_str());
	}
	Select(d, IDC_MONITOR, mon_sel);

	SetSpin(d, IDC_SCALE_SPIN, 50, 300, s.scale_percent);
	SetSpin(d, IDC_OPACITY_SPIN, 20, 100, s.opacity_percent);
	SetSpin(d, IDC_MARGIN_SPIN, 0, 400, s.margin);
	SetAccent(d, st, s.accent);
	Check(d, IDC_HIDE_CAPTURE, s.hide_from_capture);

	UpdateEnabled(d);
}

Settings ReadControls(HWND d, const DlgState &st)
{
	Settings s = st.original;
	s.enabled = Checked(d, IDC_ENABLED);

	s.show_indicator = Checked(d, IDC_IND_SHOW);
	s.indicator_corner = (Corner)Selection(d, IDC_IND_CORNER);
	s.ind_recording = Checked(d, IDC_IND_REC);
	s.ind_streaming = Checked(d, IDC_IND_STREAM);
	s.ind_replay = Checked(d, IDC_IND_REPLAY);
	s.ind_virtualcam = Checked(d, IDC_IND_VCAM);
	s.show_timer = Checked(d, IDC_IND_TIMER);
	s.mic_mode = (MicMode)Selection(d, IDC_MIC_MODE);
	const int mic = Selection(d, IDC_MIC_SOURCE);
	if (mic >= 0 && mic < (int)st.mic_names.size())
		s.mic_source = st.mic_names[mic];

	s.notifications = Checked(d, IDC_TOAST_SHOW);
	s.toast_corner = (Corner)Selection(d, IDC_TOAST_CORNER);
	s.notify_recording = Checked(d, IDC_N_REC);
	s.notify_streaming = Checked(d, IDC_N_STREAM);
	s.notify_replay = Checked(d, IDC_N_REPLAY);
	s.notify_screenshot = Checked(d, IDC_N_SHOT);
	s.notify_virtualcam = Checked(d, IDC_N_VCAM);
	s.notify_mic = Checked(d, IDC_N_MIC);
	s.toast_seconds = ReadInt(d, IDC_TOAST_SECS, s.toast_seconds);

	const int mon = Selection(d, IDC_MONITOR);
	if (mon <= 0) {
		s.monitor_mode = MonitorMode::Primary;
	} else if (mon == 1) {
		s.monitor_mode = MonitorMode::FollowForeground;
	} else if (mon - 2 < (int)st.monitor_devices.size()) {
		s.monitor_mode = MonitorMode::Specific;
		s.monitor_device = st.monitor_devices[mon - 2];
	}
	s.scale_percent = ReadInt(d, IDC_SCALE, s.scale_percent);
	s.opacity_percent = ReadInt(d, IDC_OPACITY, s.opacity_percent);
	s.margin = ReadInt(d, IDC_MARGIN, s.margin);
	s.accent = st.accent;
	s.hide_from_capture = Checked(d, IDC_HIDE_CAPTURE);
	s.Clamp();
	return s;
}

void PickColor(HWND d, DlgState &st)
{
	static COLORREF custom[16] = {RGB(0x76, 0xB9, 0x00), RGB(0x00, 0x78, 0xD4), RGB(0xE8, 0x11, 0x23),
				      RGB(0xFF, 0x8C, 0x00), RGB(0x88, 0x17, 0x98), RGB(0x00, 0xB7, 0xC3)};
	CHOOSECOLORW cc{sizeof(cc)};
	cc.hwndOwner = d;
	cc.rgbResult = ToColorRef(st.accent);
	cc.lpCustColors = custom;
	cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
	if (ChooseColorW(&cc))
		SetAccent(d, st, FromColorRef(cc.rgbResult));
}

INT_PTR CALLBACK DlgProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
	auto *st = reinterpret_cast<DlgState *>(GetWindowLongPtrW(d, DWLP_USER));
	switch (msg) {
	case WM_INITDIALOG:
		st = reinterpret_cast<DlgState *>(lp);
		SetWindowLongPtrW(d, DWLP_USER, lp);
		InitControls(d, *st);
		return TRUE;

	case WM_COMMAND: {
		if (!st)
			break;
		const int id = LOWORD(wp);
		const int code = HIWORD(wp);
		switch (id) {
		case IDOK:
			st->result = ReadControls(d, *st);
			EndDialog(d, IDOK);
			return TRUE;
		case IDCANCEL:
			EndDialog(d, IDCANCEL);
			return TRUE;
		case IDC_PREVIEW:
			if (st->ctx->preview)
				st->ctx->preview(ReadControls(d, *st));
			return TRUE;
		case IDC_ACCENT_PICK:
			PickColor(d, *st);
			return TRUE;
		case IDC_ENABLED:
		case IDC_IND_SHOW:
		case IDC_TOAST_SHOW:
		case IDC_N_MIC:
			if (code == BN_CLICKED)
				UpdateEnabled(d);
			return TRUE;
		case IDC_MIC_MODE:
			if (code == CBN_SELCHANGE)
				UpdateEnabled(d);
			return TRUE;
		}
		break;
	}

	case WM_CTLCOLORSTATIC:
		if (st && st->swatch && reinterpret_cast<HWND>(lp) == GetDlgItem(d, IDC_ACCENT_SWATCH))
			return reinterpret_cast<INT_PTR>(st->swatch);
		break;

	case WM_DESTROY:
		if (st && st->swatch) {
			DeleteObject(st->swatch);
			st->swatch = nullptr;
		}
		break;
	}
	return FALSE;
}

} // namespace

bool ShowSettingsDialog(HWND parent, HINSTANCE resources, Settings &settings, const DialogContext &ctx)
{
	ActivationScope scope(resources);
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS};
	InitCommonControlsEx(&icc);

	DlgState st;
	st.original = settings;
	st.ctx = &ctx;
	const INT_PTR r = DialogBoxParamW(resources, MAKEINTRESOURCEW(IDD_SETTINGS), parent, DlgProc,
					  reinterpret_cast<LPARAM>(&st));
	if (r != IDOK)
		return false;
	settings = st.result;
	return true;
}

} // namespace indicator
