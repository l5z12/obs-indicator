// SPDX-License-Identifier: GPL-3.0-or-later
//
// obs-indicator-hook.dll. With "Draw inside OpenGL games" on, the plugin loads this into a
// fullscreen OpenGL game such as Minecraft. In fullscreen the graphics driver sends the game's
// frames straight to the display, past Windows' compositor, so no window (whatever its z-order
// band) can be seen over it. This DLL instead draws the overlay into the game's own frame, just
// before the frame is shown.
//
// It hooks DrvSwapBuffers in the OpenGL driver (the ICD), not SwapBuffers or wglSwapBuffers.
// OBS's game capture hooks those two and copies the frame there before calling through, so
// drawing one level further down keeps the overlay out of OBS's game capture, just as the
// window version is kept out of screen capture.
//
// Drawing happens in the game's context with all touched state saved and restored. The pixels
// come from shared memory (see game-overlay-shared.hpp). Once loaded the DLL stays for the
// life of the game; it goes idle whenever OBS is gone or the option is off.

#include "game-overlay-shared.hpp"

#include <windows.h>
#include <psapi.h>

#include <GL/gl.h>

#include <cstdarg>
#include <cstdio>

#include "mhook-lib/mhook.h"

using namespace indicator::shared;

namespace {

// --- the OpenGL 2.0 - 3.3 entry points and constants we need beyond <GL/gl.h> (1.1) ---------
using GLchar = char;
#define GL_FN(ret, name, ...)                         \
	using PFN_##name = ret(APIENTRY *)(__VA_ARGS__); \
	PFN_##name p_##name = nullptr
GL_FN(GLuint, glCreateShader, GLenum);
GL_FN(void, glShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *);
GL_FN(void, glCompileShader, GLuint);
GL_FN(void, glGetShaderiv, GLuint, GLenum, GLint *);
GL_FN(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei *, GLchar *);
GL_FN(void, glDeleteShader, GLuint);
GL_FN(GLuint, glCreateProgram, void);
GL_FN(void, glAttachShader, GLuint, GLuint);
GL_FN(void, glBindFragDataLocation, GLuint, GLuint, const GLchar *);
GL_FN(void, glLinkProgram, GLuint);
GL_FN(void, glGetProgramiv, GLuint, GLenum, GLint *);
GL_FN(void, glUseProgram, GLuint);
GL_FN(GLint, glGetUniformLocation, GLuint, const GLchar *);
GL_FN(void, glUniform1i, GLint, GLint);
GL_FN(void, glUniform1f, GLint, GLfloat);
GL_FN(void, glUniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat);
GL_FN(void, glGenVertexArrays, GLsizei, GLuint *);
GL_FN(void, glBindVertexArray, GLuint);
GL_FN(void, glBindFramebuffer, GLenum, GLuint);
GL_FN(void, glBindBuffer, GLenum, GLuint);
GL_FN(void, glActiveTexture, GLenum);
GL_FN(void, glBlendFuncSeparate, GLenum, GLenum, GLenum, GLenum);
GL_FN(void, glBlendEquationSeparate, GLenum, GLenum);
GL_FN(void, glBindSampler, GLuint, GLuint); // GL 3.3; optional
#undef GL_FN

constexpr GLenum kBGRA = 0x80E1, kClampToEdge = 0x812F, kTexture0 = 0x84C0, kActiveTexture = 0x84E0,
		 kCurrentProgram = 0x8B8D, kVertexArrayBinding = 0x85B5, kDrawFramebuffer = 0x8CA9,
		 kDrawFramebufferBinding = 0x8CA6, kBlendSrcRgb = 0x80C9, kBlendDstRgb = 0x80C8,
		 kBlendSrcAlpha = 0x80CB, kBlendDstAlpha = 0x80CA, kBlendEquationRgb = 0x8009,
		 kBlendEquationAlpha = 0x883D, kFuncAdd = 0x8006, kFramebufferSrgb = 0x8DB9,
		 kSamplerBinding = 0x8919, kPixelUnpackBuffer = 0x88EC, kPixelUnpackBufferBinding = 0x88EF,
		 kVertexShader = 0x8B31, kFragmentShader = 0x8B30, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82,
		 kMajorVersion = 0x821B, kMinorVersion = 0x821C, kRasterizerDiscard = 0x8C89;

// --- shared memory and status reporting ----------------------------------------------------
HANDLE g_map = nullptr;
Header *g_hdr = nullptr; // set under g_msg_lock
HANDLE g_owner = nullptr;
DWORD g_owner_pid = 0;
ULONGLONG g_next_connect = 0;
// Status lines not yet delivered (the shared memory isn't open yet), oldest first.
char g_pending[kHookMessages][sizeof(HookMessage::text)] = {};
int g_pending_count = 0;
SRWLOCK g_msg_lock = SRWLOCK_INIT; // Report() runs on the install thread and the render thread

void FlushMessagesLocked()
{
	if (!g_hdr || g_hdr->magic != kMagic || g_hdr->version != kVersion)
		return;
	for (int i = 0; i < g_pending_count; ++i) {
		HookMessage &m = g_hdr->hook_msgs[(ULONG)g_hdr->hook_msg_seq % kHookMessages];
		m.pid = GetCurrentProcessId();
		memcpy(m.text, g_pending[i], sizeof(m.text));
		InterlockedIncrement(&g_hdr->hook_msg_seq);
	}
	g_pending_count = 0;
}

// A status line for OBS's log; delivered as soon as the plugin's shared memory is open.
void Report(const char *fmt, ...)
{
	AcquireSRWLockExclusive(&g_msg_lock);
	if (g_pending_count == kHookMessages) { // keep the newest
		memmove(g_pending[0], g_pending[1], sizeof(g_pending) - sizeof(g_pending[0]));
		--g_pending_count;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_pending[g_pending_count++], sizeof(g_pending[0]), fmt, ap);
	va_end(ap);
	FlushMessagesLocked();
	ReleaseSRWLockExclusive(&g_msg_lock);
}

// Opens the plugin's shared memory if it exists. Retried at most every 2 s.
bool OpenShared()
{
	const ULONGLONG now = GetTickCount64();
	if (now < g_next_connect)
		return false;
	g_next_connect = now + 2000;
	HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kMappingName);
	if (!map)
		return false;
	auto *hdr = static_cast<Header *>(MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingSize));
	if (!hdr) {
		CloseHandle(map);
		return false;
	}
	AcquireSRWLockExclusive(&g_msg_lock);
	g_map = map;
	g_hdr = hdr;
	FlushMessagesLocked();
	ReleaseSRWLockExclusive(&g_msg_lock);
	return true;
}

// True while an OBS with the option on owns the shared memory. Cheap enough to run every frame.
bool Connect()
{
	if (!g_hdr && !OpenShared())
		return false;
	if (g_pending_count) { // written before an OBS of this version owned the memory
		AcquireSRWLockExclusive(&g_msg_lock);
		FlushMessagesLocked();
		ReleaseSRWLockExclusive(&g_msg_lock);
	}
	if (g_hdr->magic != kMagic || g_hdr->version != kVersion || !(g_hdr->flags & kFlagEnabled))
		return false;
	// Our handle keeps the mapping alive after OBS exits; a later OBS reuses it and writes its pid.
	const DWORD pid = g_hdr->owner_pid;
	if (pid != g_owner_pid) {
		if (g_owner)
			CloseHandle(g_owner);
		g_owner = OpenProcess(SYNCHRONIZE, FALSE, pid);
		g_owner_pid = pid;
	}
	return g_owner && WaitForSingleObject(g_owner, 0) == WAIT_TIMEOUT;
}

// --- drawing -------------------------------------------------------------------------------
struct LayerTex {
	GLuint tex = 0;
	int w = 0, h = 0;
	LONG seq = -1; // the layer seq the texture holds; -1 = must upload
};

HGLRC g_ctx = nullptr; // context our GL objects belong to
bool g_gl_ok = false;
GLuint g_prog = 0, g_vao = 0;
GLint g_u_rect = -1, g_u_tex = -1, g_u_alpha = -1;
LayerTex g_tex[kLayers];

template<class T> bool Load(T &fn, const char *name)
{
	fn = reinterpret_cast<T>(wglGetProcAddress(name));
	const auto v = reinterpret_cast<intptr_t>(fn);
	if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1)
		fn = nullptr; // some drivers return small sentinels instead of null
	return fn != nullptr;
}

GLuint Compile(GLenum type, const char *src)
{
	const GLuint s = p_glCreateShader(type);
	p_glShaderSource(s, 1, &src, nullptr);
	p_glCompileShader(s);
	GLint ok = 0;
	p_glGetShaderiv(s, kCompileStatus, &ok);
	if (!ok) {
		char log[200] = {};
		p_glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
		Report("couldn't compile the overlay shader: %s", log);
		p_glDeleteShader(s);
		return 0;
	}
	return s;
}

// (Re)creates our GL objects in the game's current context.
bool InitGL(HGLRC ctx)
{
	g_ctx = ctx;
	g_gl_ok = false;
	for (auto &t : g_tex)
		t = LayerTex{};

	GLint major = 0, minor = 0;
	glGetIntegerv(kMajorVersion, &major);
	glGetIntegerv(kMinorVersion, &minor);
	glGetError(); // GL_MAJOR_VERSION is unknown before 3.0
	if (major * 10 + minor < 32) {
		Report("the game uses OpenGL %d.%d; drawing inside the game needs 3.2 or later", major, minor);
		return false;
	}
	const bool loaded = Load(p_glCreateShader, "glCreateShader") && Load(p_glShaderSource, "glShaderSource") &&
			    Load(p_glCompileShader, "glCompileShader") && Load(p_glGetShaderiv, "glGetShaderiv") &&
			    Load(p_glGetShaderInfoLog, "glGetShaderInfoLog") && Load(p_glDeleteShader, "glDeleteShader") &&
			    Load(p_glCreateProgram, "glCreateProgram") && Load(p_glAttachShader, "glAttachShader") &&
			    Load(p_glBindFragDataLocation, "glBindFragDataLocation") &&
			    Load(p_glLinkProgram, "glLinkProgram") && Load(p_glGetProgramiv, "glGetProgramiv") &&
			    Load(p_glUseProgram, "glUseProgram") && Load(p_glGetUniformLocation, "glGetUniformLocation") &&
			    Load(p_glUniform1i, "glUniform1i") && Load(p_glUniform1f, "glUniform1f") &&
			    Load(p_glUniform4f, "glUniform4f") && Load(p_glGenVertexArrays, "glGenVertexArrays") &&
			    Load(p_glBindVertexArray, "glBindVertexArray") && Load(p_glBindFramebuffer, "glBindFramebuffer") &&
			    Load(p_glBindBuffer, "glBindBuffer") && Load(p_glActiveTexture, "glActiveTexture") &&
			    Load(p_glBlendFuncSeparate, "glBlendFuncSeparate") &&
			    Load(p_glBlendEquationSeparate, "glBlendEquationSeparate");
	if (!loaded) {
		Report("the game's OpenGL driver is missing functions the overlay needs");
		return false;
	}
	Load(p_glBindSampler, "glBindSampler");

	// A quad from gl_VertexID, so no vertex buffer is needed. GLSL 1.50 works in both core
	// and compatibility contexts of OpenGL 3.2+.
	const char *vs = "#version 150\n"
			 "uniform vec4 u_rect;\n" // left, top, right, bottom in clip space
			 "out vec2 v_uv;\n"
			 "void main() {\n"
			 "  vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
			 "  v_uv = c;\n"
			 "  gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0);\n"
			 "}\n";
	const char *fs = "#version 150\n"
			 "uniform sampler2D u_tex;\n"
			 "uniform float u_alpha;\n"
			 "in vec2 v_uv;\n"
			 "out vec4 o_color;\n"
			 "void main() { o_color = texture(u_tex, v_uv) * u_alpha; }\n";
	const GLuint v = Compile(kVertexShader, vs);
	const GLuint f = v ? Compile(kFragmentShader, fs) : 0;
	if (!f)
		return false;
	g_prog = p_glCreateProgram();
	p_glAttachShader(g_prog, v);
	p_glAttachShader(g_prog, f);
	p_glBindFragDataLocation(g_prog, 0, "o_color");
	p_glLinkProgram(g_prog);
	p_glDeleteShader(v);
	p_glDeleteShader(f);
	GLint ok = 0;
	p_glGetProgramiv(g_prog, kLinkStatus, &ok);
	if (!ok) {
		Report("couldn't link the overlay shader");
		return false;
	}
	g_u_rect = p_glGetUniformLocation(g_prog, "u_rect");
	g_u_tex = p_glGetUniformLocation(g_prog, "u_tex");
	g_u_alpha = p_glGetUniformLocation(g_prog, "u_alpha");
	p_glGenVertexArrays(1, &g_vao);
	g_gl_ok = true;
	Report("drawing the overlay inside the game (OpenGL %d.%d)", major, minor);
	return true;
}

// Everything Draw() changes, so the game finds its context exactly as it left it.
struct SavedState {
	GLint active_texture, program, texture, sampler, vao, draw_fb, unpack_buffer;
	GLint viewport[4], polygon_mode[2];
	GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;
	GLint unpack_alignment, unpack_row_length, unpack_skip_rows, unpack_skip_pixels;
	GLboolean color_mask[4];
	GLboolean blend, cull, depth, stencil, scissor, srgb, discard;

	void Save()
	{
		glGetIntegerv(kActiveTexture, &active_texture);
		p_glActiveTexture(kTexture0);
		glGetIntegerv(kCurrentProgram, &program);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		sampler = 0;
		if (p_glBindSampler)
			glGetIntegerv(kSamplerBinding, &sampler);
		glGetIntegerv(kVertexArrayBinding, &vao);
		glGetIntegerv(kDrawFramebufferBinding, &draw_fb);
		glGetIntegerv(kPixelUnpackBufferBinding, &unpack_buffer);
		glGetIntegerv(GL_VIEWPORT, viewport);
		glGetIntegerv(GL_POLYGON_MODE, polygon_mode);
		glGetIntegerv(kBlendSrcRgb, &blend_src_rgb);
		glGetIntegerv(kBlendDstRgb, &blend_dst_rgb);
		glGetIntegerv(kBlendSrcAlpha, &blend_src_a);
		glGetIntegerv(kBlendDstAlpha, &blend_dst_a);
		glGetIntegerv(kBlendEquationRgb, &blend_eq_rgb);
		glGetIntegerv(kBlendEquationAlpha, &blend_eq_a);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
		glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
		glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
		glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
		blend = glIsEnabled(GL_BLEND);
		cull = glIsEnabled(GL_CULL_FACE);
		depth = glIsEnabled(GL_DEPTH_TEST);
		stencil = glIsEnabled(GL_STENCIL_TEST);
		scissor = glIsEnabled(GL_SCISSOR_TEST);
		srgb = glIsEnabled(kFramebufferSrgb);
		discard = glIsEnabled(kRasterizerDiscard);
	}

	static void Set(GLenum cap, GLboolean on)
	{
		if (on)
			glEnable(cap);
		else
			glDisable(cap);
	}

	void Restore() const
	{
		p_glUseProgram((GLuint)program);
		glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
		if (p_glBindSampler)
			p_glBindSampler(0, (GLuint)sampler);
		p_glActiveTexture((GLenum)active_texture);
		p_glBindVertexArray((GLuint)vao);
		p_glBindFramebuffer(kDrawFramebuffer, (GLuint)draw_fb);
		p_glBindBuffer(kPixelUnpackBuffer, (GLuint)unpack_buffer);
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		glPolygonMode(GL_FRONT_AND_BACK, (GLenum)polygon_mode[0]);
		p_glBlendFuncSeparate((GLenum)blend_src_rgb, (GLenum)blend_dst_rgb, (GLenum)blend_src_a,
				      (GLenum)blend_dst_a);
		p_glBlendEquationSeparate((GLenum)blend_eq_rgb, (GLenum)blend_eq_a);
		glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
		glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
		Set(GL_BLEND, blend);
		Set(GL_CULL_FACE, cull);
		Set(GL_DEPTH_TEST, depth);
		Set(GL_STENCIL_TEST, stencil);
		Set(GL_SCISSOR_TEST, scissor);
		Set(kFramebufferSrgb, srgb);
		Set(kRasterizerDiscard, discard);
	}
};

// Brings layer i's texture up to date. Returns false if there is nothing to draw yet.
bool Upload(int i, const Layer &l)
{
	LayerTex &t = g_tex[i];
	const LONG seq = ReadAcquire(&l.seq);
	if (seq == t.seq)
		return t.tex != 0;
	if (seq & 1) // the plugin is writing it: draw last frame's pixels, if any
		return t.tex != 0 && t.w == l.w && t.h == l.h;
	const int w = l.w, h = l.h;
	if (w <= 0 || h <= 0 || (size_t)w * h * 4 > kMaxLayerBytes)
		return false;
	if (!t.tex) {
		glGenTextures(1, &t.tex);
		glBindTexture(GL_TEXTURE_2D, t.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
	} else {
		glBindTexture(GL_TEXTURE_2D, t.tex);
	}
	const void *px = LayerPixels(g_hdr, i);
	if (w != t.w || h != t.h) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, kBGRA, GL_UNSIGNED_BYTE, px);
		t.w = w;
		t.h = h;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, kBGRA, GL_UNSIGNED_BYTE, px);
	}
	// Changed while we read it? Draw what we have and read it again next frame.
	t.seq = ReadAcquire(&l.seq) == seq ? seq : -1;
	return true;
}

void Draw(HDC dc)
{
	if (!Connect())
		return;
	bool any = false;
	for (const Layer &l : g_hdr->layers)
		any = any || l.visible;
	if (!any)
		return;
	HWND hwnd = WindowFromDC(dc);
	HGLRC ctx = wglGetCurrentContext();
	if (!hwnd || !ctx || WindowFromDC(wglGetCurrentDC()) != hwnd)
		return; // the game's context isn't current on this window

	// The window in physical pixels (the plugin's coordinates) and in the game's own DPI
	// space, which is the size of its framebuffer.
	RECT fb{}, phys{};
	GetClientRect(hwnd, &fb);
	const DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	GetClientRect(hwnd, &phys);
	MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT *>(&phys), 2);
	MONITORINFO mi{sizeof(mi)};
	GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
	if (old)
		SetThreadDpiAwarenessContext(old);
	const RECT &m = mi.rcMonitor;
	const bool fullscreen = phys.left <= m.left && phys.top <= m.top && phys.right >= m.right && phys.bottom >= m.bottom;
	if (!fullscreen && !(g_hdr->flags & kFlagAnyWindow))
		return; // the overlay's own windows show over windowed games
	const int fbw = fb.right, fbh = fb.bottom;
	const int pw = phys.right - phys.left, ph = phys.bottom - phys.top;
	if (fbw <= 0 || fbh <= 0 || pw <= 0 || ph <= 0)
		return;

	if (ctx != g_ctx)
		InitGL(ctx);
	if (!g_gl_ok)
		return;

	SavedState s;
	s.Save();
	p_glUseProgram(g_prog);
	p_glUniform1i(g_u_tex, 0);
	if (p_glBindSampler)
		p_glBindSampler(0, 0);
	p_glBindVertexArray(g_vao);
	p_glBindFramebuffer(kDrawFramebuffer, 0);
	p_glBindBuffer(kPixelUnpackBuffer, 0);
	glViewport(0, 0, fbw, fbh);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glEnable(GL_BLEND);
	p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
	p_glBlendEquationSeparate(kFuncAdd, kFuncAdd);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(kFramebufferSrgb); // the pixels are already sRGB
	glDisable(kRasterizerDiscard);

	const float sx = (float)fbw / pw, sy = (float)fbh / ph;
	const LONG64 now = (LONG64)GetTickCount64();
	for (int i = 0; i < kLayers; ++i) {
		Layer &l = g_hdr->layers[i];
		if (!l.visible || l.x >= phys.right || l.y >= phys.bottom || l.x + l.w <= phys.left ||
		    l.y + l.h <= phys.top)
			continue;
		if (!Upload(i, l))
			continue;
		const LayerTex &t = g_tex[i];
		const float x0 = (l.x - phys.left) * sx, y0 = (l.y - phys.top) * sy;
		const float x1 = x0 + t.w * sx, y1 = y0 + t.h * sy;
		const bool scaled = sx < 0.999f || sx > 1.001f || sy < 0.999f || sy > 1.001f;
		glBindTexture(GL_TEXTURE_2D, t.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, scaled ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, scaled ? GL_LINEAR : GL_NEAREST);
		p_glUniform4f(g_u_rect, x0 / fbw * 2 - 1, 1 - y0 / fbh * 2, x1 / fbw * 2 - 1, 1 - y1 / fbh * 2);
		p_glUniform1f(g_u_alpha, l.alpha / 255.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		InterlockedExchange64(&l.drawn_tick, now);
	}
	s.Restore();
}

// --- the hook ------------------------------------------------------------------------------
using SwapFn = BOOL(WINAPI *)(HDC);
SwapFn g_real_swap = nullptr;

BOOL WINAPI HookSwapBuffers(HDC dc)
{
	Draw(dc);
	return g_real_swap(dc);
}

// The ICD is the loaded module (other than opengl32.dll) exporting the driver entry points.
HMODULE FindIcd()
{
	HMODULE mods[1024];
	DWORD needed = 0;
	if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
		return nullptr;
	const HMODULE ogl = GetModuleHandleW(L"opengl32.dll");
	for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
		if (mods[i] != ogl && GetProcAddress(mods[i], "DrvSwapBuffers") && GetProcAddress(mods[i], "DrvSetContext"))
			return mods[i];
	}
	return nullptr;
}

DWORD WINAPI InstallThread(void *)
{
	// Open the shared memory first, so what we report below reaches OBS's log. The render
	// thread only touches it once the hook is in.
	OpenShared();
	// The game already renders with OpenGL when the plugin injects us, so the driver is loaded;
	// wait a little in case it isn't yet.
	HMODULE icd = nullptr;
	for (int i = 0; i < 50 && !(icd = FindIcd()); ++i)
		Sleep(200);
	if (!icd) {
		Report("no OpenGL driver loaded in the game");
		return 0;
	}
	wchar_t name[MAX_PATH] = L"?";
	GetModuleBaseNameW(GetCurrentProcess(), icd, name, MAX_PATH);
	g_real_swap = reinterpret_cast<SwapFn>(GetProcAddress(icd, "DrvSwapBuffers"));
	if (!Mhook_SetHook(reinterpret_cast<PVOID *>(&g_real_swap), reinterpret_cast<PVOID>(HookSwapBuffers))) {
		Report("couldn't hook %ls!DrvSwapBuffers", name);
		return 0;
	}
	Report("hooked %ls!DrvSwapBuffers; waiting for the game to go fullscreen", name);
	return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		// Pin: the hook jumps into this DLL, so it must never be unloaded.
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				   reinterpret_cast<LPCWSTR>(&DllMain), &self);
		if (HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr))
			CloseHandle(t);
	}
	return TRUE;
}
