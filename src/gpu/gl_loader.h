#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#if defined(_WIN32)
#define SAN4_GL_APIENTRY __stdcall
#else
#define SAN4_GL_APIENTRY
#endif

namespace san4::gpu {

// Minimal self-contained OpenGL 3.3 Core types and constants so OpenSan4 links
// zero static GL libraries (-lGL / opengl32.dll) and needs no system GL headers.
using GLenum = std::uint32_t;
using GLboolean = std::uint8_t;
using GLbitfield = std::uint32_t;
using GLint = std::int32_t;
using GLuint = std::uint32_t;
using GLsizei = std::int32_t;
using GLfloat = float;
using GLchar = char;
using GLubyte = std::uint8_t;
using GLsizeiptr = std::ptrdiff_t;

inline constexpr GLboolean kGlFalse = 0u;
inline constexpr GLboolean kGlTrue = 1u;
inline constexpr GLenum kGlNoError = 0u;

inline constexpr GLbitfield kGlColorBufferBit = 0x00004000u;
inline constexpr GLenum kGlTriangles = 0x0004u;
inline constexpr GLenum kGlScissorTest = 0x0C11u;
inline constexpr GLenum kGlUnpackRowLength = 0x0CF2u;
inline constexpr GLenum kGlUnpackSkipRows = 0x0CF3u;
inline constexpr GLenum kGlUnpackSkipPixels = 0x0CF4u;
inline constexpr GLenum kGlUnpackAlignment = 0x0CF5u;
inline constexpr GLenum kGlPackRowLength = 0x0D02u;
inline constexpr GLenum kGlPackAlignment = 0x0D05u;
inline constexpr GLenum kGlMaxTextureSize = 0x0D33u;
inline constexpr GLenum kGlTexture2D = 0x0DE1u;

inline constexpr GLenum kGlUnsignedByte = 0x1401u;
inline constexpr GLenum kGlUnsignedShort = 0x1403u;
inline constexpr GLenum kGlRgba = 0x1908u;
inline constexpr GLenum kGlVendor = 0x1F00u;
inline constexpr GLenum kGlRenderer = 0x1F01u;
inline constexpr GLenum kGlVersion = 0x1F02u;

inline constexpr GLenum kGlNearest = 0x2600u;
inline constexpr GLenum kGlLinear = 0x2601u;
inline constexpr GLenum kGlTextureMagFilter = 0x2800u;
inline constexpr GLenum kGlTextureMinFilter = 0x2801u;
inline constexpr GLenum kGlTextureWrapS = 0x2802u;
inline constexpr GLenum kGlTextureWrapT = 0x2803u;

inline constexpr GLenum kGlRgba8 = 0x8058u;
inline constexpr GLenum kGlBgra = 0x80E1u;
inline constexpr GLenum kGlClampToEdge = 0x812Fu;
inline constexpr GLenum kGlMajorVersion = 0x821Bu;
inline constexpr GLenum kGlMinorVersion = 0x821Cu;
inline constexpr GLenum kGlR8ui = 0x8232u;
inline constexpr GLenum kGlR16ui = 0x8234u;
inline constexpr GLenum kGlTexture0 = 0x84C0u;

inline constexpr GLenum kGlFragmentShader = 0x8B30u;
inline constexpr GLenum kGlVertexShader = 0x8B31u;
inline constexpr GLenum kGlCompileStatus = 0x8B81u;
inline constexpr GLenum kGlLinkStatus = 0x8B82u;
inline constexpr GLenum kGlInfoLogLength = 0x8B84u;

inline constexpr GLenum kGlFramebufferComplete = 0x8CD5u;
inline constexpr GLenum kGlColorAttachment0 = 0x8CE0u;
inline constexpr GLenum kGlFramebuffer = 0x8D40u;

inline constexpr GLenum kGlRgba8ui = 0x8D7Cu;
inline constexpr GLenum kGlRedInteger = 0x8D94u;
inline constexpr GLenum kGlRgbaInteger = 0x8D99u;
inline constexpr GLenum kGlBgraInteger = 0x8D9Bu;

using GlGetProcFn = void* (*)(const char*);

// Parses major.minor from an OpenGL GL_VERSION string (e.g. "4.6.0 NVIDIA 550"
// or "OpenGL ES 3.2 ..."). Returns false if no major.minor pair is present.
bool parse_gl_version(std::string_view version_str, int& out_major,
                      int& out_minor) noexcept;

struct GlProcs {
  const GLubyte* (SAN4_GL_APIENTRY* GetString)(GLenum name) = nullptr;
  void (SAN4_GL_APIENTRY* GetIntegerv)(GLenum pname, GLint* data) = nullptr;
  GLenum (SAN4_GL_APIENTRY* GetError)() = nullptr;
  void (SAN4_GL_APIENTRY* Enable)(GLenum cap) = nullptr;
  void (SAN4_GL_APIENTRY* Disable)(GLenum cap) = nullptr;
  void (SAN4_GL_APIENTRY* Viewport)(GLint x, GLint y, GLsizei w,
                                    GLsizei h) = nullptr;
  void (SAN4_GL_APIENTRY* Scissor)(GLint x, GLint y, GLsizei w,
                                   GLsizei h) = nullptr;
  void (SAN4_GL_APIENTRY* ClearColor)(GLfloat r, GLfloat g, GLfloat b,
                                      GLfloat a) = nullptr;
  void (SAN4_GL_APIENTRY* Clear)(GLbitfield mask) = nullptr;
  void (SAN4_GL_APIENTRY* PixelStorei)(GLenum pname, GLint param) = nullptr;
  void (SAN4_GL_APIENTRY* ActiveTexture)(GLenum texture) = nullptr;
  void (SAN4_GL_APIENTRY* GenTextures)(GLsizei n, GLuint* textures) = nullptr;
  void (SAN4_GL_APIENTRY* DeleteTextures)(GLsizei n,
                                          const GLuint* textures) = nullptr;
  void (SAN4_GL_APIENTRY* BindTexture)(GLenum target,
                                       GLuint texture) = nullptr;
  void (SAN4_GL_APIENTRY* TexParameteri)(GLenum target, GLenum pname,
                                         GLint param) = nullptr;
  void (SAN4_GL_APIENTRY* TexImage2D)(GLenum target, GLint level,
                                      GLint internalformat, GLsizei width,
                                      GLsizei height, GLint border,
                                      GLenum format, GLenum type,
                                      const void* pixels) = nullptr;
  void (SAN4_GL_APIENTRY* TexSubImage2D)(GLenum target, GLint level,
                                         GLint xoffset, GLint yoffset,
                                         GLsizei width, GLsizei height,
                                         GLenum format, GLenum type,
                                         const void* pixels) = nullptr;
  void (SAN4_GL_APIENTRY* GenFramebuffers)(GLsizei n, GLuint* ids) = nullptr;
  void (SAN4_GL_APIENTRY* DeleteFramebuffers)(GLsizei n,
                                              const GLuint* ids) = nullptr;
  void (SAN4_GL_APIENTRY* BindFramebuffer)(GLenum target,
                                           GLuint framebuffer) = nullptr;
  void (SAN4_GL_APIENTRY* FramebufferTexture2D)(GLenum target,
                                                GLenum attachment,
                                                GLenum textarget,
                                                GLuint texture,
                                                GLint level) = nullptr;
  GLenum (SAN4_GL_APIENTRY* CheckFramebufferStatus)(GLenum target) = nullptr;
  void (SAN4_GL_APIENTRY* ReadPixels)(GLint x, GLint y, GLsizei width,
                                      GLsizei height, GLenum format,
                                      GLenum type, void* data) = nullptr;
  void (SAN4_GL_APIENTRY* GenVertexArrays)(GLsizei n, GLuint* arrays) = nullptr;
  void (SAN4_GL_APIENTRY* DeleteVertexArrays)(GLsizei n,
                                              const GLuint* arrays) = nullptr;
  void (SAN4_GL_APIENTRY* BindVertexArray)(GLuint array) = nullptr;
  void (SAN4_GL_APIENTRY* DrawArrays)(GLenum mode, GLint first,
                                      GLsizei count) = nullptr;

  GLuint (SAN4_GL_APIENTRY* CreateShader)(GLenum shaderType) = nullptr;
  void (SAN4_GL_APIENTRY* DeleteShader)(GLuint shader) = nullptr;
  void (SAN4_GL_APIENTRY* ShaderSource)(GLuint shader, GLsizei count,
                                        const GLchar* const* string,
                                        const GLint* length) = nullptr;
  void (SAN4_GL_APIENTRY* CompileShader)(GLuint shader) = nullptr;
  void (SAN4_GL_APIENTRY* GetShaderiv)(GLuint shader, GLenum pname,
                                       GLint* params) = nullptr;
  void (SAN4_GL_APIENTRY* GetShaderInfoLog)(GLuint shader, GLsizei maxLength,
                                            GLsizei* length,
                                            GLchar* infoLog) = nullptr;
  GLuint (SAN4_GL_APIENTRY* CreateProgram)() = nullptr;
  void (SAN4_GL_APIENTRY* DeleteProgram)(GLuint program) = nullptr;
  void (SAN4_GL_APIENTRY* AttachShader)(GLuint program, GLuint shader) = nullptr;
  void (SAN4_GL_APIENTRY* LinkProgram)(GLuint program) = nullptr;
  void (SAN4_GL_APIENTRY* GetProgramiv)(GLuint program, GLenum pname,
                                        GLint* params) = nullptr;
  void (SAN4_GL_APIENTRY* GetProgramInfoLog)(GLuint program, GLsizei maxLength,
                                             GLsizei* length,
                                             GLchar* infoLog) = nullptr;
  void (SAN4_GL_APIENTRY* UseProgram)(GLuint program) = nullptr;
  GLint (SAN4_GL_APIENTRY* GetUniformLocation)(GLuint program,
                                               const GLchar* name) = nullptr;
  void (SAN4_GL_APIENTRY* Uniform1i)(GLint location, GLint v0) = nullptr;
  void (SAN4_GL_APIENTRY* Uniform1iv)(GLint location, GLsizei count,
                                      const GLint* value) = nullptr;
  void (SAN4_GL_APIENTRY* Uniform2i)(GLint location, GLint v0,
                                     GLint v1) = nullptr;
  void (SAN4_GL_APIENTRY* Uniform4uiv)(GLint location, GLsizei count,
                                       const GLuint* value) = nullptr;

  bool load(GlGetProcFn get_proc, std::string* missing_symbol = nullptr);
};

}  // namespace san4::gpu
