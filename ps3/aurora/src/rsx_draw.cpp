// GX draws on RSX: vertex decode, transform, lighting and texgen run on the PPU (porting the
// semantics of aurora's generated WGSL vertex stage), producing clip-space vertices that a
// passthrough vertex program hands to the rasterizer. Face culling happens here too, so the
// result does not depend on RSX window-origin conventions.

#include "rsx_backend.hpp"

#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "internal.hpp"

#include <rsx/rsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace aurora::rsx {
namespace {
using gx::g_gxState;

// Output vertex layout (floats): position xyzw, color0 rgba, color1 rgba, fog, then s t 0 q per texgen.
constexpr uint32_t kOffPos = 0;
constexpr uint32_t kOffColor0 = 4;
constexpr uint32_t kOffColor1 = 8;
constexpr uint32_t kOffFog = 12;
constexpr uint32_t kOffTex = 13;

struct Vec3f {
  float x = 0.f, y = 0.f, z = 0.f;
};

struct DecodedVertex {
  Vec3f pos;
  Vec3f nrm{1.f, 0.f, 0.f};
  Vec3f binormal;
  Vec3f tangent;
  std::array<std::array<float, 4>, 2> color{{{1.f, 1.f, 1.f, 1.f}, {1.f, 1.f, 1.f, 1.f}}};
  std::array<std::array<float, 2>, 8> tex{};
  uint32_t pnMtx = 0;
  std::array<uint32_t, 8> texMtx{};
};

inline uint16_t load16(const uint8_t* p, bool le) {
  return le ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t load32(const uint8_t* p, bool le) {
  return le ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24))
            : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | p[3]);
}

inline float load_component(const uint8_t* p, uint8_t compType, uint8_t frac, bool le, uint32_t index) {
  const float scale = 1.f / static_cast<float>(1u << (frac & 31));
  switch (compType) {
  case GX_U8:
    return static_cast<float>(p[index]) * scale;
  case GX_S8:
    return static_cast<float>(static_cast<int8_t>(p[index])) * scale;
  case GX_U16:
    return static_cast<float>(load16(p + index * 2, le)) * scale;
  case GX_S16:
    return static_cast<float>(static_cast<int16_t>(load16(p + index * 2, le))) * scale;
  case GX_F32: {
    const uint32_t bits = load32(p + index * 4, le);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  default:
    return 0.f;
  }
}

inline uint32_t component_size(uint8_t compType) {
  switch (compType) {
  case GX_U8:
  case GX_S8:
    return 1;
  case GX_U16:
  case GX_S16:
    return 2;
  default:
    return 4;
  }
}

void load_color(const uint8_t* p, uint8_t compType, bool le, std::array<float, 4>& out) {
  constexpr float k255 = 1.f / 255.f;
  switch (compType) {
  case GX_RGB565: {
    const uint16_t v = load16(p, le);
    out = {static_cast<float>((v >> 11) & 0x1F) / 31.f, static_cast<float>((v >> 5) & 0x3F) / 63.f,
           static_cast<float>(v & 0x1F) / 31.f, 1.f};
    break;
  }
  case GX_RGB8:
  case GX_RGBX8:
    out = {p[0] * k255, p[1] * k255, p[2] * k255, 1.f};
    break;
  case GX_RGBA4: {
    const uint16_t v = load16(p, le);
    out = {static_cast<float>((v >> 12) & 0xF) / 15.f, static_cast<float>((v >> 8) & 0xF) / 15.f,
           static_cast<float>((v >> 4) & 0xF) / 15.f, static_cast<float>(v & 0xF) / 15.f};
    break;
  }
  case GX_RGBA6: {
    const uint32_t v = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    out = {static_cast<float>((v >> 18) & 0x3F) / 63.f, static_cast<float>((v >> 12) & 0x3F) / 63.f,
           static_cast<float>((v >> 6) & 0x3F) / 63.f, static_cast<float>(v & 0x3F) / 63.f};
    break;
  }
  default: // GX_RGBA8
    out = {p[0] * k255, p[1] * k255, p[2] * k255, p[3] * k255};
    break;
  }
}

// Resolves where an attribute's data lives for one vertex: the vertex itself or an indexed array.
inline const uint8_t* attr_source(const gx::AttrConfig& mapping, int attr, const uint8_t* vtx, uint32_t group,
                                  bool& le) {
  const uint8_t* p = vtx + mapping.offset;
  le = false;
  if (mapping.attrType == GX_DIRECT) {
    return p;
  }
  const auto& array = g_gxState.arrays[attr];
  if (array.data == nullptr) {
    return nullptr;
  }
  uint32_t index;
  if (mapping.attrType == GX_INDEX8) {
    index = p[mapping.nrmIndexCount == 3 ? group : 0];
  } else {
    index = load16(p + (mapping.nrmIndexCount == 3 ? group * 2 : 0), false);
  }
  le = mapping.le;
  const size_t offset = static_cast<size_t>(index) * mapping.stride;
  if (offset >= array.size) {
    return nullptr;
  }
  return static_cast<const uint8_t*>(array.data) + offset;
}

void decode_vertex(const gx::ShaderConfig& config, const uint8_t* vtx, DecodedVertex& out) {
  for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; ++attr) {
    const auto& mapping = config.attrs[attr];
    if (mapping.attrType == GX_NONE) {
      continue;
    }
    bool le = false;
    if (attr == GX_VA_NRM && mapping.cnt == 9 && mapping.nrmIndexCount == 3 && mapping.attrType != GX_DIRECT) {
      Vec3f* groups[3] = {&out.nrm, &out.binormal, &out.tangent};
      for (uint32_t g = 0; g < 3; ++g) {
        const uint8_t* p = attr_source(mapping, attr, vtx, g, le);
        if (p != nullptr) {
          *groups[g] = {load_component(p, mapping.compType, mapping.frac, le, 0),
                        load_component(p, mapping.compType, mapping.frac, le, 1),
                        load_component(p, mapping.compType, mapping.frac, le, 2)};
        }
      }
      continue;
    }
    const uint8_t* p = attr_source(mapping, attr, vtx, 0, le);
    if (p == nullptr) {
      continue;
    }
    switch (attr) {
    case GX_VA_PNMTXIDX:
      out.pnMtx = p[0] / 3u;
      break;
    case GX_VA_POS:
      out.pos = {load_component(p, mapping.compType, mapping.frac, le, 0),
                 load_component(p, mapping.compType, mapping.frac, le, 1),
                 mapping.cnt >= 3 ? load_component(p, mapping.compType, mapping.frac, le, 2) : 0.f};
      break;
    case GX_VA_NRM:
      out.nrm = {load_component(p, mapping.compType, mapping.frac, le, 0),
                 load_component(p, mapping.compType, mapping.frac, le, 1),
                 load_component(p, mapping.compType, mapping.frac, le, 2)};
      if (mapping.cnt == 9) {
        out.binormal = {load_component(p, mapping.compType, mapping.frac, le, 3),
                        load_component(p, mapping.compType, mapping.frac, le, 4),
                        load_component(p, mapping.compType, mapping.frac, le, 5)};
        out.tangent = {load_component(p, mapping.compType, mapping.frac, le, 6),
                       load_component(p, mapping.compType, mapping.frac, le, 7),
                       load_component(p, mapping.compType, mapping.frac, le, 8)};
      }
      break;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
      load_color(p, mapping.compType, le, out.color[attr - GX_VA_CLR0]);
      break;
    default:
      if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) {
        out.texMtx[attr - GX_VA_TEX0MTXIDX] = p[0];
      } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        auto& tc = out.tex[attr - GX_VA_TEX0];
        tc[0] = load_component(p, mapping.compType, mapping.frac, le, 0);
        tc[1] = mapping.cnt >= 2 ? load_component(p, mapping.compType, mapping.frac, le, 1) : 0.f;
      }
      break;
    }
  }
}

// Matrix memory slot -> rows; slots 0-9 are position matrices, 10-19 texture matrices.
inline const Mat3x4<float>& postex_matrix(uint32_t slot) {
  if (slot < gx::MaxPnMtx) {
    return g_gxState.pnMtx[slot].pos;
  }
  return g_gxState.texMtxs[std::min<uint32_t>(slot - gx::MaxPnMtx, gx::MaxTexMtx - 1)];
}

inline float dot4(const Vec4<float>& row, float x, float y, float z, float w) {
  return row.m[0] * x + row.m[1] * y + row.m[2] * z + row.m[3] * w;
}

inline Vec3f mul3x4(const Mat3x4<float>& m, float x, float y, float z, float w) {
  return {dot4(m.m0, x, y, z, w), dot4(m.m1, x, y, z, w), dot4(m.m2, x, y, z, w)};
}

inline float dot3(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3f normalize(const Vec3f& v) {
  const float len2 = dot3(v, v);
  if (len2 <= 1e-10f) {
    return v;
  }
  const float inv = 1.f / std::sqrt(len2);
  return {v.x * inv, v.y * inv, v.z * inv};
}

struct LightingInputs {
  Vec3f mvPos;
  Vec3f mvNrm;
};

// One XF lighting channel (color or alpha); mirrors lighting_func in aurora's shader.cpp.
void light_channel(const gx::ShaderConfig& config, uint32_t chan, bool alpha, const DecodedVertex& v,
                   const LightingInputs& in, std::array<float, 4>& out) {
  const auto& cc = config.colorChannels[alpha ? chan + GX_ALPHA0 : chan];
  const auto& state = g_gxState.colorChannelState[alpha ? chan + GX_ALPHA0 : chan];
  const int vtxAttr = gx::shader_vertex_color_attr(config, chan);
  static constexpr std::array<float, 4> kWhite{1.f, 1.f, 1.f, 1.f};
  const auto& vtxColor = vtxAttr >= 0 ? v.color[vtxAttr - GX_VA_CLR0] : kWhite;
  const auto source = [&](GXColorSrc src, const Vec4<float>& reg) -> std::array<float, 4> {
    if (src == GX_SRC_VTX) {
      return vtxColor;
    }
    return {reg.m[0], reg.m[1], reg.m[2], reg.m[3]};
  };
  const auto mat = source(cc.matSrc, state.matColor);
  const int first = alpha ? 3 : 0;
  const int last = alpha ? 4 : 3;
  if (!cc.lightingEnabled) {
    for (int c = first; c < last; ++c) {
      out[c] = std::round(mat[c] * 255.f) / 255.f;
    }
    if (!alpha) {
      out[3] = 1.f;
    }
    return;
  }
  const auto amb = source(cc.ambSrc, state.ambColor);
  std::array<int, 4> lighting{};
  for (int c = first; c < last; ++c) {
    lighting[c] = static_cast<int>(std::round(amb[c] * 255.f));
  }
  const auto& lights = g_gxState.preparedLights;
  for (uint32_t i = 0; i < GX::MaxLights; ++i) {
    if (!state.lightMask.test(i)) {
      continue;
    }
    const auto& light = lights[i];
    const Vec3f lpos{light.pos.m[0], light.pos.m[1], light.pos.m[2]};
    const Vec3f ldirAxis{light.dir.m[0], light.dir.m[1], light.dir.m[2]};
    Vec3f ldir{lpos.x - in.mvPos.x, lpos.y - in.mvPos.y, lpos.z - in.mvPos.z};
    const float dist2 = dot3(ldir, ldir);
    const float dist = std::sqrt(dist2);
    float attn = 1.f;
    if (cc.attnFn == GX_AF_NONE) {
      if (dist > 0.f) {
        const float inv = 1.f / std::max(dist, 1e-20f);
        ldir = {ldir.x * inv, ldir.y * inv, ldir.z * inv};
      } else {
        ldir = in.mvNrm;
      }
    } else {
      const float inv = dist > 0.f ? 1.f / dist : 0.f;
      ldir = {ldir.x * inv, ldir.y * inv, ldir.z * inv};
      if (cc.attnFn == GX_AF_SPOT) {
        const float cosine = std::max(0.f, dot3(ldir, ldirAxis));
        const float cosAttn =
            light.cosAtt.m[0] + light.cosAtt.m[1] * cosine + light.cosAtt.m[2] * cosine * cosine;
        const float distAttn = light.distAtt.m[0] + light.distAtt.m[1] * dist + light.distAtt.m[2] * dist2;
        attn = std::max(0.f, cosAttn / distAttn);
      } else if (cc.attnFn == GX_AF_SPEC) {
        attn = dot3(in.mvNrm, ldir) >= 0.f ? std::max(0.f, dot3(in.mvNrm, ldirAxis)) : 0.f;
        const float cosAttn = light.cosAtt.m[0] + light.cosAtt.m[1] * attn + light.cosAtt.m[2] * attn * attn;
        Vec3f k{light.distAtt.m[0], light.distAtt.m[1], light.distAtt.m[2]};
        if (cc.diffFn != GX_DF_NONE) {
          k = normalize(k);
        }
        const float distAttn = k.x + k.y * attn + k.z * attn * attn;
        attn = distAttn != 0.f ? std::max(0.f, cosAttn / distAttn) : (cosAttn > 0.f ? 1.f : 0.f);
      }
    }
    float diff = 1.f;
    if (cc.diffFn == GX_DF_SIGN) {
      diff = dot3(ldir, in.mvNrm);
    } else if (cc.diffFn == GX_DF_CLAMP) {
      diff = std::max(0.f, dot3(ldir, in.mvNrm));
    }
    for (int c = first; c < last; ++c) {
      lighting[c] += static_cast<int>(std::round(attn * diff * light.color.m[c] * 255.f));
    }
  }
  for (int c = first; c < last; ++c) {
    const int lacc = std::clamp(lighting[c], 0, 255);
    const int material = static_cast<int>(std::round(mat[c] * 255.f));
    out[c] = static_cast<float>((material * (lacc + (lacc >> 7))) >> 8) / 255.f;
  }
  if (!alpha) {
    out[3] = 1.f;
  }
}

std::array<float, 4> texgen(const gx::ShaderConfig& config, uint32_t i, const DecodedVertex& v) {
  const auto& tcg = config.tcgs[i];
  float tc[4] = {0.f, 0.f, 1.f, 1.f};
  if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
    const auto& uv = v.tex[tcg.src - GX_TG_TEX0];
    tc[0] = uv[0];
    tc[1] = uv[1];
  } else if (tcg.src == GX_MAX_TEXGENSRC) {
    tc[0] = v.tex[i][0];
    tc[1] = v.tex[i][1];
  } else if (tcg.src == GX_TG_POS) {
    tc[0] = v.pos.x, tc[1] = v.pos.y, tc[2] = v.pos.z;
  } else if (tcg.src == GX_TG_NRM) {
    tc[0] = v.nrm.x, tc[1] = v.nrm.y, tc[2] = v.nrm.z;
  } else if (tcg.src == GX_TG_BINRM) {
    tc[0] = v.binormal.x, tc[1] = v.binormal.y, tc[2] = v.binormal.z;
  } else if (tcg.src == GX_TG_TANGENT) {
    tc[0] = v.tangent.x, tc[1] = v.tangent.y, tc[2] = v.tangent.z;
  } else if (tcg.src == GX_TG_COLOR0 || tcg.src == GX_TG_COLOR1) {
    const auto& c = v.color[tcg.src == GX_TG_COLOR0 ? 0 : 1];
    tc[0] = c[0], tc[1] = c[1], tc[2] = c[2];
  }
  if (tcg.inputFormAB11) {
    tc[2] = 1.f;
  }
  Vec3f out{tc[0], tc[1], 1.f};
  if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
    if (config.attrs[GX_VA_TEX0MTXIDX + i].attrType != GX_NONE) {
      out = mul3x4(postex_matrix(v.texMtx[i] / 3u), tc[0], tc[1], tc[2], tc[3]);
    } else if (tcg.mtx == GX_IDENTITY) {
      out = {tc[0], tc[1], tc[2]};
    } else {
      out = mul3x4(postex_matrix(static_cast<uint32_t>(tcg.mtx) / 3u), tc[0], tc[1], tc[2], tc[3]);
    }
    if (tcg.type == GX_TG_MTX2x4) {
      out.z = 1.f;
    }
  }
  if (config.dualTexEnabled && tcg.normalize) {
    out = normalize(out);
  }
  if (config.dualTexEnabled && tcg.postMtx != GX_PTIDENTITY) {
    const uint32_t post = (static_cast<uint32_t>(tcg.postMtx) - GX_PTTEXMTX0) / 3u;
    out = mul3x4(g_gxState.ptTexMtxs[std::min<uint32_t>(post, gx::MaxPTTexMtx - 1)], out.x, out.y, out.z, 1.f);
  }
  if ((tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) && out.z == 0.f) {
    out.x = std::clamp(out.x / 2.f, -1.f, 1.f);
    out.y = std::clamp(out.y / 2.f, -1.f, 1.f);
  }
  if (tcg.type == GX_TG_MTX3x4 && out.z != 0.f) {
    return {out.x, out.y, 0.f, out.z};
  }
  return {out.x, out.y, 0.f, 1.f};
}

uint32_t gcm_compare(GXCompare compare) { return GCM_NEVER + static_cast<uint32_t>(compare); }

uint32_t gcm_blend_factor(GXBlendFactor factor, bool isDst, bool hasAlpha) {
  if (!hasAlpha) {
    if (factor == GX_BL_DSTALPHA) {
      return GCM_ONE;
    }
    if (factor == GX_BL_INVDSTALPHA) {
      return GCM_ZERO;
    }
  }
  switch (factor) {
  case GX_BL_ZERO:
    return GCM_ZERO;
  case GX_BL_ONE:
    return GCM_ONE;
  case GX_BL_SRCCLR:
    return isDst ? GCM_SRC_COLOR : GCM_DST_COLOR;
  case GX_BL_INVSRCCLR:
    return isDst ? GCM_ONE_MINUS_SRC_COLOR : GCM_ONE_MINUS_DST_COLOR;
  case GX_BL_SRCALPHA:
    return GCM_SRC_ALPHA;
  case GX_BL_INVSRCALPHA:
    return GCM_ONE_MINUS_SRC_ALPHA;
  case GX_BL_DSTALPHA:
    return GCM_DST_ALPHA;
  default:
    return GCM_ONE_MINUS_DST_ALPHA;
  }
}

uint32_t gcm_wrap(GXTexWrapMode wrap) {
  switch (wrap) {
  case GX_REPEAT:
    return GCM_TEXTURE_REPEAT;
  case GX_MIRROR:
    return GCM_TEXTURE_MIRRORED_REPEAT;
  default:
    return GCM_TEXTURE_CLAMP_TO_EDGE;
  }
}

void bind_texture(uint8_t unit, uint32_t offset, uint32_t pitch, uint32_t width, uint32_t height, uint32_t wrapS,
                  uint32_t wrapT, bool linearMin, bool linearMag) {
  gcmTexture tex{};
  tex.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_NRM;
  tex.mipmap = 1;
  tex.dimension = GCM_TEXTURE_DIMS_2D;
  tex.cubemap = GCM_FALSE;
  tex.remap = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
              (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
              (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
              (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
              (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
              (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
              (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
              (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
  tex.width = width;
  tex.height = height;
  tex.depth = 1;
  tex.location = GCM_LOCATION_RSX;
  tex.pitch = pitch;
  tex.offset = offset;
  rsxLoadTexture(g_ctx, unit, &tex);
  rsxTextureControl(g_ctx, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
  rsxTextureFilter(g_ctx, unit, 0, linearMin ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
                   linearMag ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
  rsxTextureWrapMode(g_ctx, unit, wrapS, wrapT, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

gfx::TextureHandle white_texture() {
  static gfx::TextureHandle s_white;
  if (!s_white) {
    static const uint8_t kWhite[4] = {255, 255, 255, 255};
    s_white = gfx::new_static_texture_2d(1, 1, 1, GX_TF_RGBA8_PC, {kWhite, sizeof(kWhite)}, false, "White");
  }
  return s_white;
}

// Fixed-function state, cached to keep redundant methods out of the command buffer.
struct RenderState {
  uint32_t blendEnable = UINT32_MAX;
  uint32_t blendSrc = 0, blendDst = 0, blendSrcA = 0, blendDstA = 0, blendEq = 0, blendColor = 0;
  uint32_t logicEnable = UINT32_MAX, logicOp = 0;
  uint32_t colorMask = UINT32_MAX;
  uint32_t depthTest = UINT32_MAX, depthFunc = 0, depthWrite = UINT32_MAX;
  uint32_t attribMask = UINT32_MAX;
};
RenderState g_state;

void set_blend(uint32_t enable, uint32_t src, uint32_t dst, uint32_t srcA, uint32_t dstA, uint32_t eq,
               uint32_t color) {
  if (g_state.blendEnable != enable) {
    rsxSetBlendEnable(g_ctx, enable);
    g_state.blendEnable = enable;
  }
  if (enable == 0) {
    return;
  }
  if (g_state.blendSrc != src || g_state.blendDst != dst || g_state.blendSrcA != srcA || g_state.blendDstA != dstA) {
    rsxSetBlendFunc(g_ctx, src, dst, srcA, dstA);
    g_state.blendSrc = src, g_state.blendDst = dst, g_state.blendSrcA = srcA, g_state.blendDstA = dstA;
  }
  if (g_state.blendEq != eq) {
    rsxSetBlendEquation(g_ctx, eq, GCM_FUNC_ADD);
    g_state.blendEq = eq;
  }
  if (g_state.blendColor != color) {
    rsxSetBlendColor(g_ctx, color, 0);
    g_state.blendColor = color;
  }
}

void set_logic(uint32_t enable, uint32_t op) {
  if (g_state.logicEnable != enable) {
    rsxSetLogicOpEnable(g_ctx, enable);
    g_state.logicEnable = enable;
  }
  if (enable != 0 && g_state.logicOp != op) {
    rsxSetLogicOp(g_ctx, op);
    g_state.logicOp = op;
  }
}

void set_color_mask(uint32_t mask) {
  if (g_state.colorMask != mask) {
    rsxSetColorMask(g_ctx, mask);
    g_state.colorMask = mask;
  }
}

void set_depth(uint32_t test, uint32_t func, uint32_t write) {
  if (g_state.depthTest != test) {
    rsxSetDepthTestEnable(g_ctx, test);
    g_state.depthTest = test;
  }
  if (test != 0 && g_state.depthFunc != func) {
    rsxSetDepthFunc(g_ctx, func);
    g_state.depthFunc = func;
  }
  if (g_state.depthWrite != write) {
    rsxSetDepthWriteEnable(g_ctx, write);
    g_state.depthWrite = write;
  }
}

void bind_attributes(uint32_t offset, uint32_t strideBytes, uint32_t texCount) {
  static constexpr uint8_t kAttribs[] = {GCM_VERTEX_ATTRIB_POS, GCM_VERTEX_ATTRIB_COLOR0, 4, 5};
  static constexpr uint8_t kElems[] = {4, 4, 4, 1};
  static constexpr uint32_t kOffsets[] = {kOffPos, kOffColor0, kOffColor1, kOffFog};
  for (int i = 0; i < 4; ++i) {
    rsxBindVertexArrayAttrib(g_ctx, kAttribs[i], 0, offset + kOffsets[i] * 4, strideBytes, kElems[i],
                             GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
  }
  for (uint32_t t = 0; t < 8; ++t) {
    if (t < texCount) {
      rsxBindVertexArrayAttrib(g_ctx, GCM_VERTEX_ATTRIB_TEX0 + t, 0, offset + (kOffTex + t * 4) * 4, strideBytes, 4,
                               GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
    } else if ((g_state.attribMask & (1u << t)) != 0) {
      rsxBindVertexArrayAttrib(g_ctx, GCM_VERTEX_ATTRIB_TEX0 + t, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32,
                               GCM_LOCATION_CELL);
    }
  }
  g_state.attribMask = (1u << texCount) - 1u;
}

// Signed area in window space (y down) of the visible part of a clip-space triangle.
float facing(const float* a, const float* b, const float* c) {
  // Homogeneous orientation test: sign(det[x y w]) is the NDC orientation of the visible part.
  const float det = a[0] * (b[1] * c[3] - c[1] * b[3]) - a[1] * (b[0] * c[3] - c[0] * b[3]) +
                    a[3] * (b[0] * c[1] - c[0] * b[1]);
  return det;
}
} // namespace

void draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount, uint32_t vtxStride) noexcept {
  if (vertices == nullptr || vtxCount == 0) {
    return;
  }
  gx::PipelineConfig config{};
  gx::populate_pipeline_config(config, prim, fmt);
  const auto& sc = config.shaderConfig;
  const bool isTriangles = prim == GX_QUADS || prim == GX_TRIANGLES || prim == GX_TRIANGLESTRIP ||
                           prim == GX_TRIANGLEFAN;
  if (config.cullMode == GX_CULL_ALL && isTriangles) {
    return;
  }

  const auto info = gx::build_shader_info(sc);
  gx::resolve_sampled_textures(info);
  if (info.lightingEnabled && g_gxState.preparedLightsDirty) {
    for (size_t i = 0; i < g_gxState.preparedLights.size(); ++i) {
      g_gxState.preparedLights[i] = gx::prepare_shader_light(g_gxState.lights[i]);
    }
    g_gxState.preparedLightsDirty = false;
  }

  // ---- Vertex processing ----
  const uint32_t texCount = std::min<uint32_t>(sc.numTexGens, gx::MaxTexCoord);
  const uint32_t strideFloats = kOffTex + texCount * 4;
  const uint32_t strideBytes = strideFloats * 4;
  const Stream vtxStream = stream_alloc(strideBytes * vtxCount, 128);
  auto* out = static_cast<float*>(vtxStream.ptr);

  const auto& vp = g_gxState.renderViewport;
  const float zNear = vp.znear;
  const float zFar = vp.zfar;
  const auto& proj = g_gxState.proj;
  const bool fogEnabled = sc.fogType != GX_FOG_NONE;
  const auto& fog = g_gxState.fog;
  const bool hasNormal = sc.attrs[GX_VA_NRM].attrType != GX_NONE;
  const bool indexedPnMtx = sc.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE;
  const uint32_t currentPnMtx = g_gxState.currentPnMtx;

  for (uint32_t i = 0; i < vtxCount; ++i) {
    DecodedVertex v;
    decode_vertex(sc, vertices + static_cast<size_t>(i) * vtxStride, v);
    const uint32_t slot = indexedPnMtx ? v.pnMtx : currentPnMtx;
    const Vec3f mv = mul3x4(postex_matrix(slot), v.pos.x, v.pos.y, v.pos.z, 1.f);
    float* o = out + static_cast<size_t>(i) * strideFloats;
    const float cx = dot4(proj.m0, mv.x, mv.y, mv.z, 1.f);
    const float cy = dot4(proj.m1, mv.x, mv.y, mv.z, 1.f);
    const float cz = dot4(proj.m2, mv.x, mv.y, mv.z, 1.f);
    const float cw = dot4(proj.m3, mv.x, mv.y, mv.z, 1.f);
    // GX clip z/w spans [-1 (near), 0 (far)]; the EFB depth is the viewport depth range, 0 = near.
    o[kOffPos + 0] = cx;
    o[kOffPos + 1] = cy;
    o[kOffPos + 2] = zFar * cw + cz * (zFar - zNear);
    o[kOffPos + 3] = cw;

    LightingInputs lit;
    lit.mvPos = mv;
    if (info.lightingEnabled) {
      const auto& nm = g_gxState.pnMtx[std::min<uint32_t>(slot, gx::MaxPnMtx - 1)].nrm;
      lit.mvNrm = normalize(mul3x4(nm, v.nrm.x, v.nrm.y, v.nrm.z, 0.f));
    }
    (void)hasNormal;
    std::array<float, 4> c0{1.f, 1.f, 1.f, 1.f};
    std::array<float, 4> c1{1.f, 1.f, 1.f, 1.f};
    light_channel(sc, 0, false, v, lit, c0);
    light_channel(sc, 0, true, v, lit, c0);
    light_channel(sc, 1, false, v, lit, c1);
    light_channel(sc, 1, true, v, lit, c1);
    std::memcpy(o + kOffColor0, c0.data(), sizeof(float) * 4);
    std::memcpy(o + kOffColor1, c1.data(), sizeof(float) * 4);

    float fogValue = 0.f;
    if (fogEnabled && cw != 0.f) {
      const float depth = std::clamp(zFar + (cz / cw) * (zFar - zNear), 0.f, 1.f);
      const float zCoord = std::round(depth * 16777216.f);
      float ze;
      if ((fog.type & 8) == 0) {
        const float shifted = std::floor(zCoord / static_cast<float>(1u << std::min<uint32_t>(fog.bShift, 31)));
        ze = (fog.aRaw * 16777216.f) / (static_cast<float>(fog.bMagnitude) - shifted);
      } else {
        ze = fog.aRaw * zCoord / 16777216.f;
      }
      fogValue = ze - fog.c;
    }
    o[kOffFog] = fogValue;

    for (uint32_t t = 0; t < texCount; ++t) {
      const auto tc = texgen(sc, t, v);
      std::memcpy(o + kOffTex + t * 4, tc.data(), sizeof(float) * 4);
    }
  }

  // ---- Primitive assembly and culling ----
  uint32_t gcmType = GCM_TYPE_TRIANGLES;
  uint32_t indexCount = 0;
  Stream idxStream;
  if (isTriangles) {
    const uint32_t maxIndices = static_cast<uint32_t>(vtxCount) * 3u;
    idxStream = stream_alloc(maxIndices * 2, 128);
    auto* indices = static_cast<uint16_t*>(idxStream.ptr);
    // Window y points down, so flip the NDC orientation; a mirrored viewport flips it again.
    const float sign = (vp.width * vp.height >= 0.f) ? -1.f : 1.f;
    const auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
      if (config.cullMode != GX_CULL_NONE) {
        const float area = sign * facing(out + a * strideFloats, out + b * strideFloats, out + c * strideFloats);
        // GX treats clockwise (positive area in y-down window space) as front facing.
        const bool front = area > 0.f;
        if ((config.cullMode == GX_CULL_BACK && !front) || (config.cullMode == GX_CULL_FRONT && front)) {
          return;
        }
      }
      indices[indexCount++] = static_cast<uint16_t>(a);
      indices[indexCount++] = static_cast<uint16_t>(b);
      indices[indexCount++] = static_cast<uint16_t>(c);
    };
    switch (prim) {
    case GX_QUADS:
      for (uint32_t v = 0; v + 3 < vtxCount; v += 4) {
        emit(v, v + 1, v + 2);
        emit(v + 2, v + 3, v);
      }
      if (vtxCount % 4 == 3) {
        const uint32_t v = vtxCount & ~3u;
        emit(v, v + 1, v + 2);
      }
      break;
    case GX_TRIANGLES:
      for (uint32_t v = 0; v + 2 < vtxCount; v += 3) {
        emit(v, v + 1, v + 2);
      }
      break;
    case GX_TRIANGLESTRIP:
      for (uint32_t v = 2; v < vtxCount; ++v) {
        if ((v & 1) == 0) {
          emit(v - 2, v - 1, v);
        } else {
          emit(v - 1, v - 2, v);
        }
      }
      break;
    default: // GX_TRIANGLEFAN
      for (uint32_t v = 2; v < vtxCount; ++v) {
        emit(0, v - 1, v);
      }
      break;
    }
    if (indexCount == 0) {
      return;
    }
  } else if (prim == GX_LINES) {
    gcmType = GCM_TYPE_LINES;
  } else if (prim == GX_LINESTRIP) {
    gcmType = GCM_TYPE_LINE_STRIP;
  } else {
    gcmType = GCM_TYPE_POINTS;
    const auto& target = current_target();
    const float scale = vp.width != 0.f && g_gxState.logicalViewport.width != 0.f
                            ? std::fabs(vp.width / g_gxState.logicalViewport.width)
                            : 1.f;
    (void)target;
    rsxSetPointSize(g_ctx, std::max(1.f, static_cast<float>(g_gxState.pointSize) / 6.f * scale));
  }

  // ---- Fixed-function state ----
  const bool hasAlpha = gx::render_target_has_alpha(config.pixelFmt);
  uint32_t blendEnable = 0;
  uint32_t src = GCM_ONE, dst = GCM_ZERO, srcA = GCM_ONE, dstA = GCM_ZERO, eq = GCM_FUNC_ADD;
  uint32_t logicEnable = 0;
  uint32_t logicOp = 0x1503; // GL_COPY
  switch (config.blendMode) {
  case GX_BM_BLEND:
    blendEnable = 1;
    src = gcm_blend_factor(config.blendFacSrc, false, hasAlpha);
    dst = gcm_blend_factor(config.blendFacDst, true, hasAlpha);
    srcA = src == GCM_DST_COLOR ? GCM_DST_ALPHA : src == GCM_ONE_MINUS_DST_COLOR ? GCM_ONE_MINUS_DST_ALPHA : src;
    dstA = dst == GCM_SRC_COLOR ? GCM_SRC_ALPHA : dst == GCM_ONE_MINUS_SRC_COLOR ? GCM_ONE_MINUS_SRC_ALPHA : dst;
    break;
  case GX_BM_SUBTRACT:
    blendEnable = 1;
    src = dst = srcA = dstA = GCM_ONE;
    eq = GCM_FUNC_REVERSE_SUBTRACT;
    break;
  case GX_BM_LOGIC:
    logicEnable = 1;
    logicOp = 0x1500u + static_cast<uint32_t>(config.blendOp);
    break;
  default:
    break;
  }
  uint32_t blendColor = 0;
  if (config.dstAlpha != UINT32_MAX) {
    // The EFB alpha comes from the destination-alpha register, not the TEV output.
    blendEnable = 1;
    srcA = GCM_CONSTANT_ALPHA;
    dstA = GCM_ZERO;
    blendColor = (config.dstAlpha & 0xFFu) << 24;
  }
  set_blend(blendEnable, src, dst, srcA, dstA, eq, blendColor);
  set_logic(logicEnable, logicOp);
  uint32_t mask = 0;
  if (config.colorUpdate) {
    mask |= GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B;
  }
  if (config.alphaUpdate) {
    mask |= GCM_COLOR_MASK_A;
  }
  set_color_mask(mask);
  const bool depthOn = config.depthCompare && current_target().hasDepth;
  set_depth(depthOn ? 1 : 0, gcm_compare(config.depthFunc), depthOn && config.depthUpdate ? 1 : 0);

  // ---- Textures and fragment program ----
  const FragmentProgram* program = gx_fragment_program(sc);
  flush_texture_cache();
  const uint32_t texMask = fragment_texture_mask(program);
  for (uint32_t unit = 0; unit < gx::MaxTextures; ++unit) {
    if ((texMask & (1u << unit)) == 0) {
      continue;
    }
    const auto& bind = gx::get_texture(static_cast<GXTexMapID>(unit));
    gfx::TextureHandle handle = bind.ref;
    if (!handle || handle->rsxMemory == nullptr) {
      handle = white_texture();
    }
    const auto& obj = bind.texObj;
    bind_texture(unit, handle->rsxOffset, handle->rsxPitch, handle->size.width, handle->size.height,
                 gcm_wrap(obj.wrap_s()), gcm_wrap(obj.wrap_t()), obj.min_filter() != GX_NEAR &&
                     obj.min_filter() != GX_NEAR_MIP_NEAR && obj.min_filter() != GX_NEAR_MIP_LIN,
                 obj.mag_filter() == GX_LINEAR);
  }

  std::array<Vec4<float>, 16> constants{};
  for (int i = 0; i < 4; ++i) {
    constants[2 + i] = g_gxState.kcolors[i];
    constants[6 + i] = g_gxState.colorRegs[i];
  }
  constants[10] = fog.color;
  constants[11] = {static_cast<float>(sc.alphaCompare.ref0), static_cast<float>(sc.alphaCompare.ref1), 0.f, 0.f};
  bind_fragment_program(program, constants.data());

  // ---- Draw ----
  bind_attributes(vtxStream.offset, strideBytes, texCount);
  if (isTriangles) {
    rsxDrawIndexArray(g_ctx, GCM_TYPE_TRIANGLES, idxStream.offset, indexCount, GCM_INDEX_TYPE_16B, GCM_LOCATION_CELL);
  } else {
    rsxDrawVertexArray(g_ctx, gcmType, 0, vtxCount);
  }
  (void)gcmType;
  g_gxState.stateDirty = false;
}

void prepare_clear() noexcept {
  // Clears honour the color and depth write masks.
  set_color_mask(GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
  set_depth(0, GCM_ALWAYS, 1);
}

void draw_quad(const QuadDraw& quad) noexcept {
  const auto& target = current_target();
  // Full-target viewport in window pixels.
  const float scale[4] = {static_cast<float>(target.width) * 0.5f, -static_cast<float>(target.height) * 0.5f, 1.f,
                          0.f};
  const float offset[4] = {static_cast<float>(target.width) * 0.5f, static_cast<float>(target.height) * 0.5f, 0.f,
                           0.f};
  rsxSetViewport(g_ctx, 0, 0, target.width, target.height, 0.f, 1.f, scale, offset);
  rsxSetScissor(g_ctx, 0, 0, target.width, target.height);
  set_blend(0, GCM_ONE, GCM_ZERO, GCM_ONE, GCM_ZERO, GCM_FUNC_ADD, 0);
  set_logic(0, 0x1503);
  set_color_mask(GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
  set_depth(0, GCM_ALWAYS, 0);

  const auto toNdcX = [&](float x) { return x / static_cast<float>(target.width) * 2.f - 1.f; };
  const auto toNdcY = [&](float y) { return 1.f - y / static_cast<float>(target.height) * 2.f; };
  constexpr uint32_t strideFloats = kOffTex + 4;
  const Stream stream = stream_alloc(strideFloats * 4 * 4, 128);
  auto* v = static_cast<float*>(stream.ptr);
  const float xs[4] = {quad.x0, quad.x1, quad.x1, quad.x0};
  const float ys[4] = {quad.y0, quad.y0, quad.y1, quad.y1};
  const float us[4] = {quad.u0, quad.u1, quad.u1, quad.u0};
  const float vs[4] = {quad.v0, quad.v0, quad.v1, quad.v1};
  for (int i = 0; i < 4; ++i) {
    float* o = v + i * strideFloats;
    std::memset(o, 0, strideFloats * sizeof(float));
    o[kOffPos + 0] = toNdcX(xs[i]);
    o[kOffPos + 1] = toNdcY(ys[i]);
    o[kOffPos + 2] = 0.f;
    o[kOffPos + 3] = 1.f;
    for (int c = 0; c < 4; ++c) {
      o[kOffColor0 + c] = 1.f;
      o[kOffColor1 + c] = 1.f;
    }
    o[kOffTex + 0] = us[i];
    o[kOffTex + 1] = vs[i];
    o[kOffTex + 3] = 1.f;
  }
  flush_texture_cache();
  bind_texture(0, quad.texOffset, quad.texPitch, quad.texWidth, quad.texHeight, GCM_TEXTURE_CLAMP_TO_EDGE,
               GCM_TEXTURE_CLAMP_TO_EDGE, quad.linear, quad.linear);
  bind_fragment_program(copy_fragment_program(quad.copyFormat, quad.forceOpaqueAlpha), nullptr);
  bind_attributes(stream.offset, strideFloats * 4, 1);
  rsxDrawVertexArray(g_ctx, GCM_TYPE_QUADS, 0, 4);
}
} // namespace aurora::rsx
