// Medal of Honor: Frontline - optional preservation-friendly enhanced post-process.
// Disabling Enhanced Graphics restores the previously selected Dolphin shader.
/*
[configuration]
[OptionBool]
GUIName = Master Enable
OptionName = MASTER_ENABLE
DefaultValue = true

[OptionBool]
GUIName = Bloom
OptionName = BLOOM_ENABLE
DefaultValue = true
[OptionRangeFloat]
GUIName = Bloom Intensity
OptionName = BLOOM_INTENSITY
MinValue = 0.0
MaxValue = 1.5
StepAmount = 0.01
DefaultValue = 0.55
[OptionRangeFloat]
GUIName = Bloom Threshold
OptionName = BLOOM_THRESHOLD
MinValue = 0.2
MaxValue = 1.5
StepAmount = 0.01
DefaultValue = 0.72

[OptionBool]
GUIName = Tone Mapping
OptionName = TONEMAP_ENABLE
DefaultValue = true
[OptionRangeFloat]
GUIName = Exposure
OptionName = EXPOSURE
MinValue = 0.5
MaxValue = 2.0
StepAmount = 0.01
DefaultValue = 1.0
[OptionRangeFloat]
GUIName = Contrast
OptionName = CONTRAST
MinValue = 0.7
MaxValue = 1.4
StepAmount = 0.01
DefaultValue = 1.04
[OptionRangeFloat]
GUIName = Saturation
OptionName = SATURATION
MinValue = 0.0
MaxValue = 1.5
StepAmount = 0.01
DefaultValue = 1.03

[OptionBool]
GUIName = Sharpen
OptionName = SHARPEN_ENABLE
DefaultValue = true
[OptionRangeFloat]
GUIName = Sharpen Strength
OptionName = SHARPEN_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.22

[OptionBool]
GUIName = Cinematic DOF
OptionName = DOF_ENABLE
DefaultValue = false
[OptionRangeFloat]
GUIName = DOF Strength
OptionName = DOF_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.30

[OptionBool]
GUIName = Enhanced Lighting
OptionName = LIGHTING_ENABLE
DefaultValue = true
[OptionRangeFloat]
GUIName = Lighting Strength
OptionName = LIGHTING_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.28

[OptionBool]
GUIName = Ambient Occlusion
OptionName = SSAO_ENABLE
DefaultValue = true
[OptionRangeFloat]
GUIName = AO Strength
OptionName = SSAO_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.22

[OptionBool]
GUIName = Contact Shadows
OptionName = CONTACT_SHADOW_ENABLE
DefaultValue = true
[OptionRangeFloat]
GUIName = Contact Shadow Strength
OptionName = CONTACT_SHADOW_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.18

[OptionBool]
GUIName = Vignette
OptionName = VIGNETTE_ENABLE
DefaultValue = false
[OptionRangeFloat]
GUIName = Vignette Strength
OptionName = VIGNETTE_STRENGTH
MinValue = 0.0
MaxValue = 0.8
StepAmount = 0.01
DefaultValue = 0.12

[OptionBool]
GUIName = Film Grain
OptionName = FILM_GRAIN_ENABLE
DefaultValue = false
[OptionRangeFloat]
GUIName = Film Grain Strength
OptionName = FILM_GRAIN_STRENGTH
MinValue = 0.0
MaxValue = 0.15
StepAmount = 0.001
DefaultValue = 0.015
[/configuration]
*/

float Luma(float3 c)
{
  return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 Filmic(float3 x)
{
  // Compact ACES-like filmic shoulder suitable for the original SDR art.
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), float3(0.0), float3(1.0));
}

float HashNoise(float2 p)
{
  return frac(sin(dot(p, float2(12.9898, 78.233)) + float(GetTime()) * 0.017) * 43758.5453);
}

// Live GameCube EFB depth exposed by PostProcessing.cpp.  The PS3 capture uses
// a camera-depth reconstruction + shadow-map PCF receiver.  Until the host
// caster pass is replaying the scene four times, this path uses the same depth
// information to provide geometry-aware directional/contact shadows instead of
// the old luminance-only fake.
float DepthAt(float2 uv)
{
  return SampleRawDepthLocation(clamp(uv, float2(0.0), float2(1.0)));
}

float ComputeDepthAO(float2 uv, float depth, float2 px)
{
  float occ = 0.0;
  float2 o1 = px * 2.0;
  float2 o2 = px * 5.0;
  float ds;

  ds = DepthAt(uv + float2( o1.x, 0.0)); occ += smoothstep(0.00030, 0.0090, depth - ds);
  ds = DepthAt(uv + float2(-o1.x, 0.0)); occ += smoothstep(0.00030, 0.0090, depth - ds);
  ds = DepthAt(uv + float2(0.0,  o1.y)); occ += smoothstep(0.00030, 0.0090, depth - ds);
  ds = DepthAt(uv + float2(0.0, -o1.y)); occ += smoothstep(0.00030, 0.0090, depth - ds);
  ds = DepthAt(uv + float2( o2.x,  o2.y)); occ += smoothstep(0.00055, 0.0180, depth - ds);
  ds = DepthAt(uv + float2(-o2.x,  o2.y)); occ += smoothstep(0.00055, 0.0180, depth - ds);
  ds = DepthAt(uv + float2( o2.x, -o2.y)); occ += smoothstep(0.00055, 0.0180, depth - ds);
  ds = DepthAt(uv + float2(-o2.x, -o2.y)); occ += smoothstep(0.00055, 0.0180, depth - ds);
  return clamp(occ * 0.125, 0.0, 1.0);
}

float PS3ShadowPCF(float2 uv, float receiver_depth, float2 px,
                   float2 ray_dir, float travel_px, float bias, float thickness)
{
  float2 p = uv + ray_dir * px * travel_px;
  float2 side = float2(-ray_dir.y, ray_dir.x) * px * 0.85;

  // Four taps, mirroring the 4-tap PCF footprint recovered from the RSX
  // receiver shader.  Here the taps compare against live scene depth.
  float s0 = smoothstep(bias, thickness, receiver_depth - DepthAt(p + side));
  float s1 = smoothstep(bias, thickness, receiver_depth - DepthAt(p - side));
  float s2 = smoothstep(bias, thickness, receiver_depth - DepthAt(p + side * 0.35 + px * 0.85));
  float s3 = smoothstep(bias, thickness, receiver_depth - DepthAt(p - side * 0.35 - px * 0.85));
  return (s0 + s1 + s2 + s3) * 0.25;
}

float ComputePS3DepthShadow(float2 uv, float depth, float2 px)
{
  // Screen-space projection of the same presentation sun direction already
  // used by the remaster lighting path.  The four travel bands emulate the
  // near->far coverage of the four 1024x1024 PS3 cascades while the real host
  // CSM caster is being brought online.
  float2 ray_dir = normalize(float2(0.42, 0.50));

  float c0 = PS3ShadowPCF(uv, depth, px, ray_dir,  2.5, 0.00030, 0.0065);
  float c1 = PS3ShadowPCF(uv, depth, px, ray_dir,  5.0, 0.00040, 0.0090);
  float c2 = PS3ShadowPCF(uv, depth, px, ray_dir, 10.0, 0.00055, 0.0130);
  float c3 = PS3ShadowPCF(uv, depth, px, ray_dir, 18.0, 0.00075, 0.0190);

  float shadow = max(max(c0, c1 * 0.92), max(c2 * 0.78, c3 * 0.62));

  // Reject sky/far-plane noise and soften tiny depth discontinuities.
  float valid = 1.0 - smoothstep(0.996, 1.0, depth);
  return clamp(shadow * valid, 0.0, 1.0);
}

float3 BrightPass(float3 c)
{
  float l = Luma(c);

  // PS3 Frontline remaster uses a separate bloom capture/composite path.
  // Threshold every source sample BEFORE blur accumulation.
  const float knee = 0.22;

  float gate =
      smoothstep(BLOOM_THRESHOLD - knee,
                 BLOOM_THRESHOLD + knee,
                 l);

  return c * gate;
}

float3 BuildRemasterNormal(float ln, float ls, float le, float lw)
{
  // Temporary screen-space approximation.
  //
  // The real PS3 renderer exposes normal/detail material textures.
  // Those will later be supplied by the Remaster Asset Layer.
  float gx = le - lw;
  float gy = ls - ln;

  return normalize(float3(-gx * 3.0,
                          -gy * 3.0,
                           1.0));
}

void main()
{
  float4 src = Sample();
  if (OptionDisabled(MASTER_ENABLE))
  {
    SetOutput(src);
    return;
  }

  float2 uv = GetCoordinates();
  float2 px = GetInvResolution();
  float3 center = src.rgb;

  // Shared neighbourhood for the remaster lighting/material approximation.
  float3 n  = SampleLocation(uv + float2( 0.0, -1.0) * px).rgb;
  float3 s1 = SampleLocation(uv + float2( 0.0,  1.0) * px).rgb;
  float3 e  = SampleLocation(uv + float2( 1.0,  0.0) * px).rgb;
  float3 w  = SampleLocation(uv + float2(-1.0,  0.0) * px).rgb;

  float3 ne = SampleLocation(uv + float2( 1.0, -1.0) * px).rgb;
  float3 nw = SampleLocation(uv + float2(-1.0, -1.0) * px).rgb;
  float3 se = SampleLocation(uv + float2( 1.0,  1.0) * px).rgb;
  float3 sw = SampleLocation(uv + float2(-1.0,  1.0) * px).rgb;

  float3 local =
      (n + s1 + e + w +
       ne + nw + se + sw) * 0.125;

  float lc = Luma(center);
  float ll = Luma(local);

  if (OptionEnabled(LIGHTING_ENABLE))
  {
    // PS3 remaster material concepts found in the renderer:
    //
    //   g_NormalTexture
    //   g_DetailTexture
    //   g_vCameraPos
    //   g_vLightPosition
    //   g_vLightColor
    //   g_vLightDirWorld
    //   g_vSpecularLightMultiplier
    //
    // Preserve the original GX lighting and layer a restrained approximation
    // on top of it until the real PS3 DetailMaps are wired to ModernGekko.

    float ln = Luma(n);
    float ls = Luma(s1);
    float le = Luma(e);
    float lw = Luma(w);

    float3 normal =
        BuildRemasterNormal(ln, ls, le, lw);

    // Camera-facing directional presentation light.
    float3 light_dir =
        normalize(float3(-0.42, -0.50, 0.76));

    float3 view_dir =
        float3(0.0, 0.0, 1.0);

    float3 half_dir =
        normalize(light_dir + view_dir);

    float ndotl =
        max(dot(normal, light_dir), 0.0);

    float diffuse_delta =
        ndotl - 0.52;

    center *=
        1.0 +
        diffuse_delta *
        LIGHTING_STRENGTH *
        0.34;

    // Approximate the high-frequency response contributed by
    // DetailMaps/*_normal.ssh.
    float3 high_frequency =
        center - local;

    float detail_mask =
        clamp(
            (abs(le - lw) + abs(ls - ln)) * 1.8 +
            abs(lc - ll) * 1.2,
            0.0,
            1.0);

    center +=
        high_frequency *
        LIGHTING_STRENGTH *
        0.30 *
        (0.45 + detail_mask * 0.55);

    // Approximate g_vSpecularLightMultiplier.
    float spec =
        pow(max(dot(normal, half_dir), 0.0),
            24.0);

    // Prevent every matte wall from becoming glossy.
    float spec_gate =
        smoothstep(0.20, 0.88, lc) *
        (0.25 + detail_mask * 0.75);

    center +=
        float3(1.00, 0.95, 0.86) *
        spec *
        spec_gate *
        LIGHTING_STRENGTH *
        0.24;

    // Very restrained rim response.
    float rim =
        pow(clamp(1.0 - normal.z,
                  0.0,
                  1.0),
            1.6);

    center +=
        center *
        rim *
        LIGHTING_STRENGTH *
        0.07;
  }

  if (OptionEnabled(SSAO_ENABLE))
  {
    float cavity;
    if (HasSceneDepth())
    {
      float2 dpx = max(GetInvDepthResolution(), float2(0.00001));
      cavity = ComputeDepthAO(uv, DepthAt(uv), dpx);
    }
    else
    {
      // Safe fallback for backends/situations where an EFB depth resolve is
      // unavailable.
      cavity = clamp((ll - lc) * 2.35, 0.0, 1.0);
      float diagonal =
          (Luma(ne) + Luma(nw) + Luma(se) + Luma(sw)) * 0.25;
      cavity = max(cavity, clamp((diagonal - lc) * 1.65, 0.0, 1.0));
    }

    center *= 1.0 - cavity * SSAO_STRENGTH * 0.62;
  }

  if (OptionEnabled(CONTACT_SHADOW_ENABLE))
  {
    float shadow;
    if (HasSceneDepth())
    {
      float2 dpx = max(GetInvDepthResolution(), float2(0.00001));
      shadow = ComputePS3DepthShadow(uv, DepthAt(uv), dpx);
    }
    else
    {
      // Preserve the old luminance fallback when live depth is unavailable.
      float2 shadow_dir = normalize(float2(0.42, 0.50));
      float l1 = Luma(SampleLocation(uv + shadow_dir * px * 2.5).rgb);
      float l2 = Luma(SampleLocation(uv + shadow_dir * px * 5.0).rgb);
      float l3 = Luma(SampleLocation(uv + shadow_dir * px * 8.0).rgb);
      float occluder = max(max(l1, l2), l3);
      float directional_shadow = clamp((occluder - lc) * 1.65, 0.0, 1.0);
      float gx = abs(Luma(e) - Luma(w));
      float gy = abs(Luma(s1) - Luma(n));
      float edge = clamp((gx + gy) * 2.0, 0.0, 1.0);
      float edge_shadow = edge * clamp(ll - lc + 0.04, 0.0, 1.0) * 0.65;
      shadow = max(directional_shadow, edge_shadow);
    }

    // Existing slider now controls actual depth-aware shadows.  0.86 keeps
    // them visible at the old default 0.18 while still allowing a soft look.
    center *= 1.0 - shadow * CONTACT_SHADOW_STRENGTH * 0.86;
  }

  if (OptionEnabled(BLOOM_ENABLE))
  {
    // The PS3 executable contains distinct:
    //
    //   Bloom capture
    //   Bloom
    //   CompositeBloom
    //
    // Threshold each sample first, then accumulate multiple blur scales.

    float2 r1 = px * 1.75;
    float2 r2 = px * 4.50;
    float2 r3 = px * 8.00;

    float3 bloom = float3(0.0);

    // Inner bloom.
    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2( 1.0, 0.0) * r1).rgb) *
        1.00;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2(-1.0, 0.0) * r1).rgb) *
        1.00;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2(0.0,  1.0) * r1).rgb) *
        1.00;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2(0.0, -1.0) * r1).rgb) *
        1.00;

    // Medium diagonal glow.
    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2( 1.0,  1.0) * r2).rgb) *
        0.70;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2(-1.0,  1.0) * r2).rgb) *
        0.70;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2( 1.0, -1.0) * r2).rgb) *
        0.70;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2(-1.0, -1.0) * r2).rgb) *
        0.70;

    // Wide bloom.
    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2( 1.0, 0.0) * r3).rgb) *
        0.42;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2(-1.0, 0.0) * r3).rgb) *
        0.42;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2(0.0,  1.0) * r3).rgb) *
        0.42;

    bloom +=
        BrightPass(
            SampleLocation(
                uv + float2(0.0, -1.0) * r3).rgb) *
        0.42;

    bloom *= 1.0 / 8.48;

    center +=
        bloom *
        BLOOM_INTENSITY *
        0.68;
  }

  if (OptionEnabled(DOF_ENABLE) && DOF_STRENGTH > 0.001)
  {
    // Cinematic screen-space focus: center/iron-sight region stays sharp and
    // the periphery receives a soft bokeh-like blur.  It is deliberately
    // resolution independent and ADS strength can be driven live by the port.
    float2 centered = uv * 2.0 - float2(1.0);
    float radius = length(centered * float2(GetResolution().x / max(GetResolution().y, 1.0), 1.0));
    float blur_amount = smoothstep(0.20, 0.95, radius) * DOF_STRENGTH;
    float2 dr = px * (2.0 + DOF_STRENGTH * 4.0);
    float3 blur = SampleLocation(uv + float2( dr.x, 0.0)).rgb +
                  SampleLocation(uv + float2(-dr.x, 0.0)).rgb +
                  SampleLocation(uv + float2(0.0,  dr.y)).rgb +
                  SampleLocation(uv + float2(0.0, -dr.y)).rgb;
    blur *= 0.25;
    float dof_mix = clamp(blur_amount, 0.0, 0.85);
    center = center + (blur - center) * dof_mix;
  }

  if (OptionEnabled(SHARPEN_ENABLE))
  {
    float3 detail = center - local;
    center += detail * SHARPEN_STRENGTH * 0.55;
  }

  center *= EXPOSURE;
  if (OptionEnabled(TONEMAP_ENABLE))
    center = Filmic(max(center, float3(0.0)));
  center = (center - 0.5) * CONTRAST + 0.5;
  float lum = Luma(center);
  center = float3(lum) + (center - float3(lum)) * SATURATION;

  if (OptionEnabled(VIGNETTE_ENABLE))
  {
    float2 q = uv * (1.0 - uv.yx);
    float vignette = clamp(pow(max(q.x * q.y * 16.0, 0.0), 0.18), 0.0, 1.0);
    center *= (1.0 - VIGNETTE_STRENGTH) + VIGNETTE_STRENGTH * vignette;
  }

  if (OptionEnabled(FILM_GRAIN_ENABLE))
  {
    float grain = (HashNoise(uv * GetResolution()) - 0.5) * FILM_GRAIN_STRENGTH;
    center += float3(grain);
  }

  SetOutput(float4(clamp(center, float3(0.0), float3(1.0)), src.a));
}
