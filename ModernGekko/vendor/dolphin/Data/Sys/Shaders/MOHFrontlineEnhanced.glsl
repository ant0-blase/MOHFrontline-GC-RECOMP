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

  // Four-neighbour local statistics are intentionally shared by the lighting,
  // AO/contact-shadow heuristic and sharpening passes to keep the shader cheap.
  float3 n = SampleLocation(uv + float2(0.0, -1.0) * px).rgb;
  float3 s = SampleLocation(uv + float2(0.0,  1.0) * px).rgb;
  float3 e = SampleLocation(uv + float2(1.0,  0.0) * px).rgb;
  float3 w = SampleLocation(uv + float2(-1.0, 0.0) * px).rgb;
  float3 local = (n + s + e + w) * 0.25;
  float lc = Luma(center);
  float ll = Luma(local);

  if (OptionEnabled(LIGHTING_ENABLE))
  {
    // Local-light separation: preserves authored colors while making highlights
    // and broad lighting gradients read more clearly at high internal res.
    float light_delta = clamp(lc - ll, -0.25, 0.25);
    center += center * light_delta * LIGHTING_STRENGTH * 1.35;
  }

  if (OptionEnabled(SSAO_ENABLE))
  {
    // Screen-space/luminance AO approximation. It intentionally does not claim
    // to replace the game's geometry/light shadow system.
    float cavity = clamp((ll - lc) * 2.2, 0.0, 1.0);
    center *= 1.0 - cavity * SSAO_STRENGTH * 0.48;
  }

  if (OptionEnabled(CONTACT_SHADOW_ENABLE))
  {
    float gx = abs(Luma(e) - Luma(w));
    float gy = abs(Luma(s) - Luma(n));
    float edge = clamp((gx + gy) * 2.4, 0.0, 1.0);
    float dark_side = clamp(ll - lc + 0.08, 0.0, 1.0);
    center *= 1.0 - edge * dark_side * CONTACT_SHADOW_STRENGTH * 0.38;
  }

  if (OptionEnabled(BLOOM_ENABLE))
  {
    float2 r1 = px * 2.0;
    float2 r2 = px * 5.0;
    float3 bloom = float3(0.0);
    bloom += SampleLocation(uv + float2( 1.0,  0.0) * r1).rgb;
    bloom += SampleLocation(uv + float2(-1.0,  0.0) * r1).rgb;
    bloom += SampleLocation(uv + float2( 0.0,  1.0) * r1).rgb;
    bloom += SampleLocation(uv + float2( 0.0, -1.0) * r1).rgb;
    bloom += SampleLocation(uv + float2( 1.0,  1.0) * r2).rgb;
    bloom += SampleLocation(uv + float2(-1.0,  1.0) * r2).rgb;
    bloom += SampleLocation(uv + float2( 1.0, -1.0) * r2).rgb;
    bloom += SampleLocation(uv + float2(-1.0, -1.0) * r2).rgb;
    bloom *= 0.125;
    float gate = smoothstep(BLOOM_THRESHOLD, BLOOM_THRESHOLD + 0.35, Luma(bloom));
    center += bloom * gate * BLOOM_INTENSITY * 0.55;
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
