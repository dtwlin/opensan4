#include "gpu/gl_loader.h"

#include <cctype>
#include <cstring>

namespace san4::gpu {

bool parse_gl_version(std::string_view version_str, int& out_major,
                      int& out_minor) noexcept {
  out_major = 0;
  out_minor = 0;
  std::size_t i = 0;
  while (i < version_str.size() &&
         !std::isdigit(static_cast<unsigned char>(version_str[i]))) {
    ++i;
  }
  if (i >= version_str.size()) {
    return false;
  }
  int maj = 0;
  while (i < version_str.size() &&
         std::isdigit(static_cast<unsigned char>(version_str[i]))) {
    const int d = version_str[i] - '0';
    if (maj > 1000) return false;
    maj = maj * 10 + d;
    ++i;
  }
  if (i >= version_str.size() || version_str[i] != '.') {
    return false;
  }
  ++i;
  if (i >= version_str.size() ||
      !std::isdigit(static_cast<unsigned char>(version_str[i]))) {
    return false;
  }
  int min_v = 0;
  while (i < version_str.size() &&
         std::isdigit(static_cast<unsigned char>(version_str[i]))) {
    const int d = version_str[i] - '0';
    if (min_v > 1000) return false;
    min_v = min_v * 10 + d;
    ++i;
  }
  out_major = maj;
  out_minor = min_v;
  return true;
}

bool GlProcs::load(GlGetProcFn get_proc, std::string* missing_symbol) {
  *this = GlProcs{};
  if (missing_symbol != nullptr) {
    missing_symbol->clear();
  }
  if (get_proc == nullptr) {
    if (missing_symbol != nullptr) {
      *missing_symbol = "<null-loader>";
    }
    return false;
  }

  auto resolve = [&](auto& dst, const char* name) -> bool {
    void* sym = get_proc(name);
    if (sym == nullptr) {
      if (missing_symbol != nullptr) {
        *missing_symbol = name;
      }
      return false;
    }
    static_assert(sizeof(dst) == sizeof(sym),
                  "Function pointer size must match void*");
    std::memcpy(&dst, &sym, sizeof(dst));
    return true;
  };

  return resolve(GetString, "glGetString") &&
         resolve(GetIntegerv, "glGetIntegerv") &&
         resolve(GetError, "glGetError") &&
         resolve(Enable, "glEnable") &&
         resolve(Disable, "glDisable") &&
         resolve(Viewport, "glViewport") &&
         resolve(Scissor, "glScissor") &&
         resolve(ClearColor, "glClearColor") &&
         resolve(Clear, "glClear") &&
         resolve(PixelStorei, "glPixelStorei") &&
         resolve(ActiveTexture, "glActiveTexture") &&
         resolve(GenTextures, "glGenTextures") &&
         resolve(DeleteTextures, "glDeleteTextures") &&
         resolve(BindTexture, "glBindTexture") &&
         resolve(TexParameteri, "glTexParameteri") &&
         resolve(TexImage2D, "glTexImage2D") &&
         resolve(TexSubImage2D, "glTexSubImage2D") &&
         resolve(GenFramebuffers, "glGenFramebuffers") &&
         resolve(DeleteFramebuffers, "glDeleteFramebuffers") &&
         resolve(BindFramebuffer, "glBindFramebuffer") &&
         resolve(FramebufferTexture2D, "glFramebufferTexture2D") &&
         resolve(CheckFramebufferStatus, "glCheckFramebufferStatus") &&
         resolve(ReadPixels, "glReadPixels") &&
         resolve(GenVertexArrays, "glGenVertexArrays") &&
         resolve(DeleteVertexArrays, "glDeleteVertexArrays") &&
         resolve(BindVertexArray, "glBindVertexArray") &&
         resolve(DrawArrays, "glDrawArrays") &&
         resolve(CreateShader, "glCreateShader") &&
         resolve(DeleteShader, "glDeleteShader") &&
         resolve(ShaderSource, "glShaderSource") &&
         resolve(CompileShader, "glCompileShader") &&
         resolve(GetShaderiv, "glGetShaderiv") &&
         resolve(GetShaderInfoLog, "glGetShaderInfoLog") &&
         resolve(CreateProgram, "glCreateProgram") &&
         resolve(DeleteProgram, "glDeleteProgram") &&
         resolve(AttachShader, "glAttachShader") &&
         resolve(LinkProgram, "glLinkProgram") &&
         resolve(GetProgramiv, "glGetProgramiv") &&
         resolve(GetProgramInfoLog, "glGetProgramInfoLog") &&
         resolve(UseProgram, "glUseProgram") &&
         resolve(GetUniformLocation, "glGetUniformLocation") &&
         resolve(Uniform1i, "glUniform1i") &&
         resolve(Uniform1iv, "glUniform1iv") &&
         resolve(Uniform2i, "glUniform2i") &&
         resolve(Uniform4uiv, "glUniform4uiv");
}

}  // namespace san4::gpu
