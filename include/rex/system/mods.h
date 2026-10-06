/**
 * @file        system/mods.h
 * @brief       Mods: folders of replacement game files, textures and shaders
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::mods {

// A mod is a folder in mods_path (<executable folder>/mods by default) with a
// mod.toml describing it:
//
//   [mod]
//   name = "Sharper Bowerstone"
//   author = "Someone"
//   version = "1.0"
//   description = "Hand-painted 4x textures for Bowerstone"
//   priority = 10   # higher wins where mods replace the same thing
//
// and any of:
//   files/       Game files, replacing the game's own (or added) at the same
//                path under the game's data folder: files/data/... for
//                game:\data\....
//   textures/    Replacement textures: <hash>.png or <hash>.dds, the hash of
//                the game's texture (dump_textures writes them, named so).
//   materials/   Material shaders (materials/d3d12/<hash>_<modification>.dxbc),
//                lights.toml, and custom textures for shaders
//                (materials/textures.toml).
//
// Enabled mods apply from the highest priority down; mods_disabled lists the
// folders turned off.

struct ModInfo {
  // The folder's name.
  std::string id;
  std::string name;
  std::string author;
  std::string version;
  std::string description;
  int32_t priority = 0;
  bool enabled = true;
  std::filesystem::path folder;
  // What it contains, for display.
  uint32_t file_count = 0;
  uint32_t texture_count = 0;
  uint32_t material_count = 0;
};

// Finds the mods and publishes the enabled ones' folders (mods_folders, for
// the GPU backends). Called once at startup, before the game's file system is
// set up.
void Initialize();

// All mods found, the highest priority first.
const std::vector<ModInfo>& GetMods();

// The enabled mods' folders, the highest priority first.
std::vector<std::filesystem::path> GetEnabledFolders();

// Turns a mod on or off (in mods_disabled - most of a mod applies at the next
// start).
void SetEnabled(const std::string& id, bool enabled);

}  // namespace rex::mods
