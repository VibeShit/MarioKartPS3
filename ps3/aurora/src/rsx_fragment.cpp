// GX TEV -> NV40 fragment programs.
//
// Each ShaderConfig is translated into NV40 fragment assembly ("!!FP2.0", the dialect cgc emits)
// and assembled at runtime by the vendored PSL1GHT cgcomp assembler. TEV runs in float math
// (aurora's WGSL emulates the integer datapath exactly; this is an approximation of it).
// Dynamic values (konst colors, TEV registers, fog color, alpha references) live in program
// constants, which NV40 embeds in the instruction stream: they are patched into a per-draw copy
// of the microcode.
//
// Register use: R0 prev, R1-R3 TEV registers 0-2, R4 rasterized color, R5 texture color,
// R6 konst, R7-R9 scratch, R10-R12 operand staging.

#include "rsx_backend.hpp"

#include "internal.hpp"

#include "rsx_assembler.hpp"

#include <rsx/rsx.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aurora::rsx {
namespace {
Module Log("aurora::rsx::fp");

// Constant slots shared by every generated program.
constexpr int kConstLiterals = 0;   // {0, 0.5, 1, 2}
constexpr int kConstLiterals2 = 1;  // {255, 4, 0.5/255, 1/255}
constexpr int kConstKColor = 2;     // 2..5
constexpr int kConstTevReg = 6;     // 6..9: prev, reg0, reg1, reg2
constexpr int kConstFogColor = 10;
constexpr int kConstAlphaRef = 11;  // {ref0, ref1} in 0..255 units
constexpr int kConstFractions = 12; // {7/8, 6/8, 5/8, 4/8}
constexpr int kConstFractions2 = 13;// {3/8, 2/8, 1/8, 0}
constexpr int kConstKeys = 14;      // {1, 256, 65536, 0}
constexpr int kConstIntensity = 15; // {0.257, 0.504, 0.098, 16/255}
constexpr int kConstCount = 16;

const char* kLiteralDecls =
    "#const c[0] = 0 0.5 1 2\n"
    "#const c[1] = 255 4 0.00196078 0.00392157\n"
    "#const c[12] = 0.875 0.75 0.625 0.5\n"
    "#const c[13] = 0.375 0.25 0.125 0\n"
    "#const c[14] = 1 256 65536 0\n"
    "#const c[15] = 0.257 0.504 0.098 0.0627451\n"
    "#var float4 kcolor0 : : c[2] : 1 : 1\n"
    "#var float4 kcolor1 : : c[3] : 1 : 1\n"
    "#var float4 kcolor2 : : c[4] : 1 : 1\n"
    "#var float4 kcolor3 : : c[5] : 1 : 1\n"
    "#var float4 tevprev : : c[6] : 1 : 1\n"
    "#var float4 tevreg0 : : c[7] : 1 : 1\n"
    "#var float4 tevreg1 : : c[8] : 1 : 1\n"
    "#var float4 tevreg2 : : c[9] : 1 : 1\n"
    "#var float4 fogcolor : : c[10] : 1 : 1\n"
    "#var float4 alpharef : : c[11] : 1 : 1\n";

// One source operand: a register plus swizzle. Kind: 'r' temp, 'c' constant, 'f' input.
struct Src {
  std::string reg;
  char kind = 'r';
  std::string swz;
  bool neg = false;

  [[nodiscard]] Src swizzle(const std::string& s) const { return {reg, kind, s, neg}; }
  [[nodiscard]] Src negate() const { return {reg, kind, swz, !neg}; }
  [[nodiscard]] std::string str() const {
    std::string out = neg ? "-" : "";
    out += reg;
    if (!swz.empty()) {
      out += '.';
      out += swz;
    }
    return out;
  }
};

Src temp(int index, const std::string& swz = {}) { return {"R" + std::to_string(index), 'r', swz}; }
Src constant(int index, const std::string& swz = {}) { return {"c[" + std::to_string(index) + "]", 'c', swz}; }
Src input(const char* name, const std::string& swz = {}) { return {std::string("f[") + name + "]", 'f', swz}; }

Src zero() { return constant(kConstLiterals, "xxxx"); }
Src half() { return constant(kConstLiterals, "yyyy"); }
Src one() { return constant(kConstLiterals, "zzzz"); }

class Assembler {
public:
  std::string text;

  // NV40 instructions read at most one distinct constant and one distinct input; stage extras
  // through R10-R12.
  void op(const std::string& mnemonic, const std::string& dst, std::vector<Src> srcs) {
    std::string constReg;
    std::string inputReg;
    int staging = 10;
    for (auto& src : srcs) {
      std::string* owner = src.kind == 'c' ? &constReg : src.kind == 'f' ? &inputReg : nullptr;
      if (owner == nullptr) {
        continue;
      }
      if (owner->empty()) {
        *owner = src.reg;
      } else if (*owner != src.reg) {
        const std::string scratch = "R" + std::to_string(staging++);
        text += "MOV " + scratch + ", " + src.reg + ";\n";
        src = {scratch, 'r', src.swz, src.neg};
      }
    }
    text += mnemonic + " " + dst;
    for (const auto& src : srcs) {
      text += ", " + src.str();
    }
    text += ";\n";
  }
};

char channel(GXTevColorChan chan) {
  switch (chan) {
  case GX_CH_RED:
    return 'x';
  case GX_CH_GREEN:
    return 'y';
  case GX_CH_BLUE:
    return 'z';
  default:
    return 'w';
  }
}

std::string swap_swizzle(const gx::TevSwap& swap) {
  return {channel(swap.red), channel(swap.green), channel(swap.blue), channel(swap.alpha)};
}

std::string replicate(char c, int n) { return std::string(static_cast<size_t>(n), c); }

Src konst_color(GXTevKColorSel sel) {
  const int value = static_cast<int>(sel);
  if (value == GX_TEV_KCSEL_8_8) {
    return one();
  }
  if (value >= GX_TEV_KCSEL_7_8 && value <= GX_TEV_KCSEL_4_8) {
    return constant(kConstFractions, replicate("xyzw"[value - GX_TEV_KCSEL_7_8], 4));
  }
  if (value >= GX_TEV_KCSEL_3_8 && value <= GX_TEV_KCSEL_1_8) {
    return constant(kConstFractions2, replicate("xyz"[value - GX_TEV_KCSEL_3_8], 4));
  }
  if (value >= GX_TEV_KCSEL_K0 && value <= GX_TEV_KCSEL_K3) {
    return constant(kConstKColor + (value - GX_TEV_KCSEL_K0), "xyzz");
  }
  if (value >= GX_TEV_KCSEL_K0_R && value <= GX_TEV_KCSEL_K3_A) {
    const int rel = value - GX_TEV_KCSEL_K0_R;
    return constant(kConstKColor + (rel & 3), replicate("xyzw"[rel >> 2], 4));
  }
  return one();
}

Src konst_alpha(GXTevKAlphaSel sel) {
  const int value = static_cast<int>(sel);
  if (value == GX_TEV_KASEL_8_8) {
    return one();
  }
  if (value >= GX_TEV_KASEL_7_8 && value <= GX_TEV_KASEL_4_8) {
    return constant(kConstFractions, replicate("xyzw"[value - GX_TEV_KASEL_7_8], 4));
  }
  if (value >= GX_TEV_KASEL_3_8 && value <= GX_TEV_KASEL_1_8) {
    return constant(kConstFractions2, replicate("xyz"[value - GX_TEV_KASEL_3_8], 4));
  }
  if (value >= GX_TEV_KASEL_K0_R && value <= GX_TEV_KASEL_K3_A) {
    const int rel = value - GX_TEV_KASEL_K0_R;
    return constant(kConstKColor + (rel & 3), replicate("xyzw"[rel >> 2], 4));
  }
  return one();
}

Src color_arg(GXTevColorArg arg) {
  switch (arg) {
  case GX_CC_CPREV:
    return temp(0, "xyzz");
  case GX_CC_APREV:
    return temp(0, "wwww");
  case GX_CC_C0:
    return temp(1, "xyzz");
  case GX_CC_A0:
    return temp(1, "wwww");
  case GX_CC_C1:
    return temp(2, "xyzz");
  case GX_CC_A1:
    return temp(2, "wwww");
  case GX_CC_C2:
    return temp(3, "xyzz");
  case GX_CC_A2:
    return temp(3, "wwww");
  case GX_CC_TEXC:
    return temp(5, "xyzz");
  case GX_CC_TEXA:
    return temp(5, "wwww");
  case GX_CC_RASC:
    return temp(4, "xyzz");
  case GX_CC_RASA:
    return temp(4, "wwww");
  case GX_CC_ONE:
    return one();
  case GX_CC_HALF:
    return half();
  case GX_CC_KONST:
    return temp(6, "xyzz");
  default:
    return zero();
  }
}

Src alpha_arg(GXTevAlphaArg arg) {
  switch (arg) {
  case GX_CA_APREV:
    return temp(0, "wwww");
  case GX_CA_A0:
    return temp(1, "wwww");
  case GX_CA_A1:
    return temp(2, "wwww");
  case GX_CA_A2:
    return temp(3, "wwww");
  case GX_CA_TEXA:
    return temp(5, "wwww");
  case GX_CA_RASA:
    return temp(4, "wwww");
  case GX_CA_KONST:
    return temp(6, "wwww");
  default:
    return zero();
  }
}

bool uses_color_arg(const gx::TevStage& stage, GXTevColorArg a, GXTevColorArg b) {
  const auto& p = stage.colorPass;
  return p.a == a || p.b == a || p.c == a || p.d == a || p.a == b || p.b == b || p.c == b || p.d == b;
}

bool uses_alpha_arg(const gx::TevStage& stage, GXTevAlphaArg a) {
  const auto& p = stage.alphaPass;
  return p.a == a || p.b == a || p.c == a || p.d == a;
}

// cond = key(a) <op> key(b), written as 0/1 into `dst` (components selected by `mask`).
void emit_compare(Assembler& as, const std::string& dstMask, const Src& a, const Src& b, int keyMode, bool eq,
                  const std::string& mask) {
  // Differences are in 1/255 units after quantization; 0.5/255 separates "equal" from "greater".
  as.op("ADD", "R9" + mask, {a, b.negate()});
  if (keyMode == 1) {
    as.op("MUL", "R9.x", {temp(9, "x"), constant(kConstLiterals2, "x")});
  } else if (keyMode == 2) {
    as.op("MUL", "R9.xy", {temp(9, "xy"), constant(kConstLiterals2, "x")});
    as.op("DP3", "R9.x", {temp(9, "xyzz"), constant(kConstKeys, "xyww")});
  } else if (keyMode == 3) {
    as.op("MUL", "R9.xyz", {temp(9, "xyzz"), constant(kConstLiterals2, "x")});
    as.op("DP3", "R9.x", {temp(9, "xyzz"), constant(kConstKeys, "xyzz")});
  } else {
    as.op("MUL", "R9" + mask, {temp(9), constant(kConstLiterals2, "x")});
  }
  const std::string m = keyMode == 0 ? mask : ".x";
  if (eq) {
    as.op("MAX", "R9" + m, {temp(9), temp(9).negate()});
    as.op("SLT", dstMask, {temp(9, keyMode == 0 ? std::string() : "xxxx"), half()});
  } else {
    as.op("SGT", dstMask, {temp(9, keyMode == 0 ? std::string() : "xxxx"), half()});
  }
}

// Regular TEV op into R7 (color, .xyz) or R8 (alpha, .w).
void emit_regular(Assembler& as, const std::string& dst, GXTevOp op, GXTevBias bias, GXTevScale scale, const Src& a,
                  const Src& b, const Src& c, const Src& d) {
  as.op("LRP", dst, {c, b, a});
  const std::string reg = dst.substr(0, dst.find('.'));
  const Src r{reg, 'r', {}};
  as.op("ADD", dst, {d, op == GX_TEV_SUB ? r.negate() : r});
  if (bias == GX_TB_ADDHALF) {
    as.op("ADD", dst, {r, half()});
  } else if (bias == GX_TB_SUBHALF) {
    as.op("ADD", dst, {r, half().negate()});
  }
  switch (scale) {
  case GX_CS_SCALE_2:
    as.op("MUL", dst, {r, constant(kConstLiterals, "wwww")});
    break;
  case GX_CS_SCALE_4:
    as.op("MUL", dst, {r, constant(kConstLiterals2, "yyyy")});
    break;
  case GX_CS_DIVIDE_2:
    as.op("MUL", dst, {r, half()});
    break;
  default:
    break;
  }
}

std::string build_tev_program(const gx::ShaderConfig& config) {
  Assembler as;
  as.text = "!!FP2.0\n";
  as.text += kLiteralDecls;
  for (int i = 0; i < 4; ++i) {
    as.op("MOV", "R" + std::to_string(i), {constant(kConstTevReg + i)});
  }

  static const char* kOut[] = {"R0", "R1", "R2", "R3"};
  for (u32 idx = 0; idx < config.tevStageCount; ++idx) {
    const auto& stage = config.tevStages[idx];
    const bool needsTex = uses_color_arg(stage, GX_CC_TEXC, GX_CC_TEXA) || uses_alpha_arg(stage, GX_CA_TEXA);
    const bool needsRas = uses_color_arg(stage, GX_CC_RASC, GX_CC_RASA) || uses_alpha_arg(stage, GX_CA_RASA);
    const bool needsKonst = uses_color_arg(stage, GX_CC_KONST, GX_CC_KONST) || uses_alpha_arg(stage, GX_CA_KONST);

    if (needsTex) {
      const auto dep = gx::tev_stage_texture_dependency(config, idx);
      if (dep.canSampleTexture) {
        as.text += "TXP R5, f[TEX" + std::to_string(dep.texCoordId) + "], texture[" + std::to_string(dep.texMapId) +
                   "], 2D;\n";
        const std::string swz = swap_swizzle(config.tevSwapTable[stage.tevSwapTex]);
        if (swz != "xyzw") {
          as.op("MOV", "R5", {temp(5, swz)});
        }
      } else {
        as.op("MOV", "R5", {config.numTexGens == 0 ? zero() : one()});
      }
    }
    if (needsRas) {
      switch (stage.channelId) {
      case GX_COLOR0:
      case GX_ALPHA0:
      case GX_COLOR0A0:
        as.op("MOV", "R4", {input("COL0", swap_swizzle(config.tevSwapTable[stage.tevSwapRas]))});
        break;
      case GX_COLOR1:
      case GX_ALPHA1:
      case GX_COLOR1A1:
        as.op("MOV", "R4", {input("COL1", swap_swizzle(config.tevSwapTable[stage.tevSwapRas]))});
        break;
      default:
        // GX_COLOR_ZERO/NULL; alpha bump channels need indirect texturing, which is not emulated.
        as.op("MOV", "R4", {zero()});
        break;
      }
    }
    if (needsKonst) {
      as.op("MOV", "R6.xyz", {konst_color(stage.kcSel)});
      as.op("MOV", "R6.w", {konst_alpha(stage.kaSel)});
    }

    // Color combiner -> R7.xyz.
    const auto& cp = stage.colorPass;
    const auto& co = stage.colorOp;
    const Src ca = color_arg(cp.a);
    const Src cb = color_arg(cp.b);
    const Src cc = color_arg(cp.c);
    const Src cd = color_arg(cp.d);
    if (co.op <= GX_TEV_SUB) {
      emit_regular(as, "R7.xyz", co.op, co.bias, co.scale, ca, cb, cc, cd);
    } else {
      const int mode = co.op - GX_TEV_COMP_R8_GT; // 0..7
      const bool eq = (mode & 1) != 0;
      const int keyMode = mode < 2 ? 1 : mode < 4 ? 2 : mode < 6 ? 3 : 0;
      if (keyMode == 0) {
        emit_compare(as, "R7.xyz", ca, cb, 0, eq, ".xyz");
        as.op("MAD", "R7.xyz", {cc, temp(7), cd});
      } else {
        emit_compare(as, "R7.x", ca, cb, keyMode, eq, ".xyz");
        as.op("MAD", "R7.xyz", {cc, temp(7, "xxxx"), cd});
      }
    }

    // Alpha combiner -> R8.w.
    const auto& ap = stage.alphaPass;
    const auto& ao = stage.alphaOp;
    const Src aa = alpha_arg(ap.a);
    const Src ab = alpha_arg(ap.b);
    const Src ac = alpha_arg(ap.c);
    const Src ad = alpha_arg(ap.d);
    if (ao.op <= GX_TEV_SUB) {
      emit_regular(as, "R8.w", ao.op, ao.bias, ao.scale, aa, ab, ac, ad);
    } else {
      const int mode = ao.op - GX_TEV_COMP_R8_GT;
      const bool eq = (mode & 1) != 0;
      if (mode >= 6) {
        emit_compare(as, "R8.w", aa, ab, 0, eq, ".w");
      } else {
        // R8/GR16/BGR24 alpha compares use the color combiner's A/B inputs.
        emit_compare(as, "R8.w", ca, cb, mode < 2 ? 1 : mode < 4 ? 2 : 3, eq, ".xyz");
      }
      as.op("MAD", "R8.w", {ac, temp(8, "wwww"), ad});
    }

    as.op(co.clamp ? "MOV_SAT" : "MOV", std::string(kOut[co.outReg & 3]) + ".xyz", {temp(7)});
    as.op(ao.clamp ? "MOV_SAT" : "MOV", std::string(kOut[ao.outReg & 3]) + ".w", {temp(8, "wwww")});
  }
  if (config.tevStageCount > 0) {
    const auto& last = config.tevStages[config.tevStageCount - 1];
    if ((last.colorOp.outReg & 3) != 0) {
      as.op("MOV", "R0.xyz", {temp(last.colorOp.outReg & 3)});
    }
    if ((last.alphaOp.outReg & 3) != 0) {
      as.op("MOV", "R0.w", {temp(last.alphaOp.outReg & 3, "wwww")});
    }
  }

  const auto& ac = config.alphaCompare;
  if (ac.comp0 != GX_ALWAYS || ac.comp1 != GX_ALWAYS) {
    // Compare the 8-bit alpha against both references.
    as.op("MAD", "R7.x", {temp(0, "wwww"), constant(kConstLiterals2, "xxxx"), half()});
    as.op("FLR", "R7.x", {temp(7, "xxxx")});
    const auto compare = [&](GXCompare comp, const char* dst, const char* refSwz) {
      const Src ref = constant(kConstAlphaRef, refSwz);
      switch (comp) {
      case GX_NEVER:
        as.op("MOV", dst, {zero()});
        break;
      case GX_LESS:
        as.op("SLT", dst, {temp(7, "xxxx"), ref});
        break;
      case GX_EQUAL:
        as.op("SEQ", dst, {temp(7, "xxxx"), ref});
        break;
      case GX_LEQUAL:
        as.op("SLE", dst, {temp(7, "xxxx"), ref});
        break;
      case GX_GREATER:
        as.op("SGT", dst, {temp(7, "xxxx"), ref});
        break;
      case GX_NEQUAL:
        as.op("SNE", dst, {temp(7, "xxxx"), ref});
        break;
      case GX_GEQUAL:
        as.op("SGE", dst, {temp(7, "xxxx"), ref});
        break;
      default:
        as.op("MOV", dst, {one()});
        break;
      }
    };
    compare(ac.comp0, "R8.x", "xxxx");
    compare(ac.comp1, "R8.y", "yyyy");
    switch (ac.op) {
    case GX_AOP_AND:
      as.op("MUL", "R8.x", {temp(8, "xxxx"), temp(8, "yyyy")});
      break;
    case GX_AOP_OR:
      as.op("MAX", "R8.x", {temp(8, "xxxx"), temp(8, "yyyy")});
      break;
    case GX_AOP_XOR:
      as.op("SNE", "R8.x", {temp(8, "xxxx"), temp(8, "yyyy")});
      break;
    default:
      as.op("SEQ", "R8.x", {temp(8, "xxxx"), temp(8, "yyyy")});
      break;
    }
    as.text += "MOVC RC.x, R8.x;\nKIL EQ.x;\n";
  }

  if (config.fogType != GX_FOG_NONE) {
    as.op("MOV_SAT", "R7.x", {input("FOGC", "xxxx")});
    switch (config.fogType & 7) {
    case GX_FOG_PERSP_EXP: // 1 - 2^(-8F)
      as.op("MUL", "R7.x", {temp(7, "xxxx"), constant(kConstLiterals2, "yyyy")});
      as.op("ADD", "R7.x", {temp(7, "xxxx"), temp(7, "xxxx")});
      as.op("EX2", "R7.x", {temp(7, "xxxx").negate()});
      as.op("ADD", "R7.x", {one(), temp(7, "xxxx").negate()});
      break;
    case GX_FOG_PERSP_EXP2: // 1 - 2^(-8F^2)
      as.op("MUL", "R7.x", {temp(7, "xxxx"), temp(7, "xxxx")});
      as.op("MUL", "R7.x", {temp(7, "xxxx"), constant(kConstLiterals2, "yyyy")});
      as.op("ADD", "R7.x", {temp(7, "xxxx"), temp(7, "xxxx")});
      as.op("EX2", "R7.x", {temp(7, "xxxx").negate()});
      as.op("ADD", "R7.x", {one(), temp(7, "xxxx").negate()});
      break;
    case GX_FOG_PERSP_REVEXP: // 2^(-8(1-F))
      as.op("ADD", "R7.x", {one(), temp(7, "xxxx").negate()});
      as.op("MUL", "R7.x", {temp(7, "xxxx"), constant(kConstLiterals2, "yyyy")});
      as.op("ADD", "R7.x", {temp(7, "xxxx"), temp(7, "xxxx")});
      as.op("EX2", "R7.x", {temp(7, "xxxx").negate()});
      break;
    case GX_FOG_PERSP_REVEXP2: // 2^(-8(1-F)^2)
      as.op("ADD", "R7.x", {one(), temp(7, "xxxx").negate()});
      as.op("MUL", "R7.x", {temp(7, "xxxx"), temp(7, "xxxx")});
      as.op("MUL", "R7.x", {temp(7, "xxxx"), constant(kConstLiterals2, "yyyy")});
      as.op("ADD", "R7.x", {temp(7, "xxxx"), temp(7, "xxxx")});
      as.op("EX2", "R7.x", {temp(7, "xxxx").negate()});
      break;
    default: // linear
      break;
    }
    as.op("MOV_SAT", "R7.x", {temp(7, "xxxx")});
    as.op("LRP", "R0.xyz", {temp(7, "xxxx"), constant(kConstFogColor), temp(0)});
  }

  as.text += "MOV o[COLR], R0;\nEND\n";
  return as.text;
}

std::string build_copy_program(uint32_t format, bool forceOpaqueAlpha) {
  Assembler as;
  as.text = "!!FP2.0\n";
  as.text += kLiteralDecls;
  as.text += "TEX R0, f[TEX0], texture[0], 2D;\n";
  if (forceOpaqueAlpha) {
    as.op("MOV", "R0.w", {one()});
  }
  const auto intensity = [&](const char* dst) {
    as.op("DP3", dst, {temp(0, "xyzz"), constant(kConstIntensity, "xyzz")});
    as.op("ADD", dst, {temp(1, "xxxx"), constant(kConstIntensity, "wwww")});
  };
  switch (format) {
  case GX_TF_I4:
  case GX_TF_I8:
    intensity("R1.x");
    as.op("MOV", "R0", {temp(1, "xxxx")});
    break;
  case GX_TF_IA4:
  case GX_TF_IA8:
    intensity("R1.x");
    as.op("MOV", "R0.xyz", {temp(1, "xxxx")});
    break;
  case GX_TF_RGB565:
    as.op("MOV", "R0.w", {one()});
    break;
  case GX_CTF_R4:
  case GX_CTF_R8:
    as.op("MOV", "R0", {temp(0, "xxxx")});
    break;
  case GX_CTF_RA4:
  case GX_CTF_RA8:
    as.op("MOV", "R0.xyz", {temp(0, "xxxx")});
    break;
  case GX_CTF_A8:
    as.op("MOV", "R0", {temp(0, "wwww")});
    break;
  case GX_CTF_G8:
    as.op("MOV", "R0", {temp(0, "yyyy")});
    break;
  case GX_CTF_B8:
    as.op("MOV", "R0", {temp(0, "zzzz")});
    break;
  case GX_CTF_RG8:
    as.op("MOV", "R0", {temp(0, "xxxy")});
    break;
  case GX_CTF_GB8:
    as.op("MOV", "R0", {temp(0, "yyyz")});
    break;
  case GX_TF_Z8:
  case GX_TF_Z16:
  case GX_TF_Z24X8:
  case GX_CTF_Z4:
  case GX_CTF_Z8M:
  case GX_CTF_Z8L:
  case GX_CTF_Z16L:
    // Depth copies are not read back from the Z buffer; report the far plane.
    as.op("MOV", "R0", {one()});
    break;
  default:
    break;
  }
  as.text += "MOV o[COLR], R0;\nEND\n";
  return as.text;
}

uint32_t swap_halves(uint32_t v) { return (v >> 16) | (v << 16); }
} // namespace

struct FragmentProgram {
  rsxFragmentProgram header{};
  std::vector<uint32_t> ucode; // RSX layout (halfword-swapped words)
  // (word index, constant slot) for every embedded dynamic constant.
  std::vector<std::pair<uint32_t, uint8_t>> relocations;
  uint32_t textureMask = 0;
  // Last uploaded copy, reused while the constants it embeds are unchanged.
  uint32_t lastGeneration = 0;
  uint32_t lastOffset = 0;
  std::array<Vec4<float>, kConstCount> lastConstants{};
};

namespace {
std::unique_ptr<FragmentProgram> assemble(const std::string& source) {
  auto program = std::make_unique<FragmentProgram>();
  const AssembledProgram assembled = assemble_fragment_program(source);
  auto& fp = program->header;
  fp.magic = ('F' << 8) | 'P';
  fp.num_regs = assembled.numRegs;
  fp.fp_control = assembled.fpControl;
  // Texture coordinates carry s, t and q: leave the 2D-only interpolation hint off.
  fp.texcoords = assembled.texcoords;
  fp.texcoord2D = 0;
  fp.texcoord3D = 0;
  fp.num_insn = assembled.instructionCount;
  program->ucode.resize(assembled.words.size());
  for (size_t i = 0; i < assembled.words.size(); ++i) {
    program->ucode[i] = swap_halves(assembled.words[i]);
  }
  for (const auto& [insn, index] : assembled.constRelocations) {
    if (index >= 0 && index < kConstCount) {
      program->relocations.emplace_back(insn * 4, static_cast<uint8_t>(index));
    }
  }
  if (program->ucode.empty()) {
    Log.error("fragment program failed to assemble:\n{}", source);
  }
  return program;
}

std::unordered_map<HashType, std::unique_ptr<FragmentProgram>> g_gxPrograms;
std::unordered_map<uint32_t, std::unique_ptr<FragmentProgram>> g_copyPrograms;
const FragmentProgram* g_boundProgram = nullptr;

rsxVertexProgram g_vertexProgram{};
std::vector<uint32_t> g_vertexUcode;
} // namespace

const FragmentProgram* gx_fragment_program(const gx::ShaderConfig& config) noexcept {
  const HashType hash = xxh3_hash(config);
  auto& entry = g_gxPrograms[hash];
  if (!entry) {
    entry = assemble(build_tev_program(config));
    for (u32 i = 0; i < config.tevStageCount; ++i) {
      const auto dep = gx::tev_stage_texture_dependency(config, i);
      if (dep.canSampleTexture) {
        entry->textureMask |= 1u << dep.texMapId;
      }
    }
  }
  return entry.get();
}

const FragmentProgram* copy_fragment_program(uint32_t copyFormat, bool forceOpaqueAlpha) noexcept {
  const uint32_t key = (copyFormat << 1) | (forceOpaqueAlpha ? 1u : 0u);
  auto& entry = g_copyPrograms[key];
  if (!entry) {
    entry = assemble(build_copy_program(copyFormat, forceOpaqueAlpha));
    entry->textureMask = 1;
  }
  return entry.get();
}

uint32_t fragment_constant_count(const FragmentProgram*) noexcept { return kConstCount; }

uint32_t fragment_texture_mask(const FragmentProgram* program) noexcept { return program->textureMask; }

void bind_fragment_program(const FragmentProgram* constProgram, const Vec4<float>* constants) noexcept {
  auto* program = const_cast<FragmentProgram*>(constProgram);
  bool same = program->lastGeneration == stream_generation();
  if (same && constants != nullptr) {
    for (const auto& [word, slot] : program->relocations) {
      if (std::memcmp(&program->lastConstants[slot], &constants[slot], sizeof(Vec4<float>)) != 0) {
        same = false;
        break;
      }
    }
  }
  if (!same) {
    const uint32_t size = static_cast<uint32_t>(program->ucode.size() * sizeof(uint32_t));
    const Stream stream = stream_alloc(size, 64);
    auto* words = static_cast<uint32_t*>(stream.ptr);
    std::memcpy(words, program->ucode.data(), size);
    if (constants != nullptr) {
      for (const auto& [word, slot] : program->relocations) {
        uint32_t raw[4];
        std::memcpy(raw, &constants[slot], sizeof(raw));
        for (int i = 0; i < 4; ++i) {
          words[word + i] = swap_halves(raw[i]);
        }
        program->lastConstants[slot] = constants[slot];
      }
    }
    program->lastGeneration = stream_generation();
    program->lastOffset = stream.offset;
    rsxLoadFragmentProgramLocation(g_ctx, &program->header, stream.offset, GCM_LOCATION_CELL);
  } else if (g_boundProgram != program) {
    rsxLoadFragmentProgramLocation(g_ctx, &program->header, program->lastOffset, GCM_LOCATION_CELL);
  }
  g_boundProgram = program;
}

void bind_passthrough_vertex_program() noexcept {
  rsxLoadVertexProgram(g_ctx, &g_vertexProgram, g_vertexUcode.data());
}

void initialize_programs() noexcept {
  // Positions arrive in clip space and everything else is already final: pass it through.
  std::string source =
      "!!VP2.0\n"
      "MOV o[HPOS], v[OPOS];\n"
      "MOV o[COL0], v[COL0];\n"
      "MOV o[COL1], v[COL1];\n"
      "MOV o[FOGC].x, v[FOGC].x;\n";
  for (int i = 0; i < 8; ++i) {
    source += "MOV o[TEX" + std::to_string(i) + "], v[TEX" + std::to_string(i) + "];\n";
  }
  source += "END\n";
  const AssembledProgram assembled = assemble_vertex_program(source);
  g_vertexProgram = {};
  g_vertexProgram.magic = ('V' << 8) | 'P';
  g_vertexProgram.num_regs = assembled.numRegs;
  g_vertexProgram.input_mask = assembled.inputMask;
  g_vertexProgram.output_mask = assembled.outputMask;
  g_vertexProgram.num_insn = assembled.instructionCount;
  g_vertexUcode = assembled.words;
  bind_passthrough_vertex_program();
}
} // namespace aurora::rsx
