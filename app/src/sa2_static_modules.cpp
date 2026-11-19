// Nintendo Switch: the recompiled guest DLL (gd/DLL/xbox 360/Data_DLL.xex) is linked into the NRO instead of being a
// shared library the runtime opens (an NRO cannot load libraries). app/CMakeLists.txt partially links its archive into
// one object, makes everything but its two entry points local, and renames those two (both modules are generated
// with the same project name, so their other globals would collide); they are registered here under the shared
// library name the generated module registry uses (generated/module_registry.cpp: "renut_Data_DLL"), which
// KernelState::LoadUserModule looks up first (sdk rex/system/static_modules.h).
// sa2-nx (2026-10-06); sa2-nx-ult (2026-11-19): the base's project name, renut.

#include <rex/system/static_modules.h>

extern "C" void ReXModule_Register_renut_Data_DLL(rex::runtime::IModuleRegistrar* registrar);
extern "C" const rex::PPCImageInfo* ReXModule_GetImageInfo_renut_Data_DLL();

namespace {

const bool g_registered = [] {
  rex::system::RegisterStaticModule("renut_Data_DLL", ReXModule_Register_renut_Data_DLL,
                                    ReXModule_GetImageInfo_renut_Data_DLL);
  return true;
}();

}  // namespace
