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
GUIName = PS3 CSM Shadows
OptionName = CONTACT_SHADOW_ENABLE
DefaultValue = true
[OptionRangeFloat]
GUIName = PS3 CSM Shadow Strength
OptionName = CONTACT_SHADOW_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.55

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

float SampleCSMDepth(int cascade, float2 uv)
{
  float raw;
  if (cascade == 0)
    raw = texture(samp_csm0, float3(uv, 0.0)).r;
  else if (cascade == 1)
    raw = texture(samp_csm1, float3(uv, 0.0)).r;
  else if (cascade == 2)
    raw = texture(samp_csm2, float3(uv, 0.0)).r;
  else
    raw = texture(samp_csm3, float3(uv, 0.0)).r;

  // The caster deliberately renders reverse-Z (near=1, far=0) with GEqual.
  return raw;
}

float3 ReconstructMOHViewPosition(float2 uv, float reverse_depth)
{
  // GameCube perspective projection before Dolphin's host depth conversion:
  // clip.z = p10*z + p11, clip.w = -z, console NDC z in [-1,0].
  // SampleRawDepthLocation() returns -console_ndc_z (near=1, far=0), so:
  // z = -p11 / (p10 - reverse_depth).
  float p0 = moh_csm_camera0.x;
  float p2 = moh_csm_camera0.y;
  float p5 = moh_csm_camera0.z;
  float p6 = moh_csm_camera0.w;
  float p10 = moh_csm_camera1.x;
  float p11 = moh_csm_camera1.y;

  float denom = p10 - reverse_depth;
  if (abs(denom) < 0.0000001)
    denom = denom < 0.0 ? -0.0000001 : 0.0000001;

  float z = -p11 / denom;
  float depth_from_camera = -z;

  // Post-process UVs use a top-left screen convention; projection NDC Y points up.
  float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  if (moh_csm_flags.z != 0)
    ndc.y = -ndc.y;

  return float3(depth_from_camera * (ndc.x + p2) / p0,
                depth_from_camera * (ndc.y + p6) / p5,
                z);
}

float4 ProjectMOHCascade(int cascade, float3 view_pos)
{
  int base = cascade * 4;
  float4 p = float4(view_pos, 1.0);
  return float4(dot(moh_csm_matrix[base + 0], p),
                dot(moh_csm_matrix[base + 1], p),
                dot(moh_csm_matrix[base + 2], p),
                dot(moh_csm_matrix[base + 3], p));
}

float CompareMOHShadowTap(int cascade, float2 uv, float receiver_reverse_z, float bias)
{
  float map_reverse_z = SampleCSMDepth(cascade, clamp(uv, float2(0.0), float2(1.0)));
  // Reverse-Z map: the closest caster has the largest value.  Receiver bias is
  // the host-side equivalent of the RSX caster's polygon-offset depth bias.
  return receiver_reverse_z + bias >= map_reverse_z ? 1.0 : 0.0;
}

float SampleMOHTrueCSM(int cascade, float3 view_pos)
{
  float4 light = ProjectMOHCascade(cascade, view_pos);
  if (abs(light.w) < 0.000001)
    return 1.0;

  float3 ndc = light.xyz / light.w;
  float2 suv = ndc.xy * 0.5 + float2(0.5);
  if (moh_csm_flags.z != 0)
    suv.y = 1.0 - suv.y;

  // Caster clip Z is [-1,0]. Dolphin converts it to reverse-Z [1,0].
  float receiver_reverse_z = clamp(-ndc.z, 0.0, 1.0);
  if (suv.x <= 0.001 || suv.x >= 0.999 || suv.y <= 0.001 || suv.y >= 0.999 ||
      ndc.z < -1.001 || ndc.z > 0.001)
    return 1.0;

  // Exact 4-tap footprint recovered from the Frontline RSX receiver shader:
  // 0.000830078 ~= 0.85 / 1024.
  float o = moh_csm_camera1.w;
  float bias = moh_csm_camera1.z * (1.0 + float(cascade) * 0.55);
  float visibility = 0.0;
  visibility += CompareMOHShadowTap(cascade, suv + float2( o,  0.0), receiver_reverse_z, bias);
  visibility += CompareMOHShadowTap(cascade, suv + float2( o, -o), receiver_reverse_z, bias);
  visibility += CompareMOHShadowTap(cascade, suv + float2(-o,  0.0), receiver_reverse_z, bias);
  visibility += CompareMOHShadowTap(cascade, suv + float2(-o,  o), receiver_reverse_z, bias);
  return visibility * 0.25;
}

float ComputeMOHTrueCSMShadow(float2 uv, float reverse_depth)
{
  if (!HasTrueCSM() || reverse_depth <= 0.000001)
    return 0.0;

  float3 view_pos = ReconstructMOHViewPosition(uv, reverse_depth);
  float view_depth = max(-view_pos.z, 0.0);
  int cascade = 3;
  if (view_depth <= moh_csm_splits.x)
    cascade = 0;
  else if (view_depth <= moh_csm_splits.y)
    cascade = 1;
  else if (view_depth <= moh_csm_splits.z)
    cascade = 2;
  else if (view_depth > moh_csm_splits.w)
    return 0.0;

  float visibility = SampleMOHTrueCSM(cascade, view_pos);
  return clamp(1.0 - visibility, 0.0, 1.0);
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

  // CSM diagnostic view.  This bypasses every colour/post effect so the caster
  // can be validated directly instead of guessing whether an invisible shadow
  // is caused by map contents, projection, depth compare, or option plumbing.
  if (moh_csm_flags.w >= 1 && moh_csm_flags.w <= 4)
  {
    int cascade = moh_csm_flags.w - 1;
    float d = SampleCSMDepth(cascade, uv);
    // v2 diagnostic coding:
    //   RED    = sampler returned ~0 (binding/layout/sample problem)
    //   YELLOW = the 0.25 sentinel clear is intact (texture readable, no raster)
    //   GREEN/BLUE shapes = real caster depths were written.
    float is_zero = 1.0 - step(0.0000005, d);
    float is_clear = 1.0 - step(0.002, abs(d - 0.25));
    float is_written = (1.0 - is_zero) * (1.0 - is_clear);
    float visible_depth = pow(clamp(d, 0.0, 1.0), 0.20);
    float3 debug_rgb =
        is_zero * float3(1.0, 0.0, 0.0) +
        is_clear * float3(1.0, 1.0, 0.0) +
        is_written * float3(0.0, 1.0, visible_depth);
    SetOutput(float4(debug_rgb, 1.0));
    return;
  }
  if (moh_csm_flags.w == 5)
  {
    float mask = 0.0;
    if (HasSceneDepth() && HasTrueCSM())
      mask = ComputeMOHTrueCSMShadow(uv, DepthAt(uv));
    SetOutput(float4(mask, mask, mask, 1.0));
    return;
  }

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
    // This is a real geometry shadow receiver.  Every perspective GX batch was
    // replayed from the .lit sun into four 1024x1024 D32F maps before this
    // pre-HUD pass.  No luminance tracing or screen-space shadow ray is used.
    float shadow = 0.0;
    if (HasSceneDepth() && HasTrueCSM())
      shadow = ComputeMOHTrueCSMShadow(uv, DepthAt(uv));

    // The old option name is retained so existing UI/config plumbing keeps
    // working, but it now controls only the true CSM result.
    float csm_strength = clamp(CONTACT_SHADOW_STRENGTH * 1.60, 0.0, 0.85);
    center *= 1.0 - shadow * csm_strength;
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
