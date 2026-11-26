// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings.hpp"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace indicator {

struct AudioSourceChoice {
	std::string name; // UTF-8 OBS source name, as stored in Settings::mic_source
};

struct DialogContext {
	std::vector<AudioSourceChoice> audio_sources;
	std::wstring default_mic_label = L"Default (Mic/Aux)";
	// Called with the dialog's current values when "Preview" is pressed. The "show over fullscreen
	// games" choices are left at their saved values, so a preview never injects any helper.
	std::function<void(const Settings &)> preview;
	// Translated text for a "Settings.*" key from data/locale, or empty to keep the built-in English.
	std::function<std::wstring(const char *key)> text;
};

// Modal settings dialog. `resources` is the module holding IDD_SETTINGS.
// Returns true (and updates `settings`) when the user presses OK.
bool ShowSettingsDialog(HWND parent, HINSTANCE resources, Settings &settings, const DialogContext &ctx);

std::wstring Utf8ToWide(const char *s);
std::string WideToUtf8(const std::wstring &s);

} // namespace indicator
