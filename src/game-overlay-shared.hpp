// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared-memory layout between the plugin (inside OBS) and obs-indicator-hook.dll (inside an
// OpenGL game). The plugin publishes the pixels and screen position of each overlay window; the
// hook draws them into the game's frame when the game is fullscreen (where the driver skips
// Windows' compositor, so the real windows can't be seen) and reports back which ones it drew,
// so the plugin can hide those windows instead of showing them twice.

#include <windows.h>

#include <cstdint>

namespace indicator::shared {

constexpr wchar_t kMappingName[] = L"Local\\obs-indicator-game-overlay";
constexpr uint32_t kMagic = 0x4949424F; // "OBII"
constexpr uint32_t kVersion = 1;

constexpr int kLayers = 2;                     // the status indicator and the notifications
constexpr uint32_t kMaxLayerBytes = 32u << 20; // 32 MiB: up to 4096 x 2048 BGRA pixels

// Header::flags
constexpr uint32_t kFlagEnabled = 1;   // draw at all (the option is on)
constexpr uint32_t kFlagAnyWindow = 2; // also draw into windowed games (test tools only)

struct Layer {
	// Seqlock: the plugin makes it odd before changing anything below and even again after.
	volatile LONG seq;
	int32_t visible;
	int32_t x, y, w, h; // screen rectangle, physical pixels
	int32_t alpha;      // 0..255, multiplied with the per-pixel alpha
	// GetTickCount64() of the last game frame that drew this layer; written by the hook.
	volatile LONG64 drawn_tick;
	uint32_t reserved[6];
};

struct Header {
	uint32_t magic;
	uint32_t version;
	uint32_t owner_pid; // the OBS process; the hook stops drawing once it has exited
	volatile LONG flags;
	Layer layers[kLayers];
	// The hook's latest status line for OBS's log (UTF-8). The hook bumps hook_msg_seq after
	// writing it, and owns both fields.
	volatile LONG hook_msg_seq;
	uint32_t hook_pid;
	char hook_msg[248];
};

// Layer i's pixels: top-down, premultiplied BGRA, w * 4 bytes per row.
constexpr size_t kPixelsOffset = 4096;
constexpr size_t kMappingSize = kPixelsOffset + (size_t)kLayers * kMaxLayerBytes;
static_assert(sizeof(Header) <= kPixelsOffset);

inline uint8_t *LayerPixels(void *view, int i)
{
	return static_cast<uint8_t *>(view) + kPixelsOffset + (size_t)i * kMaxLayerBytes;
}

} // namespace indicator::shared
