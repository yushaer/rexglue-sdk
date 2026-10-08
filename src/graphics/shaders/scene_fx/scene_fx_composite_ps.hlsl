// Composites the effects into the guest's HDR scene - those computed at half
// resolution upsampled with depth-aware (joint bilateral) weights - in up to
// two passes
// (fx_composite_mode):
// - Multiply: color *= (ambient occlusion + indirect light relative to the
//   surface's brightness) * fog transmittance.
// - Add: color += light the fog scatters toward the camera + volumetric
//   lighting (light shafts, sun rays, the glow of lamps and fires in the air).
// - Debug: replaces the color with one effect.
// The fog is exponential height fog integrated analytically per pixel at full
// resolution (no ray marching), lit by the sky and with a glow toward the sun.

#include "scene_fx_common.hlsli"

// At fx_effect_scale.
FX_BINDING(0) Texture2D<float> fx_ao : register(t0);
FX_BINDING(1) Texture2D<float> fx_view_depth : register(t1);  // half resolution, MIP 0
FX_BINDING(2) Texture2D<float> fx_depth : register(t2);       // full resolution
// At fx_volumetric_scale.
FX_BINDING(3) Texture2D<float4> fx_volumetric : register(t3);
// At fx_effect_scale.
FX_BINDING(4) Texture2D<float4> fx_gi : register(t4);
// Half resolution, this frame's scene before the effects.
FX_BINDING(5) Texture2D<float4> fx_scene_color : register(t5);
FX_BINDING(6) Texture2D<float4> fx_sky : register(t6);  // 1x1
// At fx_effect_scale.
FX_BINDING(7) Texture2D<float> fx_contact_shadows : register(t7);
FX_BINDING(8) Texture2D<float4> fx_reflections : register(t8);
FX_EXTRA_CONSTANTS_BLOCK(9) {
  // xyz = view space direction to world height, w = the camera's height.
  float4 fx_fog_height;
  // Density: x = at height y (world), z = falloff with height, w = everywhere.
  float4 fx_fog_density;
  // rgb = skylight scattered by the fog relative to the sky's color, w =
  // distance the sky is at.
  float4 fx_fog_color;
  // xyz = toward the sun (view space), w = Henyey-Greenstein anisotropy.
  float4 fx_fog_sun;
  // rgb = sunlight scattered by the fog relative to the sky's color, w = how
  // much the fog and the volumetric light cover the sky (the game's sky has
  // its own haze: 1 would hide its blue).
  float4 fx_fog_sun_color;
  // x = contact shadow strength, y = reflection intensity, z = the glow of the
  // game's lights in the air (0 = off), w = the reach of that glow, a share of
  // the lights' radius.
  float4 fx_effect_strengths;
  // The game's local lights (scene_lights.h): x = count, y = time (seconds),
  // z = dynamic lighting strength (0 = off), w = embers' strength (0 = off).
  float4 fx_lights_info;
  // Per light, two: xyz = view space position, w = radius; rgb = color
  // (flickering for flames), w = 1 for a flame.
  float4 fx_lights[FX_MAX_LIGHTS * 2];
};

// An effect at fx_effect_scale at this pixel: the pixel itself at full
// resolution, otherwise the joint bilateral upsampling.
#define FX_UPSAMPLE(texture, result)                                  \
  if (fx_effect_scale == 1u) {                                        \
    result = texture.Load(int3(pixel, 0));                            \
  } else {                                                            \
    result = 0.0;                                                     \
    [unroll] for (int i = 0; i < 4; ++i) {                            \
      result += texture.Load(int3(texels[i], 0)) * weights[i];        \
    }                                                                 \
    result *= inv_weight_sum;                                         \
  }

// Jimenez et al. 2016: visibility to occlusion including interreflections.
float FxMultiBounce(float visibility, float albedo) {
  float a = 2.0404 * albedo - 0.3324;
  float b = -4.7951 * albedo + 0.6417;
  float c = 2.7552 * albedo + 0.6903;
  return max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

// Optical depth from the camera along a unit view space direction:
// integral of everywhere + at_height * exp(-falloff * (height(s) - height_0)).
float FxFogOpticalDepth(float3 direction, float distance) {
  float rise = dot(fx_fog_height.xyz, direction);
  float at_camera =
      fx_fog_density.x * exp(min(-fx_fog_density.z * (fx_fog_height.w - fx_fog_density.y), 80.0));
  float k = fx_fog_density.z * rise;
  float integral = abs(k * distance) > 1.0e-4
                       ? (1.0 - exp(min(-k * distance, 80.0))) / k
                       : distance;
  return fx_fog_density.w * distance + at_camera * integral;
}

float FxLoadDistance(int2 pixel) {
  return fx_depth.Load(int3(clamp(pixel, int2(0, 0), int2(fx_scene_size) - 1), 0));
}

float2 FxScreenFromView(float3 view) {
  float2 ndc = float2(view.x / (view.z * fx_inv_p00), view.y / (view.z * fx_inv_p11));
  return (ndc * float2(0.5, -0.5) + 0.5) * float2(fx_scene_size);
}

// The surface's view space normal from the depth of its neighbors - across the
// nearer neighbor on each axis, so edges don't bend it.
float3 FxSurfaceNormal(int2 pixel, float3 center) {
  float3 left = FxViewPosition(float2(pixel + int2(-1, 0)) + 0.5, FxLoadDistance(pixel + int2(-1, 0)));
  float3 right = FxViewPosition(float2(pixel + int2(1, 0)) + 0.5, FxLoadDistance(pixel + int2(1, 0)));
  float3 up = FxViewPosition(float2(pixel + int2(0, -1)) + 0.5, FxLoadDistance(pixel + int2(0, -1)));
  float3 down = FxViewPosition(float2(pixel + int2(0, 1)) + 0.5, FxLoadDistance(pixel + int2(0, 1)));
  float3 dx = abs(right.z - center.z) < abs(center.z - left.z) ? right - center : center - left;
  float3 dy = abs(down.z - center.z) < abs(center.z - up.z) ? down - center : center - up;
  float3 normal = cross(dx, dy);
  float length_squared = dot(normal, normal);
  return length_squared > 1.0e-12 ? normal * rsqrt(length_squared) : float3(0.0, 0.0, -1.0);
}

// Whether the way to a light is clear, marching through the depth buffer for
// up to 1.5 units (nearby occluders: the light leaking through walls and
// props, rather than shadows across the scene).
float FxLightVisibility(float3 surface, float3 to_light, float distance, float jitter) {
  float march = min(distance * 0.9, 1.5);
  float3 step = to_light / distance * (march / 8.0);
  [loop] for (uint i = 0; i < 8u; ++i) {
    float3 point_view = surface + step * (float(i) + jitter);
    if (point_view.z <= 0.05) {
      break;
    }
    float2 point_screen = FxScreenFromView(point_view);
    if (any(point_screen < 0.0) || any(point_screen >= float2(fx_scene_size))) {
      break;
    }
    float in_front = point_view.z - FxLoadDistance(int2(point_screen));
    if (in_front > 0.03 + 0.01 * point_view.z && in_front < 0.75) {
      return 0.0;
    }
  }
  return 1.0;
}

// The game's local lights at the surface (irradiance, without the surface's
// color).
float3 FxDynamicLights(int2 pixel, float2 screen, float distance) {
  float3 surface = FxViewPosition(screen, distance);
  float3 normal = FxSurfaceNormal(pixel, surface);
  float jitter = FxNoise(float2(pixel) + float(fx_frame % 64u) * 5.588238);
  float3 total = 0.0;
  uint count = uint(fx_lights_info.x);
  [loop] for (uint i = 0; i < count; ++i) {
    float4 light = fx_lights[i * 2u];
    float3 to_light = light.xyz - surface;
    float light_distance = length(to_light);
    if (light_distance >= light.w || light_distance < 1.0e-4) {
      continue;
    }
    // The game's own falloff, and a little wrap for normals from depth.
    float falloff = 1.0 - light_distance / light.w;
    float facing = saturate(dot(normal, to_light / light_distance) * 0.85 + 0.15);
    if (falloff * facing <= 0.0) {
      continue;
    }
    total += fx_lights[i * 2u + 1u].rgb *
             (falloff * facing * FxLightVisibility(surface, to_light, light_distance, jitter));
  }
  return total;
}

// How much of a lamp shows, from 5 taps of the depth around where it is on
// screen (fading out toward the screen's edges), and how bright it looks there
// (the brightest of those taps in the half resolution scene). 0 behind the
// camera.
float2 FxLampOnScreen(float3 lamp) {
  if (lamp.z <= 0.1) {
    return 0.0;
  }
  float2 center = FxScreenFromView(lamp);
  float2 edge = min(center, float2(fx_scene_size) - center) / (0.05 * float(fx_scene_size.y));
  float on_screen = saturate(min(edge.x, edge.y));
  if (on_screen <= 0.0) {
    return 0.0;
  }
  float pixels_per_unit = 0.5 * float(fx_scene_size.y) / (lamp.z * fx_inv_p11);
  float spread = max(0.08 * pixels_per_unit, 1.0);
  static const float2 kTaps[5] = {float2(0.0, 0.0), float2(1.0, 0.0), float2(-1.0, 0.0),
                                  float2(0.0, 1.0), float2(0.0, -1.0)};
  int2 half_size = int2((fx_scene_size + 1) >> 1);
  float visible = 0.0;
  float brightness = 0.0;
  [unroll] for (int i = 0; i < 5; ++i) {
    float2 tap = center + kTaps[i] * spread;
    // In front of whatever is there - the lamp's own glass, flame or wick is
    // around it, so a little behind still counts.
    visible += FxLoadDistance(int2(tap)) >= lamp.z - 0.25 ? 0.2 : 0.0;
    int2 half_texel = clamp(int2(tap * 0.5), int2(0, 0), half_size - 1);
    brightness = max(brightness, FxLuminance(fx_scene_color.Load(int3(half_texel, 0)).rgb));
  }
  return float2(visible * on_screen, min(brightness, 16.0));
}

// The light the game's lamps and fires scatter toward the camera in the air
// between it and the surface (or the sky): per light, inverse-square from it,
// integrated along the view ray analytically, fading out within a share of its
// radius - as bright as the lamp looks on screen, and none from lamps hidden
// behind something.
float3 FxLightGlow(float2 screen, float distance) {
  float3 direction = normalize(FxViewPosition(screen, 1.0));
  float ray_length = distance / direction.z;
  float3 total = 0.0;
  uint count = uint(fx_lights_info.x);
  [loop] for (uint i = 0; i < count; ++i) {
    float4 light = fx_lights[i * 2u];
    float reach = fx_effect_strengths.w * light.w;
    // Closest approach of the view ray (s along it), at miss from the light.
    float along = dot(light.xyz, direction);
    float miss = length(direction * along - light.xyz);
    if (miss >= reach || ray_length <= 0.0) {
      continue;
    }
    float2 lamp = FxLampOnScreen(light.xyz);
    if (lamp.x <= 0.0) {
      continue;
    }
    // Softened in the middle - the lamp itself is there.
    float core = max(miss, 0.1 * reach);
    // Integral over s from 0 to the surface of 1 / (core^2 + (s - along)^2).
    float scattering = (atan((ray_length - along) / core) - atan(-along / core)) / core;
    float window = 1.0 - miss / reach;
    float3 color = fx_lights[i * 2u + 1u].rgb;
    total += color / max(FxLuminance(color), 1.0e-3) *
             (lamp.x * lamp.y * scattering * window * window * reach);
  }
  return total * (fx_effect_strengths.z * 0.004);
}

float FxHash(float2 p) {
  float3 p3 = frac(p.xyx * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return frac((p3.x + p3.y) * p3.z);
}

// Embers drifting up from the flames: glowing specks rising, swaying and
// fading, in front of what's behind the fire.
float3 FxEmbers(float2 screen, float distance) {
  float3 total = 0.0;
  uint count = uint(fx_lights_info.x);
  float time = fx_lights_info.y;
  [loop] for (uint i = 0; i < count; ++i) {
    float4 light = fx_lights[i * 2u];
    float4 color = fx_lights[i * 2u + 1u];
    if (color.w < 0.5 || light.z < 0.3) {
      continue;
    }
    float2 center = FxScreenFromView(light.xyz);
    float pixels_per_unit = 0.5 * float(fx_scene_size.y) / (light.z * fx_inv_p11);
    // Around the fire in world units: x across, y up.
    float2 offset = (screen - center) / pixels_per_unit * float2(1.0, -1.0);
    if (offset.y < -0.2 || offset.y > 2.4 || abs(offset.x) > 0.9 ||
        distance < light.z - 0.5) {
      continue;
    }
    // As bright as the flames on screen (the scene's own exposure): the
    // brightest of a few points up the fire, at the half resolution capture.
    float flame_brightness = 0.0;
    int2 half_size = int2((fx_scene_size + 1) >> 1);
    [unroll] for (int sample_index = 0; sample_index < 3; ++sample_index) {
      float2 sample_screen = center - float2(0.0, (float(sample_index) * 0.15 - 0.05) * pixels_per_unit);
      int2 sample_texel = clamp(int2(sample_screen * 0.5), int2(0, 0), half_size - 1);
      flame_brightness =
          max(flame_brightness, FxLuminance(fx_scene_color.Load(int3(sample_texel, 0)).rgb));
    }
    float brightness = max(flame_brightness, 0.5 * FxLuminance(fx_sky.Load(int3(0, 0, 0)).rgb));
    float seed = float(i) * 17.31;
    float ember_radius = max(0.02 * pixels_per_unit, 1.3);
    float3 light_total = 0.0;
    [unroll] for (uint layer = 0; layer < 2u; ++layer) {
      // Cells of 0.18 x 0.3 units rising at different speeds.
      float speed = layer == 0u ? 0.9 : 1.4;
      float2 cell_space = float2(offset.x / 0.18, (offset.y - time * speed) / 0.3) +
                          float2(seed + float(layer) * 3.7, 0.0);
      float2 cell = floor(cell_space);
      float presence = FxHash(cell + float2(seed, layer * 11.0));
      if (presence < 0.74) {
        continue;
      }
      float2 in_cell = float2(FxHash(cell + 1.7), FxHash(cell + 4.1)) * 0.6 + 0.2;
      in_cell.x += sin(time * 2.3 + presence * 40.0) * 0.15;
      float2 delta_pixels = (cell_space - cell - in_cell) * float2(0.18, 0.3) * pixels_per_unit;
      float speck = saturate(1.0 - length(delta_pixels) / ember_radius);
      float height_fade = saturate(1.0 - offset.y / 2.4) * saturate((offset.y + 0.2) * 4.0) *
                          saturate(1.0 - abs(offset.x) / 0.9);
      float twinkle = 0.55 + 0.45 * sin(time * 13.0 + presence * 57.0);
      light_total += float3(1.0, 0.42, 0.1) * (speck * height_fade * twinkle);
    }
    total += light_total * (1.5 * brightness);
  }
  return total * fx_lights_info.w;
}

float4 main(float4 position : SV_Position) : SV_Target {
  float2 screen = position.xy * fx_output_scale + float2(fx_screen_offset);
  // Unchanged for either blend.
  float4 identity = fx_composite_mode == FX_COMPOSITE_ADD ? 0.0 : 1.0;
  if (fx_debug_mode == FX_DEBUG_SPLIT && screen.x >= 0.5 * float(fx_scene_size.x)) {
    return identity;
  }
  int2 pixel = clamp(int2(screen), int2(0, 0), int2(fx_scene_size) - 1);
  float depth = fx_depth.Load(int3(pixel, 0));
  bool is_sky = depth >= fx_sky_distance;

  // The four half-resolution texels around (texel i was computed at scene
  // pixel 2i), weighted bilinearly and by depth similarity.
  int2 half_size = int2((fx_scene_size + 1) >> 1);
  float2 half_position = float2(pixel) * 0.5;
  int2 base = int2(floor(half_position));
  float2 fraction = half_position - float2(base);
  float tolerance = 0.03 * depth + 0.01;
  int2 texels[4];
  float weights[4];
  float weight_sum = 0.0;
  float closest_error = 1.0e30;
  int closest = 0;
  [unroll] for (int i = 0; i < 4; ++i) {
    int2 corner = int2(i & 1, i >> 1);
    texels[i] = clamp(base + corner, int2(0, 0), half_size - 1);
    float error = abs(fx_view_depth.Load(int3(texels[i], 0)) - depth);
    float2 bilinear = lerp(1.0 - fraction, fraction, float2(corner));
    weights[i] = bilinear.x * bilinear.y * saturate(1.0 - error / tolerance);
    weight_sum += weights[i];
    if (error < closest_error) {
      closest_error = error;
      closest = i;
    }
  }
  // No similar depth around (a thin feature): the closest texel alone.
  if (weight_sum < 1.0e-3) {
    [unroll] for (int i = 0; i < 4; ++i) {
      weights[i] = i == closest ? 1.0 : 0.0;
    }
    weight_sum = 1.0;
  }
  float inv_weight_sum = 1.0 / weight_sum;
  // Effects at full resolution: the pixel itself.
  bool ao_full = fx_effect_scale == 1u;
  bool volumetric_full = fx_volumetric_scale == 1u;

  // Fog along the view ray to the surface (or the sky's distance).
  float fog_transmittance = 1.0;
  float3 fog_light = 0.0;
  if (fx_effect_flags & FX_EFFECT_FOG) {
    float3 direction = normalize(FxViewPosition(screen, 1.0));
    float distance = is_sky ? fx_fog_color.w : depth / direction.z;
    fog_transmittance =
        exp(-FxFogOpticalDepth(direction, distance) * (is_sky ? fx_fog_sun_color.w : 1.0));
    float g = fx_fog_sun.w;
    float phase = (1.0 - g * g) /
                  pow(max(1.0 + g * g - 2.0 * g * dot(direction, fx_fog_sun.xyz), 1.0e-4), 1.5);
    fog_light = (1.0 - fog_transmittance) * fx_sky.Load(int3(0, 0, 0)).rgb *
                (fx_fog_color.rgb + fx_fog_sun_color.rgb * phase);
  }

  float3 shafts = 0.0;
  if (fx_effect_flags & FX_EFFECT_VOLUMETRICS) {
    if (volumetric_full) {
      shafts = fx_volumetric.Load(int3(pixel, 0)).rgb;
    } else {
      [unroll] for (int i = 0; i < 4; ++i) {
        shafts += fx_volumetric.Load(int3(texels[i], 0)).rgb * weights[i];
      }
      shafts *= inv_weight_sum;
    }
    if (is_sky) {
      shafts *= fx_fog_sun_color.w;
    }
  }
  // On the surface, so seen through the fog.
  float3 reflections = 0.0;
  if ((fx_effect_flags & FX_EFFECT_REFLECTIONS) && !is_sky) {
    float4 reflection;
    FX_UPSAMPLE(fx_reflections, reflection)
    reflections = reflection.rgb * (fx_effect_strengths.y * fog_transmittance);
  }
  float3 glow = 0.0;
  if (fx_composite_mode != FX_COMPOSITE_MULTIPLY && fx_effect_strengths.z > 0.0 &&
      fx_lights_info.x > 0.0) {
    glow = FxLightGlow(screen, is_sky ? fx_sky_distance : depth) * fog_transmittance;
  }
  if (fx_composite_mode == FX_COMPOSITE_ADD) {
    float3 embers = 0.0;
    if (fx_lights_info.w > 0.0 && fx_lights_info.x > 0.0) {
      embers = FxEmbers(screen, depth);
    }
    return float4(fog_light + shafts + glow + reflections + embers, 0.0);
  }

  float contact = 1.0;
  if ((fx_effect_flags & FX_EFFECT_CONTACT_SHADOWS) && !is_sky) {
    FX_UPSAMPLE(fx_contact_shadows, contact)
    contact = lerp(1.0, contact, fx_effect_strengths.x);
  }

  float ao = 1.0;
  if ((fx_effect_flags & FX_EFFECT_AO) && !is_sky) {
    float visibility;
    FX_UPSAMPLE(fx_ao, visibility)
    if (fx_ao_albedo > 0.0) {
      visibility = FxMultiBounce(visibility, fx_ao_albedo);
    }
    ao = lerp(1.0, visibility, fx_ao_strength);
  }

  // Indirect light reaching the surface, as a multiplier of its color: the
  // surface reflects albedo * light, approximated as its color * light /
  // (its brightness, but not below a fraction of the sky's).
  float3 indirect = 0.0;
  // The game's local lights, likewise relative to the surface's brightness.
  float3 dynamic_light = 0.0;
  if (fx_lights_info.z > 0.0 && fx_lights_info.x > 0.0 && !is_sky) {
    float3 irradiance = FxDynamicLights(pixel, screen, depth);
    if (any(irradiance > 0.0)) {
      float3 surface = 0.0;
      [unroll] for (int i = 0; i < 4; ++i) {
        surface += fx_scene_color.Load(int3(texels[i], 0)).rgb * weights[i];
      }
      surface *= inv_weight_sum;
      float floor_brightness =
          max(0.1 * FxLuminance(fx_sky.Load(int3(0, 0, 0)).rgb), 0.02);
      dynamic_light =
          min(fx_lights_info.z * 1.2 * irradiance / max(FxLuminance(surface), floor_brightness),
              8.0);
    }
  }
  if ((fx_effect_flags & FX_EFFECT_GI) && !is_sky) {
    float3 light = 0.0;
    float3 surface = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
      if (!ao_full) {
        light += fx_gi.Load(int3(texels[i], 0)).rgb * weights[i];
      }
      // The captured scene is always at half resolution.
      surface += fx_scene_color.Load(int3(texels[i], 0)).rgb * weights[i];
    }
    light = ao_full ? fx_gi.Load(int3(pixel, 0)).rgb : light * inv_weight_sum;
    surface *= inv_weight_sum;
    float floor_brightness =
        max(0.1 * FxLuminance(fx_sky.Load(int3(0, 0, 0)).rgb), 1.0e-3);
    indirect = min(fx_gi_intensity * 0.5 * light / max(FxLuminance(surface), floor_brightness),
                   4.0);
  }

  if (fx_debug_mode == FX_DEBUG_AO) {
    return float4(ao, ao, ao, 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_VOLUMETRICS) {
    float3 volumetric = shafts + glow;
    return float4(volumetric / (1.0 + volumetric), 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_GI) {
    return float4(indirect / (1.0 + indirect), 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_FOG) {
    return float4(fog_light / (1.0 + fog_light), 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_CONTACT_SHADOWS) {
    return float4(contact, contact, contact, 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_REFLECTIONS) {
    return float4(reflections / (1.0 + reflections), 1.0);
  }
  return float4((ao * contact + indirect + dynamic_light) * fog_transmittance, 1.0);
}
