// Image effects on the guest's final image (display-referred, after its
// tonemapping): contrast-adaptive sharpening - after AMD FidelityFX CAS: a
// cross-shaped sharpening kernel whose strength adapts to the local contrast,
// so edges get crisper without halos or noise amplification - then color
// grading.

#include "scene_fx_common.hlsli"

// The final image (a copy), at render target pixels.
FX_BINDING(0) Texture2D<float4> fx_image : register(t0);
FX_EXTRA_CONSTANTS_BLOCK(1) {
  // x = sharpness (0...1), y = 1 to sharpen.
  float4 fx_image_sharpen;
  // x = exposure (multiplier), y = contrast, z = saturation, w = vibrance.
  float4 fx_image_grade;
  // rgb = white balance (temperature and tint), w = gamma.
  float4 fx_image_white;
  // x = shadow lift, y = vignette, z = film grain.
  float4 fx_image_finish;
  // Heat haze over flames: x = count, y = time (seconds), z = strength.
  float4 fx_image_haze;
  // Per flame: xy = its base (render target pixels), z = pixels per world unit.
  float4 fx_image_flames[FX_MAX_HAZE_FLAMES];
};

float3 FxLoadImage(int2 pixel) {
  pixel = clamp(pixel, int2(fx_pass_rect.xy), int2(fx_pass_rect.zw) - 1);
  return fx_image.Load(int3(pixel, 0)).rgb;
}

float3 FxSampleImage(float2 position) {
  float2 texel = position - 0.5;
  int2 base = int2(floor(texel));
  float2 f = texel - float2(base);
  return lerp(lerp(FxLoadImage(base), FxLoadImage(base + int2(1, 0)), f.x),
              lerp(FxLoadImage(base + int2(0, 1)), FxLoadImage(base + int2(1, 1)), f.x), f.y);
}

float FxImageHash(float2 p) {
  float3 p3 = frac(p.xyx * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return frac((p3.x + p3.y) * p3.z);
}

float FxImageNoise(float2 p) {
  float2 cell = floor(p);
  float2 f = p - cell;
  float2 u = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(FxImageHash(cell), FxImageHash(cell + float2(1.0, 0.0)), u.x),
              lerp(FxImageHash(cell + float2(0.0, 1.0)), FxImageHash(cell + float2(1.0, 1.0)), u.x),
              u.y);
}

// The shimmer of hot air rising over flames: the image displaced by rising
// noise above each one (render target pixels).
float2 FxHeatHaze(float2 position) {
  float2 displacement = 0.0;
  uint count = uint(fx_image_haze.x);
  float time = fx_image_haze.y;
  [loop] for (uint i = 0; i < count; ++i) {
    float4 flame = fx_image_flames[i];
    // Around the flame in world units: x across, y up.
    float2 offset = (position - flame.xy) / flame.z * float2(1.0, -1.0);
    if (offset.y < 0.0 || offset.y > 2.2 || abs(offset.x) > 0.6) {
      continue;
    }
    float weight = smoothstep(0.0, 0.35, offset.y) * (1.0 - smoothstep(1.1, 2.2, offset.y)) *
                   (1.0 - smoothstep(0.3, 0.6, abs(offset.x)));
    float2 rising = float2(offset.x * 6.0 + float(i) * 13.1, offset.y * 4.0 - time * 3.2);
    float2 wave = float2(FxImageNoise(rising), FxImageNoise(rising + float2(17.3, 5.9))) - 0.5;
    // Up to a few pixels - more for nearer flames.
    displacement += wave * (weight * min(flame.z * 0.03, 4.0));
  }
  return displacement * fx_image_haze.z;
}

float4 main(float4 position : SV_Position) : SV_Target {
  int2 pixel = int2(position.xy);
  float3 color = FxLoadImage(pixel);
  if (fx_image_haze.x > 0.0) {
    float2 haze = FxHeatHaze(position.xy);
    if (any(abs(haze) > 0.01)) {
      color = FxSampleImage(position.xy + haze);
    }
  }
  if (fx_image_sharpen.y > 0.0) {
    //   a b c
    //   d e f
    //   g h i
    float3 a = FxLoadImage(pixel + int2(-1, -1));
    float3 b = FxLoadImage(pixel + int2(0, -1));
    float3 c = FxLoadImage(pixel + int2(1, -1));
    float3 d = FxLoadImage(pixel + int2(-1, 0));
    float3 e = color;
    float3 f = FxLoadImage(pixel + int2(1, 0));
    float3 g = FxLoadImage(pixel + int2(-1, 1));
    float3 h = FxLoadImage(pixel + int2(0, 1));
    float3 i = FxLoadImage(pixel + int2(1, 1));
    // Soft minimum and maximum: the cross plus the whole 3x3 neighborhood.
    float3 cross_min = min(min(min(d, e), min(f, b)), h);
    float3 cross_max = max(max(max(d, e), max(f, b)), h);
    float3 soft_min = cross_min + min(cross_min, min(min(a, c), min(g, i)));
    float3 soft_max = cross_max + max(cross_max, max(max(a, c), max(g, i)));
    // Less sharpening where the contrast is already high (or near clipping).
    float3 amplitude = sqrt(saturate(min(soft_min, 2.0 - soft_max) / max(soft_max, 1.0e-4)));
    float peak = -1.0 / lerp(8.0, 5.0, saturate(fx_image_sharpen.x));
    float3 weight = amplitude * peak;
    color = saturate(((b + d + f + h) * weight + e) / (1.0 + 4.0 * weight));
  }

  color *= fx_image_grade.x * fx_image_white.rgb;
  float luminance = FxLuminance(color);
  // Vibrance: saturates muted colors more than already vivid ones.
  float chroma = max(max(color.r, color.g), color.b) - min(min(color.r, color.g), color.b);
  float saturation = fx_image_grade.z * (1.0 + fx_image_grade.w * (1.0 - saturate(chroma)));
  color = lerp(luminance.xxx, color, saturation);
  color = (color - 0.5) * fx_image_grade.y + 0.5;
  // Shadow lift: raises the darks, leaving the whites.
  color = saturate(color);
  color += fx_image_finish.x * (1.0 - color) * (1.0 - color) * (1.0 - color);
  color = pow(saturate(color), fx_image_white.w);

  // Vignette, over the image's rectangle.
  float2 rect_size = float2(fx_pass_rect.zw - fx_pass_rect.xy);
  float2 centered = (position.xy - float2(fx_pass_rect.xy)) / rect_size * 2.0 - 1.0;
  centered.x *= rect_size.x / rect_size.y;
  color *= saturate(1.0 - fx_image_finish.y * dot(centered, centered) * 0.25);
  // Film grain, changing every frame, stronger in the midtones.
  float grain = FxNoise(position.xy + float2(17.0, 59.0) * float(fx_frame & 255)) - 0.5;
  float midtones = 4.0 * FxLuminance(color) * (1.0 - FxLuminance(color));
  color += grain * fx_image_finish.z * 0.15 * midtones;
  return float4(saturate(color), 1.0);
}
