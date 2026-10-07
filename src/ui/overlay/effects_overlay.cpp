/**
 * @file        ui/overlay/effects_overlay.cpp
 *
 * @brief       In-game graphics effects menu. See effects_overlay.h for details.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/effects_overlay.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/mods.h>

namespace rex::ui {

namespace {

constexpr std::string_view kEffectsCategory = "GPU/Effects";
constexpr std::string_view kMaterialsCategory = "GPU/Materials";

const rex::cvar::FlagEntry* FindCvar(std::string_view name) {
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

bool HasCvar(std::string_view name) { return FindCvar(name) != nullptr; }

bool GetBool(std::string_view name) { return rex::cvar::GetFlagByName(name) == "true"; }

void SetBool(std::string_view name, bool value) {
  rex::cvar::SetFlagByName(name, value ? "true" : "false");
}

double GetDouble(std::string_view name) {
  return std::strtod(rex::cvar::GetFlagByName(name).c_str(), nullptr);
}

void SetDouble(std::string_view name, double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.6g", value);
  rex::cvar::SetFlagByName(name, text);
}

// Each widget writes the cvar only when the user changes it.
void CheckboxCvar(const char* label, std::string_view name) {
  bool value = GetBool(name);
  if (ImGui::Checkbox(label, &value)) {
    SetBool(name, value);
  }
}

void SliderCvar(const char* label, std::string_view name, float min, float max,
                const char* format = "%.2f", ImGuiSliderFlags flags = 0) {
  float value = float(GetDouble(name));
  if (ImGui::SliderFloat(label, &value, min, max, format, flags)) {
    SetDouble(name, value);
  }
}

// An integer cvar: item i is the value values[i].
void ComboCvar(const char* label, std::string_view name, const char* const* items,
               const int* values, int count) {
  int current = std::atoi(rex::cvar::GetFlagByName(name).c_str());
  int index = 0;
  for (int i = 0; i < count; ++i) {
    if (values[i] == current) {
      index = i;
    }
  }
  if (ImGui::Combo(label, &index, items, count)) {
    rex::cvar::SetFlagByName(name, std::to_string(values[index]));
  }
}

// A string cvar with a fixed set of values, shown with friendlier labels.
void StringComboCvar(const char* label, std::string_view name, const char* const* values,
                     const char* const* labels, int count) {
  std::string current = rex::cvar::GetFlagByName(name);
  int index = 0;
  for (int i = 0; i < count; ++i) {
    if (current == values[i]) {
      index = i;
    }
  }
  if (ImGui::Combo(label, &index, labels, count)) {
    rex::cvar::SetFlagByName(name, values[index]);
  }
}

// "r,g,b" string cvars.
void ColorCvar(const char* label, std::string_view name) {
  float color[3] = {1.0f, 1.0f, 1.0f};
  std::string text = rex::cvar::GetFlagByName(name);
  const char* cursor = text.c_str();
  for (float& component : color) {
    char* end;
    float parsed = std::strtof(cursor, &end);
    if (end == cursor) {
      break;
    }
    component = parsed;
    cursor = end;
    while (*cursor == ',' || *cursor == ' ') {
      ++cursor;
    }
  }
  if (ImGui::ColorEdit3(label, color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
    char value[64];
    std::snprintf(value, sizeof(value), "%.3f,%.3f,%.3f", color[0], color[1], color[2]);
    rex::cvar::SetFlagByName(name, value);
  }
}

void Description(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// A collapsing section headed by its effect's enable checkbox.
bool EffectHeader(const char* title, std::string_view enable_cvar) {
  ImGui::PushID(title);
  bool enabled = GetBool(enable_cvar);
  if (ImGui::Checkbox("##enabled", &enabled)) {
    SetBool(enable_cvar, enabled);
  }
  ImGui::SameLine();
  bool open = ImGui::CollapsingHeader(title);
  ImGui::PopID();
  return open;
}

const char* const kQualities[] = {"Low", "Medium", "High", "Ultra"};
const int kQualityValues[] = {0, 1, 2, 3};
const char* const kPresets[] = {"Off", "Low", "Medium", "High", "Ultra"};
const char* const kDebugViews[] = {"Off",
                                   "Split screen (effects on the left)",
                                   "Ambient occlusion",
                                   "Volumetric lighting",
                                   "Global illumination",
                                   "Fog",
                                   "Contact shadows",
                                   "Reflections"};
const int kDebugValues[] = {0, 1, 2, 3, 4, 5, 6, 7};
const char* const kResolutionScales[] = {"1x (native)", "2x", "3x"};
const int kResolutionScaleValues[] = {1, 2, 3};
const char* const kAnisotropy[] = {"Game's own", "Off", "2x", "4x", "8x", "16x"};
const int kAnisotropyValues[] = {-1, 0, 2, 3, 4, 5};
const char* const kAntiAliasingValues[] = {"none", "fxaa", "fxaa_extreme"};
const char* const kAntiAliasingLabels[] = {"Off", "FXAA", "FXAA (extreme)"};

// The shader options the material folders declare (options.toml - see
// rex/graphics/pipeline/material_shaders.h), mods' first.
struct ShaderOption {
  std::string id;
  std::string label;
  std::string group;
  std::string description;
  bool checkbox = false;
  float default_value = 0.0f;
  float min = 0.0f;
  float max = 1.0f;
};

std::filesystem::path Utf8Path(std::string_view text) {
  return std::filesystem::u8path(text.begin(), text.end());
}

std::vector<ShaderOption> LoadShaderOptions() {
  std::vector<std::filesystem::path> folders;
  std::string mod_folders = rex::cvar::GetFlagByName("mods_folders");
  size_t start = 0;
  while (start < mod_folders.size()) {
    size_t end = mod_folders.find('|', start);
    if (end == std::string::npos) {
      end = mod_folders.size();
    }
    if (end > start) {
      folders.push_back(Utf8Path(std::string_view(mod_folders).substr(start, end - start)) /
                        "materials");
    }
    start = end + 1;
  }
  std::filesystem::path base = Utf8Path(rex::cvar::GetFlagByName("material_shaders_path"));
  if (base.is_relative()) {
    base = rex::filesystem::GetExecutableFolder() / base;
  }
  folders.push_back(base);
  std::vector<ShaderOption> options;
  std::error_code error;
  for (const std::filesystem::path& folder : folders) {
    std::filesystem::path path = folder / "options.toml";
    if (!std::filesystem::is_regular_file(path, error)) {
      continue;
    }
    try {
      toml::table config = toml::parse_file(path.u8string());
      const toml::array* entries = config["option"].as_array();
      if (!entries) {
        continue;
      }
      for (const toml::node& node : *entries) {
        const toml::table* entry = node.as_table();
        if (!entry) {
          continue;
        }
        ShaderOption option;
        option.id = (*entry)["id"].value_or(std::string());
        if (option.id.empty() ||
            std::any_of(options.begin(), options.end(),
                        [&](const ShaderOption& other) { return other.id == option.id; })) {
          continue;
        }
        option.label = (*entry)["label"].value_or(option.id);
        option.group = (*entry)["group"].value_or(std::string("Other"));
        option.description = (*entry)["description"].value_or(std::string());
        option.checkbox = (*entry)["type"].value_or(std::string("slider")) == "checkbox";
        option.default_value = float((*entry)["default"].value_or(0.0));
        option.min = float((*entry)["min"].value_or(0.0));
        option.max = float((*entry)["max"].value_or(1.0));
        options.push_back(std::move(option));
      }
    } catch (const toml::parse_error&) {
    }
  }
  std::stable_sort(options.begin(), options.end(),
                   [](const ShaderOption& a, const ShaderOption& b) { return a.group < b.group; });
  return options;
}

}  // namespace

void EffectsDialog::DrawShaderOptions() {
  static std::vector<ShaderOption> options;
  static bool loaded = false;
  if (!loaded || ImGui::Button("Reload options")) {
    options = LoadShaderOptions();
    loaded = true;
  }
  Description(
      "Settings the material shaders and mods declare (options.toml in their folders).");
  if (options.empty()) {
    ImGui::TextUnformatted("No shader options installed.");
    return;
  }
  // The values set: id=value pairs in material_options.
  std::string overrides = rex::cvar::GetFlagByName("material_options");
  auto value_of = [&](const ShaderOption& option) {
    std::string key = option.id + "=";
    size_t position = 0;
    while ((position = overrides.find(key, position)) != std::string::npos) {
      if (position == 0 || overrides[position - 1] == ',') {
        return std::strtof(overrides.c_str() + position + key.size(), nullptr);
      }
      position += key.size();
    }
    return option.default_value;
  };
  bool changed = false;
  std::vector<float> values;
  for (const ShaderOption& option : options) {
    values.push_back(value_of(option));
  }
  std::string group;
  bool group_open = false;
  for (size_t i = 0; i < options.size(); ++i) {
    const ShaderOption& option = options[i];
    if (option.group != group || i == 0) {
      if (group_open) {
        ImGui::TreePop();
      }
      group = option.group;
      group_open = ImGui::TreeNodeEx(group.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    }
    if (!group_open) {
      continue;
    }
    ImGui::PushID(option.id.c_str());
    if (option.checkbox) {
      bool checked = values[i] > 0.5f;
      if (ImGui::Checkbox(option.label.c_str(), &checked)) {
        values[i] = checked ? 1.0f : 0.0f;
        changed = true;
      }
    } else if (ImGui::SliderFloat(option.label.c_str(), &values[i], option.min, option.max)) {
      changed = true;
    }
    if (!option.description.empty() && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", option.description.c_str());
    }
    ImGui::PopID();
  }
  if (group_open) {
    ImGui::TreePop();
  }
  if (ImGui::Button("Defaults##shader_options")) {
    for (size_t i = 0; i < options.size(); ++i) {
      values[i] = options[i].default_value;
    }
    changed = true;
  }
  if (changed) {
    std::string text;
    for (size_t i = 0; i < options.size(); ++i) {
      if (std::abs(values[i] - options[i].default_value) < 1.0e-6f) {
        continue;
      }
      char value[32];
      std::snprintf(value, sizeof(value), "%.4g", values[i]);
      if (!text.empty()) {
        text += ',';
      }
      text += options[i].id + "=" + value;
    }
    rex::cvar::SetFlagByName("material_options", text);
  }
}

void EffectsDialog::DrawMods() {
  Description(
      "Mods in the mods folder: game files, texture packs (with normal, roughness and height "
      "maps), and shaders. Turning one on or off, and texture pack changes, apply at the next "
      "start; Reload picks up shader changes now.");
  const std::vector<rex::mods::ModInfo>& mods = rex::mods::GetMods();
  if (mods.empty()) {
    ImGui::TextUnformatted("No mods installed.");
  }
  for (const rex::mods::ModInfo& mod : mods) {
    ImGui::PushID(mod.id.c_str());
    bool enabled = mod.enabled;
    std::string label = mod.name + (mod.version.empty() ? "" : " " + mod.version);
    if (ImGui::Checkbox(label.c_str(), &enabled)) {
      rex::mods::SetEnabled(mod.id, enabled);
      status_ = "Mods change at the next start (Reload applies their shaders now)";
    }
    std::string details;
    if (!mod.author.empty()) {
      details += "by " + mod.author + " - ";
    }
    details += std::to_string(mod.texture_count) + " textures, " +
               std::to_string(mod.material_count) + " shaders, " +
               std::to_string(mod.file_count) + " game files, priority " +
               std::to_string(mod.priority);
    Description(details.c_str());
    if (!mod.description.empty()) {
      Description(mod.description.c_str());
    }
    ImGui::PopID();
  }
  if (HasCvar("mods_reload") && ImGui::Button("Reload mods' shaders")) {
    rex::cvar::InvokeCommand("mods_reload", "");
    status_ = "Shaders, shader options and lights reloaded (textures at the next start)";
  }
  if (HasCvar("texture_replacement")) {
    CheckboxCvar("Texture packs (applies after restarting)", "texture_replacement");
    bool dumping = !rex::cvar::GetFlagByName("dump_textures").empty();
    if (ImGui::Checkbox("Dump textures for modding", &dumping)) {
      rex::cvar::SetFlagByName("dump_textures", dumping ? "texture_dump" : "");
    }
    Description(
        "Writes each texture the game loads to texture_dump next to the executable, named by "
        "its hash - a mod replaces one with <hash>.png or .dds in its textures folder.");
  }
}

EffectsDialog::EffectsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path)
    : ImGuiDialog(imgui_drawer), config_path_(std::move(config_path)) {}

EffectsDialog::~EffectsDialog() {}

void EffectsDialog::ApplyPreset(int preset) {
  // Which effects run, and their quality - the look (strengths, colors, fog)
  // is kept.
  struct Preset {
    bool ao, contact_shadows, fog, volumetrics, gi, full_resolution;
    int ao_quality, volumetrics_quality;
  };
  static const Preset kPresetValues[] = {
      {false, false, false, false, false, false, 0, 0},  // Off
      {true, false, true, false, false, false, 0, 0},    // Low
      {true, true, true, true, false, false, 1, 0},      // Medium
      {true, true, true, true, false, false, 2, 2},      // High
      {true, true, true, true, true, true, 3, 3},        // Ultra
  };
  const Preset& values = kPresetValues[preset];
  SetBool("scene_fx_ao", values.ao);
  SetBool("scene_fx_contact_shadows", values.contact_shadows);
  SetBool("scene_fx_fog", values.fog);
  SetBool("scene_fx_volumetrics", values.volumetrics);
  SetBool("scene_fx_gi", values.gi);
  SetBool("scene_fx_ao_full_resolution", values.full_resolution);
  rex::cvar::SetFlagByName("scene_fx_ao_quality", std::to_string(values.ao_quality));
  rex::cvar::SetFlagByName("scene_fx_volumetrics_quality",
                           std::to_string(values.volumetrics_quality));
  if (preset == 0) {
    SetBool("scene_fx_reflections", false);
  }
  status_ = std::string("Preset: ") + kPresets[preset];
}

void EffectsDialog::ResetToDefaults() {
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.category == kEffectsCategory || entry.category == kMaterialsCategory) {
      rex::cvar::SetFlagByName(entry.name, entry.default_value);
    }
  }
  status_ = "Defaults restored";
}

void EffectsDialog::OnDraw(ImGuiIO& /*io*/) {
  ImGui::SetNextWindowSize(ImVec2(500, 640), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (!ImGui::Begin("Graphics Enhancements (F6)##rex_effects", nullptr,
                    ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  if (!HasCvar("scene_fx_ao")) {
    ImGui::TextWrapped("The running GPU backend doesn't provide the graphics enhancements.");
    ImGui::End();
    return;
  }
  ImGui::PushItemWidth(-180.0f);

  ImGui::TextUnformatted("Preset");
  for (int i = 0; i < int(std::size(kPresets)); ++i) {
    ImGui::SameLine();
    if (ImGui::Button(kPresets[i])) {
      ApplyPreset(i);
    }
  }
  Description("Tick an effect to turn it on; open it to tune it. Changes apply instantly.");
  ImGui::Separator();

  if (EffectHeader("Ambient Occlusion", "scene_fx_ao")) {
    ImGui::PushID("ao");
    Description(
        "Ground-truth ambient occlusion with visibility bitmasks: soft shadowing in corners, "
        "creases and under objects, without dark halos around thin objects.");
    ComboCvar("Quality", "scene_fx_ao_quality", kQualities, kQualityValues, 4);
    SliderCvar("Strength", "scene_fx_ao_strength", 0.0f, 1.0f);
    SliderCvar("Radius", "scene_fx_ao_radius", 0.25f, 5.0f, "%.2f m");
    SliderCvar("Contrast", "scene_fx_ao_power", 0.5f, 3.0f);
    SliderCvar("Object thickness", "scene_fx_ao_thickness", 0.05f, 2.0f, "%.2f m");
    SliderCvar("Light bounce", "scene_fx_ao_albedo", 0.0f, 0.9f);
    CheckboxCvar("Full resolution (also GI, contact shadows, reflections)",
                 "scene_fx_ao_full_resolution");
    ImGui::PopID();
  }

  if (EffectHeader("Global Illumination", "scene_fx_gi")) {
    ImGui::PushID("gi");
    Description(
        "Light bouncing between nearby surfaces - color bleeding and softly lit shadows - "
        "gathered with the ambient occlusion's rays (its quality, radius and resolution "
        "apply).");
    SliderCvar("Strength", "scene_fx_gi_intensity", 0.0f, 4.0f);
    ImGui::PopID();
  }

  if (EffectHeader("Contact Shadows", "scene_fx_contact_shadows")) {
    ImGui::PushID("contact");
    Description(
        "Fine sun shadows the game's shadow maps are too coarse for: under feet and props, in "
        "grass and crevices.");
    SliderCvar("Strength", "scene_fx_contact_shadows_strength", 0.0f, 1.0f);
    SliderCvar("Length", "scene_fx_contact_shadows_length", 0.05f, 3.0f, "%.2f m");
    SliderCvar("Object thickness", "scene_fx_contact_shadows_thickness", 0.02f, 2.0f,
               "%.2f m");
    ImGui::PopID();
  }

  if (EffectHeader("Volumetric Lighting", "scene_fx_volumetrics")) {
    ImGui::PushID("volumetrics");
    Description(
        "Light shafts: sunlight scattering in the air, shadowed by the game's own sun shadows "
        "- through buildings, trees and windows.");
    ComboCvar("Quality", "scene_fx_volumetrics_quality", kQualities, kQualityValues, 4);
    SliderCvar("Brightness", "scene_fx_volumetrics_intensity", 0.0f, 4.0f);
    SliderCvar("Haze", "scene_fx_volumetrics_density", 0.0f, 0.03f, "%.4f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Glow toward the sun", "scene_fx_volumetrics_anisotropy", 0.0f, 0.95f);
    SliderCvar("Distance", "scene_fx_volumetrics_distance", 20.0f, 500.0f, "%.0f m");
    ColorCvar("Sunlight color", "scene_fx_sun_tint");
    CheckboxCvar("Smooth over frames (temporal)", "scene_fx_volumetrics_temporal");
    CheckboxCvar("Full resolution", "scene_fx_volumetrics_full_resolution");
    ImGui::PopID();
  }

  if (EffectHeader("Fog", "scene_fx_fog")) {
    ImGui::PushID("fog");
    Description(
        "Height fog lit by the sky, with a glow toward the sun - thicker near the ground and "
        "in valleys.");
    SliderCvar("Density", "scene_fx_fog_density", 0.0f, 0.02f, "%.4f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Ground fog", "scene_fx_fog_ground_density", 0.0f, 0.2f, "%.4f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Ground fog height", "scene_fx_fog_ground_height", -20.0f, 20.0f, "%.1f m");
    SliderCvar("Ground fog falloff", "scene_fx_fog_ground_falloff", 0.02f, 2.0f, "%.3f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Brightness", "scene_fx_fog_brightness", 0.0f, 2.0f);
    SliderCvar("Sun glow", "scene_fx_fog_sun_glow", 0.0f, 2.0f);
    ImGui::PopID();
  }

  if (EffectHeader("Reflections", "scene_fx_reflections")) {
    ImGui::PushID("reflections");
    Description(
        "Screen-space reflections. The game doesn't say which surfaces are glossy, so all "
        "reflect a little, most at grazing angles - or only floors and streets.");
    ComboCvar("Quality", "scene_fx_reflections_quality", kQualities, kQualityValues, 4);
    SliderCvar("Strength", "scene_fx_reflections_intensity", 0.0f, 4.0f);
    SliderCvar("Shininess", "scene_fx_reflections_reflectance", 0.0f, 0.5f, "%.3f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Distance", "scene_fx_reflections_distance", 2.0f, 100.0f, "%.0f m");
    CheckboxCvar("Floors only", "scene_fx_reflections_floors_only");
    ImGui::PopID();
  }

  if (HasCvar("scene_fx_dynamic_lights") &&
      EffectHeader("Dynamic Lights", "scene_fx_dynamic_lights")) {
    ImGui::PushID("dynamic_lights");
    Description(
        "The game's fires, lamps and other lights light everything around them - walls and "
        "ground too, not only characters and props - with short shadows, flickering with "
        "their flames.");
    SliderCvar("Strength", "scene_fx_dynamic_lights_intensity", 0.0f, 4.0f);
    ImGui::PopID();
  }

  if (HasCvar("scene_fx_fire") && EffectHeader("Fire", "scene_fx_fire")) {
    ImGui::PushID("fire");
    Description("Heat haze shimmering over flames, and glowing embers drifting up from them.");
    SliderCvar("Strength", "scene_fx_fire_intensity", 0.0f, 3.0f);
    ImGui::PopID();
  }

  if (EffectHeader("Sharpening", "scene_fx_sharpen")) {
    ImGui::PushID("sharpen");
    Description(
        "Contrast-adaptive sharpening of the final image: crisper detail without halos (no "
        "upscaling).");
    SliderCvar("Strength", "scene_fx_sharpen_amount", 0.0f, 1.0f);
    ImGui::PopID();
  }

  if (EffectHeader("Color Grading", "scene_fx_grading")) {
    ImGui::PushID("grading");
    SliderCvar("Exposure", "scene_fx_grading_exposure", 0.25f, 4.0f, "%.2f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Contrast", "scene_fx_grading_contrast", 0.5f, 2.0f);
    SliderCvar("Saturation", "scene_fx_grading_saturation", 0.0f, 2.0f);
    SliderCvar("Vibrance", "scene_fx_grading_vibrance", -1.0f, 1.0f);
    SliderCvar("Temperature", "scene_fx_grading_temperature", -1.0f, 1.0f);
    SliderCvar("Tint", "scene_fx_grading_tint", -1.0f, 1.0f);
    SliderCvar("Gamma", "scene_fx_grading_gamma", 0.5f, 2.0f);
    SliderCvar("Shadow lift", "scene_fx_grading_shadow_lift", 0.0f, 0.5f);
    SliderCvar("Vignette", "scene_fx_vignette", 0.0f, 1.0f);
    SliderCvar("Film grain", "scene_fx_film_grain", 0.0f, 1.0f);
    ImGui::PopID();
  }

  if (HasCvar("material_shaders") && ImGui::CollapsingHeader("Materials")) {
    ImGui::PushID("materials");
    Description(
        "Rewritten versions of the game's own shaders with modern lighting, replacing its "
        "rendering rather than adding to the finished image (Direct3D 12).");
    CheckboxCvar("Material shaders", "material_shaders");
    if (HasCvar("material_shaders_reload")) {
      ImGui::SameLine();
      if (ImGui::Button("Reload from disk")) {
        rex::cvar::InvokeCommand("material_shaders_reload", "");
        status_ = "Material shaders reloaded";
      }
    }
    CheckboxCvar("Soft sun shadows", "material_soft_shadows");
    Description(
        "Percentage-closer soft shadows: sharp where objects touch the ground, softer the "
        "further the shadow falls.");
    SliderCvar("Sun size", "material_shadow_softness", 0.25f, 4.0f);
    CheckboxCvar("Physically based highlights", "material_specular");
    Description(
        "GGX highlights with Fresnel from each surface's specular map, and reflections "
        "strongest at grazing angles.");
    SliderCvar("Highlight strength", "material_specular_intensity", 0.0f, 4.0f);
    SliderCvar("Roughness", "material_roughness", 0.05f, 1.0f);
    if (HasCvar("material_character_lighting")) {
      CheckboxCvar("Character lighting", "material_character_lighting");
      Description(
          "Softer light falloff across characters and props, and a sheen from the sky at "
          "grazing angles. Shadows from nearby lights are smoothed too.");
      SliderCvar("Sheen", "material_sheen_intensity", 0.0f, 2.0f);
      CheckboxCvar("Foliage", "material_foliage");
      Description(
          "Light shining through grass and leaves, softer lighting, and blades that don't "
          "thin out in the distance.");
      SliderCvar("Translucency", "material_foliage_translucency", 0.0f, 2.0f);
      CheckboxCvar("Fire", "material_fire");
      Description("Turbulent, animated flames with brighter cores that glow.");
      SliderCvar("Flame brightness", "material_fire_intensity", 0.0f, 3.0f);
    }
    ImGui::PopID();
  }

  if (HasCvar("material_options") && ImGui::CollapsingHeader("Shader Options")) {
    ImGui::PushID("shader_options");
    DrawShaderOptions();
    ImGui::PopID();
  }

  if (HasCvar("mods") && ImGui::CollapsingHeader("Mods")) {
    ImGui::PushID("mods");
    DrawMods();
    ImGui::PopID();
  }

  if (ImGui::CollapsingHeader("Rendering")) {
    ImGui::PushID("rendering");
    if (HasCvar("anisotropic_override")) {
      ComboCvar("Texture filtering", "anisotropic_override", kAnisotropy, kAnisotropyValues,
                int(std::size(kAnisotropy)));
      Description("Anisotropic filtering keeps textures sharp at oblique angles.");
    }
    if (HasCvar("resolution_scale")) {
      ComboCvar("Internal resolution", "resolution_scale", kResolutionScales,
                kResolutionScaleValues, int(std::size(kResolutionScales)));
      Description(
          "Renders the game at a multiple of its resolution - much sharper, but the GPU cost "
          "grows with the square. Applies after restarting the game.");
    }
    if (HasCvar("swap_post_effect")) {
      StringComboCvar("Anti-aliasing", "swap_post_effect", kAntiAliasingValues,
                      kAntiAliasingLabels, int(std::size(kAntiAliasingValues)));
      Description("Applies after restarting the game.");
    }
    ImGui::PopID();
  }

  if (ImGui::CollapsingHeader("Debug")) {
    ImGui::PushID("debug");
    ComboCvar("View", "scene_fx_debug", kDebugViews, kDebugValues, int(std::size(kDebugViews)));
    ImGui::PopID();
  }

  ImGui::PopItemWidth();
  ImGui::Separator();
  if (ImGui::Button("Save")) {
    rex::cvar::SaveConfig(config_path_);
    status_ = "Saved to " + config_path_.filename().string();
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset to defaults")) {
    ResetToDefaults();
  }
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_.c_str());
  }
  ImGui::End();
}

}  // namespace rex::ui
