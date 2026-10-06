/**
 * @file        graphics/pipeline/scene_lights.cpp
 * @brief       The game's local lights, captured from its draws
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/scene_lights.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <toml++/toml.hpp>

#include <rex/graphics/pipeline/material_shaders.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>

namespace rex::graphics::scene_lights {

namespace {

// A float constant ("c80") or one component of it ("c94.x").
struct ConstantRef {
  uint32_t index = 0;
  // 0-3, or 4 for the whole vector.
  uint32_t component = 4;
};

struct LightSpec {
  ConstantRef position;
  ConstantRef color;
  std::optional<ConstantRef> local;
  ConstantRef inverse_radius;
};

// Pixel shader hash -> its lights.
std::unordered_map<uint64_t, std::vector<LightSpec>> sources;

constexpr size_t kMaxLights = 64;
std::vector<Light> current_lights, previous_lights;

std::optional<ConstantRef> ParseConstant(std::string_view text) {
  if (text.size() < 2 || (text[0] != 'c' && text[0] != 'C')) {
    return std::nullopt;
  }
  ConstantRef ref;
  size_t i = 1;
  uint32_t index = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    index = index * 10 + uint32_t(text[i] - '0');
  }
  if (i == 1 || index >= 256) {
    return std::nullopt;
  }
  ref.index = index;
  if (i < text.size()) {
    if (text[i] != '.' || i + 2 != text.size()) {
      return std::nullopt;
    }
    static const char kComponents[] = "xyzw";
    const char* component = std::strchr(kComponents, text[i + 1]);
    if (!component || !*component) {
      return std::nullopt;
    }
    ref.component = uint32_t(component - kComponents);
  }
  return ref;
}

const float* PixelConstant(const RegisterFile& regs, uint32_t index) {
  // Pixel shader float constants follow the vertex shaders' 256.
  return reinterpret_cast<const float*>(
      &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + (256 + index) * 4]);
}

float ReadScalar(const RegisterFile& regs, const ConstantRef& ref) {
  return PixelConstant(regs, ref.index)[ref.component < 4 ? ref.component : 0];
}

void AddLight(const float position[3], float radius, const float color[3]) {
  for (Light& light : current_lights) {
    float dx = light.position[0] - position[0], dy = light.position[1] - position[1],
          dz = light.position[2] - position[2];
    if (dx * dx + dy * dy + dz * dz < 0.25f * 0.25f) {
      light.radius = std::max(light.radius, radius);
      for (uint32_t i = 0; i < 3; ++i) {
        light.color[i] = std::max(light.color[i], color[i]);
      }
      return;
    }
  }
  if (current_lights.size() >= kMaxLights) {
    return;
  }
  Light light;
  std::memcpy(light.position, position, sizeof(light.position));
  light.radius = radius;
  std::memcpy(light.color, color, sizeof(light.color));
  float brightest = std::max({color[0], color[1], color[2]});
  light.is_fire = brightest > 0.0f && color[0] >= color[1] && color[1] >= color[2] &&
                  (color[0] - color[2]) / brightest > 0.55f;
  current_lights.push_back(light);
}

}  // namespace

void LoadConfig(const std::vector<std::filesystem::path>& folders) {
  sources.clear();
  for (const std::filesystem::path& folder : folders) {
    std::filesystem::path path = folder / "lights.toml";
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      continue;
    }
    toml::table config;
    try {
      config = toml::parse_file(path.u8string());
    } catch (const toml::parse_error& parse_error) {
      REXGPU_WARN("Scene lights: {}: {}", path.u8string(), parse_error.description());
      continue;
    }
    size_t light_count = 0;
    if (const toml::array* source_array = config["source"].as_array()) {
      for (const toml::node& source_node : *source_array) {
        const toml::table* source = source_node.as_table();
        if (!source) {
          continue;
        }
        std::vector<LightSpec> lights;
        if (const toml::array* light_array = (*source)["light"].as_array()) {
          for (const toml::node& light_node : *light_array) {
            const toml::table* light = light_node.as_table();
            if (!light) {
              continue;
            }
            auto position = ParseConstant((*light)["position"].value_or(std::string_view()));
            auto color = ParseConstant((*light)["color"].value_or(std::string_view()));
            auto inverse_radius =
                ParseConstant((*light)["inverse_radius"].value_or(std::string_view()));
            if (!position || !color || !inverse_radius) {
              REXGPU_WARN("Scene lights: {}: a light without a valid position, color and "
                          "inverse_radius",
                          path.u8string());
              continue;
            }
            LightSpec spec;
            spec.position = *position;
            spec.color = *color;
            spec.inverse_radius = *inverse_radius;
            if (auto local = (*light)["local"].value<std::string_view>()) {
              spec.local = ParseConstant(*local);
            }
            lights.push_back(spec);
          }
        }
        if (const toml::array* shaders = (*source)["pixel_shaders"].as_array()) {
          for (const toml::node& shader_node : *shaders) {
            auto hash_text = shader_node.value<std::string_view>();
            if (!hash_text) {
              continue;
            }
            uint64_t hash = std::strtoull(std::string(*hash_text).c_str(), nullptr, 16);
            std::vector<LightSpec>& specs = sources[hash];
            specs.insert(specs.end(), lights.begin(), lights.end());
            light_count += lights.size();
          }
        }
      }
    }
    REXGPU_INFO("Scene lights: {} - {} lights in {} shaders", path.u8string(), light_count,
                sources.size());
  }
}

void OnDraw(const RegisterFile& regs, const Shader* pixel_shader) {
  if (!pixel_shader || sources.empty()) {
    return;
  }
  auto it = sources.find(pixel_shader->ucode_data_hash());
  if (it == sources.end()) {
    return;
  }
  for (const LightSpec& spec : it->second) {
    const float* position = PixelConstant(regs, spec.position.index);
    // Positioned (not a direction), falling off, reaching some distance.
    if (!(position[3] > 0.5f) || (spec.local && !(ReadScalar(regs, *spec.local) > 0.5f))) {
      continue;
    }
    float inverse_radius = ReadScalar(regs, spec.inverse_radius);
    if (!(inverse_radius > 1.0e-3f)) {
      continue;
    }
    const float* color = PixelConstant(regs, spec.color.index);
    if (!(color[0] + color[1] + color[2] > 1.0e-3f) || !std::isfinite(position[0]) ||
        !std::isfinite(position[1]) || !std::isfinite(position[2])) {
      continue;
    }
    AddLight(position, 1.0f / inverse_radius, color);
  }
}

void OnFrameSwap() {
  static bool config_loaded = false;
  if (!config_loaded) {
    config_loaded = true;
    LoadConfig(material_shaders::GetFolders());
  }
  previous_lights.swap(current_lights);
  current_lights.clear();
}

const std::vector<Light>& GetLights() {
  return current_lights.empty() ? previous_lights : current_lights;
}

}  // namespace rex::graphics::scene_lights
