#include <stdint.h>

/*
 * Android-side library boundary for Roblox graphics.  These functions are
 * intentionally compiled with the NDK, so linker64 sees Android ELF DSOs.
 * Their command transport will connect to Nuah's Vulkan bridge next; for now
 * they make the dependency boundary explicit without loading host Mesa into
 * bionic's namespace.
 */
#if defined(NUAH_EGL_PROXY)
void* eglGetDisplay(void* native_display) { (void)native_display; return (void*)1; }
int eglInitialize(void* display, int* major, int* minor) {
  (void)display; if (major) *major = 1; if (minor) *minor = 5; return 1;
}
int eglTerminate(void* display) { (void)display; return 1; }
int eglGetError(void) { return 0x3000; }
void* eglGetProcAddress(const char* name) { (void)name; return 0; }
void* eglGetCurrentContext(void) { return 0; }
#define EGL_STUB(name) void name(void) {}
EGL_STUB(eglChooseConfig) EGL_STUB(eglCreateContext)
EGL_STUB(eglCreatePbufferSurface) EGL_STUB(eglCreateWindowSurface)
EGL_STUB(eglDestroyContext) EGL_STUB(eglDestroySurface)
EGL_STUB(eglGetConfigAttrib) EGL_STUB(eglMakeCurrent)
EGL_STUB(eglQuerySurface) EGL_STUB(eglSwapBuffers) EGL_STUB(eglSwapInterval)
#endif

#if defined(NUAH_GLES_PROXY)
int glGetError(void) { return 0; }
const unsigned char* glGetString(unsigned int name) { (void)name; return (const unsigned char*)"Nuah"; }
void glGetIntegerv(unsigned int name, int* value) { (void)name; if (value) *value = 0; }
unsigned int glCreateProgram(void) { return 1; }
unsigned int glCreateShader(unsigned int type) { (void)type; return 1; }
int glGetUniformLocation(void) { return -1; }
#define GL_STUB(name) void name(void) {}
GL_STUB(glActiveTexture) GL_STUB(glAttachShader) GL_STUB(glBindAttribLocation)
GL_STUB(glBindBuffer) GL_STUB(glBindFramebuffer) GL_STUB(glBindRenderbuffer)
GL_STUB(glBindTexture) GL_STUB(glBlendFunc) GL_STUB(glBlendFuncSeparate)
GL_STUB(glBufferData) GL_STUB(glBufferSubData) GL_STUB(glCheckFramebufferStatus)
GL_STUB(glClear) GL_STUB(glClearColor) GL_STUB(glClearDepthf) GL_STUB(glClearStencil)
GL_STUB(glColorMask) GL_STUB(glCompileShader) GL_STUB(glCompressedTexImage2D)
GL_STUB(glCompressedTexSubImage2D) GL_STUB(glCopyTexSubImage2D) GL_STUB(glCullFace)
GL_STUB(glDeleteBuffers) GL_STUB(glDeleteFramebuffers) GL_STUB(glDeleteProgram)
GL_STUB(glDeleteRenderbuffers) GL_STUB(glDeleteShader) GL_STUB(glDeleteTextures)
GL_STUB(glDepthFunc) GL_STUB(glDepthMask) GL_STUB(glDisable)
GL_STUB(glDisableVertexAttribArray) GL_STUB(glDrawArrays) GL_STUB(glDrawElements)
GL_STUB(glEnable) GL_STUB(glEnableVertexAttribArray) GL_STUB(glFramebufferRenderbuffer)
GL_STUB(glFramebufferTexture2D) GL_STUB(glGenBuffers) GL_STUB(glGenFramebuffers)
GL_STUB(glGenRenderbuffers) GL_STUB(glGenTextures) GL_STUB(glGenerateMipmap)
GL_STUB(glGetActiveUniform) GL_STUB(glGetProgramInfoLog) GL_STUB(glGetProgramiv)
GL_STUB(glGetShaderInfoLog) GL_STUB(glGetShaderiv) GL_STUB(glLinkProgram)
GL_STUB(glPixelStorei) GL_STUB(glPolygonOffset) GL_STUB(glReadPixels)
GL_STUB(glReleaseShaderCompiler) GL_STUB(glRenderbufferStorage) GL_STUB(glScissor)
GL_STUB(glShaderSource) GL_STUB(glStencilFunc) GL_STUB(glStencilMask)
GL_STUB(glStencilOp) GL_STUB(glTexImage2D) GL_STUB(glTexParameterf)
GL_STUB(glTexParameterfv) GL_STUB(glTexParameteri) GL_STUB(glTexSubImage2D)
GL_STUB(glUniform1i) GL_STUB(glUseProgram) GL_STUB(glVertexAttribPointer)
GL_STUB(glViewport)
#endif
