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
// Drawing happens in the game's context with all touched state saved and restored, and only
// while the game's window is fullscreen and in front. The pixels come from shared memory (see
// game-overlay-shared.hpp). Once loaded the DLL stays for the life of the game; it goes idle
// whenever OBS is gone or the option is off.

#include "game-overlay-shared.hpp"

#include <windows.h>
#include <psapi.h>

#include <GL/gl.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "mhook-lib/mhook.h"

using namespace indicator::shared;

namespace {

// --- the OpenGL 2.0 - 4.1 entry points and constants we need beyond <GL/gl.h> (1.1) ---------
using GLchar = char;
using GLsizeiptr = intptr_t;
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
GL_FN(void, glBindAttribLocation, GLuint, GLuint, const GLchar *);
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
GL_FN(void, glGenBuffers, GLsizei, GLuint *);
GL_FN(void, glBindBuffer, GLenum, GLuint);
GL_FN(void, glBufferData, GLenum, GLsizeiptr, const void *, GLenum);
GL_FN(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
GL_FN(void, glEnableVertexAttribArray, GLuint);
GL_FN(void, glBindFramebuffer, GLenum, GLuint);
GL_FN(void, glActiveTexture, GLenum);
GL_FN(void, glBlendFuncSeparate, GLenum, GLenum, GLenum, GLenum);
GL_FN(void, glBlendEquationSeparate, GLenum, GLenum);
GL_FN(GLboolean, glIsEnabledi, GLenum, GLuint);
GL_FN(void, glEnablei, GLenum, GLuint);
GL_FN(void, glDisablei, GLenum, GLuint);
GL_FN(void, glColorMaski, GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
GL_FN(void, glGetBooleani_v, GLenum, GLuint, GLboolean *);
GL_FN(void, glGetIntegeri_v, GLenum, GLuint, GLint *);
GL_FN(void, glBindSampler, GLuint, GLuint);                                 // 3.3
GL_FN(void, glBlendFuncSeparatei, GLuint, GLenum, GLenum, GLenum, GLenum); // 4.0
GL_FN(void, glBlendEquationSeparatei, GLuint, GLenum, GLenum);             // 4.0
GL_FN(void, glViewportIndexedf, GLuint, GLfloat, GLfloat, GLfloat, GLfloat); // 4.1
GL_FN(void, glGetFloati_v, GLenum, GLuint, GLfloat *);                     // 4.1
#undef GL_FN

constexpr GLenum kBGRA = 0x80E1, kClampToEdge = 0x812F, kTexture0 = 0x84C0, kActiveTexture = 0x84E0,
		 kCurrentProgram = 0x8B8D, kVertexArrayBinding = 0x85B5, kArrayBuffer = 0x8892,
		 kArrayBufferBinding = 0x8894, kStaticDraw = 0x88E4, kDrawFramebuffer = 0x8CA9,
		 kDrawFramebufferBinding = 0x8CA6, kBlendSrcRgb = 0x80C9, kBlendDstRgb = 0x80C8,
		 kBlendSrcAlpha = 0x80CB, kBlendDstAlpha = 0x80CA, kBlendEquationRgb = 0x8009,
		 kBlendEquationAlpha = 0x883D, kFuncAdd = 0x8006, kFramebufferSrgb = 0x8DB9,
		 kSamplerBinding = 0x8919, kPixelUnpackBuffer = 0x88EC, kPixelUnpackBufferBinding = 0x88EF,
		 kVertexShader = 0x8B31, kFragmentShader = 0x8B30, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82,
		 kRasterizerDiscard = 0x8C89, kContextProfileMask = 0x9126;
constexpr GLint kCoreProfileBit = 1;

// --- shared memory and status reporting ----------------------------------------------------
HANDLE g_map = nullptr;
Header *g_hdr = nullptr; // set under g_msg_lock
ULONGLONG g_next_connect = 0;
// Status lines not yet delivered (the shared memory isn't open yet), oldest first.
char g_pending[kHookMessages][sizeof(HookMessage::text)] = {};
int g_pending_count = 0;
SRWLOCK g_msg_lock = SRWLOCK_INIT; // Report() runs on the install thread and the render thread

bool SameVersion()
{
	return g_hdr && g_hdr->magic == kMagic && g_hdr->version == kVersion;
}

void FlushMessagesLocked()
{
	if (!SameVersion())
		return;
	for (int i = 0; i < g_pending_count; ++i) {
		// Reserve the slot first, so hooks in two games never write the same one.
		const LONG s = InterlockedIncrement(&g_hdr->hook_msg_seq) - 1;
		HookMessage &m = g_hdr->hook_msgs[(ULONG)s % kHookMessages];
		InterlockedExchange(&m.seq, 0);
		m.pid = GetCurrentProcessId();
		memcpy(m.text, g_pending[i], sizeof(m.text));
		m.text[sizeof(m.text) - 1] = '\0';
		InterlockedExchange(&m.seq, s + 1);
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
	// The plugin commits the header just after creating the mapping; reading it before that
	// would fault, so wait until it is committed.
	MEMORY_BASIC_INFORMATION mbi{};
	if (!hdr || !VirtualQuery(hdr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
		if (hdr)
			UnmapViewOfFile(hdr);
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
// OBS's liveness comes from its heartbeat, so we never need a handle to OBS's process (which a
// game can't open when OBS runs as administrator).
bool Connect()
{
	if (!g_hdr && !OpenShared())
		return false;
	if (!SameVersion())
		return false; // a different build of the plugin owns it
	AcquireSRWLockExclusive(&g_msg_lock);
	FlushMessagesLocked(); // anything reported before this version's OBS owned the memory
	ReleaseSRWLockExclusive(&g_msg_lock);
	if (!(g_hdr->flags & kFlagEnabled))
		return false;
	const LONG64 beat = g_hdr->owner_tick;
	const LONG64 age = (LONG64)GetTickCount64() - beat;
	return beat && age < (LONG64)kOwnerTimeoutMs;
}

// --- drawing -------------------------------------------------------------------------------
struct LayerTex {
	GLuint tex = 0;
	int w = 0, h = 0;
	LONG seq = -1; // the layer seq the texture holds; -1 = must upload
	// Where and how it was last drawn, for frames where the plugin is mid-write.
	int x = 0, y = 0, alpha = 0;
	bool shown = false;
};

// Our GL objects in one of the game's contexts. Keyed by HGLRC, and forgotten when the game
// deletes that context (opengl32 reuses handle values).
struct Ctx {
	HGLRC rc = nullptr;
	bool ok = false;     // objects created; false after a failure (not retried)
	int version = 0;     // major * 10 + minor
	bool compat = false; // compatibility profile
	GLuint prog = 0, vao = 0, vbo = 0;
	GLint u_rect = -1, u_tex = -1, u_alpha = -1;
	LayerTex tex[kLayers];
	ULONGLONG used = 0;
};
constexpr int kContexts = 4;
Ctx g_ctxs[kContexts];
SRWLOCK g_draw_lock = SRWLOCK_INIT; // games may swap different windows on different threads
bool g_fns_loaded = false, g_fns_failed = false;
bool g_reported_ok = false, g_reported_old = false;

template<class T> bool Load(T &fn, const char *name)
{
	fn = reinterpret_cast<T>(wglGetProcAddress(name));
	const auto v = reinterpret_cast<intptr_t>(fn);
	if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1)
		fn = nullptr; // some drivers return small sentinels instead of null
	return fn != nullptr;
}

bool LoadFunctions()
{
	if (g_fns_loaded || g_fns_failed)
		return g_fns_loaded;
	const bool ok =
		Load(p_glCreateShader, "glCreateShader") && Load(p_glShaderSource, "glShaderSource") &&
		Load(p_glCompileShader, "glCompileShader") && Load(p_glGetShaderiv, "glGetShaderiv") &&
		Load(p_glGetShaderInfoLog, "glGetShaderInfoLog") && Load(p_glDeleteShader, "glDeleteShader") &&
		Load(p_glCreateProgram, "glCreateProgram") && Load(p_glAttachShader, "glAttachShader") &&
		Load(p_glBindAttribLocation, "glBindAttribLocation") &&
		Load(p_glBindFragDataLocation, "glBindFragDataLocation") && Load(p_glLinkProgram, "glLinkProgram") &&
		Load(p_glGetProgramiv, "glGetProgramiv") && Load(p_glUseProgram, "glUseProgram") &&
		Load(p_glGetUniformLocation, "glGetUniformLocation") && Load(p_glUniform1i, "glUniform1i") &&
		Load(p_glUniform1f, "glUniform1f") && Load(p_glUniform4f, "glUniform4f") &&
		Load(p_glGenVertexArrays, "glGenVertexArrays") && Load(p_glBindVertexArray, "glBindVertexArray") &&
		Load(p_glGenBuffers, "glGenBuffers") && Load(p_glBindBuffer, "glBindBuffer") &&
		Load(p_glBufferData, "glBufferData") && Load(p_glVertexAttribPointer, "glVertexAttribPointer") &&
		Load(p_glEnableVertexAttribArray, "glEnableVertexAttribArray") &&
		Load(p_glBindFramebuffer, "glBindFramebuffer") && Load(p_glActiveTexture, "glActiveTexture") &&
		Load(p_glBlendFuncSeparate, "glBlendFuncSeparate") &&
		Load(p_glBlendEquationSeparate, "glBlendEquationSeparate") && Load(p_glIsEnabledi, "glIsEnabledi") &&
		Load(p_glEnablei, "glEnablei") && Load(p_glDisablei, "glDisablei") &&
		Load(p_glColorMaski, "glColorMaski") && Load(p_glGetBooleani_v, "glGetBooleani_v") &&
		Load(p_glGetIntegeri_v, "glGetIntegeri_v");
	if (!ok) {
		g_fns_failed = true;
		Report("the game's OpenGL driver is missing functions the overlay needs");
		return false;
	}
	Load(p_glBindSampler, "glBindSampler");
	Load(p_glBlendFuncSeparatei, "glBlendFuncSeparatei");
	Load(p_glBlendEquationSeparatei, "glBlendEquationSeparatei");
	Load(p_glViewportIndexedf, "glViewportIndexedf");
	Load(p_glGetFloati_v, "glGetFloati_v");
	g_fns_loaded = true;
	return true;
}

// The current context's version from GL_VERSION ("4.6.0 NVIDIA ..."), which, unlike
// GL_MAJOR_VERSION, works (and raises no GL error) in every version.
int ContextVersion()
{
	const char *v = reinterpret_cast<const char *>(glGetString(GL_VERSION));
	int major = 0, minor = 0;
	if (!v || sscanf_s(v, "%d.%d", &major, &minor) != 2)
		return 0;
	return major * 10 + minor;
}

GLuint Compile(GLenum type, const char *src)
{
	const GLuint s = p_glCreateShader(type);
	p_glShaderSource(s, 1, &src, nullptr);
	p_glCompileShader(s);
	GLint ok = 0;
	p_glGetShaderiv(s, kCompileStatus, &ok);
	if (!ok) {
		char log[96] = {};
		p_glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
		Report("couldn't compile the overlay shader: %s", log);
		p_glDeleteShader(s);
		return 0;
	}
	return s;
}

// Everything Draw() changes, so the game finds its context exactly as it left it. Only draw
// buffer and viewport 0 are touched, through the indexed calls where the version has them, so
// per-buffer blending and per-viewport state survive.
struct SavedState {
	bool indexed_blend = false, indexed_viewport = false, compat = false;
	GLint active_texture = 0, program = 0, texture = 0, sampler = 0, vao = 0, array_buffer = 0;
	GLint draw_fb = 0, unpack_buffer = 0;
	GLint viewport[4] = {};
	GLfloat viewport_f[4] = {};
	GLint polygon_mode[2] = {GL_FILL, GL_FILL};
	GLint blend_src_rgb = 0, blend_dst_rgb = 0, blend_src_a = 0, blend_dst_a = 0, blend_eq_rgb = 0, blend_eq_a = 0;
	GLint unpack_alignment = 4, unpack_row_length = 0, unpack_skip_rows = 0, unpack_skip_pixels = 0;
	GLboolean color_mask[4] = {};
	GLboolean blend = 0, cull = 0, depth = 0, stencil = 0, scissor = 0, srgb = 0, discard = 0;
	bool sampler_saved = false;

	void Save(const Ctx &c)
	{
		compat = c.compat;
		indexed_blend = c.version >= 40 && p_glBlendFuncSeparatei && p_glBlendEquationSeparatei;
		indexed_viewport = c.version >= 41 && p_glViewportIndexedf && p_glGetFloati_v;
		sampler_saved = c.version >= 33 && p_glBindSampler;
		glGetIntegerv(kActiveTexture, &active_texture);
		p_glActiveTexture(kTexture0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		if (sampler_saved)
			glGetIntegerv(kSamplerBinding, &sampler);
		glGetIntegerv(kCurrentProgram, &program);
		glGetIntegerv(kVertexArrayBinding, &vao);
		glGetIntegerv(kArrayBufferBinding, &array_buffer);
		glGetIntegerv(kDrawFramebufferBinding, &draw_fb);
		glGetIntegerv(kPixelUnpackBufferBinding, &unpack_buffer);
		glGetIntegerv(GL_POLYGON_MODE, polygon_mode);
		if (indexed_blend) {
			p_glGetIntegeri_v(kBlendSrcRgb, 0, &blend_src_rgb);
			p_glGetIntegeri_v(kBlendDstRgb, 0, &blend_dst_rgb);
			p_glGetIntegeri_v(kBlendSrcAlpha, 0, &blend_src_a);
			p_glGetIntegeri_v(kBlendDstAlpha, 0, &blend_dst_a);
			p_glGetIntegeri_v(kBlendEquationRgb, 0, &blend_eq_rgb);
			p_glGetIntegeri_v(kBlendEquationAlpha, 0, &blend_eq_a);
		} else {
			glGetIntegerv(kBlendSrcRgb, &blend_src_rgb);
			glGetIntegerv(kBlendDstRgb, &blend_dst_rgb);
			glGetIntegerv(kBlendSrcAlpha, &blend_src_a);
			glGetIntegerv(kBlendDstAlpha, &blend_dst_a);
			glGetIntegerv(kBlendEquationRgb, &blend_eq_rgb);
			glGetIntegerv(kBlendEquationAlpha, &blend_eq_a);
		}
		if (indexed_viewport) {
			p_glGetFloati_v(GL_VIEWPORT, 0, viewport_f);
			scissor = p_glIsEnabledi(GL_SCISSOR_TEST, 0);
		} else {
			glGetIntegerv(GL_VIEWPORT, viewport);
			scissor = glIsEnabled(GL_SCISSOR_TEST);
		}
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
		glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
		glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
		p_glGetBooleani_v(GL_COLOR_WRITEMASK, 0, color_mask);
		blend = p_glIsEnabledi(GL_BLEND, 0);
		cull = glIsEnabled(GL_CULL_FACE);
		depth = glIsEnabled(GL_DEPTH_TEST);
		stencil = glIsEnabled(GL_STENCIL_TEST);
		srgb = glIsEnabled(kFramebufferSrgb);
		discard = glIsEnabled(kRasterizerDiscard);
	}

	// The state Draw() needs: premultiplied blending into draw buffer 0 of the window.
	void SetForOverlay(int fbw, int fbh) const
	{
		if (sampler_saved)
			p_glBindSampler(0, 0);
		p_glBindFramebuffer(kDrawFramebuffer, 0);
		p_glBindBuffer(kPixelUnpackBuffer, 0);
		if (indexed_viewport) {
			p_glViewportIndexedf(0, 0, 0, (GLfloat)fbw, (GLfloat)fbh);
			p_glDisablei(GL_SCISSOR_TEST, 0);
		} else {
			glViewport(0, 0, fbw, fbh);
			glDisable(GL_SCISSOR_TEST);
		}
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		p_glEnablei(GL_BLEND, 0);
		if (indexed_blend) {
			p_glBlendFuncSeparatei(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			p_glBlendEquationSeparatei(0, kFuncAdd, kFuncAdd);
		} else {
			p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			p_glBlendEquationSeparate(kFuncAdd, kFuncAdd);
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		p_glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDisable(GL_CULL_FACE);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_STENCIL_TEST);
		glDisable(kFramebufferSrgb); // the pixels are already sRGB
		glDisable(kRasterizerDiscard);
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
		if (sampler_saved)
			p_glBindSampler(0, (GLuint)sampler);
		p_glActiveTexture((GLenum)active_texture);
		p_glBindVertexArray((GLuint)vao);
		p_glBindBuffer(kArrayBuffer, (GLuint)array_buffer);
		p_glBindFramebuffer(kDrawFramebuffer, (GLuint)draw_fb);
		p_glBindBuffer(kPixelUnpackBuffer, (GLuint)unpack_buffer);
		if (indexed_viewport) {
			p_glViewportIndexedf(0, viewport_f[0], viewport_f[1], viewport_f[2], viewport_f[3]);
			if (scissor)
				p_glEnablei(GL_SCISSOR_TEST, 0);
		} else {
			glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
			Set(GL_SCISSOR_TEST, scissor);
		}
		if (compat) {
			glPolygonMode(GL_FRONT, (GLenum)polygon_mode[0]);
			glPolygonMode(GL_BACK, (GLenum)polygon_mode[1]);
		} else {
			glPolygonMode(GL_FRONT_AND_BACK, (GLenum)polygon_mode[0]);
		}
		if (indexed_blend) {
			p_glBlendFuncSeparatei(0, (GLenum)blend_src_rgb, (GLenum)blend_dst_rgb, (GLenum)blend_src_a,
					       (GLenum)blend_dst_a);
			p_glBlendEquationSeparatei(0, (GLenum)blend_eq_rgb, (GLenum)blend_eq_a);
		} else {
			p_glBlendFuncSeparate((GLenum)blend_src_rgb, (GLenum)blend_dst_rgb, (GLenum)blend_src_a,
					      (GLenum)blend_dst_a);
			p_glBlendEquationSeparate((GLenum)blend_eq_rgb, (GLenum)blend_eq_a);
		}
		if (!blend)
			p_glDisablei(GL_BLEND, 0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
		p_glColorMaski(0, color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
		Set(GL_CULL_FACE, cull);
		Set(GL_DEPTH_TEST, depth);
		Set(GL_STENCIL_TEST, stencil);
		Set(kFramebufferSrgb, srgb);
		Set(kRasterizerDiscard, discard);
	}
};

// Creates our objects in context `c` (current). Binds a VAO and a buffer, so run it between
// SavedState::Save() and Restore().
bool InitContext(Ctx &c)
{
	// The quad's corners come from a tiny vertex buffer on attribute 0: some drivers draw
	// nothing without an enabled attribute 0 in compatibility contexts. GLSL 1.50 works in
	// core and compatibility contexts of OpenGL 3.2+.
	const char *vs = "#version 150\n"
			 "in vec2 a_corner;\n"
			 "uniform vec4 u_rect;\n" // left, top, right, bottom in clip space
			 "out vec2 v_uv;\n"
			 "void main() {\n"
			 "  v_uv = a_corner;\n"
			 "  gl_Position = vec4(mix(u_rect.xy, u_rect.zw, a_corner), 0.0, 1.0);\n"
			 "}\n";
	const char *fs = "#version 150\n"
			 "uniform sampler2D u_tex;\n"
			 "uniform float u_alpha;\n"
			 "in vec2 v_uv;\n"
			 "out vec4 o_color;\n"
			 "void main() { o_color = texture(u_tex, v_uv) * u_alpha; }\n";
	const GLuint v = Compile(kVertexShader, vs);
	const GLuint f = v ? Compile(kFragmentShader, fs) : 0;
	if (!f) {
		if (v)
			p_glDeleteShader(v);
		return false;
	}
	c.prog = p_glCreateProgram();
	p_glAttachShader(c.prog, v);
	p_glAttachShader(c.prog, f);
	p_glBindAttribLocation(c.prog, 0, "a_corner");
	p_glBindFragDataLocation(c.prog, 0, "o_color");
	p_glLinkProgram(c.prog);
	p_glDeleteShader(v);
	p_glDeleteShader(f);
	GLint ok = 0;
	p_glGetProgramiv(c.prog, kLinkStatus, &ok);
	if (!ok) {
		Report("couldn't link the overlay shader");
		return false;
	}
	c.u_rect = p_glGetUniformLocation(c.prog, "u_rect");
	c.u_tex = p_glGetUniformLocation(c.prog, "u_tex");
	c.u_alpha = p_glGetUniformLocation(c.prog, "u_alpha");

	static const GLfloat corners[8] = {0, 0, 1, 0, 0, 1, 1, 1}; // a triangle strip
	p_glGenVertexArrays(1, &c.vao);
	p_glBindVertexArray(c.vao);
	p_glGenBuffers(1, &c.vbo);
	p_glBindBuffer(kArrayBuffer, c.vbo);
	p_glBufferData(kArrayBuffer, sizeof(corners), corners, kStaticDraw);
	p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	p_glEnableVertexAttribArray(0);
	return true;
}

// Our objects for the current context `rc`, creating them on first use. Null if they can't be.
Ctx *ContextFor(HGLRC rc)
{
	Ctx *lru = &g_ctxs[0];
	for (Ctx &c : g_ctxs) {
		if (c.rc == rc)
			return c.ok ? &c : nullptr;
		if (c.used < lru->used)
			lru = &c;
	}
	// A new context. With more than kContexts in use, the least recently drawn one's objects
	// are given up (left to the game's context).
	Ctx &c = *lru;
	c = Ctx{};
	c.rc = rc;
	c.used = GetTickCount64();
	c.version = ContextVersion();
	if (c.version < 32) {
		if (!g_reported_old) {
			g_reported_old = true;
			Report("the game uses OpenGL %d.%d; drawing inside the game needs 3.2 or later", c.version / 10,
			       c.version % 10);
		}
		return nullptr;
	}
	if (!LoadFunctions())
		return nullptr;
	// Core only if it says so: a context made by plain wglCreateContext reports no profile bits
	// and is a compatibility context.
	GLint mask = 0;
	glGetIntegerv(kContextProfileMask, &mask);
	c.compat = (mask & kCoreProfileBit) == 0;
	SavedState s;
	s.Save(c);
	c.ok = InitContext(c);
	s.Restore();
	if (c.ok && !g_reported_ok) {
		g_reported_ok = true;
		Report("drawing the overlay inside the game (OpenGL %d.%d%s)", c.version / 10, c.version % 10,
		       c.compat ? ", compatibility profile" : "");
	}
	return c.ok ? &c : nullptr;
}

// A consistent copy of a layer's description, or false while the plugin is writing it.
struct LayerSnap {
	LONG seq;
	bool visible;
	int x, y, w, h, alpha;
};

bool ReadLayer(const Layer &l, LayerSnap &s)
{
	s.seq = ReadAcquire(&l.seq);
	if (s.seq & 1)
		return false;
	s.visible = l.visible != 0;
	s.x = l.x;
	s.y = l.y;
	s.w = l.w;
	s.h = l.h;
	s.alpha = l.alpha;
	std::atomic_thread_fence(std::memory_order_acquire); // the reads above stay before the check
	return ReadAcquire(&l.seq) == s.seq;
}

// Brings layer i's texture up to the snapshot. The size comes from the snapshot, which the
// plugin only publishes after committing the pages it needs, so the read below stays inside
// committed memory even if the plugin is already writing the next frame.
bool Upload(LayerTex &t, int i, const Layer &l, const LayerSnap &s)
{
	if (s.seq == t.seq)
		return t.tex != 0;
	if (s.w <= 0 || s.h <= 0 || (size_t)s.w * s.h * 4 > kMaxLayerBytes)
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
	if (s.w != t.w || s.h != t.h) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s.w, s.h, 0, kBGRA, GL_UNSIGNED_BYTE, px);
		t.w = s.w;
		t.h = s.h;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.w, s.h, kBGRA, GL_UNSIGNED_BYTE, px);
	}
	// Changed while we read it? Draw what we have and read it again next frame.
	std::atomic_thread_fence(std::memory_order_acquire);
	t.seq = ReadAcquire(&l.seq) == s.seq ? s.seq : -1;
	return true;
}

void DrawLocked(HGLRC rc, const RECT &phys, const RECT &monitor, int fbw, int fbh)
{
	Ctx *c = ContextFor(rc);
	if (!c)
		return;
	const ULONGLONG now = GetTickCount64();
	c->used = now;
	// Tell the plugin we draw on this monitor, so it hides its windows there, even before a
	// layer is visible: a new notification then never flashes up as a window first.
	g_hdr->game_monitor = monitor;
	InterlockedExchange64(&g_hdr->game_tick, (LONG64)now);

	bool any = false;
	for (const Layer &l : g_hdr->layers)
		any = any || l.visible;
	if (!any)
		return;

	SavedState s;
	s.Save(*c);
	s.SetForOverlay(fbw, fbh);
	p_glUseProgram(c->prog);
	p_glUniform1i(c->u_tex, 0);
	p_glBindVertexArray(c->vao);

	const int pw = phys.right - phys.left, ph = phys.bottom - phys.top;
	const float sx = (float)fbw / pw, sy = (float)fbh / ph;
	const bool scaled = sx < 0.999f || sx > 1.001f || sy < 0.999f || sy > 1.001f;
	for (int i = 0; i < kLayers; ++i) {
		const Layer &l = g_hdr->layers[i];
		LayerTex &t = c->tex[i];
		LayerSnap snap;
		if (ReadLayer(l, snap)) {
			t.shown = snap.visible && Upload(t, i, l, snap);
			t.x = snap.x;
			t.y = snap.y;
			t.alpha = snap.alpha;
		} // else the plugin is writing it: draw last frame's pixels, if any
		if (!t.shown || !t.tex || t.x >= phys.right || t.y >= phys.bottom || t.x + t.w <= phys.left ||
		    t.y + t.h <= phys.top)
			continue;
		const float x0 = (t.x - phys.left) * sx, y0 = (t.y - phys.top) * sy;
		const float x1 = x0 + t.w * sx, y1 = y0 + t.h * sy;
		glBindTexture(GL_TEXTURE_2D, t.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, scaled ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, scaled ? GL_LINEAR : GL_NEAREST);
		p_glUniform4f(c->u_rect, x0 / fbw * 2 - 1, 1 - y0 / fbh * 2, x1 / fbw * 2 - 1, 1 - y1 / fbh * 2);
		p_glUniform1f(c->u_alpha, t.alpha / 255.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	s.Restore();
}

void Draw(HDC dc)
{
	if (!Connect())
		return;
	HWND hwnd = WindowFromDC(dc);
	HGLRC rc = wglGetCurrentContext();
	if (!hwnd || !rc || WindowFromDC(wglGetCurrentDC()) != hwnd)
		return; // the game's context isn't current on this window
	const bool any_window = (g_hdr->flags & kFlagAnyWindow) != 0;
	// Behind other windows (alt-tabbed, or another monitor in use), the overlay's own windows
	// show, so leave them to it.
	if (!any_window && GetAncestor(hwnd, GA_ROOT) != GetForegroundWindow())
		return;

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
	const bool fullscreen = phys.left <= m.left && phys.top <= m.top && phys.right >= m.right &&
				phys.bottom >= m.bottom;
	if (!fullscreen && !any_window)
		return; // the overlay's own windows show over windowed games
	if (fb.right <= 0 || fb.bottom <= 0 || phys.right <= phys.left || phys.bottom <= phys.top)
		return;

	AcquireSRWLockExclusive(&g_draw_lock);
	DrawLocked(rc, phys, m, fb.right, fb.bottom);
	ReleaseSRWLockExclusive(&g_draw_lock);
}

// --- the hooks -----------------------------------------------------------------------------
// DrvSwapBuffers in every loaded OpenGL driver (a game on a hybrid-GPU machine can have two),
// each with its own trampoline, plus wglDeleteContext to forget a context's objects.
using SwapFn = BOOL(WINAPI *)(HDC);
using DeleteContextFn = BOOL(WINAPI *)(HGLRC);
constexpr int kMaxIcds = 4;
SwapFn g_real_swap[kMaxIcds] = {};
DeleteContextFn g_real_delete = nullptr;

template<int I> BOOL WINAPI HookSwapBuffers(HDC dc)
{
	Draw(dc);
	return g_real_swap[I](dc);
}
const SwapFn kSwapHooks[kMaxIcds] = {HookSwapBuffers<0>, HookSwapBuffers<1>, HookSwapBuffers<2>,
				     HookSwapBuffers<3>};

BOOL WINAPI HookDeleteContext(HGLRC rc)
{
	AcquireSRWLockExclusive(&g_draw_lock);
	for (Ctx &c : g_ctxs)
		if (c.rc == rc)
			c = Ctx{}; // its objects go with it
	ReleaseSRWLockExclusive(&g_draw_lock);
	return g_real_delete(rc);
}

// The loaded OpenGL drivers: modules other than opengl32.dll exporting the driver entry points.
int FindIcds(HMODULE (&icds)[kMaxIcds])
{
	HMODULE mods[1024];
	DWORD needed = 0;
	if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
		return 0;
	const HMODULE ogl = GetModuleHandleW(L"opengl32.dll");
	int n = 0;
	for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024 && n < kMaxIcds; ++i) {
		if (mods[i] != ogl && GetProcAddress(mods[i], "DrvSwapBuffers") &&
		    GetProcAddress(mods[i], "DrvSetContext"))
			icds[n++] = mods[i];
	}
	return n;
}

DWORD WINAPI InstallThread(void *)
{
	// Open the shared memory first, so what we report below reaches OBS's log. The render
	// thread only touches it once the hook is in.
	OpenShared();
	// The plugin only loads us once the game has an OpenGL driver loaded; wait a little anyway.
	HMODULE icds[kMaxIcds] = {};
	int n = 0;
	for (int i = 0; i < 50 && !(n = FindIcds(icds)); ++i)
		Sleep(200);
	if (!n) {
		Report("no OpenGL driver loaded in the game");
		return 0;
	}
	HOOK_INFO hooks[kMaxIcds + 1];
	SwapFn originals[kMaxIcds] = {};
	int count = 0;
	char names[100] = "";
	for (int i = 0; i < n; ++i) {
		originals[i] = g_real_swap[i] = reinterpret_cast<SwapFn>(GetProcAddress(icds[i], "DrvSwapBuffers"));
		hooks[count++] = {reinterpret_cast<PVOID *>(&g_real_swap[i]), reinterpret_cast<PVOID>(kSwapHooks[i])};
		char name[MAX_PATH] = "?";
		GetModuleBaseNameA(GetCurrentProcess(), icds[i], name, MAX_PATH);
		if (i)
			strncat_s(names, ", ", _TRUNCATE);
		strncat_s(names, name, _TRUNCATE);
	}
	g_real_delete = reinterpret_cast<DeleteContextFn>(
		GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglDeleteContext"));
	if (g_real_delete)
		hooks[count++] = {reinterpret_cast<PVOID *>(&g_real_delete),
				  reinterpret_cast<PVOID>(HookDeleteContext)};
	Mhook_SetHookEx(hooks, count);
	// A hook is in once its pointer has been swapped for the trampoline.
	int hooked = 0;
	for (int i = 0; i < n; ++i)
		hooked += g_real_swap[i] != originals[i];
	if (hooked < n)
		Report("hooked DrvSwapBuffers in %d of %d drivers (%s)", hooked, n, names);
	else
		Report("hooked DrvSwapBuffers in %s; waiting for the game to go fullscreen", names);
	return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		// Pin: the hooks jump into this DLL, so it must never be unloaded.
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				   reinterpret_cast<LPCWSTR>(&DllMain), &self);
		if (HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr))
			CloseHandle(t);
	}
	return TRUE;
}
