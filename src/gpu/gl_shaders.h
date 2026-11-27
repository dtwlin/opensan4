#pragma once

namespace san4::gpu {

// Vendor-neutral OpenGL 3.3 Core GLSL sources (`#version 330 core`).
// Uses explicit integer arithmetic so GPU Nearest/Sharp2x/Scale2x/Xbr2x/Xbr4x
// upscaling and HD 3D-LUT trilinear grading + alpha compositing match the CPU
// reference implementation bit-for-bit across NVIDIA, AMD, and Intel drivers.

const char* fullscreen_vertex_shader_src() noexcept;
const char* upscale_composite_fragment_shader_src() noexcept;
const char* present_fragment_shader_src() noexcept;

}  // namespace san4::gpu
