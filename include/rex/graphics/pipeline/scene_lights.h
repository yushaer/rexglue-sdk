/**
 * @file        graphics/pipeline/scene_lights.h
 * @brief       The game's local lights, captured from its draws
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace rex::graphics {
class RegisterFile;
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::scene_lights {

// Local lights - fires, lamps, magic - that a game passes to its shaders as
// float constants, captured from its draws each frame so that effects can
// light everything with them, including surfaces whose shaders don't take
// them (scene_fx_dynamic_lights), and dress up fires (scene_fx_fire).
//
// Where the lights are is data, per game, in lights.toml in the material
// shaders' folder (and in mods): the pixel shaders that carry lights, and for
// each light the float constants of its position, color, whether it falls off
// (is local), and its inverse radius. For example:
//
//   [[source]]
//   pixel_shaders = ["648B965C200A5113"]
//   [[source.light]]
//   position = "c80"          # xyz, w = 1 for a positioned light
//   color = "c87"             # rgb
//   local = "c94.x"           # nonzero if it falls off with distance
//   inverse_radius = "c96.x"  # 1 / the distance it reaches
//
// Lights at the same place in several draws are merged.

struct Light {
  // World space.
  float position[3];
  float radius;
  float color[3];
  // Warm-colored: a flame, which flickers.
  bool is_fire;
};

// (Re)reads the lights.toml files in these folders, later ones adding to
// earlier ones.
void LoadConfig(const std::vector<std::filesystem::path>& folders);

// Called for each draw with the guest's state.
void OnDraw(const RegisterFile& regs, const Shader* pixel_shader);

// Called at each guest frame swap.
void OnFrameSwap();

// The lights of the current frame captured so far, or of the previous frame if
// none yet. The command processor thread only.
const std::vector<Light>& GetLights();

}  // namespace rex::graphics::scene_lights
