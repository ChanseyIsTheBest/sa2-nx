#pragma once
// banjo-nx (2026-10-29): banjo-nx is recompiled from Banjo-Kazooie: Nuts & Bolts with title update 3 (v0.0.3.1, the
// XEX patch default.xexp applied to default.xex). The runtime applies the patch it finds next to default.xex
// (sdk/src/system/user_module.cpp, "<xex>p"), and mounts update_data_root as update:, where the game finds the update's
// data (db_index.txt and its bundles).
//
//   game folder:   default.xex, default.xexp (the update's), and the disc's files
//   update folder: the update's db_index.txt and bundle files (update_data_root; by default "update" next to the
//                  NRO / exe, or next to the game folder)

#include <rex/rex_app.h>

namespace banjo::title_update {

// RenutApp::OnConfigurePaths: the update folder when update_data_root is not set, and the check that default.xexp is
// there (the code is the update's: without the patch the game would run the base XEX's data through it).
void ConfigurePaths(rex::PathConfig &paths);

}  // namespace banjo::title_update
