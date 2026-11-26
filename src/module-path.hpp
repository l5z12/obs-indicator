// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <string>

namespace indicator {

// Full path to a file next to `module` (the plugin DLL), or empty if the path can't be read.
inline std::wstring SiblingPath(HMODULE module, const wchar_t *name)
{
	wchar_t buf[MAX_PATH];
	const DWORD n = GetModuleFileNameW(module, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return {};
	std::wstring path(buf, n);
	const size_t slash = path.find_last_of(L"\\/");
	path.resize(slash == std::wstring::npos ? 0 : slash + 1);
	path += name;
	return path;
}

} // namespace indicator
