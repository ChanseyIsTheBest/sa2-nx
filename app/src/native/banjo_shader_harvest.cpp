// banjo-nx - collects every shader container the game loads, for the native renderer's shader library.
//
// Nuts & Bolts builds most of its shader objects in place from its bundles (D3D "Common" word 0x0010000x):
// they never go through D3DDevice_CreateVertexShader/CreatePixelShader, and most draws reach the GPU through
// paths that bypass the hooked Draw* functions, so neither hook sees them. Instead, right after each bundle
// is loaded (assetDbBundleLoadBundle) and before D3D has patched anything, committed guest memory is scanned
// for in-place shader objects:
//   pixel shader:  word 0x00100007 at the object, the original container's virtual part at +0x28 (0x102A1100),
//                  the GPU address of its physical part at +0x18;
//   vertex shader: word 0x00100006 at the object, the virtual part at +0x368 (0x102A1101), physical part at +0x20.
// Each container (virtual part + physical part) is written to the "shaders" folder next to the exe as
// p_<hash>.bin / v_<hash>.bin, deduplicated by its virtual part: the first copy seen wins, which is the
// unpatched one when the scan follows the load. tools/build_shader_library.sh turns the folder into
// banjo_shaders.nfsp. Also scanned every 20 s as a fallback for loads through other paths.
//
// Development tool, PC only (it reads guest memory directly; on the Switch the guest memory is committed
// lazily by the exception handler). Enabled with banjo_volcar_shaders_en_sitio = true.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(banjo_volcar_shaders_en_sitio, false, "Nuts&Bolts/Graphics",
                    "Harvest the game's shader containers for the native renderer's library: after each bundle load "
                    "(and every 600 frames) scan memory for in-place shader objects and write each new container to a "
                    "'shaders' folder next to the exe. PC development tool.");

namespace {

inline uint32_t BE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

std::mutex g_mutex;
std::unordered_set<uint64_t> g_vistos;  // FNV of each virtual part already written
std::atomic<uint32_t> g_escritos{0};

#if !REX_PLATFORM_SWITCH
// Whether the host can read `n` bytes at `p`.
bool Legible(const uint8_t* p, size_t n) {
  while (n) {
    size_t largo = 0;
    rex::memory::PageAccess acceso;
    if (!rex::memory::QueryProtect(const_cast<uint8_t*>(p), largo, acceso) || !largo ||
        !(uint32_t(acceso) & uint32_t(rex::memory::PageAccess::kReadOnly)))
      return false;
    // The size counts from the start of p's page.
    const size_t dentro = uintptr_t(p) & 0xFFF;
    if (largo <= dentro) return false;
    largo -= dentro;
    if (largo >= n) return true;
    p += largo;
    n -= largo;
  }
  return true;
}

// One object candidate at `objeto`; returns true if a new container was written.
bool Recoger(rex::memory::Memory* memoria, uint32_t objeto, bool vertices) {
  const uint8_t* base = memoria->TranslateVirtual(objeto);
  const uint8_t* v = base + (vertices ? 0x368 : 0x28);
  const uint32_t virtuales = BE(v + 4), fisicos = BE(v + 8);
  if (virtuales < 36 || virtuales > 65536 || !fisicos || fisicos > 65536 || (fisicos & 3)) return false;
  const uint32_t gpu = BE(base + (vertices ? 0x20 : 0x18));
  if (gpu < 0xA0000000u) return false;  // the physical part lives in a physical-memory view
  if (!Legible(memoria->TranslateVirtual(gpu), fisicos) || !Legible(v, virtuales)) return false;
  const uint64_t clave = Fnv(v, virtuales) ^ (vertices ? 0x9E3779B97F4A7C15ull : 0);
  {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    if (!g_vistos.insert(clave).second) return false;
  }
  std::vector<uint8_t> contenedor(size_t(virtuales) + fisicos);
  std::memcpy(contenedor.data(), v, virtuales);
  std::memcpy(contenedor.data() + virtuales, memoria->TranslateVirtual(gpu), fisicos);
  const auto carpeta = rex::filesystem::GetExecutableFolder() / "shaders";
  std::error_code ec;
  std::filesystem::create_directories(carpeta, ec);
  char nombre[48];
  std::snprintf(nombre, sizeof(nombre), "%c_h%016llx.bin", vertices ? 'v' : 'p',
                (unsigned long long)Fnv(contenedor.data(), contenedor.size()));
  if (FILE* f = std::fopen((carpeta / nombre).string().c_str(), "wb")) {
    std::fwrite(contenedor.data(), 1, contenedor.size(), f);
    std::fclose(f);
    g_escritos.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}
#endif

void Escanear(const char* motivo) {
#if !REX_PLATFORM_SWITCH
  auto* memoria = rex::system::kernel_state()->memory();
  const auto inicio = std::chrono::steady_clock::now();
  uint32_t nuevos = 0;
  uint64_t bytes = 0;
  // Virtual heaps only (below the physical-memory views at 0xA0000000, which would repeat the same memory).
  uint64_t direccion = 0x00010000;
  while (direccion < 0xA0000000ull) {
    auto* heap = memoria->LookupHeap(uint32_t(direccion));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(direccion), &info) || !info.region_size) {
      direccion += 0x10000;
      continue;
    }
    const uint64_t fin = uint64_t(info.base_address) + info.region_size;
    if ((info.state & rex::memory::kMemoryAllocationCommit) && info.region_size >= 0x400) {
      // The guest heap's commit state is not always the host's (the SDK may commit host pages lazily): scan only
      // the host-readable runs inside the region.
      uint32_t inicio_tramo = 0;
      while (inicio_tramo < info.region_size) {
        uint8_t* h = memoria->TranslateVirtual(info.base_address + inicio_tramo);
        size_t largo = 0;
        rex::memory::PageAccess acceso;
        if (!rex::memory::QueryProtect(h, largo, acceso) || !largo) break;
        const uint32_t tramo = uint32_t(std::min<uint64_t>(largo, info.region_size - inicio_tramo));
        if (uint32_t(acceso) & uint32_t(rex::memory::PageAccess::kReadOnly)) {
          const uint32_t guest = info.base_address + inicio_tramo;
          bytes += tramo;
          for (uint32_t o = 0x28; o + 12 <= tramo; o += 4) {
            const uint32_t w = BE(h + o);
            if (w == 0x102A1100u && BE(h + o - 0x28) == 0x00100007u) {
              nuevos += Recoger(memoria, guest + o - 0x28, false) ? 1 : 0;
            } else if (w == 0x102A1101u && o >= 0x368 && BE(h + o - 0x368) == 0x00100006u) {
              nuevos += Recoger(memoria, guest + o - 0x368, true) ? 1 : 0;
            }
          }
        }
        inicio_tramo += tramo;
      }
    }
    direccion = fin > direccion ? fin : direccion + 0x1000;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inicio).count();
  REXLOG_INFO("[banjo] shader harvest ({}): {} new containers ({} in total), {:.0f} MB scanned in {:.0f} ms",
              motivo, nuevos, g_escritos.load(), double(bytes) / (1024.0 * 1024.0), ms);
#else
  (void)motivo;
#endif
}

}  // namespace

namespace banjo::harvest {

// From the frame boundary: a fallback scan every 600 game frames (~20 s) while harvesting.
void Fotograma() {
  static const bool activo = REXCVAR_GET(banjo_volcar_shaders_en_sitio);
  static uint32_t fotogramas = 0;
  if (activo && (++fotogramas % 600) == 30) Escanear("periodic");
}

}  // namespace banjo::harvest

REX_EXTERN(__imp__assetDbBundleLoadBundle);
REX_HOOK_RAW(assetDbBundleLoadBundle) {
  __imp__assetDbBundleLoadBundle(ctx, base);
  static const bool activo = REXCVAR_GET(banjo_volcar_shaders_en_sitio);
  if (activo) Escanear("bundle loaded");
}
