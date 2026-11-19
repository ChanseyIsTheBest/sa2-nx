// banjo-nx - NFSMW-only modules the native renderer core calls into, reduced to their neutral behaviour.
//
// The renderer core (nfsmw_nativo_*) came from the NFSMW port, where these lived in game-specific files
// (nfsmw_recorte_sombras.cpp, nfsmw_escenario_lod.cpp, nfsmw_render_targets.cpp, nfsmw_recortes_carrera.cpp,
// nfsmw_ajustes_graficos.cpp) that hook NFSMW functions and must not be built into Banjo. Each stub returns
// what makes the core behave as if the feature were off.

#include <cstdint>
#include <string>

#include "native/nfsmw_ajustes_graficos.h"
#include "native/nfsmw_reflejo_demanda.h"

// The 30 FPS guard dropped NFSMW's shadow pass when frames were slow. Off: the user's choice stands.
namespace nfsmw::guardia30 {
void Latir(double) {}
void Informe() {}
bool SinSombras(bool del_usuario) { return del_usuario; }
}  // namespace nfsmw::guardia30

namespace nfsmw::escenario_lod {
std::string Resumen() { return {}; }
}  // namespace nfsmw::escenario_lod

// NFSMW read its own MSAA mode from a game global (0x82A89574) to scale occlusion query results. Banjo's
// native path draws without MSAA (render targets are single-sampled), so the sample count is 1.
namespace nfsmw::render_targets {
uint32_t MuestrasOriginalesModoActual(const uint8_t*) { return 1; }
}  // namespace nfsmw::render_targets

// NFSMW's road reflection (resolved to kDireccion) was only drawn when read. Never measured here:
// MedirVisibilidad() == false keeps the core on its normal path. kDireccion (0x07C5A000) would only
// matter if Banjo resolved exactly there; the reflection hooks are not installed, so nothing enables it.
namespace nfsmw::reflejo_demanda {
void AnotarLectura() {}
void AnotarCopia() {}
void AnotarSwap() {}
bool MedirVisibilidad() { return false; }
bool VisibilidadComprobada() { return false; }
void AnotarVisible(bool) {}
void AnotarOculto() {}
void AnotarTestigo(bool) {}
}  // namespace nfsmw::reflejo_demanda

// NFSMW's graphics settings (post-processing, FXAA, glow, visual treatment): all at the game's look.
namespace nfsmw::ajustes {
Posproceso LeerPosproceso() { return {}; }
uint64_t VersionPosproceso() { return 0; }
bool AntialiasingFxaa() { return false; }
int ResplandorCielo() { return 0; }
int TratamientoVisual() { return 0; }
void VigilarAjustesEnVivo() {}
void AplicarAjustesGraficos() {}
void OcultarAjustesSinEfecto() {}
}  // namespace nfsmw::ajustes

// NFSMW's FlushState marker (nfsmw_d3d_registros_nativo.cpp) replaced ~13 register packets per draw with
// one NOP the ring decodes. It needs a hook on the game's FlushState, which Banjo inlines into every
// Draw*; without it no marker is ever written, so the consumer side only needs to exist.
namespace nfsmw::nativo {
void ActivarConsumidorMarcadores(bool) {}
void AnotarComprobacionMarcador(bool, uint32_t, uint32_t, uint32_t, uint32_t) {}
}  // namespace nfsmw::nativo
