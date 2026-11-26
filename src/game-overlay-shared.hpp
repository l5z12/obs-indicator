// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared-memory layout between the plugin (inside OBS) and obs-indicator-hook.dll (inside an
// OpenGL game). The plugin publishes the pixels and screen position of each overlay window; the
// hook draws them into the game's frame when the game is fullscreen and in front (where the
// driver skips Windows' compositor, so the real windows can't be seen), and reports that it is
// doing so, so the plugin can hide those windows instead of showing them twice.

#include <windows.h>

#include <cstdint>

namespace indicator::shared {

constexpr wchar_t kMappingName[] = L"Local\\obs-indicator-game-overlay";
constexpr uint32_t kMagic = 0x4949424F; // "OBII"
constexpr uint32_t kVersion = 3;

constexpr int kLayers = 2;                     // the status indicator and the notifications
constexpr uint32_t kMaxLayerBytes = 32u << 20; // 32 MiB: up to 4096 x 2048 BGRA pixels

// Header::flags
constexpr uint32_t kFlagEnabled = 1;   // draw at all (the option is on)
constexpr uint32_t kFlagAnyWindow = 2; // also draw into windowed games (test tools only)

// Hooks stop drawing once the plugin's heartbeat is this old (OBS exited, crashed or hung).
constexpr ULONGLONG kOwnerTimeoutMs = 3000;
// The plugin hides its windows while a game reported drawing within this long.
constexpr ULONGLONG kGameActiveMs = 500;

struct Layer {
	// Seqlock: the plugin makes it odd before changing anything below and even again after.
	volatile LONG seq;
	int32_t visible;
	int32_t x, y, w, h; // screen rectangle, physical pixels
	int32_t alpha;      // 0..255, multiplied with the per-pixel alpha
	uint32_t reserved[8];
};

// A status line from a hook for OBS's log. A hook reserves slot s by incrementing
// Header::hook_msg_seq, sets seq to 0, writes the rest, then sets seq to s + 1.
struct HookMessage {
	volatile LONG seq;
	uint32_t pid;
	char text[120]; // UTF-8, NUL-terminated
};
constexpr int kHookMessages = 8;

struct Header {
	uint32_t magic;
	uint32_t version;
	uint32_t owner_pid; // the OBS process that owns the memory
	volatile LONG flags;
	volatile LONG64 owner_tick; // the plugin's heartbeat: GetTickCount64(), at least 10 times a second
	Layer layers[kLayers];
	// Written by a hook on each frame it can draw (its window fullscreen and in front): the
	// monitor it covers (physical pixels), then the time.
	RECT game_monitor;
	volatile LONG64 game_tick;
	volatile LONG hook_msg_seq; // slots reserved so far; slot s is hook_msgs[s % kHookMessages]
	HookMessage hook_msgs[kHookMessages];
};

// Layer i's pixels: top-down, premultiplied BGRA, w * 4 bytes per row. The plugin commits the
// pages before it publishes a size that needs them, and never decommits.
constexpr size_t kPixelsOffset = 4096;
constexpr size_t kMappingSize = kPixelsOffset + (size_t)kLayers * kMaxLayerBytes;
static_assert(sizeof(Header) <= kPixelsOffset);

inline uint8_t *LayerPixels(void *view, int i)
{
	return static_cast<uint8_t *>(view) + kPixelsOffset + (size_t)i * kMaxLayerBytes;
}

} // namespace indicator::shared
