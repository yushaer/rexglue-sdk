// Volumetric lighting at full or half resolution (fx_effect_scale), from the
// sun:
// - light shafts: ray-marches each pixel's view ray through the air,
//   accumulating the sunlight it scatters toward the camera wherever the
//   guest's own sun shadow cascades say the sun reaches - so shafts form
//   through buildings, trees and windows. Air in the sun all along the ray only
//   veils everything evenly, like fog; the contrast (fx_sun_rays.x) has sunlit
//   air count less the longer the ray has been in the sun, so the light stands
//   out where it crosses shadow - a beam between buildings, through a window;
// - sun rays: with the sun on or near the screen, the sky seen around what's
//   in front of it streams out from the sun (a radial blur of the sky toward
//   it), strongest close to it.
// Shadows are bilinearly filtered (PCF) for smooth shaft edges, and the
// per-pixel jitter changes every frame for the temporal accumulation. The light
// is relative to the guest's sky, following its time of day, weather and
// exposure.
// Output: rgb = scattered sunlight reaching the camera.

#include "scene_fx_sun.hlsli"

// View distance at the effect's resolution (MIP 0).
FX_BINDING(0) Texture2D<float> fx_effect_depth : register(t0);
FX_BINDING(1) Texture2D<float> fx_shadow0 : register(t1);
FX_BINDING(2) Texture2D<float> fx_shadow1 : register(t2);
FX_BINDING(3) Texture2D<float4> fx_sky : register(t3);  // 1x1
FX_BINDING(4) SamplerState fx_clamp : register(s0);
FX_SUN_CONSTANTS(5)
FX_BINDING(6) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_volumetric : register(u0);

// Samples toward the sun for the sun rays.
static const uint kFxSunRaySamples = 32u;

// Henyey-Greenstein, scaled so isotropic scattering is 1.
float FxPhase(float g, float cos_theta) {
  return (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cos_theta, 1.0e-4), 1.5);
}

// The sky seen around what's in front of the sun, streaming out from it toward
// the pixel at screen: the sky's glow around the sun wherever it shows,
// summed along the way to the sun, nearer samples counting more.
float FxSunRays(float2 screen, float jitter) {
  float2 sun_screen = (float2(fx_sun_direction.x / (fx_sun_direction.z * fx_inv_p00),
                              fx_sun_direction.y / (fx_sun_direction.z * fx_inv_p11)) *
                           float2(0.5, -0.5) +
                       0.5) *
                      float2(fx_scene_size);
  // Fading out as the sun leaves the screen, gone half a screen beyond it.
  float2 beyond = max(abs(sun_screen / float2(fx_scene_size) - 0.5) - 0.5, 0.0);
  float on_screen = saturate(1.0 - 2.0 * max(beyond.x, beyond.y));
  if (on_screen <= 0.0) {
    return 0.0;
  }
  int2 size = FxEffectSize(fx_effect_scale);
  float2 step_screen = (sun_screen - screen) * (fx_sun_rays.z / float(kFxSunRaySamples));
  float2 position = screen + step_screen * jitter;
  float light = 0.0;
  float decay = 1.0;
  for (uint i = 0; i < kFxSunRaySamples; ++i, position += step_screen, decay *= 0.96) {
    int2 sample_texel = int2(position / float(fx_effect_scale));
    if (any(sample_texel < 0) || any(sample_texel >= size)) {
      continue;
    }
    if (fx_effect_depth.Load(int3(sample_texel, 0)) < fx_sky_distance) {
      continue;  // something in front of the sky
    }
    // The glow around the sun the rays come out of.
    float toward_sun =
        saturate(dot(normalize(FxViewPosition(position, 1.0)), fx_sun_direction.xyz));
    light += decay * (pow(toward_sun, 16.0) + 0.15 * pow(toward_sun, 4.0));
  }
  return on_screen * light * (2.0 / float(kFxSunRaySamples));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(int2(id.xy) >= FxEffectSize(fx_effect_scale))) {
    return;
  }
  int2 texel = int2(id.xy);
  float view_distance = fx_effect_depth.Load(int3(texel, 0));
  float2 screen = FxEffectScreen(texel, fx_effect_scale);
  float3 direction = normalize(FxViewPosition(screen, 1.0));
  // Distance along the ray to the surface (the sky is beyond the maximum).
  float surface_distance =
      view_distance >= fx_sky_distance ? 1.0e30 : view_distance / direction.z;
  float march_distance = min(surface_distance, fx_sun_volumetric.x);
  uint step_count = max(uint(fx_sun_volumetric.y), 1u);
  // Varies every frame - the temporal accumulation averages it out.
  float jitter = FxNoise(float2(texel) + 5.588238 * float(fx_frame & 63));

  float cos_theta = dot(direction, fx_sun_direction.xyz);
  float3 sun_color = fx_sky.Load(int3(0, 0, 0)).rgb * fx_sun_tint.rgb;
  float3 sun_light = sun_color * (fx_sun_volumetric.z * FxPhase(fx_sun_direction.w, cos_theta));

  float contrast = fx_sun_rays.x;
  // How much of the last stretch of the ray (about fx_sun_rays.w units) was in
  // the sun - starting in it, so the air around the camera doesn't veil the
  // view either.
  float recent_lit = 1.0;
  float transmittance = 1.0;
  float3 scattered = 0.0;
  float previous_distance = 0.0;
  for (uint i = 0; i < step_count; ++i) {
    // Quadratically spaced: denser near the camera, where shafts are sharpest.
    float s = (float(i) + jitter) / float(step_count);
    float distance = s * s * march_distance;
    float step_length = distance - previous_distance;
    previous_distance = distance;
    float3 position = direction * distance;
    float density = fx_sun_volumetric.w;
    if (fx_sun_tint.w > 0.0) {
      float height = dot(fx_sun_height, float4(position, 1.0));
      density += fx_sun_fog.w + fx_sun_fog.x * exp(min(-(height - fx_sun_fog.y) * fx_sun_fog.z,
                                                       20.0));
    }
    float lit;
    FX_SUN_SHADOW(fx_shadow0, fx_shadow1, fx_clamp, position, lit)
    float step_transmittance = exp(-density * step_length);
    // Energy-conserving: what this segment scatters toward the camera.
    scattered += transmittance * (1.0 - step_transmittance) * (lit * (1.0 - contrast * recent_lit)) *
                 sun_light;
    transmittance *= step_transmittance;
    recent_lit = lerp(lit, recent_lit, exp(-step_length / fx_sun_rays.w));
  }

  if (fx_sun_rays.y > 0.0 && fx_sun_direction.z > 0.0) {
    scattered += sun_color * (fx_sun_rays.y * FxSunRays(screen, jitter));
  }
  fx_volumetric[texel] = float4(scattered, 1.0);
}
