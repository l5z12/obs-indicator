// SPDX-License-Identifier: GPL-3.0-or-later
//
// OBS glue: turns frontend events into overlay status/notifications and owns
// the persisted settings, the Tools menu entry and the toggle hotkey.
// Everything here runs on the OBS UI thread; signals from other threads are
// bounced onto it with obs_queue_task(OBS_TASK_UI, ...).

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "overlay.hpp"
#include "settings-dialog.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-indicator", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Overlay Indicator";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "ShadowPlay-style on-screen status indicator and notifications for recording, streaming and the replay buffer.";
}

using namespace indicator;

namespace {

constexpr uint32_t kMicAuxChannel = 3; // OBS's global "Mic/Aux" audio device

HMODULE g_module = nullptr;
std::unique_ptr<Overlay> g_overlay;
Settings g_settings;
Status g_status;
obs_hotkey_id g_hotkey = OBS_INVALID_HOTKEY_ID;
obs_weak_source_t *g_mic = nullptr;
bool g_shut_down = false;

// ---------------------------------------------------------------- helpers

const char *Text(const char *key, const char *fallback)
{
	const char *out = nullptr;
	if (obs_module_get_string(key, &out) && out)
		return out;
	return fallback;
}

std::string Replace(std::string s, const char *token, const std::string &value)
{
	const size_t pos = s.find(token);
	if (pos != std::string::npos)
		s.replace(pos, strlen(token), value);
	return s;
}

std::string FileName(char *path_owned)
{
	std::string out;
	if (path_owned) {
		const char *p = path_owned;
		const char *slash = std::max(strrchr(p, '/'), strrchr(p, '\\'));
		out = slash ? slash + 1 : p;
		bfree(path_owned);
	}
	return out;
}

void PushStatus()
{
	if (g_overlay)
		g_overlay->SetStatus(g_status);
}

void Notify(ToastCategory cat, Icon icon, const char *title, const std::string &subtitle = {})
{
	if (!g_overlay)
		return;
	Toast t;
	t.category = cat;
	t.content.icon = icon;
	t.content.title = Utf8ToWide(title);
	t.content.subtitle = Utf8ToWide(subtitle.c_str());
	g_overlay->ShowToast(t);
}

void LogFromOverlay(bool error, const char *msg)
{
	blog(error ? LOG_WARNING : LOG_INFO, "[obs-indicator] %s", msg);
}

// ------------------------------------------------------------ persistence

std::string ConfigPath(const char *file)
{
	char *p = obs_module_config_path(file);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void LoadSettings()
{
	const std::string dir = ConfigPath("");
	if (!dir.empty())
		os_mkdirs(dir.c_str());
	const std::string path = ConfigPath("settings.json");
	obs_data_t *d = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!d)
		d = obs_data_create();

	Settings s;
	auto b = [d](const char *k, bool &v) {
		obs_data_set_default_bool(d, k, v);
		v = obs_data_get_bool(d, k);
	};
	auto i = [d](const char *k, auto &v) {
		obs_data_set_default_int(d, k, (long long)v);
		v = static_cast<std::remove_reference_t<decltype(v)>>(obs_data_get_int(d, k));
	};
	b("enabled", s.enabled);
	b("indicator.show", s.show_indicator);
	i("indicator.corner", s.indicator_corner);
	b("indicator.recording", s.ind_recording);
	b("indicator.streaming", s.ind_streaming);
	b("indicator.replay", s.ind_replay);
	b("indicator.virtualcam", s.ind_virtualcam);
	b("indicator.timer", s.show_timer);
	i("indicator.mic_mode", s.mic_mode);
	s.mic_source = obs_data_get_string(d, "indicator.mic_source");
	b("notify.show", s.notifications);
	i("notify.corner", s.toast_corner);
	i("notify.seconds", s.toast_seconds);
	b("notify.recording", s.notify_recording);
	b("notify.streaming", s.notify_streaming);
	b("notify.replay", s.notify_replay);
	b("notify.screenshot", s.notify_screenshot);
	b("notify.virtualcam", s.notify_virtualcam);
	b("notify.mic", s.notify_mic);
	i("appearance.scale", s.scale_percent);
	i("appearance.opacity", s.opacity_percent);
	i("appearance.margin", s.margin);
	i("appearance.accent", s.accent);
	i("appearance.monitor_mode", s.monitor_mode);
	s.monitor_device = Utf8ToWide(obs_data_get_string(d, "appearance.monitor_device"));
	b("appearance.hide_from_capture", s.hide_from_capture);
	b("appearance.show_over_fullscreen", s.show_over_fullscreen);
	i("appearance.fullscreen_band", s.fullscreen_band);
	s.Clamp();
	g_settings = s;

	if (g_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *hk = obs_data_get_array(d, "hotkey.toggle");
		if (hk) {
			obs_hotkey_load(g_hotkey, hk);
			obs_data_array_release(hk);
		}
	}
	obs_data_release(d);
}

void SaveSettings()
{
	const Settings &s = g_settings;
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "enabled", s.enabled);
	obs_data_set_bool(d, "indicator.show", s.show_indicator);
	obs_data_set_int(d, "indicator.corner", (int)s.indicator_corner);
	obs_data_set_bool(d, "indicator.recording", s.ind_recording);
	obs_data_set_bool(d, "indicator.streaming", s.ind_streaming);
	obs_data_set_bool(d, "indicator.replay", s.ind_replay);
	obs_data_set_bool(d, "indicator.virtualcam", s.ind_virtualcam);
	obs_data_set_bool(d, "indicator.timer", s.show_timer);
	obs_data_set_int(d, "indicator.mic_mode", (int)s.mic_mode);
	obs_data_set_string(d, "indicator.mic_source", s.mic_source.c_str());
	obs_data_set_bool(d, "notify.show", s.notifications);
	obs_data_set_int(d, "notify.corner", (int)s.toast_corner);
	obs_data_set_int(d, "notify.seconds", s.toast_seconds);
	obs_data_set_bool(d, "notify.recording", s.notify_recording);
	obs_data_set_bool(d, "notify.streaming", s.notify_streaming);
	obs_data_set_bool(d, "notify.replay", s.notify_replay);
	obs_data_set_bool(d, "notify.screenshot", s.notify_screenshot);
	obs_data_set_bool(d, "notify.virtualcam", s.notify_virtualcam);
	obs_data_set_bool(d, "notify.mic", s.notify_mic);
	obs_data_set_int(d, "appearance.scale", s.scale_percent);
	obs_data_set_int(d, "appearance.opacity", s.opacity_percent);
	obs_data_set_int(d, "appearance.margin", s.margin);
	obs_data_set_int(d, "appearance.accent", s.accent);
	obs_data_set_int(d, "appearance.monitor_mode", (int)s.monitor_mode);
	obs_data_set_string(d, "appearance.monitor_device", WideToUtf8(s.monitor_device).c_str());
	obs_data_set_bool(d, "appearance.hide_from_capture", s.hide_from_capture);
	obs_data_set_bool(d, "appearance.show_over_fullscreen", s.show_over_fullscreen);
	obs_data_set_int(d, "appearance.fullscreen_band", s.fullscreen_band);

	if (g_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *hk = obs_hotkey_save(g_hotkey);
		obs_data_set_array(d, "hotkey.toggle", hk);
		obs_data_array_release(hk);
	}

	const std::string path = ConfigPath("settings.json");
	if (!obs_data_save_json_safe(d, path.c_str(), "tmp", "bak"))
		blog(LOG_WARNING, "[obs-indicator] Failed to save settings to %s", path.c_str());
	obs_data_release(d);
}

// ------------------------------------------------------------- microphone

void MicMuteTask(void *param)
{
	const bool muted = param != nullptr;
	if (!g_mic || g_status.mic_muted == muted)
		return;
	g_status.mic_muted = muted;
	PushStatus();
	Notify(ToastCategory::Mic, muted ? Icon::MicMuted : Icon::Mic,
	       muted ? Text("Toast.MicMuted", "Microphone muted") : Text("Toast.MicUnmuted", "Microphone on"));
}

void OnMicMute(void *, calldata_t *cd)
{
	const bool muted = calldata_bool(cd, "muted");
	obs_queue_task(OBS_TASK_UI, MicMuteTask, muted ? reinterpret_cast<void *>(1) : nullptr, false);
}

void AttachMic();
void DetachMic();

void MicGoneTask(void *)
{
	DetachMic();
	PushStatus();
}

void OnMicRemoved(void *, calldata_t *)
{
	obs_queue_task(OBS_TASK_UI, MicGoneTask, nullptr, false);
}

void MicRenamedTask(void *param)
{
	char *name = static_cast<char *>(param);
	if (!g_shut_down && !g_settings.mic_source.empty() && name) {
		g_settings.mic_source = name;
		SaveSettings();
	}
	bfree(name);
}

void OnMicRenamed(void *, calldata_t *cd)
{
	const char *name = calldata_string(cd, "new_name");
	obs_queue_task(OBS_TASK_UI, MicRenamedTask, name ? bstrdup(name) : nullptr, false);
}

void ConnectMicSignals(obs_source_t *src, bool connect)
{
	signal_handler_t *sh = obs_source_get_signal_handler(src);
	if (!sh)
		return;
	auto op = connect ? signal_handler_connect : signal_handler_disconnect;
	op(sh, "mute", OnMicMute, nullptr);
	op(sh, "remove", OnMicRemoved, nullptr);
	op(sh, "destroy", OnMicRemoved, nullptr);
	op(sh, "rename", OnMicRenamed, nullptr);
}

void DetachMic()
{
	if (g_mic) {
		if (obs_source_t *src = obs_weak_source_get_source(g_mic)) {
			ConnectMicSignals(src, false);
			obs_source_release(src);
		}
		obs_weak_source_release(g_mic);
		g_mic = nullptr;
	}
	g_status.mic_present = false;
	g_status.mic_muted = false;
}

void AttachMic()
{
	DetachMic();
	if (g_settings.mic_mode != MicMode::Off || g_settings.notify_mic) {
		obs_source_t *src = g_settings.mic_source.empty() ? obs_get_output_source(kMicAuxChannel)
								  : obs_get_source_by_name(g_settings.mic_source.c_str());
		if (src) {
			ConnectMicSignals(src, true);
			g_mic = obs_source_get_weak_source(src);
			g_status.mic_present = true;
			g_status.mic_muted = obs_source_muted(src);
			obs_source_release(src);
		}
	}
	PushStatus();
}

// A configured mic that appears later (scene collection load, source re-created).
// Signals arrive on arbitrary threads, so all checks of our state happen on the UI thread.
void SourceCreatedTask(void *param)
{
	char *name = static_cast<char *>(param);
	if (!g_shut_down && !g_mic && name && !g_settings.mic_source.empty() && g_settings.mic_source == name)
		AttachMic();
	bfree(name);
}

void OnSourceCreate(void *, calldata_t *cd)
{
	obs_source_t *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!src || obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT ||
	    !(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO))
		return;
	const char *name = obs_source_get_name(src);
	if (name && *name)
		obs_queue_task(OBS_TASK_UI, SourceCreatedTask, bstrdup(name), false);
}

void MicChannelTask(void *)
{
	if (!g_shut_down && g_settings.mic_source.empty())
		AttachMic();
}

void OnChannelChange(void *, calldata_t *cd)
{
	if (calldata_int(cd, "channel") == kMicAuxChannel)
		obs_queue_task(OBS_TASK_UI, MicChannelTask, nullptr, false);
}

// -------------------------------------------------------------- outputs

// "Press Ctrl + F10 to save the last 5 minutes"
std::string ReplayHint()
{
	int64_t secs = 0;
	if (obs_output_t *out = obs_frontend_get_replay_buffer_output()) {
		obs_data_t *s = obs_output_get_settings(out);
		secs = obs_data_get_int(s, "max_time_sec");
		obs_data_release(s);
		obs_output_release(out);
	}

	struct Find {
		obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
		std::string keys;
	} f;
	obs_enum_hotkeys(
		[](void *p, obs_hotkey_id id, obs_hotkey_t *key) {
			const char *name = obs_hotkey_get_name(key);
			if (name && (strcmp(name, "OBSBasic.SaveReplayBuffer") == 0 ||
				     strcmp(name, "ReplayBuffer.Save") == 0)) {
				static_cast<Find *>(p)->id = id;
				return false;
			}
			return true;
		},
		&f);
	if (f.id != OBS_INVALID_HOTKEY_ID) {
		obs_enum_hotkey_bindings(
			[](void *p, size_t, obs_hotkey_binding_t *b) {
				auto *f = static_cast<Find *>(p);
				if (obs_hotkey_binding_get_hotkey_id(b) != f->id)
					return true;
				struct dstr str = {};
				obs_key_combination_to_str(obs_hotkey_binding_get_key_combination(b), &str);
				const bool found = str.array && *str.array;
				if (found)
					f->keys = str.array;
				dstr_free(&str);
				return !found;
			},
			&f);
	}

	std::string length;
	if (secs > 0) {
		const int64_t m = secs / 60, r = secs % 60;
		if (r == 0)
			length = m == 1 ? Text("Duration.OneMinute", "minute")
					: Replace(Text("Duration.Minutes", "%1 minutes"), "%1", std::to_string(m));
		else if (m == 0)
			length = Replace(Text("Duration.Seconds", "%1 seconds"), "%1", std::to_string(r));
		else
			length = Replace(Replace(Text("Duration.MinSec", "%1 min %2 s"), "%1", std::to_string(m)),
					 "%2", std::to_string(r));
	}

	if (!f.keys.empty() && !length.empty())
		return Replace(Replace(Text("Toast.ReplayHintKey", "Press %1 to save the last %2"), "%1", f.keys),
			       "%2", length);
	if (!f.keys.empty())
		return Replace(Text("Toast.ReplayHintKeyOnly", "Press %1 to save a replay"), "%1", f.keys);
	if (!length.empty())
		return Replace(Text("Toast.ReplayHintLength", "Keeping the last %1"), "%1", length);
	return {};
}

void SyncOutputs()
{
	const uint64_t now = GetTickCount64();
	const bool rec = obs_frontend_recording_active();
	if (rec != g_status.recording) {
		g_status.recording = rec;
		g_status.rec_start = rec ? now : 0;
		g_status.rec_paused_total = 0;
	}
	g_status.rec_paused = rec && obs_frontend_recording_paused();
	g_status.rec_pause_start = g_status.rec_paused ? (g_status.rec_pause_start ? g_status.rec_pause_start : now) : 0;
	const bool stream = obs_frontend_streaming_active();
	if (stream != g_status.streaming) {
		g_status.streaming = stream;
		g_status.stream_start = stream ? now : 0;
	}
	g_status.replay = obs_frontend_replay_buffer_active();
	g_status.virtualcam = obs_frontend_virtualcam_active();
	PushStatus();
}

void Shutdown()
{
	if (g_shut_down)
		return;
	g_shut_down = true;

	if (signal_handler_t *sh = obs_get_signal_handler()) {
		signal_handler_disconnect(sh, "source_create", OnSourceCreate, nullptr);
		signal_handler_disconnect(sh, "channel_change", OnChannelChange, nullptr);
	}
	DetachMic();
	if (g_hotkey != OBS_INVALID_HOTKEY_ID) {
		SaveSettings(); // persists the hotkey binding while it is still registered
		obs_hotkey_unregister(g_hotkey);
		g_hotkey = OBS_INVALID_HOTKEY_ID;
	}
	if (g_overlay) {
		g_overlay->Stop();
		g_overlay.reset();
	}
}

void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	if (g_shut_down)
		return;
	const uint64_t now = GetTickCount64();
	Status &st = g_status;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		SyncOutputs();
		AttachMic();
		break;

	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		st.recording = true;
		st.rec_paused = false;
		st.rec_start = now;
		st.rec_paused_total = 0;
		st.rec_pause_start = 0;
		PushStatus();
		Notify(ToastCategory::Recording, Icon::Record, Text("Toast.RecordingStarted", "Recording started"));
		break;

	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: {
		const bool was = st.recording;
		st.recording = false;
		st.rec_paused = false;
		st.rec_start = 0;
		st.rec_paused_total = 0;
		st.rec_pause_start = 0;
		PushStatus();
		if (was)
			Notify(ToastCategory::Recording, Icon::Saved, Text("Toast.RecordingSaved", "Recording saved"),
			       FileName(obs_frontend_get_last_recording()));
		break;
	}

	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		st.rec_paused = true;
		st.rec_pause_start = now;
		PushStatus();
		Notify(ToastCategory::Recording, Icon::Pause, Text("Toast.RecordingPaused", "Recording paused"));
		break;

	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		if (st.rec_pause_start)
			st.rec_paused_total += now - st.rec_pause_start;
		st.rec_pause_start = 0;
		st.rec_paused = false;
		PushStatus();
		Notify(ToastCategory::Recording, Icon::Record, Text("Toast.RecordingResumed", "Recording resumed"));
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		st.streaming = true;
		st.stream_start = now;
		PushStatus();
		Notify(ToastCategory::Streaming, Icon::Stream, Text("Toast.StreamStarted", "You're live"),
		       Text("Toast.StreamStartedSub", "Stream started"));
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STOPPED: {
		const bool was = st.streaming;
		const uint64_t dur = st.StreamingElapsed(now);
		st.streaming = false;
		st.stream_start = 0;
		PushStatus();
		if (was)
			Notify(ToastCategory::Streaming, Icon::Stop, Text("Toast.StreamEnded", "Stream ended"),
			       Replace(Text("Toast.StreamDuration", "Streamed for %1"), "%1",
				       WideToUtf8(FormatDuration(dur))));
		break;
	}

	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		st.replay = true;
		PushStatus();
		Notify(ToastCategory::Replay, Icon::Replay, Text("Toast.ReplayOn", "Replay buffer is on"),
		       ReplayHint());
		break;

	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		if (st.replay) {
			st.replay = false;
			PushStatus();
			Notify(ToastCategory::Replay, Icon::Replay, Text("Toast.ReplayOff", "Replay buffer is off"));
		}
		break;

	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		Notify(ToastCategory::Replay, Icon::Saved, Text("Toast.ReplaySaved", "Replay saved"),
		       FileName(obs_frontend_get_last_replay()));
		break;

	case OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN:
		Notify(ToastCategory::Screenshot, Icon::Screenshot, Text("Toast.ScreenshotSaved", "Screenshot saved"),
		       FileName(obs_frontend_get_last_screenshot()));
		break;

	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
		st.virtualcam = true;
		PushStatus();
		Notify(ToastCategory::VirtualCam, Icon::VirtualCam,
		       Text("Toast.VirtualCamOn", "Virtual camera started"));
		break;

	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		st.virtualcam = false;
		PushStatus();
		Notify(ToastCategory::VirtualCam, Icon::VirtualCam,
		       Text("Toast.VirtualCamOff", "Virtual camera stopped"));
		break;

	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		AttachMic();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		Shutdown();
		break;

	default:
		break;
	}
}

// ------------------------------------------------------- menu + hotkey

void ToggleTask(void *)
{
	if (g_shut_down)
		return;
	g_settings.enabled = !g_settings.enabled;
	SaveSettings();
	if (g_overlay) {
		g_overlay->SetSettings(g_settings);
		if (g_settings.enabled)
			Notify(ToastCategory::General, Icon::Info, Text("Toast.OverlayOn", "Overlay indicator on"));
	}
}

void OnToggleHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		obs_queue_task(OBS_TASK_UI, ToggleTask, nullptr, false);
}

std::vector<AudioSourceChoice> AudioSources()
{
	std::vector<AudioSourceChoice> list;
	obs_enum_sources(
		[](void *p, obs_source_t *src) {
			if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) &&
			    obs_source_get_type(src) == OBS_SOURCE_TYPE_INPUT) {
				const char *name = obs_source_get_name(src);
				if (name && *name)
					static_cast<std::vector<AudioSourceChoice> *>(p)->push_back({name});
			}
			return true;
		},
		&list);
	std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
	return list;
}

void OpenSettings(void *)
{
	if (g_shut_down || !g_overlay)
		return;
	DialogContext ctx;
	ctx.audio_sources = AudioSources();
	ctx.default_mic_label = Utf8ToWide(Text("Settings.DefaultMic", "Default (Mic/Aux)"));
	ctx.preview = [](const Settings &s) {
		if (g_overlay) {
			g_overlay->SetSettings(s);
			g_overlay->Preview(Utf8ToWide(Text("Toast.ReplaySaved", "Replay saved")));
		}
	};
	ctx.text = [](const char *key) {
		const char *out = nullptr;
		return obs_module_get_string(key, &out) && out ? Utf8ToWide(out) : std::wstring();
	};

	Settings s = g_settings;
	HWND parent = static_cast<HWND>(obs_frontend_get_main_window_handle());
	if (ShowSettingsDialog(parent, g_module, s, ctx)) {
		g_settings = s;
		SaveSettings();
		AttachMic();
	}
	if (g_overlay)
		g_overlay->SetSettings(g_settings); // also reverts a preview after Cancel
}

} // namespace

bool obs_module_load(void)
{
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&OpenSettings), &g_module);

	Overlay::SetLogger(LogFromOverlay);
	g_hotkey = obs_hotkey_register_frontend("obs_indicator.toggle",
						Text("Hotkey.Toggle", "Toggle overlay indicator"), OnToggleHotkey,
						nullptr);
	LoadSettings();

	g_overlay = std::make_unique<Overlay>();
	g_overlay->SetSettings(g_settings);
	if (!g_overlay->Start(g_module)) {
		blog(LOG_ERROR, "[obs-indicator] Could not start the overlay thread");
		g_overlay.reset();
		obs_hotkey_unregister(g_hotkey);
		g_hotkey = OBS_INVALID_HOTKEY_ID;
		return false;
	}

	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
	obs_frontend_add_tools_menu_item(Text("Menu.Settings", "Overlay Indicator"), OpenSettings, nullptr);
	if (signal_handler_t *sh = obs_get_signal_handler()) {
		signal_handler_connect(sh, "source_create", OnSourceCreate, nullptr);
		signal_handler_connect(sh, "channel_change", OnChannelChange, nullptr);
	}

	blog(LOG_INFO, "[obs-indicator] Loaded version %s", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	Shutdown();
}
