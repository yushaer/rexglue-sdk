/**
 * @file        graphics/pipeline/material_shaders.cpp
 * @brief       Hand-written replacements for the guest's shaders
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/material_shaders.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <unordered_map>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/pipeline/scene_lights.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/logging.h>

#define REX_MATERIAL_LIVE .lifecycle(rex::cvar::Lifecycle::kHotReload)

REXCVAR_DEFINE_BOOL(material_shaders, true, "GPU/Materials",
                    "Replace the game's own shaders with the improved material shaders");
REXCVAR_DEFINE_STRING(material_shaders_path, "materials", "GPU/Materials",
                      "Folder of the material shaders, relative to the executable");
REXCVAR_DEFINE_COMMAND(material_shaders_reload,
                       [] { rex::graphics::material_shaders::RequestReload(); }, "GPU/Materials",
                       "Read the material shaders from disk again");

REXCVAR_DEFINE_BOOL(material_soft_shadows, true, "GPU/Materials",
                    "Soft sun shadows: penumbras widen with the distance from what casts them")
    REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_shadow_softness, 1.0, "GPU/Materials",
                      "Size of the sun for soft shadows - larger is softer")
    .range(0.25, 4.0) REX_MATERIAL_LIVE;
REXCVAR_DEFINE_BOOL(material_specular, true, "GPU/Materials",
                    "Physically based highlights (GGX with Fresnel) instead of the game's "
                    "Phong")
    REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_specular_intensity, 1.0, "GPU/Materials",
                      "Strength of the physically based highlights")
    .range(0.0, 4.0) REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_roughness, 0.45, "GPU/Materials",
                      "Surface roughness - lower gives smaller, sharper highlights")
    .range(0.05, 1.0) REX_MATERIAL_LIVE;
REXCVAR_DEFINE_BOOL(material_character_lighting, true, "GPU/Materials",
                    "Characters: softer light falloff, and sheen from the sky at grazing "
                    "angles")
    REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_sheen_intensity, 0.5, "GPU/Materials",
                      "Strength of the sky's sheen on characters at grazing angles")
    .range(0.0, 2.0) REX_MATERIAL_LIVE;
REXCVAR_DEFINE_BOOL(material_foliage, true, "GPU/Materials",
                    "Grass and plants: light shining through the blades, softer lighting, "
                    "and blades that stay full in the distance")
    REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_foliage_translucency, 1.0, "GPU/Materials",
                      "How much light shines through grass and leaves")
    .range(0.0, 2.0) REX_MATERIAL_LIVE;
REXCVAR_DEFINE_BOOL(material_fire, true, "GPU/Materials",
                    "Fire: turbulent animated flames with brighter, glowing cores")
    REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_fire_intensity, 1.0, "GPU/Materials",
                      "Brightness of the flames' cores")
    .range(0.0, 3.0) REX_MATERIAL_LIVE;
REXCVAR_DEFINE_STRING(material_options, "", "GPU/Materials",
                      "The shader options (options.toml) set differently from their defaults: "
                      "id=value, separated by commas")
    REX_MATERIAL_LIVE;

namespace rex::graphics::material_shaders {

namespace {

std::filesystem::path Utf8Path(std::string_view text) {
  return std::filesystem::u8path(text.begin(), text.end());
}

std::atomic<bool> reload_requested{false};

// Seconds since the first frame, latched once per frame so that the draws of a
// frame share the system constants.
float frame_time = 0.0f;

// The shader options declared, by id: their slot and default.
struct OptionDefinition {
  uint32_t slot;
  float default_value;
};
std::unordered_map<std::string, OptionDefinition> option_definitions;
std::vector<CustomTexture> custom_textures;
bool options_loaded = false;
// The option values: the defaults with material_options applied (reparsed when
// it changes).
float option_values[kOptionSlotCount] = {};
std::string option_values_source;
bool option_values_valid = false;

void UpdateOptionValues() {
  if (!options_loaded) {
    LoadOptions();
  }
  const std::string& overrides = REXCVAR_GET(material_options);
  if (option_values_valid && overrides == option_values_source) {
    return;
  }
  option_values_source = overrides;
  option_values_valid = true;
  std::fill(std::begin(option_values), std::end(option_values), 0.0f);
  for (const auto& [id, definition] : option_definitions) {
    option_values[definition.slot] = definition.default_value;
  }
  size_t start = 0;
  while (start < overrides.size()) {
    size_t end = overrides.find(',', start);
    if (end == std::string::npos) {
      end = overrides.size();
    }
    std::string_view item = std::string_view(overrides).substr(start, end - start);
    size_t equals = item.find('=');
    if (equals != std::string_view::npos) {
      auto it = option_definitions.find(std::string(item.substr(0, equals)));
      if (it != option_definitions.end()) {
        option_values[it->second.slot] =
            std::strtof(std::string(item.substr(equals + 1)).c_str(), nullptr);
      }
    }
    start = end + 1;
  }
}

}  // namespace

void LoadOptions() {
  option_definitions.clear();
  custom_textures.clear();
  bool slot_taken[kOptionSlotCount] = {};
  bool texture_slot_taken[kCustomTextureSlotCount] = {};
  std::error_code error;
  for (const std::filesystem::path& folder : GetFolders()) {
    std::filesystem::path options_path = folder / "options.toml";
    if (std::filesystem::is_regular_file(options_path, error)) {
      try {
        toml::table config = toml::parse_file(options_path.u8string());
        if (const toml::array* options = config["option"].as_array()) {
          for (const toml::node& node : *options) {
            const toml::table* option = node.as_table();
            if (!option) {
              continue;
            }
            std::string id = (*option)["id"].value_or(std::string());
            int64_t slot = (*option)["slot"].value_or(int64_t(-1));
            if (id.empty() || slot < 0 || slot >= int64_t(kOptionSlotCount) ||
                option_definitions.count(id)) {
              continue;
            }
            if (slot_taken[slot]) {
              REXGPU_WARN("Material options: {}: slot {} of {} is already taken",
                          options_path.u8string(), slot, id);
              continue;
            }
            slot_taken[slot] = true;
            OptionDefinition definition;
            definition.slot = uint32_t(slot);
            definition.default_value = float((*option)["default"].value_or(0.0));
            option_definitions.emplace(id, definition);
          }
        }
      } catch (const toml::parse_error& parse_error) {
        REXGPU_WARN("Material options: {}: {}", options_path.u8string(),
                    parse_error.description());
      }
    }
    std::filesystem::path textures_path = folder / "textures.toml";
    if (std::filesystem::is_regular_file(textures_path, error)) {
      try {
        toml::table config = toml::parse_file(textures_path.u8string());
        if (const toml::array* textures = config["texture"].as_array()) {
          for (const toml::node& node : *textures) {
            const toml::table* texture = node.as_table();
            if (!texture) {
              continue;
            }
            int64_t slot = (*texture)["slot"].value_or(int64_t(-1));
            std::string file = (*texture)["file"].value_or(std::string());
            if (slot < 0 || slot >= int64_t(kCustomTextureSlotCount) || file.empty() ||
                texture_slot_taken[slot]) {
              continue;
            }
            texture_slot_taken[slot] = true;
            custom_textures.push_back({uint32_t(slot), folder / Utf8Path(file)});
          }
        }
      } catch (const toml::parse_error& parse_error) {
        REXGPU_WARN("Material textures: {}: {}", textures_path.u8string(),
                    parse_error.description());
      }
    }
  }
  options_loaded = true;
  option_values_valid = false;
  REXGPU_INFO("Material options: {} options, {} custom textures", option_definitions.size(),
              custom_textures.size());
}

const std::vector<CustomTexture>& GetCustomTextures() {
  if (!options_loaded) {
    LoadOptions();
  }
  return custom_textures;
}

bool IsEnabled() { return REXCVAR_GET(material_shaders); }

std::filesystem::path GetFolder() {
  std::filesystem::path folder = Utf8Path(REXCVAR_GET(material_shaders_path));
  if (folder.is_relative()) {
    folder = rex::filesystem::GetExecutableFolder() / folder;
  }
  return folder;
}

std::vector<std::filesystem::path> GetModFolders() {
  std::vector<std::filesystem::path> folders;
  std::string list = rex::cvar::GetFlagByName("mods_folders");
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find('|', start);
    if (end == std::string::npos) {
      end = list.size();
    }
    if (end > start) {
      folders.push_back(Utf8Path(std::string_view(list).substr(start, end - start)));
    }
    start = end + 1;
  }
  return folders;
}

std::vector<std::filesystem::path> GetFolders() {
  std::vector<std::filesystem::path> folders;
  for (const std::filesystem::path& mod_folder : GetModFolders()) {
    folders.push_back(mod_folder / "materials");
  }
  folders.push_back(GetFolder());
  return folders;
}

void RequestReload() { reload_requested.store(true, std::memory_order_relaxed); }

bool ConsumeReloadRequest() {
  // Translations made so far used the setting from the first frame on.
  static bool last_enabled = IsEnabled();
  bool enabled = IsEnabled();
  bool toggled = enabled != last_enabled;
  last_enabled = enabled;
  bool reload = reload_requested.exchange(false, std::memory_order_relaxed);
  if (reload) {
    scene_lights::LoadConfig(GetFolders());
    LoadOptions();
  }
  return reload || toggled;
}

void AdvanceFrame() {
  static const auto start = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  // Wrapped to keep the precision of animations in single-precision floats.
  frame_time = float(std::fmod(seconds, 3600.0));
}

float GetTime() { return frame_time; }

bool Load(uint64_t ucode_hash, uint64_t modification, std::string_view backend,
          std::string_view extension, std::vector<uint8_t>& binary_out) {
  std::error_code error;
  // In each folder (mods first), the exact modification first, then one for
  // any.
  std::vector<std::filesystem::path> paths;
  for (const std::filesystem::path& folder : GetFolders()) {
    std::filesystem::path directory = folder / Utf8Path(backend);
    if (!std::filesystem::is_directory(directory, error)) {
      continue;
    }
    paths.push_back(directory / Utf8Path(fmt::format("{:016X}_{:016X}.{}", ucode_hash,
                                                     modification, extension)));
    paths.push_back(directory / Utf8Path(fmt::format("{:016X}.{}", ucode_hash, extension)));
  }
  for (const std::filesystem::path& path : paths) {
    if (!std::filesystem::is_regular_file(path, error)) {
      continue;
    }
    FILE* file = rex::filesystem::OpenFile(path, "rb");
    if (!file) {
      continue;
    }
    std::vector<uint8_t> binary(size_t(std::filesystem::file_size(path, error)));
    bool read = !error && !binary.empty() &&
                fread(binary.data(), 1, binary.size(), file) == binary.size();
    fclose(file);
    if (!read) {
      REXGPU_WARN("Material shader {} could not be read", path.filename().string());
      continue;
    }
    REXGPU_INFO("Material shader {:016X} (modification {:016X}): {}", ucode_hash, modification,
                path.u8string());
    binary_out = std::move(binary);
    return true;
  }
  return false;
}

uint32_t GetVertexShaderTraits(const Shader& vertex_shader) {
  uint32_t traits = 0;
  if (vertex_shader.constant_register_map().float_dynamic_addressing) {
    traits |= kVertexShaderTraitIndexedConstants;
  }
  // Skinning blends bone matrices fetched from one stream at each of the
  // vertex's bone indices: full fetches of a binding at different index
  // registers or components.
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    bool first = true;
    uint32_t first_register = 0;
    SwizzleSource first_component = SwizzleSource::kX;
    for (const Shader::VertexBinding::Attribute& attribute : binding.attributes) {
      const ParsedVertexFetchInstruction& fetch = attribute.fetch_instr;
      if (fetch.is_mini_fetch) {
        continue;
      }
      const InstructionOperand& index = fetch.operands[0];
      if (first) {
        first = false;
        first_register = index.storage_index;
        first_component = index.components[0];
      } else if (index.storage_index != first_register || index.components[0] != first_component) {
        traits |= kVertexShaderTraitSkinned;
        return traits;
      }
    }
  }
  return traits;
}

void GetParams(float params_out[kMaterialParamsCount][4], uint32_t draw_resolution_scale_x,
               uint32_t draw_resolution_scale_y, uint32_t translation_flags,
               uint32_t vertex_shader_traits, uint64_t vertex_shader_hash) {
  uint32_t features = 0;
  if (REXCVAR_GET(material_soft_shadows)) {
    features |= kMaterialFeatureSoftShadows;
  }
  if (REXCVAR_GET(material_specular)) {
    features |= kMaterialFeatureSpecular;
  }
  if (REXCVAR_GET(material_character_lighting)) {
    features |= kMaterialFeatureCharacterLighting;
  }
  if (REXCVAR_GET(material_foliage)) {
    features |= kMaterialFeatureFoliage;
  }
  if (REXCVAR_GET(material_fire)) {
    features |= kMaterialFeatureFire;
  }
  params_out[0][0] = std::bit_cast<float>(features);
  params_out[0][1] = float(REXCVAR_GET(material_shadow_softness));
  params_out[0][2] = float(REXCVAR_GET(material_specular_intensity));
  params_out[0][3] = float(REXCVAR_GET(material_roughness));
  params_out[1][0] = frame_time;
  params_out[1][1] = float(REXCVAR_GET(material_sheen_intensity));
  params_out[1][2] = float(REXCVAR_GET(material_foliage_translucency));
  params_out[1][3] = float(REXCVAR_GET(material_fire_intensity));
  params_out[2][0] = std::bit_cast<float>(vertex_shader_traits);
  params_out[2][1] = std::bit_cast<float>(uint32_t(vertex_shader_hash));
  // Layer blending (2.zw, 3.w) is the backend's.
  params_out[2][2] = 0.0f;
  params_out[2][3] = 0.0f;
  params_out[3][0] = float(draw_resolution_scale_x);
  params_out[3][1] = float(draw_resolution_scale_y);
  params_out[3][2] = std::bit_cast<float>(translation_flags);
  params_out[3][3] = 0.0f;
  UpdateOptionValues();
  for (uint32_t slot = 0; slot < kOptionSlotCount; ++slot) {
    params_out[4 + slot / 4][slot % 4] = option_values[slot];
  }
  // The textures' rows are the backend's.
  for (uint32_t row = 12; row < kMaterialParamsCount; ++row) {
    for (uint32_t i = 0; i < 4; ++i) {
      params_out[row][i] = std::bit_cast<float>(kNoTexture);
    }
  }
}

}  // namespace rex::graphics::material_shaders
