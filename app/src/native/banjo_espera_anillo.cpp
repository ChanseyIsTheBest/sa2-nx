// banjo-nx - ring progress for the game's D3D waits (generic part of nfsmw_espera_anillo.cpp).
//
// The ring thread bumps a counter every time it writes the read pointer back or delivers an interrupt;
// a game thread that would spin waiting for the GPU can instead sleep until it changes. NFSMW hooked its
// two D3D polling loops (sub_8258B040, sub_8257BC3C) onto this; Banjo's equivalents are still to be
// identified, so for now only the ring side runs (AvisarProgresoAnillo is cheap with nobody waiting).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "native/nfsmw_nativo_sistema.h"

namespace nfsmw::nativo {
namespace {

std::mutex g_progreso_mutex;
std::condition_variable g_progreso_cv;
std::atomic<uint32_t> g_progreso{0};
std::atomic<int> g_esperando{0};

}  // namespace

uint32_t ProgresoAnillo() {
  return g_progreso.load(std::memory_order_acquire);
}

// Order: the ring thread writes the read pointer, bumps the counter and only then checks whether anyone
// is waiting, with the lock held (an unlocked check is a Dekker handshake that ARM may reorder: a lost
// wake-up). The waiter registers itself and checks the counter under the same lock.
void AvisarProgresoAnillo() {
  g_progreso.fetch_add(1, std::memory_order_acq_rel);
  bool avisar;
  {
    std::lock_guard<std::mutex> cerrojo(g_progreso_mutex);
    avisar = g_esperando.load(std::memory_order_acquire) > 0;
  }
  if (avisar) {
    g_progreso_cv.notify_all();
  }
}

bool EsperarProgresoAnillo(uint32_t visto, std::chrono::microseconds limite) {
  bool avanzo = false;
  {
    std::unique_lock<std::mutex> cerrojo(g_progreso_mutex);
    g_esperando.fetch_add(1, std::memory_order_acq_rel);
    avanzo = g_progreso_cv.wait_for(cerrojo, limite, [visto] {
      return g_progreso.load(std::memory_order_acquire) != visto;
    });
    g_esperando.fetch_sub(1, std::memory_order_acq_rel);
  }
  return avanzo;
}

}  // namespace nfsmw::nativo
