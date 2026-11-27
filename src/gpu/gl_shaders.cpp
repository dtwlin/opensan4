#include "gpu/gl_shaders.h"

namespace san4::gpu {

const char* fullscreen_vertex_shader_src() noexcept {
  return R"GLSL(#version 330 core
uniform int u_flip_y;
out vec2 v_uv;

void main() {
  float x = float((gl_VertexID & 1) << 2) - 1.0;
  float y = float((gl_VertexID & 2) << 1) - 1.0;
  vec2 uv = vec2((x + 1.0) * 0.5, (y + 1.0) * 0.5);
  if (u_flip_y != 0) {
    uv.y = 1.0 - uv.y;
  }
  v_uv = uv;
  gl_Position = vec4(x, y, 0.0, 1.0);
}
)GLSL";
}

const char* upscale_composite_fragment_shader_src() noexcept {
  return R"GLSL(#version 330 core
uniform usampler2D u_vram;
uniform usampler2D u_glyph;
uniform usampler2D u_hd_canvas;
uniform usampler2D u_hd_lut_idx;
uniform usampler2D u_luts;

uniform uvec4 u_palette[16];
uniform int u_lut_slot_map[256];
uniform ivec2 u_src_size;
uniform int u_mode;
uniform int u_scale;
uniform int u_hd_active;

in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

uint sample_vram(int x, int y) {
  int cx = clamp(x, 0, u_src_size.x - 1);
  int cy = clamp(y, 0, u_src_size.y - 1);
  return texelFetch(u_vram, ivec2(cx, cy), 0).r & 15u;
}

bool is_glyph_tile(int x, int y) {
  return texelFetch(u_glyph, ivec2(x >> 4, y >> 4), 0).r != 0u;
}

int color_dist(uint p, uint q) {
  ivec3 dp = ivec3(u_palette[p].rgb) - ivec3(u_palette[q].rgb);
  return 3 * dp.r * dp.r + 4 * dp.g * dp.g + 2 * dp.b * dp.b;
}

uvec3 blend50(uvec3 a, uvec3 b) {
  return (((a ^ b) & uvec3(254u)) >> 1u) + (a & b);
}

uvec3 blend75(uvec3 primary, uvec3 secondary) {
  return blend50(primary, blend50(primary, secondary));
}

bool is_checkerboard_dither(uint a, uint b, uint c, uint d, uint e,
                            uint f, uint g, uint h, uint i) {
  return (e == a && b == d && e != b) ||
         (e == c && b == f && e != b) ||
         (e == g && d == h && e != d) ||
         (e == i && f == h && e != f);
}

bool xbr_corner(uint e, uint p_diag, uint p_h, uint p_v, uint p_opp_v,
                uint p_opp_h, uint p_far_h1, uint p_far_h2, uint p_far_v1,
                uint p_far_v2, out uvec3 out_color) {
  out_color = u_palette[e].rgb;
  if (e == p_h && e == p_v) {
    return false;
  }
  int wd1 = color_dist(e, p_diag) + color_dist(e, p_opp_v) +
            color_dist(e, p_opp_h) + color_dist(p_far_v2, p_h) +
            color_dist(p_far_h2, p_v) + 4 * color_dist(p_v, p_h);
  int wd2 = color_dist(p_v, p_far_v1) + color_dist(p_v, p_opp_h) +
            color_dist(p_h, p_far_h1) + color_dist(p_h, p_opp_v) +
            4 * color_dist(e, p_diag);
  if (wd1 < wd2) {
    uint cand = (color_dist(e, p_h) <= color_dist(e, p_v)) ? p_h : p_v;
    if (cand != e) {
      out_color = u_palette[cand].rgb;
      return true;
    }
  }
  return false;
}

void fill_4x_corner(inout uvec3 block[16], bool active, uvec3 pe, uvec3 cc,
                    uint e, uint d_px, uint f, uint p_h, uint p_v,
                    uint p_adj_h, uint p_adj_v, int r_outer, int c_outer,
                    int r_step, int c_step) {
  if (!active) {
    return;
  }
  int fg = color_dist(p_h, p_adj_v);
  int hc = color_dist(p_v, p_adj_h);
  bool steep_v = (2 * fg <= hc) && (e != p_adj_v) &&
                 (d_px != p_adj_v || f != p_adj_v);
  bool steep_h = (2 * hc <= fg) && (e != p_adj_h);

  block[r_outer * 4 + c_outer] = cc;
  if (steep_v && steep_h) {
    block[(r_outer + r_step) * 4 + c_outer] = blend75(cc, pe);
    block[r_outer * 4 + (c_outer + c_step)] = blend75(cc, pe);
    block[(r_outer + r_step) * 4 + (c_outer + c_step)] = blend50(cc, pe);
  } else if (steep_v) {
    block[(r_outer + r_step) * 4 + c_outer] = cc;
    block[r_outer * 4 + (c_outer + c_step)] = blend50(cc, pe);
    block[(r_outer + r_step * 2) * 4 + c_outer] = blend50(cc, pe);
  } else if (steep_h) {
    block[r_outer * 4 + (c_outer + c_step)] = cc;
    block[(r_outer + r_step) * 4 + c_outer] = blend50(cc, pe);
    block[r_outer * 4 + (c_outer + c_step * 2)] = blend50(cc, pe);
  } else {
    block[(r_outer + r_step) * 4 + c_outer] = blend50(cc, pe);
    block[r_outer * 4 + (c_outer + c_step)] = blend50(cc, pe);
  }
}

uvec3 upscale_base_pixel(int ox, int oy, int sx, int sy, int sub_x, int sub_y) {
  uint e = sample_vram(sx, sy);
  uvec3 pe = u_palette[e].rgb;

  if (u_mode == 0 || u_mode == 1) {
    return pe;
  }
  if (is_glyph_tile(sx, sy)) {
    return pe;
  }

  if (u_mode == 2) {
    uint B = sample_vram(sx, sy - 1);
    uint D = sample_vram(sx - 1, sy);
    uint F = sample_vram(sx + 1, sy);
    uint Hh = sample_vram(sx, sy + 1);
    uint sel = e;
    if (B != Hh && D != F) {
      if (sub_y == 0) {
        if (sub_x == 0) {
          if (D == B) sel = D;
        } else {
          if (B == F) sel = F;
        }
      } else {
        if (sub_x == 0) {
          if (D == Hh) sel = D;
        } else {
          if (Hh == F) sel = F;
        }
      }
    }
    return u_palette[sel].rgb;
  }

  uint a = sample_vram(sx - 1, sy - 1);
  uint b = sample_vram(sx, sy - 1);
  uint c = sample_vram(sx + 1, sy - 1);
  uint d_px = sample_vram(sx - 1, sy);
  uint f = sample_vram(sx + 1, sy);
  uint g = sample_vram(sx - 1, sy + 1);
  uint h = sample_vram(sx, sy + 1);
  uint i = sample_vram(sx + 1, sy + 1);

  if (a == e && b == e && c == e && d_px == e &&
      f == e && g == e && h == e && i == e) {
    return pe;
  }
  if (is_checkerboard_dither(a, b, c, d_px, e, f, g, h, i)) {
    return pe;
  }

  uint a1 = sample_vram(sx - 1, sy - 2);
  uint b1 = sample_vram(sx, sy - 2);
  uint c1 = sample_vram(sx + 1, sy - 2);
  uint a0 = sample_vram(sx - 2, sy - 1);
  uint c4 = sample_vram(sx + 2, sy - 1);
  uint d0 = sample_vram(sx - 2, sy);
  uint f4 = sample_vram(sx + 2, sy);
  uint g0 = sample_vram(sx - 2, sy + 1);
  uint i4 = sample_vram(sx + 2, sy + 1);
  uint g5 = sample_vram(sx - 1, sy + 2);
  uint h5 = sample_vram(sx, sy + 2);
  uint i5 = sample_vram(sx + 1, sy + 2);

  if (u_mode == 3) {
    uvec3 cc = pe;
    bool ed = false;
    if (sub_y == 0) {
      if (sub_x == 0) {
        ed = xbr_corner(e, a, d_px, b, g, c, a0, d0, a1, b1, cc);
      } else {
        ed = xbr_corner(e, c, f, b, i, a, c4, f4, c1, b1, cc);
      }
    } else {
      if (sub_x == 0) {
        ed = xbr_corner(e, g, d_px, h, a, i, g0, d0, g5, h5, cc);
      } else {
        ed = xbr_corner(e, i, f, h, c, g, i4, f4, i5, h5, cc);
      }
    }
    return ed ? blend50(pe, cc) : pe;
  }

  uvec3 col_br = pe;
  uvec3 col_bl = pe;
  uvec3 col_tr = pe;
  uvec3 col_tl = pe;
  bool ed_br = xbr_corner(e, i, f, h, c, g, i4, f4, i5, h5, col_br);
  bool ed_bl = xbr_corner(e, g, d_px, h, a, i, g0, d0, g5, h5, col_bl);
  bool ed_tr = xbr_corner(e, c, f, b, i, a, c4, f4, c1, b1, col_tr);
  bool ed_tl = xbr_corner(e, a, d_px, b, g, c, a0, d0, a1, b1, col_tl);

  uvec3 block[16];
  for (int k = 0; k < 16; ++k) {
    block[k] = pe;
  }
  fill_4x_corner(block, ed_tl, pe, col_tl, e, d_px, f, d_px, b, c, g, 0, 0, 1, 1);
  fill_4x_corner(block, ed_tr, pe, col_tr, e, d_px, f, f, b, a, i, 0, 3, 1, -1);
  fill_4x_corner(block, ed_bl, pe, col_bl, e, d_px, f, d_px, h, i, a, 3, 0, -1, 1);
  fill_4x_corner(block, ed_br, pe, col_br, e, d_px, f, f, h, c, g, 3, 3, -1, -1);
  return block[sub_y * 4 + sub_x];
}

// Host little-endian 0xAARRGGBB uploaded as GL_RGBA_INTEGER + GL_UNSIGNED_BYTE
// lands in RGBA channels as (B, G, R, A); .bgra restores exact (R, G, B, A).
uvec4 fetch_hd_argb(int hx, int hy) {
  return texelFetch(u_hd_canvas, ivec2(hx, hy), 0).bgra;
}

ivec3 fetch_lut_rgb(int lut_tile, int ir, int ig, int ib) {
  int base_x = (lut_tile & 31) * 17;
  int base_y = (lut_tile >> 5) * 289;
  uvec4 raw = texelFetch(u_luts, ivec2(base_x + ib, base_y + ir * 17 + ig), 0);
  return ivec3(raw.bgr);
}

uvec3 sample_lut_trilinear(int lut_tile, uvec3 rgb) {
  int r = int(rgb.r);
  int g = int(rgb.g);
  int b = int(rgb.b);

  int ri = (r * 16) / 255;
  int gi = (g * 16) / 255;
  int bi = (b * 16) / 255;
  int r1 = min(ri + 1, 16);
  int g1 = min(gi + 1, 16);
  int b1 = min(bi + 1, 16);

  int rf = r * 16 - ri * 255;
  int gf = g * 16 - gi * 255;
  int bf = b * 16 - bi * 255;

  ivec3 c000 = fetch_lut_rgb(lut_tile, ri, gi, bi);
  ivec3 c001 = fetch_lut_rgb(lut_tile, ri, gi, b1);
  ivec3 c010 = fetch_lut_rgb(lut_tile, ri, g1, bi);
  ivec3 c011 = fetch_lut_rgb(lut_tile, ri, g1, b1);
  ivec3 c100 = fetch_lut_rgb(lut_tile, r1, gi, bi);
  ivec3 c101 = fetch_lut_rgb(lut_tile, r1, gi, b1);
  ivec3 c110 = fetch_lut_rgb(lut_tile, r1, g1, bi);
  ivec3 c111 = fetch_lut_rgb(lut_tile, r1, g1, b1);

  ivec3 v00 = (c000 * (255 - bf) + c001 * bf + ivec3(127)) / 255;
  ivec3 v01 = (c010 * (255 - bf) + c011 * bf + ivec3(127)) / 255;
  ivec3 v10 = (c100 * (255 - bf) + c101 * bf + ivec3(127)) / 255;
  ivec3 v11 = (c110 * (255 - bf) + c111 * bf + ivec3(127)) / 255;

  ivec3 v0 = (v00 * (255 - gf) + v01 * gf + ivec3(127)) / 255;
  ivec3 v1 = (v10 * (255 - gf) + v11 * gf + ivec3(127)) / 255;

  ivec3 v = (v0 * (255 - rf) + v1 * rf + ivec3(127)) / 255;
  return uvec3(clamp(v, ivec3(0), ivec3(255)));
}

uvec4 sample_hd_for_output(int ox, int oy) {
  if (u_scale == 4) {
    return fetch_hd_argb(ox, oy);
  }
  int ds = 4 / u_scale;
  int hx0 = ox * ds;
  int hy0 = oy * ds;
  uint sum_a = 0u;
  uvec3 sum_rgb = uvec3(0u);
  uint cnt_a = 0u;
  for (int by = 0; by < ds; ++by) {
    for (int bx = 0; bx < ds; ++bx) {
      uvec4 p = fetch_hd_argb(hx0 + bx, hy0 + by);
      uint pa = p.a;
      sum_a += pa;
      if (pa > 0u) {
        sum_rgb += p.rgb * pa;
        cnt_a += pa;
      }
    }
  }
  uint nsub = uint(ds * ds);
  uint avg_a = (sum_a + (nsub >> 1u)) / nsub;
  if (avg_a == 0u || cnt_a == 0u) {
    return uvec4(0u);
  }
  uvec3 avg_rgb = (sum_rgb + uvec3(cnt_a >> 1u)) / cnt_a;
  return uvec4(avg_rgb, avg_a);
}

void main() {
  ivec2 out_xy = ivec2(gl_FragCoord.xy);
  int ox = out_xy.x;
  int oy = out_xy.y;
  int sx = ox / u_scale;
  int sy = oy / u_scale;
  int sub_x = ox - sx * u_scale;
  int sub_y = oy - sy * u_scale;

  uvec3 dst_rgb = upscale_base_pixel(ox, oy, sx, sy, sub_x, sub_y);

  if (u_hd_active != 0 && sx < 800 && sy < 600) {
    uint key_slot = texelFetch(u_hd_lut_idx, ivec2(sx, sy), 0).r;
    if (key_slot > 0u && key_slot <= 256u) {
      uvec4 hd = sample_hd_for_output(ox, oy);
      if (hd.a > 0u) {
        uvec3 graded = hd.rgb;
        int lut_tile = u_lut_slot_map[int(key_slot - 1u)];
        if (lut_tile >= 0) {
          graded = sample_lut_trilinear(lut_tile, graded);
        }
        if (hd.a == 255u) {
          dst_rgb = graded;
        } else {
          uint inv_a = 255u - hd.a;
          dst_rgb = (graded * hd.a + dst_rgb * inv_a + uvec3(127u)) / 255u;
        }
      }
    }
  }

  frag_color = vec4(vec3(dst_rgb) * (1.0 / 255.0), 1.0);
}
)GLSL";
}

const char* present_fragment_shader_src() noexcept {
  return R"GLSL(#version 330 core
uniform sampler2D u_frame;
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main() {
  frag_color = vec4(texture(u_frame, v_uv).rgb, 1.0);
}
)GLSL";
}

}  // namespace san4::gpu
