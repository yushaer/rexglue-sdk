/**
 * @file        system/mods.cpp
 * @brief       Mods: folders of replacement game files, textures and shaders
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/mods.h>

#include <algorithm>
#include <string_view>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mods, true, "Mods", "Load mods (restart to apply)");
REXCVAR_DEFINE_STRING(mods_path, "mods", "Mods",
                      "Folder of the mods, relative to the executable (restart to apply)");
REXCVAR_DEFINE_STRING(mods_disabled, "", "Mods",
                      "Mods turned off: their folder names, separated by commas (restart to "
                      "apply)");
REXCVAR_DEFINE_STRING(mods_folders, "", "Mods",
                      "Set at startup: the enabled mods' folders, the highest priority first, "
                      "separated by |")
    .transient();
REXCVAR_DEFINE_COMMAND(
    mods_reload,
    [] {
      rex::mods::Initialize();
      rex::cvar::InvokeCommand("material_shaders_reload", "");
    },
    "Mods",
    "Find the mods again, and reload their material shaders, shader options "
    "and lights (texture packs and game files apply at the next start)");

namespace rex::mods {

namespace {

std::vector<ModInfo> mods_found;

std::filesystem::path Utf8Path(std::string_view text) {
  return std::filesystem::u8path(text.begin(), text.end());
}

std::vector<std::string> SplitList(std::string_view text, char separator) {
  std::vector<std::string> items;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(separator, start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view item = text.substr(start, end - start);
    while (!item.empty() && item.front() == ' ') {
      item.remove_prefix(1);
    }
    while (!item.empty() && item.back() == ' ') {
      item.remove_suffix(1);
    }
    if (!item.empty()) {
      items.emplace_back(item);
    }
    start = end + 1;
  }
  return items;
}

uint32_t CountFiles(const std::filesystem::path& folder, bool recursive) {
  std::error_code error;
  if (!std::filesystem::is_directory(folder, error)) {
    return 0;
  }
  uint32_t count = 0;
  if (recursive) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, error)) {
      count += entry.is_regular_file(error) ? 1 : 0;
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
      count += entry.is_regular_file(error) ? 1 : 0;
    }
  }
  return count;
}

void PublishFolders() {
  std::string folders;
  for (const std::filesystem::path& folder : GetEnabledFolders()) {
    if (!folders.empty()) {
      folders += '|';
    }
    folders += folder.u8string();
  }
  rex::cvar::SetFlagByName("mods_folders", folders);
}

}  // namespace

void Initialize() {
  mods_found.clear();
  if (!REXCVAR_GET(mods)) {
    PublishFolders();
    return;
  }
  std::filesystem::path root = Utf8Path(REXCVAR_GET(mods_path));
  if (root.is_relative()) {
    root = rex::filesystem::GetExecutableFolder() / root;
  }
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) {
    PublishFolders();
    return;
  }
  std::vector<std::string> disabled = SplitList(REXCVAR_GET(mods_disabled), ',');
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    if (!entry.is_directory(error)) {
      continue;
    }
    ModInfo mod;
    mod.folder = entry.path();
    mod.id = entry.path().filename().u8string();
    mod.name = mod.id;
    std::filesystem::path manifest = entry.path() / "mod.toml";
    if (std::filesystem::is_regular_file(manifest, error)) {
      try {
        toml::table config = toml::parse_file(manifest.u8string());
        if (const toml::table* info = config["mod"].as_table()) {
          mod.name = (*info)["name"].value_or(mod.id);
          mod.author = (*info)["author"].value_or(std::string());
          mod.version = (*info)["version"].value_or(std::string());
          mod.description = (*info)["description"].value_or(std::string());
          mod.priority = int32_t((*info)["priority"].value_or(int64_t(0)));
        }
      } catch (const toml::parse_error& parse_error) {
        REXSYS_WARN("Mods: {}: {}", manifest.u8string(), parse_error.description());
      }
    } else {
      // Not a mod (no manifest) - but a folder of the known kinds still is.
      bool any = false;
      for (const char* kind : {"files", "textures", "materials"}) {
        any |= std::filesystem::is_directory(entry.path() / kind, error);
      }
      if (!any) {
        continue;
      }
    }
    mod.enabled = std::find(disabled.begin(), disabled.end(), mod.id) == disabled.end();
    mod.file_count = CountFiles(entry.path() / "files", true);
    mod.texture_count = CountFiles(entry.path() / "textures", true);
    mod.material_count = CountFiles(entry.path() / "materials" / "d3d12", false);
    mods_found.push_back(std::move(mod));
  }
  std::stable_sort(mods_found.begin(), mods_found.end(), [](const ModInfo& a, const ModInfo& b) {
    return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
  });
  for (const ModInfo& mod : mods_found) {
    REXSYS_INFO("Mods: {} ({}{}{}) - priority {}, {} files, {} textures, {} material shaders{}",
                mod.name, mod.id, mod.version.empty() ? "" : " ", mod.version, mod.priority,
                mod.file_count, mod.texture_count, mod.material_count,
                mod.enabled ? "" : " - disabled");
  }
  PublishFolders();
}

const std::vector<ModInfo>& GetMods() { return mods_found; }

std::vector<std::filesystem::path> GetEnabledFolders() {
  std::vector<std::filesystem::path> folders;
  for (const ModInfo& mod : mods_found) {
    if (mod.enabled) {
      folders.push_back(mod.folder);
    }
  }
  return folders;
}

void SetEnabled(const std::string& id, bool enabled) {
  std::vector<std::string> disabled = SplitList(REXCVAR_GET(mods_disabled), ',');
  disabled.erase(std::remove(disabled.begin(), disabled.end(), id), disabled.end());
  if (!enabled) {
    disabled.push_back(id);
  }
  std::string text;
  for (const std::string& item : disabled) {
    if (!text.empty()) {
      text += ',';
    }
    text += item;
  }
  rex::cvar::SetFlagByName("mods_disabled", text);
  for (ModInfo& mod : mods_found) {
    if (mod.id == id) {
      mod.enabled = enabled;
    }
  }
}

}  // namespace rex::mods
