// banjo-nx (2026-10-29): title update 3's files (title_update.h).
#include "renut_engine/title_update.h"

#include <cstdlib>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system.h>

namespace banjo::title_update {

namespace fs = std::filesystem;

void ConfigurePaths(rex::PathConfig &paths) {
  std::error_code ec;
  if (paths.update_data_root.empty()) {
    const fs::path candidates[] = {
        rex::filesystem::GetExecutableFolder() / "update",
        paths.game_data_root.empty() ? fs::path() : paths.game_data_root.parent_path() / "update",
        paths.game_data_root.empty() ? fs::path() : paths.game_data_root / "update",
    };
    for (const fs::path &c : candidates) {
      if (!c.empty() && fs::exists(c / "db_index.txt", ec)) {
        paths.update_data_root = c;
        break;
      }
    }
  }
  if (paths.update_data_root.empty())
    REXLOG_WARN("[title update] no update folder (update_data_root, or \"update\" next to banjo-nx or the game "
                "folder, holding title update 3's db_index.txt and bundles): the update's data is not there");
  else
    REXLOG_INFO("[title update] update data: {}", rex::path_to_utf8(paths.update_data_root));
  if (!paths.game_data_root.empty() && !fs::exists(paths.game_data_root / "default.xexp", ec)) {
    // The recompiled code is title update 3's: the base XEX's image would not match it at all.
    REXLOG_ERROR("[title update] {} has no default.xexp. banjo-nx is title update 3 of the game: put the update's "
                 "default.xexp next to default.xex (and its data files in the update folder)",
                 rex::path_to_utf8(paths.game_data_root));
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                              "banjo-nx needs title update 3: put the update's default.xexp next to default.xex in the "
                              "game folder, and its data files (db_index.txt and the bundles) in an \"update\" folder "
                              "next to banjo-nx.");
  }
}

}  // namespace banjo::title_update
