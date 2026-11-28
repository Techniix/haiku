/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27, Steam groundwork): OpenGL ES 2 through the Debian
 * root's libglvnd and Mesa, without a display: EGL's surfaceless platform,
 * a context without a surface, a framebuffer object on a texture, a cleared colour and a
 * triangle from a shader program, read back with glReadPixels(). Mesa's
 * llvmpipe compiles the shaders with LLVM's JIT (executable mappings made
 * at run time under Renguin). Prints one line per check, then "ok".
 */
#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <string.h>

static int sFailures = 0;

static void
check(const char* name, int pass, const char* detail)
{
	if (pass)
		printf("%s ok\n", name);
	else {
		printf("%s FAIL: %s\n", name, detail);
		sFailures++;
	}
	fflush(stdout);
}

static const char* kVertex =
	"attribute vec2 position;\n"
	"void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
static const char* kFragment =
	"precision mediump float;\n"
	"void main() { gl_FragColor = vec4(1.0, 0.5, 0.0, 1.0); }\n";

static GLuint
shader(GLenum type, const char* source)
{
	GLuint result = glCreateShader(type);
	glShaderSource(result, 1, &source, NULL);
	glCompileShader(result);
	GLint compiled = 0;
	glGetShaderiv(result, GL_COMPILE_STATUS, &compiled);
	return compiled ? result : 0;
}

int
main(void)
{
	char detail[256];
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay
		= (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
			"eglGetPlatformDisplayEXT");
	EGLDisplay display = getPlatformDisplay != NULL
		? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
			NULL)
		: EGL_NO_DISPLAY;
	EGLint major = 0, minor = 0;
	int initialised = display != EGL_NO_DISPLAY
		&& eglInitialize(display, &major, &minor);
	snprintf(detail, sizeof(detail), "display %p, error 0x%x", display,
		eglGetError());
	check("egl", initialised, detail);
	if (!initialised)
		return 1;

	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint configAttributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE };
	EGLConfig config;
	EGLint configs = 0;
	eglChooseConfig(display, configAttributes, &config, 1, &configs);
	EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext context = eglCreateContext(display,
		configs > 0 ? config : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
		contextAttributes);
	int current = context != EGL_NO_CONTEXT && eglMakeCurrent(display,
		EGL_NO_SURFACE, EGL_NO_SURFACE, context);
	snprintf(detail, sizeof(detail), "%d configs, error 0x%x", configs,
		eglGetError());
	check("context", current, detail);
	if (!current)
		return 1;

	const char* renderer = (const char*)glGetString(GL_RENDERER);
	snprintf(detail, sizeof(detail), "GL_RENDERER %s",
		renderer != NULL ? renderer : "NULL");
	check("llvmpipe", renderer != NULL && strstr(renderer, "llvmpipe") != NULL,
		detail);

	enum { kSize = 64 };
	GLuint framebuffer, colour;
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glGenTextures(1, &colour);
	glBindTexture(GL_TEXTURE_2D, colour);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSize, kSize, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, NULL);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, colour, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glViewport(0, 0, kSize, kSize);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	unsigned char pixel[4] = { 0 };
	glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	snprintf(detail, sizeof(detail), "framebuffer 0x%x, pixel %u %u %u",
		status, pixel[0], pixel[1], pixel[2]);
	check("clear", status == GL_FRAMEBUFFER_COMPLETE && pixel[0] == 0
		&& pixel[1] == 0 && pixel[2] == 255, detail);

	GLuint vertex = shader(GL_VERTEX_SHADER, kVertex);
	GLuint fragment = shader(GL_FRAGMENT_SHADER, kFragment);
	GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, 0, "position");
	glLinkProgram(program);
	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	static const GLfloat kTriangle[] = { -1.0f, -1.0f, 1.0f, -1.0f, 0.0f,
		1.0f };
	glUseProgram(program);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kTriangle);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	unsigned char centre[4] = { 0 }, corner[4] = { 0 };
	glReadPixels(kSize / 2, kSize / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, centre);
	glReadPixels(1, kSize - 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
	snprintf(detail, sizeof(detail), "shaders %u/%u linked %d, centre %u %u "
		"%u, corner %u %u %u, error 0x%x", vertex, fragment, linked,
		centre[0], centre[1], centre[2], corner[0], corner[1], corner[2],
		glGetError());
	check("triangle", linked && centre[0] == 255 && centre[1] >= 0x7f
		&& centre[1] <= 0x80 && centre[2] == 0 && corner[2] == 255, detail);

	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglTerminate(display);
	if (sFailures == 0)
		printf("ok\n");
	return sFailures == 0 ? 0 : 1;
}
