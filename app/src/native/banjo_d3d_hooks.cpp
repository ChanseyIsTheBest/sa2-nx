// banjo-nx - Banjo-Kazooie: Nuts & Bolts hooks for the native renderer (replaces NFSMW's hook addresses).
//
// The native renderer reads the game's PM4 ring, but two facts are only known on the game thread:
//   - which original shader container each D3D shader object came from (the vertex shader microcode
//     reaches the ring already patched by D3D, so it cannot be looked up by content);
//   - which shaders each Draw* used, so the ring can pair its DRAW_INDX packets with them.
// These hooks provide both, exactly as nfsmw_nativo_ganchos.cpp did for NFSMW, at Banjo's addresses
// (config/renut_gpu_funcs.toml, confirmed against the recompiled code):
//
//   D3DDevice_CreatePixelShader   0x826375D8  r3 = container (pFunction) -> r3 = object
//   D3DDevice_CreateVertexShader  0x826377C0  r3 = container (pFunction) -> r3 = object
//   D3DDevice_DrawVertices        0x821FB880  (dev, type, start, count)
//   D3DDevice_DrawIndexedVertices 0x8263F358  (dev, type, base, start, count)
//   D3DDevice_DrawVerticesUP      0x821FD698  (dev, type, count, data, stride)
//   D3DDevice_Swap                0x822554F8  game frame counter
//
// reNut's midasm hooks on the two constructors (shader_dump.cpp) live inside the __imp__ bodies and keep
// working: these hooks wrap the whole function.
//
// The bound-shader offsets of the 2008 XDK device (nfsmw_nativo_ganchos.cpp, kDispositivoVs/Ps) come from
// reading the recompiled FlushState. ComprobarDispositivo checks them while the game runs: every bound
// object must be one the constructors returned. A mismatch is logged once with the counts.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "native/nfsmw_nativo_ganchos.h"

// Game frames presented through D3D (the overlay's FPS and the native renderer's reports count them).
std::atomic<uint64_t> g_nfsmw_fotogramas_juego{0};

namespace {

namespace nativo = nfsmw::nativo;

inline uint32_t LeerBE32(const uint8_t* base, uint32_t direccion) {
  uint32_t v;
  std::memcpy(&v, base + direccion, 4);
  return __builtin_bswap32(v);
}

// ---- Check of the device offsets ------------------------------------------------------------------

std::mutex g_objetos_mutex;
std::unordered_set<uint32_t> g_vs_creados;
std::unordered_set<uint32_t> g_ps_creados;
std::atomic<uint32_t> g_comprobaciones{0};
std::atomic<uint32_t> g_vs_desconocidos{0};
std::atomic<uint32_t> g_ps_desconocidos{0};
std::atomic<bool> g_informado{false};

void RecordarObjeto(uint32_t objeto, bool vertices) {
  if (!objeto) return;
  std::lock_guard<std::mutex> cerrojo(g_objetos_mutex);
  (vertices ? g_vs_creados : g_ps_creados).insert(objeto);
}

// One draw in 64: are the objects at the device offsets ones the constructors returned? While checking,
// the first 16 KB of the device are also scanned for words that hold a known VS / PS object, so if the
// offsets are wrong the log names the right ones (the offset that holds a known object most often).
constexpr uint32_t kBytesDispositivo = 0x4000;
uint32_t g_aciertos_vs[kBytesDispositivo / 4];
uint32_t g_aciertos_ps[kBytesDispositivo / 4];

void InformarMejores(const char* que, const uint32_t* aciertos, uint32_t n) {
  uint32_t mejores[4] = {};
  for (uint32_t i = 0; i < kBytesDispositivo / 4; ++i) {
    for (int k = 0; k < 4; ++k) {
      if (aciertos[i] > aciertos[mejores[k]]) {
        for (int j = 3; j > k; --j) mejores[j] = mejores[j - 1];
        mejores[k] = i;
        break;
      }
    }
  }
  REXLOG_INFO("[banjo] device offsets holding a known {} object most often (of {} draws): +0x{:X} {}x, "
              "+0x{:X} {}x, +0x{:X} {}x, +0x{:X} {}x",
              que, n, mejores[0] * 4, aciertos[mejores[0]], mejores[1] * 4, aciertos[mejores[1]],
              mejores[2] * 4, aciertos[mejores[2]], mejores[3] * 4, aciertos[mejores[3]]);
}

void ComprobarDispositivo(const uint8_t* base, uint32_t dispositivo) {
  static uint32_t contador = 0;  // game thread only
  if ((++contador & 63) != 0 || !dispositivo || g_informado.load(std::memory_order_relaxed)) return;
  const uint32_t vs = LeerBE32(base, dispositivo + 0x3198);
  const uint32_t ps = LeerBE32(base, dispositivo + 0x3194);
  bool vs_ok, ps_ok;
  {
    std::lock_guard<std::mutex> cerrojo(g_objetos_mutex);
    vs_ok = !vs || g_vs_creados.count(vs) != 0;
    ps_ok = !ps || g_ps_creados.count(ps) != 0;
    for (uint32_t i = 0; i < kBytesDispositivo / 4; ++i) {
      const uint32_t v = LeerBE32(base, dispositivo + i * 4);
      if (v < 0x1000) continue;
      if (g_vs_creados.count(v)) ++g_aciertos_vs[i];
      if (g_ps_creados.count(v)) ++g_aciertos_ps[i];
    }
  }
  // Diagnostic: the first few unknown and known bound objects, word by word, to compare their layout.
  static uint32_t volcados_desconocidos = 0, volcados_conocidos = 0;
  auto volcar = [&](const char* que, uint32_t objeto) {
    std::string palabras;
    for (uint32_t i = 0; i < 24; ++i) palabras += fmt::format(" {:08X}", LeerBE32(base, objeto + i * 4));
    REXLOG_INFO("[banjo] {} bound object {:08X}:{}", que, objeto, palabras);
  };
  if (vs && !vs_ok && volcados_desconocidos < 3) {
    ++volcados_desconocidos;
    volcar("unknown VS", vs);
    for (uint32_t o = 0; o < 0x800; o += 4) {
      const uint32_t w = LeerBE32(base, vs + o);
      if ((w & 0xFFFFFF00u) == 0x102A1100u) {
        REXLOG_INFO("[banjo] unknown VS {:08X}: container magic {:08X} at +0x{:X}, vsize {:X}, psize {:X}", vs, w, o,
                    LeerBE32(base, vs + o + 4), LeerBE32(base, vs + o + 8));
      }
    }
  }
  if (vs && vs_ok && volcados_conocidos < 2) {
    for (uint32_t o = 0; o < 0x800; o += 4) {
      const uint32_t w = LeerBE32(base, vs + o);
      if ((w & 0xFFFFFF00u) == 0x102A1100u) {
        REXLOG_INFO("[banjo] known VS {:08X}: container magic {:08X} at +0x{:X}, vsize {:X}, psize {:X}", vs, w, o,
                    LeerBE32(base, vs + o + 4), LeerBE32(base, vs + o + 8));
      }
    }
  }
  if (ps && !ps_ok && volcados_desconocidos < 6) { ++volcados_desconocidos; volcar("unknown PS", ps); }
  if (vs && vs_ok && volcados_conocidos < 2) { ++volcados_conocidos; volcar("known VS", vs); }
  if (ps && ps_ok && volcados_conocidos < 4) { ++volcados_conocidos; volcar("known PS", ps); }

  const uint32_t n = g_comprobaciones.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!vs_ok) g_vs_desconocidos.fetch_add(1, std::memory_order_relaxed);
  if (!ps_ok) g_ps_desconocidos.fetch_add(1, std::memory_order_relaxed);
  if (n == 512 && !g_informado.exchange(true)) {
    const uint32_t mv = g_vs_desconocidos.load(), mp = g_ps_desconocidos.load();
    if (mv || mp) {
      REXLOG_ERROR("[banjo] device offsets: {} of {} bound VS and {} of {} bound PS were not created by "
                   "the hooked constructors: kDispositivoVs/Ps (0x3198/0x3194) are probably wrong",
                   mv, n, mp, n);
    } else {
      REXLOG_INFO("[banjo] device offsets confirmed: {} draws checked, every bound VS/PS is a known object", n);
    }
    std::lock_guard<std::mutex> cerrojo(g_objetos_mutex);
    REXLOG_INFO("[banjo] {} VS and {} PS objects created through the hooked constructors so far",
                g_vs_creados.size(), g_ps_creados.size());
    InformarMejores("VS", g_aciertos_vs, n);
    InformarMejores("PS", g_aciertos_ps, n);
  }
}

}  // namespace

// ---- Shader constructors ------------------------------------------------------------------------------

REX_EXTERN(__imp__sub_826375D8);
REX_HOOK_RAW(sub_826375D8) {  // D3DDevice_CreatePixelShader
  const auto* entrada = nativo::ganchos_detalle::IdentificarCreacion(base, ctx.r3.u32, false);
  __imp__sub_826375D8(ctx, base);
  nativo::ganchos_detalle::RecordarCreacion(ctx.r3.u32, entrada, false);
  RecordarObjeto(ctx.r3.u32, false);
}

REX_EXTERN(__imp__sub_826377C0);
REX_HOOK_RAW(sub_826377C0) {  // D3DDevice_CreateVertexShader
  const auto* entrada = nativo::ganchos_detalle::IdentificarCreacion(base, ctx.r3.u32, true);
  __imp__sub_826377C0(ctx, base);
  nativo::ganchos_detalle::RecordarCreacion(ctx.r3.u32, entrada, true);
  RecordarObjeto(ctx.r3.u32, true);
}

// ---- Draws: one record each, before the original, as NFSMW's d3d_trace hooks did ---------------------

REX_EXTERN(__imp__sub_821FB880);
REX_HOOK_RAW(sub_821FB880) {  // D3DDevice_DrawVertices(dev, type, start, count)
  ComprobarDispositivo(base, ctx.r3.u32);
  nativo::AnotarDibujo(nativo::FuncionDibujo::kVertices, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                       ctx.r6.u32, ctx.r7.u32);
  __imp__sub_821FB880(ctx, base);
  nativo::TerminarDibujo();
}

REX_EXTERN(__imp__sub_8263F358);
REX_HOOK_RAW(sub_8263F358) {  // D3DDevice_DrawIndexedVertices(dev, type, base, start, count)
  ComprobarDispositivo(base, ctx.r3.u32);
  nativo::AnotarDibujo(nativo::FuncionDibujo::kIndexados, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                       ctx.r6.u32, ctx.r7.u32);
  __imp__sub_8263F358(ctx, base);
  nativo::TerminarDibujo();
}

REX_EXTERN(__imp__sub_821FD698);
REX_HOOK_RAW(sub_821FD698) {  // D3DDevice_DrawVerticesUP(dev, type, count, data, stride)
  nativo::AnotarDibujo(nativo::FuncionDibujo::kVerticesUP, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                       ctx.r6.u32, ctx.r7.u32);
  __imp__sub_821FD698(ctx, base);
  nativo::TerminarDibujo();
}

// ---- Swap: the game's frame counter ------------------------------------------------------------------

REX_EXTERN(__imp__sub_822554F8);
REX_HOOK_RAW(sub_822554F8) {  // D3DDevice_Swap
  __imp__sub_822554F8(ctx, base);
  g_nfsmw_fotogramas_juego.fetch_add(1, std::memory_order_relaxed);
}
