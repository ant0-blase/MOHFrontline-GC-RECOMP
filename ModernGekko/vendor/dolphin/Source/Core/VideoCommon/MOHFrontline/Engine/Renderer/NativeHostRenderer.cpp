#include "VideoCommon/MOHFrontline/Engine/Renderer/NativeHostRenderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Core/System.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/NativeRenderBridge.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/PS3MeshPort.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace MOHFrontline::NativeHostRenderer
{
namespace
{
enum class Style
{
  Wireframe,
  Solid,
  Textured,
  TexturedWire,
};

struct OverlayVertex
{
  float position[4]{};
  float uv0[3]{};
  std::array<u8, 4> color{};
};
static_assert(std::is_standard_layout_v<OverlayVertex>);

struct State
{
  std::unique_ptr<NativeVertexFormat> vertex_format;
  std::unique_ptr<AbstractShader> color_vertex_shader;
  std::unique_ptr<AbstractShader> textured_vertex_shader;
  std::unique_ptr<AbstractShader> textured_pixel_shader;
  std::unique_ptr<AbstractPipeline> line_pipeline;
  std::unique_ptr<AbstractPipeline> solid_pipeline;
  std::unique_ptr<AbstractPipeline> textured_pipeline;
  FramebufferState framebuffer_state{};
  DepthState depth_state{};
  bool depth_state_valid = false;
  const AbstractShader* color_pixel_shader = nullptr;
  std::vector<OverlayVertex> vertices;
  std::vector<u16> triangle_indices;
  std::vector<u16> line_indices;
  std::vector<u8> valid_vertices;
  u64 submitted_draws = 0;
  u64 submitted_triangles = 0;
  u64 guard_fallbacks = 0;
  bool logged_ready = false;
  bool logged_stereo = false;
  bool logged_first_msh = false;
  bool logged_first_dmf = false;
};

std::unique_ptr<State> s_state;

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool EnvSwitch(const char* name, bool fallback)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  const std::string v = Lower(value);
  if (v == "0" || v == "false" || v == "off" || v == "no")
    return false;
  if (v == "1" || v == "true" || v == "on" || v == "yes")
    return true;
  return fallback;
}

float EnvFloat(const char* name, float fallback, float low, float high)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  if (end == value || !std::isfinite(parsed))
    return fallback;
  return std::clamp(parsed, low, high);
}

bool ContainsAny(std::string_view value, std::initializer_list<std::string_view> needles)
{
  for (const std::string_view needle : needles)
  {
    if (value.find(needle) != std::string_view::npos)
      return true;
  }
  return false;
}

struct MaterialProfile
{
  float ambient = 0.28f;
  float diffuse = 0.82f;
  float specular = 0.14f;
  float shininess = 24.0f;
};

MaterialProfile GetMaterialProfile(const NativeRender::DrawPacket& packet)
{
  const std::string key = Lower(packet.material_name + " " + packet.source_name);
  MaterialProfile profile{};

  if (ContainsAny(key, {"metal", "_met", "met256", "chrome", "steel", "gun", "weapon",
                        "pistol", "rifle", "kar98", "mp40", "mg42", "col_", "tom_"}))
  {
    profile.ambient = 0.24f;
    profile.diffuse = 0.82f;
    profile.specular = 0.46f;
    profile.shininess = 52.0f;
  }
  else if (ContainsAny(key, {"glass", "window", "scope", "lens"}))
  {
    profile.ambient = 0.22f;
    profile.diffuse = 0.66f;
    profile.specular = 0.62f;
    profile.shininess = 72.0f;
  }
  else if (ContainsAny(key, {"skin", "face", "hand", "uniform", "cloth", "fabric", "leather"}))
  {
    profile.ambient = 0.32f;
    profile.diffuse = 0.86f;
    profile.specular = 0.07f;
    profile.shininess = 12.0f;
  }
  else if (ContainsAny(key, {"wood", "crate", "barrel", "debris", "terrain", "rock", "wall"}))
  {
    profile.ambient = 0.31f;
    profile.diffuse = 0.84f;
    profile.specular = 0.05f;
    profile.shininess = 10.0f;
  }

  // PERF v21: these overrides are process-launch settings. Previously this
  // called getenv()/strtof four times for every native draw.
  static const float ambient_override =
      EnvFloat("MOH_NATIVE_RENDER_AMBIENT",
               std::numeric_limits<float>::quiet_NaN(), 0.0f, 2.0f);
  static const float diffuse_override =
      EnvFloat("MOH_NATIVE_RENDER_DIFFUSE",
               std::numeric_limits<float>::quiet_NaN(), 0.0f, 2.0f);
  static const float specular_override =
      EnvFloat("MOH_NATIVE_RENDER_SPECULAR",
               std::numeric_limits<float>::quiet_NaN(), 0.0f, 2.0f);
  static const float shininess_override =
      EnvFloat("MOH_NATIVE_RENDER_SHININESS",
               std::numeric_limits<float>::quiet_NaN(), 1.0f, 256.0f);

  if (std::isfinite(ambient_override))
    profile.ambient = ambient_override;
  if (std::isfinite(diffuse_override))
    profile.diffuse = diffuse_override;
  if (std::isfinite(specular_override))
    profile.specular = specular_override;
  if (std::isfinite(shininess_override))
    profile.shininess = shininess_override;
  return profile;
}

std::array<float, 3> Normalize3(std::array<float, 3> v)
{
  const float length2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  if (!std::isfinite(length2) || length2 < 1.0e-12f)
    return {0.0f, 0.0f, 1.0f};
  const float inv = 1.0f / std::sqrt(length2);
  v[0] *= inv;
  v[1] *= inv;
  v[2] *= inv;
  return v;
}

float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> ModelNormalToGCLocal(const NativeRender::DrawPacket& packet,
                                          const NativeRender::Vertex& vertex)
{
  std::array<float, 3> normal = Normalize3(vertex.normal);
  if (!packet.skinned)
    return normal;

  const std::size_t slot = static_cast<std::size_t>(vertex.matrix_index) / 3u;
  if (slot >= packet.model_to_gc_local.size())
    return {0.0f, 0.0f, 1.0f};

  const auto& m = packet.model_to_gc_local[slot];

  // Positions use model_to_gc_local directly. Normals need the inverse-transpose of its
  // linear 3x3 part. For the DMF bind-local bridge this is the transpose(bind) transform
  // used by the existing PS3 skin validation path, not a second position transform.
  const float a00 = m[0], a01 = m[1], a02 = m[2];
  const float a10 = m[4], a11 = m[5], a12 = m[6];
  const float a20 = m[8], a21 = m[9], a22 = m[10];

  const float c00 = a11 * a22 - a12 * a21;
  const float c01 = a12 * a20 - a10 * a22;
  const float c02 = a10 * a21 - a11 * a20;
  const float c10 = a02 * a21 - a01 * a22;
  const float c11 = a00 * a22 - a02 * a20;
  const float c12 = a01 * a20 - a00 * a21;
  const float c20 = a01 * a12 - a02 * a11;
  const float c21 = a02 * a10 - a00 * a12;
  const float c22 = a00 * a11 - a01 * a10;
  const float det = a00 * c00 + a01 * c01 + a02 * c02;

  if (!std::isfinite(det) || std::fabs(det) < 1.0e-9f)
  {
    return Normalize3({a00 * normal[0] + a01 * normal[1] + a02 * normal[2],
                       a10 * normal[0] + a11 * normal[1] + a12 * normal[2],
                       a20 * normal[0] + a21 * normal[1] + a22 * normal[2]});
  }

  const float inv_det = 1.0f / det;
  return Normalize3({(c00 * normal[0] + c01 * normal[1] + c02 * normal[2]) * inv_det,
                     (c10 * normal[0] + c11 * normal[1] + c12 * normal[2]) * inv_det,
                     (c20 * normal[0] + c21 * normal[1] + c22 * normal[2]) * inv_det});
}

std::array<float, 3> GCLocalToView(const std::array<float, 3>& position, u32 matrix_index)
{
  const float* m = &xfmem.posMatrices[(matrix_index & 0x3fu) * 4u];
  return {m[0] * position[0] + m[1] * position[1] + m[2] * position[2] + m[3],
          m[4] * position[0] + m[5] * position[1] + m[6] * position[2] + m[7],
          m[8] * position[0] + m[9] * position[1] + m[10] * position[2] + m[11]};
}

std::array<float, 3> GCLocalNormalToView(const std::array<float, 3>& normal, u32 matrix_index)
{
  const u32 normal_index = matrix_index & 31u;
  if (normal_index > 29u)
    return Normalize3(normal);

  const float* m = &xfmem.normalMatrices[3u * normal_index];
  return Normalize3({m[0] * normal[0] + m[1] * normal[1] + m[2] * normal[2],
                     m[3] * normal[0] + m[4] * normal[1] + m[5] * normal[2],
                     m[6] * normal[0] + m[7] * normal[1] + m[8] * normal[2]});
}

std::array<u8, 4> ComputeHostLighting(const MaterialProfile& profile,
                                      const std::array<float, 3>& view_position,
                                      const std::array<float, 3>& view_normal)
{
  const std::array<float, 3> n = Normalize3(view_normal);
  std::array<float, 3> rgb{profile.ambient, profile.ambient, profile.ambient};

  const LitChannel& channel = xfmem.color[0];
  const u32 light_mask = channel.GetFullLightMask();
  bool used_light = false;

  for (u32 light_index = 0; light_index < 8; ++light_index)
  {
    if ((light_mask & (1u << light_index)) == 0)
      continue;

    const Light& light = xfmem.lights[light_index];
    std::array<float, 3> to_light{light.dpos[0] - view_position[0],
                                  light.dpos[1] - view_position[1],
                                  light.dpos[2] - view_position[2]};
    const float distance2 = Dot3(to_light, to_light);
    if (!std::isfinite(distance2) || distance2 < 1.0e-10f)
    {
      to_light = {-light.ddir[0], -light.ddir[1], -light.ddir[2]};
    }

    const float distance = std::sqrt(std::max(Dot3(to_light, to_light), 1.0e-10f));
    const std::array<float, 3> l = Normalize3(to_light);
    const float ndotl = std::max(Dot3(n, l), 0.0f);
    if (ndotl <= 0.0f)
      continue;

    float attenuation = 1.0f;
    const float denom = light.distatt[0] + light.distatt[1] * distance +
                        light.distatt[2] * distance * distance;
    if (std::isfinite(denom) && denom > 1.0e-5f)
      attenuation = std::clamp(1.0f / denom, 0.0f, 1.0f);

    const std::array<float, 3> light_rgb{light.color[3] / 255.0f, light.color[2] / 255.0f,
                                          light.color[1] / 255.0f};
    for (int c = 0; c < 3; ++c)
      rgb[c] += light_rgb[c] * ndotl * profile.diffuse * attenuation;

    const std::array<float, 3> view = Normalize3(
        {-view_position[0], -view_position[1], -view_position[2]});
    const std::array<float, 3> half_vector =
        Normalize3({l[0] + view[0], l[1] + view[1], l[2] + view[2]});
    const float spec = std::pow(std::max(Dot3(n, half_vector), 0.0f), profile.shininess) *
                       profile.specular * attenuation;
    for (int c = 0; c < 3; ++c)
      rgb[c] += light_rgb[c] * spec;
    used_light = true;
  }

  // Some UI-ish/object draws do not expose a useful GX light mask.  In that case keep a
  // conservative camera-space key light rather than making the native replacement black.
  if (!used_light)
  {
    const std::array<float, 3> l = Normalize3({-0.35f, 0.45f, 0.82f});
    const float ndotl = std::max(Dot3(n, l), 0.0f);
    const float fallback = profile.ambient + profile.diffuse * (0.22f + 0.78f * ndotl);
    rgb = {fallback, fallback, fallback};
  }

  static const float exposure = EnvFloat("MOH_NATIVE_RENDER_EXPOSURE", 1.0f, 0.1f, 4.0f);
  std::array<u8, 4> out{255, 255, 255, 255};
  for (int c = 0; c < 3; ++c)
  {
    const float value = std::clamp(rgb[c] * exposure, 0.08f, 1.35f);
    out[c] = static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));
  }
  return out;
}

Style GetStyle()
{
  static const Style cached = [] {
    const char* value = std::getenv("MOH_NATIVE_RENDER_STYLE");
    if (!value || !*value)
      return Style::Wireframe;

    const std::string v = Lower(value);
    if (v == "solid" || v == "flat" || v == "fill")
      return Style::Solid;
    if (v == "textured" || v == "texture" || v == "tex")
      return Style::Textured;
    if (v == "textured-wire" || v == "textured_wire" || v == "texture-wire" ||
        v == "tex-wire" || v == "both")
    {
      return Style::TexturedWire;
    }
    return Style::Wireframe;
  }();
  return cached;
}

const char* StyleName(Style style)
{
  switch (style)
  {
  case Style::Solid: return "solid";
  case Style::Textured: return "textured";
  case Style::TexturedWire: return "textured-wire";
  default: return "wireframe";
  }
}

bool WantsWire(Style style)
{
  return style == Style::Wireframe || style == Style::TexturedWire;
}

bool WantsSolid(Style style)
{
  return style != Style::Wireframe;
}

bool WantsTexture(Style style)
{
  return style == Style::Textured || style == Style::TexturedWire;
}

std::size_t MaxTrianglesPerDraw()
{
  static const std::size_t cached = [] {
    constexpr std::size_t fallback = 32768;
    const char* value = std::getenv("MOH_NATIVE_RENDER_MAX_TRIS");
    if (!value || !*value)
      return fallback;

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || parsed == 0)
      return fallback;
    const unsigned long long hard_cap =
        static_cast<unsigned long long>(VertexManagerBase::MAXIBUFFERSIZE / 6u);
    return static_cast<std::size_t>(std::min(parsed, hard_cap));
  }();
  return cached;
}

std::array<float, 3> ModelToGCLocal(const NativeRender::DrawPacket& packet,
                                    const NativeRender::Vertex& vertex)
{
  std::array<float, 3> position = vertex.position;

  if (!packet.skinned)
  {
    const auto& current = PS3MeshPort::CurrentStaticDraw();
    if (current && current.world_translation_valid)
    {
      position[0] += current.world_translation[0];
      position[1] += current.world_translation[1];
      position[2] += current.world_translation[2];
    }
    return position;
  }

  const std::size_t slot = static_cast<std::size_t>(vertex.matrix_index) / 3u;
  if (slot >= packet.model_to_gc_local.size())
    return {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};

  const auto& m = packet.model_to_gc_local[slot];
  const float x = position[0];
  const float y = position[1];
  const float z = position[2];
  return {
      m[0] * x + m[1] * y + m[2] * z + m[3],
      m[4] * x + m[5] * y + m[6] * z + m[7],
      m[8] * x + m[9] * y + m[10] * z + m[11],
  };
}

std::string GenerateTexturedVertexShader()
{
  ShaderCode code;
  switch (g_backend_info.api_type)
  {
  case APIType::D3D:
  case APIType::Metal:
  case APIType::OpenGL:
  case APIType::Vulkan:
    code.Write("ATTRIBUTE_LOCATION({:s}) in float3 rawtex0;\n", ShaderAttrib::TexCoord0);
    code.Write("ATTRIBUTE_LOCATION({:s}) in float4 rawcolor0;\n", ShaderAttrib::Color0);
    code.Write("ATTRIBUTE_LOCATION({:s}) in float4 rawpos;\n", ShaderAttrib::Position);
    if (g_backend_info.bSupportsGeometryShaders)
    {
      code.Write("VARYING_LOCATION(0) out VertexData {{\n"
                 "  float3 v_tex0;\n"
                 "  float4 v_col0;\n"
                 "}};\n");
    }
    else
    {
      code.Write("VARYING_LOCATION(0) out float3 v_tex0;\n");
      code.Write("VARYING_LOCATION(1) out float4 v_col0;\n");
    }
    code.Write("#define opos gl_Position\n"
               "void main()\n"
               "{{\n"
               "  v_tex0 = rawtex0;\n"
               "  v_col0 = rawcolor0;\n"
               "  opos = float4(rawpos.xyz, 1.0f);\n");
    if (g_backend_info.api_type == APIType::Vulkan)
      code.Write("  opos.y = -opos.y;\n");
    code.Write("}}\n");
    break;
  default:
    break;
  }
  return code.GetBuffer();
}

std::string GenerateTexturedPixelShader()
{
  ShaderCode code;
  switch (g_backend_info.api_type)
  {
  case APIType::D3D:
  case APIType::Metal:
  case APIType::OpenGL:
  case APIType::Vulkan:
    code.Write("SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n");
    if (g_backend_info.bSupportsGeometryShaders)
    {
      code.Write("VARYING_LOCATION(0) in VertexData {{\n"
                 "  float3 v_tex0;\n"
                 "  float4 v_col0;\n"
                 "}};\n");
    }
    else
    {
      code.Write("VARYING_LOCATION(0) in float3 v_tex0;\n");
      code.Write("VARYING_LOCATION(1) in float4 v_col0;\n");
    }
    code.Write("FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n"
               "void main()\n"
               "{{\n"
               "  ocol0 = texture(samp0, v_tex0) * v_col0;\n"
               "}}\n");
    break;
  default:
    break;
  }
  return code.GetBuffer();
}

float HostDepthNDC(const VertexShaderManager& vertex_shader_manager, const float clip[4])
{
  const auto& correction = vertex_shader_manager.constants.pixelcentercorrection;
  float z = (clip[3] * correction[3] - clip[2] * correction[2]) / clip[3];
  if (!ShaderHostConfig::GetCurrent().backend_clip_control)
    z = z * 2.0f - 1.0f;
  return z;
}

bool EnsurePipelines(State& state, bool use_depth)
{
  if (!g_gfx || !g_vertex_manager || !g_framebuffer_manager || !g_shader_cache)
    return false;

  if (g_framebuffer_manager->IsEFBStereo())
  {
    if (!state.logged_stereo)
    {
      state.logged_stereo = true;
      std::fprintf(stderr,
                   "[moh-native-render] host renderer disabled in stereo mode -> GX only\n");
    }
    return false;
  }

  AbstractFramebuffer* efb = g_framebuffer_manager->GetEFBFramebuffer();
  if (!efb || g_gfx->GetCurrentFramebuffer() != efb)
    return false;

  const AbstractShader* color_pixel_shader = g_shader_cache->GetColorPixelShader();
  if (!color_pixel_shader)
    return false;

  if (!state.vertex_format)
  {
    PortableVertexDeclaration decl{};
    decl.position.enable = true;
    decl.position.type = ComponentFormat::Float;
    decl.position.components = 4;
    decl.position.integer = false;
    decl.position.offset = offsetof(OverlayVertex, position);
    decl.colors[0].enable = true;
    decl.colors[0].type = ComponentFormat::UByte;
    decl.colors[0].components = 4;
    decl.colors[0].integer = false;
    decl.colors[0].offset = offsetof(OverlayVertex, color);
    decl.texcoords[0].enable = true;
    decl.texcoords[0].type = ComponentFormat::Float;
    decl.texcoords[0].components = 3;
    decl.texcoords[0].integer = false;
    decl.texcoords[0].offset = offsetof(OverlayVertex, uv0);
    decl.stride = sizeof(OverlayVertex);

    state.vertex_format = g_gfx->CreateNativeVertexFormat(decl);
    if (!state.vertex_format)
      return false;
  }

  if (!state.color_vertex_shader)
  {
    state.color_vertex_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Vertex, FramebufferShaderGen::GenerateEFBPokeVertexShader(), nullptr,
        "MOH PS3 native color vertex shader");
    if (!state.color_vertex_shader)
      return false;
  }

  if (!state.textured_vertex_shader)
  {
    state.textured_vertex_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Vertex, GenerateTexturedVertexShader(), nullptr,
        "MOH PS3 native textured vertex shader");
    if (!state.textured_vertex_shader)
      return false;
  }

  if (!state.textured_pixel_shader)
  {
    state.textured_pixel_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Pixel, GenerateTexturedPixelShader(), nullptr,
        "MOH PS3 native lit textured pixel shader");
    if (!state.textured_pixel_shader)
      return false;
  }

  const FramebufferState framebuffer_state = g_framebuffer_manager->GetEFBFramebufferState();
  const bool common_changed = state.framebuffer_state.hex != framebuffer_state.hex ||
                              state.color_pixel_shader != color_pixel_shader;

  if (!state.line_pipeline || common_changed)
  {
    AbstractPipelineConfig config{};
    config.vertex_format = state.vertex_format.get();
    config.vertex_shader = state.color_vertex_shader.get();
    config.geometry_shader = nullptr;
    config.pixel_shader = color_pixel_shader;
    config.rasterization_state =
        RenderState::GetNoCullRasterizationState(PrimitiveType::Lines);
    config.depth_state = RenderState::GetNoDepthTestingDepthState();
    config.blending_state = RenderState::GetNoBlendingBlendState();
    config.framebuffer_state = framebuffer_state;
    config.usage = AbstractPipelineUsage::Utility;

    state.line_pipeline = g_gfx->CreatePipeline(config);
    if (!state.line_pipeline)
      return false;
  }

  DepthState depth_state = RenderState::GetNoDepthTestingDepthState();
  if (use_depth)
    depth_state.Generate(bpmem);

  const bool depth_changed = !state.depth_state_valid || state.depth_state.hex != depth_state.hex;
  if (!state.solid_pipeline || !state.textured_pipeline || common_changed || depth_changed)
  {
    AbstractPipelineConfig config{};
    config.vertex_format = state.vertex_format.get();
    config.geometry_shader = nullptr;
    config.rasterization_state =
        RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
    config.depth_state = depth_state;
    config.blending_state = RenderState::GetNoBlendingBlendState();
    config.framebuffer_state = framebuffer_state;
    config.usage = AbstractPipelineUsage::Utility;

    config.vertex_shader = state.color_vertex_shader.get();
    config.pixel_shader = color_pixel_shader;
    state.solid_pipeline = g_gfx->CreatePipeline(config);
    if (!state.solid_pipeline)
      return false;

    config.vertex_shader = state.textured_vertex_shader.get();
    config.pixel_shader = state.textured_pixel_shader.get();
    state.textured_pipeline = g_gfx->CreatePipeline(config);
    if (!state.textured_pipeline)
      return false;

    state.depth_state = depth_state;
    state.depth_state_valid = true;
  }

  state.framebuffer_state = framebuffer_state;
  state.color_pixel_shader = color_pixel_shader;

  if (!state.logged_ready)
  {
    state.logged_ready = true;
    std::fprintf(stderr,
                 "[moh-native-render] HOST RENDERER v9 ready: asset normals + host lighting + "
                 "material response + source-neutral stage0 texture -> EFB\n");
  }
  return true;
}

bool Submit(const NativeRender::DrawPacket& packet, void*)
{
  static const bool overlay = EnvSwitch("MOH_NATIVE_RENDER_OVERLAY", true);
  if (!overlay || packet.vertices.empty() ||
      packet.indices.size() < 3 || packet.vertices.size() > 65535)
  {
    return false;
  }

  const Style style = GetStyle();
  static const bool use_depth = EnvSwitch(
      "MOH_NATIVE_RENDER_DEPTH", NativeRender::GetMode() == NativeRender::Mode::PreferNative);
  static const bool lighting = EnvSwitch("MOH_NATIVE_RENDER_LIGHTING", true);
  const bool use_lighting = lighting && WantsSolid(style);
  static const bool deform_guard = EnvSwitch(
      "MOH_NATIVE_RENDER_DEFORM_GUARD", NativeRender::GetMode() == NativeRender::Mode::PreferNative);
  const MaterialProfile material = GetMaterialProfile(packet);

  if (!s_state)
    s_state = std::make_unique<State>();
  State& state = *s_state;
  if (!EnsurePipelines(state, use_depth))
    return false;

  state.vertices.resize(packet.vertices.size());
  state.valid_vertices.assign(packet.vertices.size(), 0);

  auto& vertex_shader_manager = Core::System::GetInstance().GetVertexShaderManager();
  const u32 static_matrix_index = g_main_cp_state.matrix_index_a.PosNormalMtxIdx.Value();
  std::size_t invalid_vertices = 0;

  for (std::size_t i = 0; i < packet.vertices.size(); ++i)
  {
    const NativeRender::Vertex& source = packet.vertices[i];
    const std::array<float, 3> local = ModelToGCLocal(packet, source);
    if (!std::isfinite(local[0]) || !std::isfinite(local[1]) || !std::isfinite(local[2]))
    {
      ++invalid_vertices;
      continue;
    }

    float clip[4]{};
    const u32 matrix_index = packet.skinned ? static_cast<u32>(source.matrix_index) :
                                              static_matrix_index;
    vertex_shader_manager.TransformToClipSpace(local.data(), clip, matrix_index);

    if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[2]) ||
        !std::isfinite(clip[3]) || clip[3] <= 1.0e-5f)
    {
      ++invalid_vertices;
      continue;
    }

    const float x = clip[0] / clip[3];
    const float y = clip[1] / clip[3];
    const float z = use_depth ? HostDepthNDC(vertex_shader_manager, clip) : 0.0f;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        std::fabs(x) > 10000.0f || std::fabs(y) > 10000.0f || std::fabs(z) > 10000.0f)
    {
      ++invalid_vertices;
      continue;
    }

    state.vertices[i].position[0] = x;
    state.vertices[i].position[1] = y;
    state.vertices[i].position[2] = z;
    state.vertices[i].position[3] = 1.0f;
    state.vertices[i].uv0[0] = source.uv0[0];
    state.vertices[i].uv0[1] = source.uv0[1];
    state.vertices[i].uv0[2] = 0.0f;

    if (use_lighting)
    {
      const std::array<float, 3> local_normal = ModelNormalToGCLocal(packet, source);
      const std::array<float, 3> view_position = GCLocalToView(local, matrix_index);
      const std::array<float, 3> view_normal = GCLocalNormalToView(local_normal, matrix_index);
      state.vertices[i].color = ComputeHostLighting(material, view_position, view_normal);
    }
    else
    {
      state.vertices[i].color = {255, 255, 255, 255};
    }
    state.valid_vertices[i] = 1;
  }

  if (deform_guard && NativeRender::GetMode() == NativeRender::Mode::PreferNative)
  {
    const float valid_ratio = packet.vertices.empty() ? 0.0f :
        static_cast<float>(packet.vertices.size() - invalid_vertices) /
            static_cast<float>(packet.vertices.size());
    static const float minimum = EnvFloat("MOH_NATIVE_RENDER_MIN_VALID_VERTS", 0.82f, 0.25f, 1.0f);
    if (valid_ratio < minimum)
    {
      ++state.guard_fallbacks;
      if (state.guard_fallbacks <= 32)
      {
        std::fprintf(stderr,
                     "[moh-native-render] v9 DEFORM GUARD -> GX source=%s valid=%.3f "
                     "required=%.3f invalid=%zu/%zu\n",
                     packet.source_name.c_str(), valid_ratio, minimum, invalid_vertices,
                     packet.vertices.size());
      }
      return false;
    }
  }

  state.triangle_indices.clear();
  state.line_indices.clear();
  const std::size_t triangle_count =
      std::min(packet.indices.size() / 3u, MaxTrianglesPerDraw());
  if (WantsSolid(style))
    state.triangle_indices.reserve(triangle_count * 3u);
  if (WantsWire(style))
    state.line_indices.reserve(triangle_count * 6u);

  std::size_t accepted_triangles = 0;
  for (std::size_t triangle = 0; triangle < triangle_count; ++triangle)
  {
    const u32 a = packet.indices[triangle * 3u + 0u];
    const u32 b = packet.indices[triangle * 3u + 1u];
    const u32 c = packet.indices[triangle * 3u + 2u];
    if (a >= packet.vertices.size() || b >= packet.vertices.size() || c >= packet.vertices.size() ||
        !state.valid_vertices[a] || !state.valid_vertices[b] || !state.valid_vertices[c])
    {
      continue;
    }

    if (WantsSolid(style))
    {
      state.triangle_indices.push_back(static_cast<u16>(a));
      state.triangle_indices.push_back(static_cast<u16>(b));
      state.triangle_indices.push_back(static_cast<u16>(c));
    }

    if (WantsWire(style))
    {
      state.line_indices.push_back(static_cast<u16>(a));
      state.line_indices.push_back(static_cast<u16>(b));
      state.line_indices.push_back(static_cast<u16>(b));
      state.line_indices.push_back(static_cast<u16>(c));
      state.line_indices.push_back(static_cast<u16>(c));
      state.line_indices.push_back(static_cast<u16>(a));
    }
    ++accepted_triangles;
  }

  if (accepted_triangles == 0 ||
      (WantsSolid(style) && state.triangle_indices.empty()) ||
      (WantsWire(style) && state.line_indices.empty()))
  {
    return false;
  }

  if (deform_guard && NativeRender::GetMode() == NativeRender::Mode::PreferNative &&
      triangle_count != 0)
  {
    const float triangle_ratio =
        static_cast<float>(accepted_triangles) / static_cast<float>(triangle_count);
    static const float minimum = EnvFloat("MOH_NATIVE_RENDER_MIN_VALID_TRIS", 0.88f, 0.25f, 1.0f);
    if (triangle_ratio < minimum)
    {
      ++state.guard_fallbacks;
      if (state.guard_fallbacks <= 32)
      {
        std::fprintf(stderr,
                     "[moh-native-render] v9 DEFORM GUARD -> GX source=%s tris=%.3f "
                     "required=%.3f accepted=%zu/%zu\n",
                     packet.source_name.c_str(), triangle_ratio, minimum, accepted_triangles,
                     triangle_count);
      }
      return false;
    }
  }

  g_gfx->BeginUtilityDrawing();

  if (WantsSolid(style))
  {
    u32 base_vertex = 0;
    u32 base_index = 0;
    g_vertex_manager->UploadUtilityVertices(
        state.vertices.data(), sizeof(OverlayVertex), static_cast<u32>(state.vertices.size()),
        state.triangle_indices.data(), static_cast<u32>(state.triangle_indices.size()), &base_vertex,
        &base_index);

    if (WantsTexture(style))
    {
      // TextureCacheBase already bound the current GX stage-0 texture before
      // DrawCurrentBatch().  When PS3MaterialCatalog/TextureCache replaced that
      // stage, this samples the already-resolved PS3 texture without feeding a
      // PS3 binary texture through a GameCube decoder.
      g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
      g_gfx->SetPipeline(state.textured_pipeline.get());
    }
    else
    {
      g_gfx->SetPipeline(state.solid_pipeline.get());
    }
    g_gfx->DrawIndexed(base_index, static_cast<u32>(state.triangle_indices.size()), base_vertex);
  }

  if (WantsWire(style))
  {
    const std::array<u8, 4> wire_color =
        packet.skinned ? std::array<u8, 4>{255, 156, 0, 255} :
                         std::array<u8, 4>{0, 230, 255, 255};
    for (std::size_t i = 0; i < state.vertices.size(); ++i)
    {
      if (state.valid_vertices[i])
        state.vertices[i].color = wire_color;
    }

    u32 base_vertex = 0;
    u32 base_index = 0;
    g_vertex_manager->UploadUtilityVertices(
        state.vertices.data(), sizeof(OverlayVertex), static_cast<u32>(state.vertices.size()),
        state.line_indices.data(), static_cast<u32>(state.line_indices.size()), &base_vertex,
        &base_index);
    g_gfx->SetPipeline(state.line_pipeline.get());
    g_gfx->DrawIndexed(base_index, static_cast<u32>(state.line_indices.size()), base_vertex);
  }

  g_gfx->EndUtilityDrawing();

  ++state.submitted_draws;
  state.submitted_triangles += accepted_triangles;
  const bool first_kind = packet.skinned ? !state.logged_first_dmf : !state.logged_first_msh;
  if (packet.skinned)
    state.logged_first_dmf = true;
  else
    state.logged_first_msh = true;

  if (state.submitted_draws <= 128 || first_kind)
  {
    std::fprintf(stderr,
                 "[moh-native-render] HOST DRAW %s source=%s material=%s tris=%zu style=%s "
                 "depth=%d texture=%s lighting=%s profile[a=%.2f d=%.2f s=%.2f sh=%.1f] "
                 "guard_fallbacks=%llu total=%llu\n",
                 packet.skinned ? "DMF" : "MSH", packet.source_name.c_str(),
                 packet.material_name.empty() ? "<none>" : packet.material_name.c_str(),
                 accepted_triangles, StyleName(style), use_depth ? 1 : 0,
                 WantsTexture(style) ? "stage0-current" : "none",
                 use_lighting ? "asset-normal/GX-lights" : "off", material.ambient,
                 material.diffuse, material.specular, material.shininess,
                 static_cast<unsigned long long>(state.guard_fallbacks),
                 static_cast<unsigned long long>(state.submitted_draws));
  }

  return true;
}
}  // namespace

void Initialize()
{
  NativeRender::SetSubmitter(&Submit, nullptr);
}

void Shutdown()
{
  NativeRender::SetSubmitter(nullptr, nullptr);
  s_state.reset();
}
}  // namespace MOHFrontline::NativeHostRenderer
