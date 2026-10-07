/**
 * @file        graphics/pipeline/material_shaders.h
 * @brief       Hand-written replacements for the guest's shaders
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace rex::graphics {
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::material_shaders {

// Material shaders replace the translations of the game's own shaders with
// improved host shaders - better lighting, shadows and materials, rather than
// effects on top of the finished image. One is found by the guest shader's
// fingerprint (ucode hash) and the translation's modification bits:
//   <material_shaders_path>/<backend>/<HASH>_<MODIFICATION>.<extension>
//   <material_shaders_path>/<backend>/<HASH>.<extension> (any modification)
// It takes the translation's place with the same resource bindings (written
// by dump_shaders next to each translation, in .bindings.txt), and gets the
// material settings in the system constants (xe_material_params) - see
// MaterialParams.
//
// The parameters (kMaterialParamsCount float4s, after the translator's own
// system constants):
// 0.x - Features (uint bits, MaterialFeature).
// 0.y - Soft shadow light size scale.
// 0.z - Specular intensity.
// 0.w - Surface roughness.
// 1.x - Time in seconds (wrapped hourly), the same for a whole frame.
// 1.y - Sky sheen intensity on characters.
// 1.z - Foliage translucency.
// 1.w - Fire core intensity.
// 2.x - Traits of the draw's vertex shader (uint bits, VertexShaderTrait) - to
//       tell characters (skinned) from props with the same pixel shader.
// 2.y - The draw's vertex shader hash, low 32 bits (uint).
// 2.z, 2.w - Layer blending: the sums buffer's width and height in pixels.
// 3.x, 3.y - Draw resolution scale.
// 3.z - Translation flags (uint bits, TranslationFlag) - what the translator
//       bakes into its shaders.
// 3.w - Layer blending flags (LayerBlendingFlag, as a float).
// 4-11 - The shader options: 32 floats, slot i in [4 + i / 4][i % 4], declared
//        in options.toml (see LoadOptions) and set in the menu.
// 12-13 - The custom textures (textures.toml): slot i's bindless descriptor
//         index (uint) in [12 + i / 4][i % 4], or kNoTexture.
// 14-21 - Companion maps of the textures in fetch constants 0-7: [14 + fetch]
//         = the descriptor indices (uint) of its CompanionMap kinds, or
//         kNoTexture.
constexpr uint32_t kMaterialParamsCount = 22;

// Layer blending - for surfaces the game draws as layers, one pass each, added
// up weighted by their alphas (the first pass replacing what's there): a
// material shader declaring the sums buffer (XE_LAYER_BLENDING in
// xenos_d3d12.hlsli - register u0 in kLayerSumsRegisterSpace) can weight its
// layer by a factor of its own (a height map) and have the layers normalized
// together, though each pass only knows its own layer. The buffer keeps, per
// pixel, the sums of the alphas and of the weighted alphas of the layers
// drawn so far (rasterizer ordered); the pass blends its color as
// color + destination * second color's alpha (dual-source) and rescales the
// earlier layers' sum with that. Needs rasterizer ordered views, a single
// render target and the host render target path; without them the shader
// isn't used.
constexpr uint32_t kLayerSumsRegisterSpace = 20;
enum LayerBlendingFlag : uint32_t {
  // The pass replaces the destination (the game's color blend has a zero
  // destination factor): the surface's first layer.
  kLayerBlendingFlagFirstLayer = 1u << 0,
  // The blending above is set up for this draw (else the game's blending is).
  kLayerBlendingFlagActive = 1u << 1,
};
// The name the sums buffer's declaration leaves in a shader's binary.
constexpr char kLayerSumsName[] = "xe_layer_sums";

constexpr uint32_t kOptionSlotCount = 32;
constexpr uint32_t kCustomTextureSlotCount = 8;
constexpr uint32_t kCompanionFetchCount = 8;
constexpr uint32_t kNoTexture = UINT32_MAX;

// Maps a texture pack adds to a game texture, named <hash>_<kind>.png (or
// .dds) beside its replacement (texture_replacement.h).
enum CompanionMap : uint32_t {
  // Tangent-space normals in RG (as the game's own normal maps).
  kCompanionMapNormal,
  // Occlusion, roughness, metalness in RGB ("_orm").
  kCompanionMapOrm,
  // Height in R, for parallax.
  kCompanionMapHeight,
  // Light given off, RGB.
  kCompanionMapEmissive,
  kCompanionMapCount,
};

// options.toml in the material folders declares the options shaders read
// (XeMaterialOption(slot) in xenos_d3d12.hlsli):
//   [[option]]
//   id = "character_rim"        # unique, the key in material_options
//   slot = 0                    # 0-31
//   label = "Rim light"
//   group = "Characters"
//   description = "..."
//   type = "slider"             # or "checkbox" (0 or 1)
//   default = 1.0
//   min = 0.0
//   max = 2.0
// The values set differ from the defaults in material_options ("id=value,...").
// textures.toml lists custom textures for shaders (noise, lookup tables...):
//   [[texture]]
//   slot = 0                    # 0-7
//   file = "textures/blue_noise.png"   # relative to that folder
// The first declaration of an id or slot (mods first) wins.

// (Re)reads options.toml and textures.toml.
void LoadOptions();
// A custom texture slot's file, if declared.
struct CustomTexture {
  uint32_t slot;
  std::filesystem::path path;
};
const std::vector<CustomTexture>& GetCustomTextures();

enum MaterialFeature : uint32_t {
  // Penumbras that widen with the distance from the caster.
  kMaterialFeatureSoftShadows = 1u << 0,
  // GGX specular with Fresnel instead of the game's Phong.
  kMaterialFeatureSpecular = 1u << 1,
  // Characters: wrapped diffuse and the sky's sheen at grazing angles.
  kMaterialFeatureCharacterLighting = 1u << 2,
  // Foliage: translucency, wrapped diffuse, mip-corrected alpha.
  kMaterialFeatureFoliage = 1u << 3,
  // Fire: animated turbulence and brighter cores.
  kMaterialFeatureFire = 1u << 4,
};

enum VertexShaderTrait : uint32_t {
  // Indexes float constants dynamically (particles, instancing, some props).
  kVertexShaderTraitIndexedConstants = 1u << 0,
  // Fetches a vertex stream at several indices per vertex - blending bones
  // from a matrix palette: skinned characters and creatures (rigid props with
  // one bone each don't count).
  kVertexShaderTraitSkinned = 1u << 1,
};

enum TranslationFlag : uint32_t {
  kTranslationFlagGammaRenderTargetAsUnorm8 = 1u << 0,
  kTranslationFlagMsaa2xSupported = 1u << 1,
  kTranslationFlagFuzzyAlphaEpsilon = 1u << 2,
  kTranslationFlagScaledTextureOffsets = 1u << 3,
};

// Whether material shaders replace translations (read when translating).
bool IsEnabled();

// The material shaders' folder (material_shaders_path, from the executable's).
std::filesystem::path GetFolder();

// The enabled mods' folders, the highest priority first (rex/system/mods.h,
// published in the mods_folders cvar).
std::vector<std::filesystem::path> GetModFolders();

// The folders with material shaders, the first found winning: each enabled
// mod's materials folder, then GetFolder().
std::vector<std::filesystem::path> GetFolders();

// Asks the backend to read the material shaders again (material_shaders_reload).
void RequestReload();

// Whether the material shaders must be read again - a reload was requested, or
// material_shaders was toggled since the last call. Called by the backend once
// per frame; clears the request.
bool ConsumeReloadRequest();

// Advances the time the materials animate with. Called by the backend once per
// frame.
void AdvanceFrame();

// The time the materials animate with this frame (material_params 1.x).
float GetTime();

// The material shader for a translation, if there is one. backend: the
// subdirectory ("d3d12"), extension: the file extension ("dxbc").
bool Load(uint64_t ucode_hash, uint64_t modification, std::string_view backend,
          std::string_view extension, std::vector<uint8_t>& binary_out);

// The VertexShaderTrait bits of an analyzed vertex shader.
uint32_t GetVertexShaderTraits(const Shader& vertex_shader);

// The system constants' material parameters for the current settings and draw
// (rows 0-11 - the backend fills the texture rows).
void GetParams(float params_out[kMaterialParamsCount][4], uint32_t draw_resolution_scale_x,
               uint32_t draw_resolution_scale_y, uint32_t translation_flags,
               uint32_t vertex_shader_traits, uint64_t vertex_shader_hash);

}  // namespace rex::graphics::material_shaders
