/**
 * @file    gpu/render/format.cpp
 */
#include "renut_engine/gpu/render/format.h"

#include "renut_engine/gpu/backend/backend.h"      // banjo-nx (2026-10-19): IsDeko3D
#include "renut_engine/gpu/banjo_renut_tuning.h"  // banjo-nx (2026-10-19): Depth24On
#include "renut_engine/gpu/ultimate.h"  // renut-ultimate: Fixed16FloatOn

#include <cstring>

#include <rex/cvar.h>

// renut-ultimate (2026-11-16, port 145; tw2de_nx's tw2de_deko3d_native_formats of 2026-10-25, ON at the user's word:
// "port the speed ups as on by default").
REXCVAR_DEFINE_BOOL(banjo_deko3d_native_16bit, true, "Banjo/Renut",
                    "tw2de_nx (exact): the game's unsigned 5:6:5, 1:5:5:5 and 4:4:4:4 textures in the GPU's own 16-bit "
                    "formats (deko3d RGB565, RGB5A1, RGBA4: the same bit layout), the guest's words copied. false: "
                    "expanded on the CPU to RGBA8 at upload, twice the memory and the GPU's reads, as before. deko3d only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

namespace {
// Port 145: banjo_deko3d_native_16bit, read once.
bool Native16Bit() {
  static const bool on = IsDeko3D() && REXCVAR_GET(banjo_deko3d_native_16bit);
  return on;
}
}  // namespace

// sasrt-nx (2026-11-11, port 75; its formats batch, row 38 and the diagnostic): the 32-bit float formats the Xenos
// cannot filter (renut_float32_point_filter), and the formats' names for renut_diag_formats' lines.
bool IsFloat32TextureFormat(GpuTextureFormat format) {
  return format == GpuTextureFormat::Fmt32_Float || format == GpuTextureFormat::Fmt32_32_Float ||
         format == GpuTextureFormat::Fmt32_32_32_32_Float || format == GpuTextureFormat::Fmt32_32_32_Float;
}
const char *GpuTextureFormatName(u32 format) {
  switch (format & 0x3F) {
  case 2: return "8";
  case 6: return "8_8_8_8";
  case 7: return "2_10_10_10";
  case 10: return "8_8";
  case 18: return "DXT1";
  case 19: return "DXT2_3";
  case 20: return "DXT4_5";
  case 22: return "24_8 (D24S8)";
  case 23: return "24_8_FLOAT (D24FS8)";
  case 25: return "16_16";
  case 26: return "16_16_16_16";
  case 31: return "16_16_FLOAT";
  case 32: return "16_16_16_16_FLOAT";
  case 36: return "32_FLOAT";
  case 37: return "32_32_FLOAT";
  case 38: return "32_32_32_32_FLOAT";
  case 49: return "DXN";
  case 54: return "2_10_10_10_AS_16_16_16_16";
  case 58: return "DXT3A";
  case 59: return "DXT5A";
  case 60: return "CTX1";
  case 57: return "32_32_32_FLOAT";
  case 62: return "8_8_8_8_GAMMA_EDRAM";
  case 63: return "2_10_10_10_FLOAT (7e3)";
  default: return "?";
  }
}

plume::RenderFormat ColorTargetFormat(GpuColorFormat format) {
  switch (format) {
  case GpuColorFormat::Fmt8_8_8_8:
  case GpuColorFormat::Fmt8_8_8_8_Gamma:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  // sasrt-nx (2026-11-11, port 75; its formats batch, rows 6, 8, 10, 74, 93: renut_format_10bit_exact): the console's
  // own 32 bits; 16 bits a channel before.
  case GpuColorFormat::Fmt2_10_10_10:
    return banjo::renut_gpu::Format10BitExactOn() ? plume::RenderFormat::R10G10B10A2_UNORM
                                                  : plume::RenderFormat::R16G16B16A16_UNORM;
  // sasrt-nx (2026-11-11, port 75; its formats batch, rows 7, 66, 73, 95, 96: renut_format_7e3_rg11b10, NOT exact,
  // off): 7e3 in 32 bits without alpha.
  case GpuColorFormat::Fmt2_10_10_10_Float:
    return banjo::renut_gpu::Format7e3Rg11b10On() ? plume::RenderFormat::R11G11B10_FLOAT
                                                  : plume::RenderFormat::R16G16B16A16_FLOAT;
  case GpuColorFormat::Fmt16_16_16_16_Float:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  // sasrt-nx (2026-11-11, port 75; its formats batch, found on the way: renut_format_edram_as_variants): these two fell
  // to the default (8-bit RGBA) before. Format 12 follows format 3, one storage format (its audit's finding 7).
  case GpuColorFormat::Fmt2_10_10_10_As10_10_10_10:
    if (!banjo::renut_gpu::FormatEdramAsVariantsOn())
      return plume::RenderFormat::R8G8B8A8_UNORM;
    return banjo::renut_gpu::Format10BitExactOn() ? plume::RenderFormat::R10G10B10A2_UNORM
                                                  : plume::RenderFormat::R16G16B16A16_UNORM;
  case GpuColorFormat::Fmt2_10_10_10_Float_As16_16_16_16:
    if (!banjo::renut_gpu::FormatEdramAsVariantsOn())
      return plume::RenderFormat::R8G8B8A8_UNORM;
    return banjo::renut_gpu::Format7e3Rg11b10On() ? plume::RenderFormat::R11G11B10_FLOAT
                                                  : plume::RenderFormat::R16G16B16A16_FLOAT;
  // sasrt-nx (2026-10-20, sasrt_renut_fixed16_float): Xenos' 16-bit fixed-point targets span -32..32; FP16 holds that
  // range, SNORM clipped it at 1.
  case GpuColorFormat::Fmt16_16:
    return banjo::renut_gpu::Fixed16FloatOn() ? plume::RenderFormat::R16G16_FLOAT : plume::RenderFormat::R16G16_SNORM;
  case GpuColorFormat::Fmt16_16_16_16:
    return banjo::renut_gpu::Fixed16FloatOn() ? plume::RenderFormat::R16G16B16A16_FLOAT : plume::RenderFormat::R16G16B16A16_SNORM;
  case GpuColorFormat::Fmt16_16_Float:
    return plume::RenderFormat::R16G16_FLOAT;
  case GpuColorFormat::Fmt32_Float:
    return plume::RenderFormat::R32_FLOAT;
  case GpuColorFormat::Fmt32_32_Float:
    return plume::RenderFormat::R32G32_FLOAT;
  default:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  }
}

plume::RenderFormat DepthTargetFormat() {
  // Both Xenos depth formats (24-bit unorm and 20e4 float) fit in 32-bit float
  // depth; plume offers no 24-bit depth format.
  return plume::RenderFormat::D32_FLOAT_S8_UINT;
}

plume::RenderFormat DepthTargetFormat(u32 d3d_format) {
  // banjo-nx (2026-10-19, banjo_deko3d_depth24): the Xenos stores D24S8 depth as 24-bit unorm; a host Z24S8 target
  // holds the same values (a resolve or a fetch gives the unorm as a float, as the Xenos's does) in half the bytes of
  // D32_FLOAT_S8_UINT. The 20e4 float format (D24FS8) has no host equivalent and stays 32-bit float.
  if ((d3d_format & 0x3F) == u32(GpuTextureFormat::Fmt24_8) && IsDeko3D() && banjo::renut_gpu::Depth24On())
    return plume::RenderFormat::D24_UNORM_S8_UINT;
  return DepthTargetFormat();
}

plume::RenderBlend ConvertBlendFactor(u32 factor) {
  switch (static_cast<D3DBlend>(factor)) {
  case D3DBlend::Zero:
    return plume::RenderBlend::ZERO;
  case D3DBlend::One:
    return plume::RenderBlend::ONE;
  case D3DBlend::SrcColor:
    return plume::RenderBlend::SRC_COLOR;
  case D3DBlend::InvSrcColor:
    return plume::RenderBlend::INV_SRC_COLOR;
  case D3DBlend::SrcAlpha:
    return plume::RenderBlend::SRC_ALPHA;
  case D3DBlend::InvSrcAlpha:
    return plume::RenderBlend::INV_SRC_ALPHA;
  case D3DBlend::DestColor:
    return plume::RenderBlend::DEST_COLOR;
  case D3DBlend::InvDestColor:
    return plume::RenderBlend::INV_DEST_COLOR;
  case D3DBlend::DestAlpha:
    return plume::RenderBlend::DEST_ALPHA;
  case D3DBlend::InvDestAlpha:
    return plume::RenderBlend::INV_DEST_ALPHA;
  case D3DBlend::BlendFactor:
  case D3DBlend::ConstantAlpha:
    return plume::RenderBlend::BLEND_FACTOR;
  case D3DBlend::InvBlendFactor:
  case D3DBlend::InvConstantAlpha:
    return plume::RenderBlend::INV_BLEND_FACTOR;
  case D3DBlend::SrcAlphaSat:
    return plume::RenderBlend::SRC_ALPHA_SAT;
  default:
    return plume::RenderBlend::ONE;
  }
}

plume::RenderBlendOperation ConvertBlendOp(u32 op) {
  switch (static_cast<D3DBlendOp>(op)) {
  case D3DBlendOp::Subtract:
    return plume::RenderBlendOperation::SUBTRACT;
  case D3DBlendOp::Min:
    return plume::RenderBlendOperation::MIN;
  case D3DBlendOp::Max:
    return plume::RenderBlendOperation::MAX;
  case D3DBlendOp::RevSubtract:
    return plume::RenderBlendOperation::REV_SUBTRACT;
  case D3DBlendOp::Add:
  default:
    return plume::RenderBlendOperation::ADD;
  }
}

plume::RenderComparisonFunction ConvertCompareFunction(u32 func) {
  switch (static_cast<D3DCmpFunc>(func & 7)) {
  case D3DCmpFunc::Never:
    return plume::RenderComparisonFunction::NEVER;
  case D3DCmpFunc::Less:
    return plume::RenderComparisonFunction::LESS;
  case D3DCmpFunc::Equal:
    return plume::RenderComparisonFunction::EQUAL;
  case D3DCmpFunc::LessEqual:
    return plume::RenderComparisonFunction::LESS_EQUAL;
  case D3DCmpFunc::Greater:
    return plume::RenderComparisonFunction::GREATER;
  case D3DCmpFunc::NotEqual:
    return plume::RenderComparisonFunction::NOT_EQUAL;
  case D3DCmpFunc::GreaterEqual:
    return plume::RenderComparisonFunction::GREATER_EQUAL;
  case D3DCmpFunc::Always:
  default:
    return plume::RenderComparisonFunction::ALWAYS;
  }
}

plume::RenderStencilOp ConvertStencilOp(u32 op) {
  switch (static_cast<D3DStencilOp>(op & 7)) {
  case D3DStencilOp::Zero:
    return plume::RenderStencilOp::ZERO;
  case D3DStencilOp::Replace:
    return plume::RenderStencilOp::REPLACE;
  case D3DStencilOp::IncrSat:
    return plume::RenderStencilOp::INCREMENT_AND_CLAMP;
  case D3DStencilOp::DecrSat:
    return plume::RenderStencilOp::DECREMENT_AND_CLAMP;
  case D3DStencilOp::Invert:
    return plume::RenderStencilOp::INVERT;
  case D3DStencilOp::Incr:
    return plume::RenderStencilOp::INCREMENT_AND_WRAP;
  case D3DStencilOp::Decr:
    return plume::RenderStencilOp::DECREMENT_AND_WRAP;
  case D3DStencilOp::Keep:
  default:
    return plume::RenderStencilOp::KEEP;
  }
}

TextureFormatInfo GetTextureFormatInfo(GpuTextureFormat format, bool is_signed) {
  using F = plume::RenderFormat;
  using C = TexelConversion;
  auto plain = [](F host, u32 bytes) {
    return TextureFormatInfo{host, C::Copy, 1, bytes, bytes, false};
  };
  auto expand = [](F host, C conversion, u32 guest_bytes, u32 host_bytes) {
    return TextureFormatInfo{host, conversion, 1, guest_bytes, host_bytes, false};
  };
  auto bc = [](F host, u32 bytes) { return TextureFormatInfo{host, C::Copy, 4, bytes, bytes, true}; };

  using T = GpuTextureFormat;
  switch (format) {
  case T::Fmt8:
  case T::Fmt8_A:
  case T::Fmt8_B:
    return plain(is_signed ? F::R8_SNORM : F::R8_UNORM, 1);
  case T::Fmt8_8:
    return plain(is_signed ? F::R8G8_SNORM : F::R8G8_UNORM, 2);
  case T::Fmt8_8_8_8:
  case T::Fmt8_8_8_8_A:
  case T::Fmt8_8_8_8_As16_16_16_16:
    return plain(is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM, 4);
  // renut-ultimate (2026-11-16, port 145; tw2de_nx's, banjo_deko3d_native_16bit): the unsigned ones as the GPU's own
  // formats, the guest's words copied (the signed ones as before: the expansion makes them unsigned too).
  case T::Fmt1_5_5_5:
    return Native16Bit() && !is_signed ? plain(F::R5G5B5A1_UNORM, 2) : expand(F::R8G8B8A8_UNORM, C::R5G5B5A1, 2, 4);
  case T::Fmt5_6_5:
    return Native16Bit() && !is_signed ? plain(F::R5G6B5_UNORM, 2) : expand(F::R8G8B8A8_UNORM, C::R5G6B5, 2, 4);
  case T::Fmt4_4_4_4:
    return Native16Bit() && !is_signed ? plain(F::R4G4B4A4_UNORM, 2) : expand(F::R8G8B8A8_UNORM, C::R4G4B4A4, 2, 4);
  // sasrt-nx (2026-11-11, port 75; its formats batch, rows 6, 8, 10, 74, 93: renut_format_10bit_exact): 2_10_10_10
  // copied as it is into 10:10:10:2 (X in bits 0-9 is the host's R, as the expansion below read it); the
  // _AS_16_16_16_16 form (filtered at 16 bits on the console) keeps 16.
  case T::Fmt2_10_10_10:
    if (banjo::renut_gpu::Format10BitExactOn())
      return plain(F::R10G10B10A2_UNORM, 4);
    return expand(F::R16G16B16A16_UNORM, C::R10G10B10A2, 4, 8);
  case T::Fmt2_10_10_10_As16_16_16_16:
    return expand(F::R16G16B16A16_UNORM, C::R10G10B10A2, 4, 8);
  case T::Fmt11_11_10:
  case T::Fmt11_11_10_As16_16_16_16:
    return expand(F::R16G16B16A16_UNORM, C::R11G11B10, 4, 8);
  case T::Fmt10_11_11:
  case T::Fmt10_11_11_As16_16_16_16:
    return expand(F::R16G16B16A16_UNORM, C::R10G11B11, 4, 8);
  case T::FmtDXT1:
  case T::FmtDXT1_As16_16_16_16:
    return bc(F::BC1_UNORM, 8);
  case T::FmtDXT2_3:
  case T::FmtDXT2_3_As16_16_16_16:
    return bc(F::BC2_UNORM, 16);
  case T::FmtDXT4_5:
  case T::FmtDXT4_5_As16_16_16_16:
    return bc(F::BC3_UNORM, 16);
  case T::FmtDXT5A:
    return bc(F::BC4_UNORM, 8);
  case T::FmtDXT3A: {  // vivapinata-nx (2026-11-02): Viva Pinata's 512x512 masks; no host format holds it alone
    TextureFormatInfo info{F::R8_UNORM, C::DXT3A, 4, 8, 4 * 4, false};
    info.decoded_blocks = true;
    return info;
  }
  case T::FmtDXN:
    return bc(F::BC5_UNORM, 16);
  case T::FmtCTX1: {
    TextureFormatInfo info{F::R8G8_UNORM, C::CTX1, 4, 8, 4 * 4 * 2, false};
    info.decoded_blocks = true;
    return info;
  }
  case T::Fmt24_8:
  case T::Fmt24_8_Float:
    return expand(F::R32_FLOAT, C::Depth24, 4, 4);
  case T::Fmt16:
  case T::Fmt16_Expand:
    return plain(is_signed ? F::R16_SNORM : F::R16_UNORM, 2);
  case T::Fmt16_16:
  case T::Fmt16_16_Expand:
    return plain(is_signed ? F::R16G16_SNORM : F::R16G16_UNORM, 4);
  case T::Fmt16_16_16_16:
  case T::Fmt16_16_16_16_Expand:
    return plain(is_signed ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM, 8);
  case T::Fmt16_Float:
    return plain(F::R16_FLOAT, 2);
  case T::Fmt16_16_Float:
    return plain(F::R16G16_FLOAT, 4);
  case T::Fmt16_16_16_16_Float:
    return plain(F::R16G16B16A16_FLOAT, 8);
  case T::Fmt32_Float:
    return plain(F::R32_FLOAT, 4);
  case T::Fmt32_32_Float:
    return plain(F::R32G32_FLOAT, 8);
  case T::Fmt32_32_32_32_Float:
    return plain(F::R32G32B32A32_FLOAT, 16);
  default:
    return {};
  }
}

namespace {

inline uint8_t Expand(uint32_t value, uint32_t bits) {
  const uint32_t max = (1u << bits) - 1;
  return uint8_t((value * 255u + max / 2) / max);
}

inline uint16_t Expand16(uint32_t value, uint32_t bits) {
  const uint32_t max = (1u << bits) - 1;
  return uint16_t((uint64_t(value) * 65535u + max / 2) / max);
}

} // namespace

void ConvertTexels(TexelConversion conversion, const uint8_t *src, uint8_t *dst, uint32_t count) {
  // Xenos packs the first component in the lowest bits; the host formats put
  // it first in memory, so components are kept in XYZW order and the fetch
  // constant's swizzle is applied by the texture view.
  switch (conversion) {
  case TexelConversion::Copy:
    break;
  case TexelConversion::R5G6B5:
    for (uint32_t i = 0; i < count; ++i, src += 2, dst += 4) {
      uint16_t v;
      std::memcpy(&v, src, 2);
      dst[0] = Expand(v & 0x1F, 5);
      dst[1] = Expand((v >> 5) & 0x3F, 6);
      dst[2] = Expand((v >> 11) & 0x1F, 5);
      dst[3] = 0xFF;
    }
    break;
  case TexelConversion::R5G5B5A1:
    for (uint32_t i = 0; i < count; ++i, src += 2, dst += 4) {
      uint16_t v;
      std::memcpy(&v, src, 2);
      dst[0] = Expand(v & 0x1F, 5);
      dst[1] = Expand((v >> 5) & 0x1F, 5);
      dst[2] = Expand((v >> 10) & 0x1F, 5);
      dst[3] = (v & 0x8000) ? 0xFF : 0x00;
    }
    break;
  case TexelConversion::R4G4B4A4:
    for (uint32_t i = 0; i < count; ++i, src += 2, dst += 4) {
      uint16_t v;
      std::memcpy(&v, src, 2);
      dst[0] = Expand(v & 0xF, 4);
      dst[1] = Expand((v >> 4) & 0xF, 4);
      dst[2] = Expand((v >> 8) & 0xF, 4);
      dst[3] = Expand((v >> 12) & 0xF, 4);
    }
    break;
  case TexelConversion::R10G10B10A2:
  case TexelConversion::R11G11B10:
  case TexelConversion::R10G11B11: {
    uint32_t bits[4];
    if (conversion == TexelConversion::R10G10B10A2) {
      bits[0] = 10, bits[1] = 10, bits[2] = 10, bits[3] = 2;
    } else if (conversion == TexelConversion::R11G11B10) {
      bits[0] = 11, bits[1] = 11, bits[2] = 10, bits[3] = 0;
    } else {
      bits[0] = 10, bits[1] = 11, bits[2] = 11, bits[3] = 0;
    }
    for (uint32_t i = 0; i < count; ++i, src += 4, dst += 8) {
      uint32_t v;
      std::memcpy(&v, src, 4);
      uint16_t out[4];
      uint32_t shift = 0;
      for (uint32_t c = 0; c < 4; ++c) {
        if (!bits[c]) {
          out[c] = 0xFFFF;
          continue;
        }
        out[c] = Expand16((v >> shift) & ((1u << bits[c]) - 1), bits[c]);
        shift += bits[c];
      }
      std::memcpy(dst, out, 8);
    }
    break;
  }
  case TexelConversion::Depth24:
    for (uint32_t i = 0; i < count; ++i, src += 4, dst += 4) {
      uint32_t v;
      std::memcpy(&v, src, 4);
      const float depth = float(v >> 8) / float(0xFFFFFF);
      std::memcpy(dst, &depth, 4);
    }
    break;
  case TexelConversion::CTX1:
  case TexelConversion::DXT3A:
  case TexelConversion::Unsupported:
    break;
  }
}

void DecodeBlockRow(TexelConversion conversion, const uint8_t *src, uint8_t *dst, uint32_t count,
                    uint32_t dst_row_pitch) {
  if (conversion == TexelConversion::DXT3A) {
    // vivapinata-nx (2026-11-02): DXT3's alpha block: after the fetch's 8in16 swap, as BC2 reads it on the host, four
    // little-endian rows of 16 bits, texel x in bits 4x. A value v is v * 17 in 8 bits.
    for (uint32_t b = 0; b < count; ++b, src += 8) {
      for (uint32_t y = 0; y < 4; ++y) {
        const uint32_t row = uint32_t(src[y * 2]) | (uint32_t(src[y * 2 + 1]) << 8);
        uint8_t *out = dst + size_t(y) * dst_row_pitch + size_t(b) * 4;
        for (uint32_t x = 0; x < 4; ++x)
          out[x] = uint8_t(((row >> (4 * x)) & 15) * 17);
      }
    }
    return;
  }
  if (conversion != TexelConversion::CTX1)
    return;
  // CTX1, as the game's own encoder writes it (EncodeBlockCTX1, 0x828AC7A8):
  // two endpoints A and B of two components, then a 2-bit palette entry per
  // texel, one byte per row, texel x in bits 2x. Entries 0 and 1 are A and B,
  // 2 and 3 its 168:88 and 88:168 blends of them. In memory the block is
  // A.x A.y B.x B.y and rows 1 0 3 2; the fetch's 8in16 swap (applied before
  // this) leaves A.y A.x B.y B.x and rows 0 to 3.
  for (uint32_t b = 0; b < count; ++b, src += 8) {
    const uint8_t a[2] = {src[1], src[0]};
    const uint8_t e[2] = {src[3], src[2]};
    uint8_t palette[4][2];
    for (uint32_t c = 0; c < 2; ++c) {
      palette[0][c] = a[c];
      palette[1][c] = e[c];
      palette[2][c] = uint8_t((168u * a[c] + 88u * e[c] + 128u) >> 8);
      palette[3][c] = uint8_t((88u * a[c] + 168u * e[c] + 128u) >> 8);
    }
    for (uint32_t y = 0; y < 4; ++y) {
      const uint8_t row = src[4 + y];
      uint8_t *out = dst + size_t(y) * dst_row_pitch + size_t(b) * 4 * 2;
      for (uint32_t x = 0; x < 4; ++x) {
        const uint8_t *entry = palette[(row >> (2 * x)) & 3];
        out[x * 2 + 0] = entry[0];
        out[x * 2 + 1] = entry[1];
      }
    }
  }
}

// Port 164 (from fnxf_xiii, its note of 2026-11-06): eight bytes at a time (the texture uploads' untile swaps every
// byte of a DXT level 8in16, one byte pair at a time before); the same bytes in the same places, the tail as before.
// src and dst may be the same.
void SwapEndian(u32 endian, const void *src, void *dst, size_t bytes) {
  const auto *s = static_cast<const uint8_t *>(src);
  auto *d = static_cast<uint8_t *>(dst);
  size_t i = 0;
  switch (endian & 3) {
  case 1:  // 8in16
    for (; i + 8 <= bytes; i += 8) {
      uint64_t v;
      std::memcpy(&v, s + i, 8);
      v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
      std::memcpy(d + i, &v, 8);
    }
    for (; i + 1 < bytes; i += 2) {
      const uint8_t a = s[i], b = s[i + 1];
      d[i] = b;
      d[i + 1] = a;
    }
    break;
  case 2:  // 8in32
    for (; i + 8 <= bytes; i += 8) {
      uint64_t v;
      std::memcpy(&v, s + i, 8);
      v = __builtin_bswap64(v);
      v = (v >> 32) | (v << 32);  // each 32-bit word swapped in place
      std::memcpy(d + i, &v, 8);
    }
    for (; i + 3 < bytes; i += 4) {
      uint32_t v;
      std::memcpy(&v, s + i, 4);
      v = __builtin_bswap32(v);
      std::memcpy(d + i, &v, 4);
    }
    break;
  case 3:  // 16in32
    for (; i + 8 <= bytes; i += 8) {
      uint64_t v;
      std::memcpy(&v, s + i, 8);
      v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v >> 16) & 0x0000FFFF0000FFFFull);
      std::memcpy(d + i, &v, 8);
    }
    for (; i + 3 < bytes; i += 4) {
      uint32_t v;
      std::memcpy(&v, s + i, 4);
      v = (v >> 16) | (v << 16);
      std::memcpy(d + i, &v, 4);
    }
    break;
  default:
    if (d != s)
      std::memmove(d, s, bytes);
    break;
  }
}

} // namespace renut::gpu
