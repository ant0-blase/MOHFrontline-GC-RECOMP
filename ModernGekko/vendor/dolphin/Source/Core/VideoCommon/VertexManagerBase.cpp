// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexManagerBase.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/SmallVector.h"

#include "Core/DolphinAnalytics.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/CustomShaderCache.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/PS3MeshPort.h"
#include "VideoCommon/MOHFrontline/Engine/Audio/NativeAudio.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/NativeHostRenderer.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/NativeRenderBridge.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

std::unique_ptr<VertexManagerBase> g_vertex_manager;

namespace
{
constexpr u32 MOH_CSM_CASCADES = 4;
struct MohCSMQuality
{
  u32 cascades = 3;
  u32 resolution = 768;
};
const MohCSMQuality& GetMohCSMQuality()
{
  static const MohCSMQuality quality = [] {
    const char* value = std::getenv("MOH_CSM_QUALITY");
    if (value && std::string_view(value) == "low") return MohCSMQuality{2, 512};
    if (value && std::string_view(value) == "high") return MohCSMQuality{4, 1024};
    return MohCSMQuality{};
  }();
  return quality;
}

struct MohVec3
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

MohVec3 operator+(const MohVec3& a, const MohVec3& b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
MohVec3 operator-(const MohVec3& a, const MohVec3& b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
MohVec3 operator*(const MohVec3& a, float s)
{
  return {a.x * s, a.y * s, a.z * s};
}
float MohDot(const MohVec3& a, const MohVec3& b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
MohVec3 MohCross(const MohVec3& a, const MohVec3& b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
MohVec3 MohNormalize(MohVec3 v)
{
  const float len2 = MohDot(v, v);
  if (!std::isfinite(len2) || len2 < 1.0e-10f)
    return {};
  const float inv = 1.0f / std::sqrt(len2);
  return v * inv;
}

bool MohEnvSwitch(const char* name, bool fallback)
{
  const char* raw = std::getenv(name);
  if (!raw || !*raw)
    return fallback;
  const std::string_view v(raw);
  if (v == "0" || v == "false" || v == "off" || v == "no")
    return false;
  if (v == "1" || v == "true" || v == "on" || v == "yes")
    return true;
  return fallback;
}

float MohEnvFloat(const char* name, float fallback)
{
  const char* raw = std::getenv(name);
  if (!raw || !*raw)
    return fallback;
  char* end = nullptr;
  const float value = std::strtof(raw, &end);
  return end != raw && end && *end == '\0' && std::isfinite(value) ? value : fallback;
}

bool MohCSMEnabled()
{
  // Host cascaded shadow maps are disabled for MOH Frontline.
  // The PS3 asset/material/lighting renderer remains enabled independently.
  // Returning here prevents CSM state allocation, D32F targets, caster draws and sampling.
  return false;
}

MohVec3 FindMohSunDirection(const VertexShaderConstants& constants, bool* from_gx)
{
  // GX light directions are already in the same view space as the vertices.
  // Pick the brightest valid directional light so the host CSM remains stable
  // when the camera rotates instead of using a screen-relative fake direction.
  float best_score = -1.0f;
  MohVec3 best{};
  for (const auto& light : constants.lights)
  {
    const MohVec3 v = MohNormalize({light.dir[0], light.dir[1], light.dir[2]});
    if (MohDot(v, v) < 0.5f)
      continue;

    const float score = static_cast<float>(std::max({light.color[0], light.color[1], light.color[2]}));
    if (score > best_score)
    {
      best_score = score;
      best = v;
    }
  }

  if (best_score >= 0.0f)
  {
    if (from_gx)
      *from_gx = true;
    return best;
  }

  if (from_gx)
    *from_gx = false;
  // Only used before the first usable GX directional light reaches the host.
  return MohNormalize({-0.38f, 0.46f, 0.80f});
}

std::array<MohVec3, 8> BuildFrustumCorners(float p0, float p2, float p5, float p6,
                                           float near_d, float far_d)
{
  std::array<MohVec3, 8> corners{};
  int n = 0;
  for (float d : {near_d, far_d})
  {
    const float z = -d;
    for (int y = 0; y < 2; ++y)
    {
      const float ndc_y = y ? 1.0f : -1.0f;
      for (int x = 0; x < 2; ++x)
      {
        const float ndc_x = x ? 1.0f : -1.0f;
        corners[n++] = {d * (ndc_x + p2) / p0, d * (ndc_y + p6) / p5, z};
      }
    }
  }
  return corners;
}

std::array<float, 16> BuildCascadeMatrix(float p0, float p2, float p5, float p6,
                                         float split_near, float split_far,
                                         const MohVec3& sun_dir)
{
  const auto corners = BuildFrustumCorners(p0, p2, p5, p6, split_near, split_far);

  const MohVec3 forward = MohNormalize(sun_dir);
  const MohVec3 up_seed = std::fabs(forward.y) < 0.92f ? MohVec3{0.0f, 1.0f, 0.0f} :
                                                        MohVec3{1.0f, 0.0f, 0.0f};
  const MohVec3 right = MohNormalize(MohCross(up_seed, forward));
  const MohVec3 up = MohNormalize(MohCross(forward, right));

  float min_x = 1.0e30f, max_x = -1.0e30f;
  float min_y = 1.0e30f, max_y = -1.0e30f;
  float min_z = 1.0e30f, max_z = -1.0e30f;
  for (const MohVec3& p : corners)
  {
    const float lx = MohDot(right, p);
    const float ly = MohDot(up, p);
    const float lz = MohDot(forward, p);
    min_x = std::min(min_x, lx);
    max_x = std::max(max_x, lx);
    min_y = std::min(min_y, ly);
    max_y = std::max(max_y, ly);
    min_z = std::min(min_z, lz);
    max_z = std::max(max_z, lz);
  }

  // Square, texel-snapped XY coverage prevents the shadow projection from
  // swimming as the camera translates.  Z is deliberately expanded so a
  // visible object can cast onto another visible object across a cascade.
  float extent = std::max(max_x - min_x, max_y - min_y) * 1.08f;
  extent = std::max(extent, 1.0f);
  float center_x = (min_x + max_x) * 0.5f;
  float center_y = (min_y + max_y) * 0.5f;
  const float units_per_texel = extent / static_cast<float>(GetMohCSMQuality().resolution);
  center_x = std::floor(center_x / units_per_texel + 0.5f) * units_per_texel;
  center_y = std::floor(center_y / units_per_texel + 0.5f) * units_per_texel;

  const float z_margin = std::max(24.0f, split_far * 0.35f);
  min_z -= z_margin;
  max_z += z_margin;
  const float z_range = std::max(max_z - min_z, 1.0f);

  const float xy_scale = 2.0f / extent;
  const float z_scale = 1.0f / z_range;

  // Dolphin's GX vertex path clips console Z in [-W,0].  X/Y are normal NDC;
  // Z maps min_z -> -1 and max_z -> 0.  The vertex generator later converts
  // this to the host depth convention, giving us a reverse-Z D32F shadow map.
  return {
      right.x * xy_scale, right.y * xy_scale, right.z * xy_scale, -center_x * xy_scale,
      up.x * xy_scale,    up.y * xy_scale,    up.z * xy_scale,    -center_y * xy_scale,
      forward.x * z_scale, forward.y * z_scale, forward.z * z_scale,
      -1.0f - min_z * z_scale,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
}
}  // namespace

struct MOHCSMState
{
  static constexpr u32 SET_COUNT = 2;
  std::array<std::array<std::unique_ptr<AbstractTexture>, MOH_CSM_CASCADES>, SET_COUNT>
      depth_textures;
  std::array<std::array<std::unique_ptr<AbstractFramebuffer>, MOH_CSM_CASCADES>, SET_COUNT>
      framebuffers;
  std::unordered_map<const AbstractPipeline*, std::unique_ptr<AbstractPipeline>> pipelines;
  std::unique_ptr<AbstractShader> depth_only_pixel_shader;
  std::array<MOHCSMReceiverData, SET_COUNT> receivers{};
  std::array<bool, SET_COUNT> wrote{};
  std::array<bool, SET_COUNT> finished_for_sampling{};
  std::array<u64, SET_COUNT> caster_batches{};
  u32 render_set = 0;
  u32 sample_set = 1;
  bool sample_valid = false;
  bool resources_ready = false;
  bool frame_started = false;
  bool logged_active = false;
  bool logged_fallback_sun = false;
  bool logged_pipeline_ready = false;
  bool logged_pipeline_failure = false;
  bool logged_first_caster_draw = false;
  bool logged_double_buffer = false;
};

using OpcodeDecoder::Primitive;

// GX primitive -> RenderState primitive, no primitive restart
constexpr Common::EnumMap<PrimitiveType, Primitive::GX_DRAW_POINTS> primitive_from_gx{
    PrimitiveType::Triangles,  // GX_DRAW_QUADS
    PrimitiveType::Triangles,  // GX_DRAW_QUADS_2
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLES
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLE_STRIP
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLE_FAN
    PrimitiveType::Lines,      // GX_DRAW_LINES
    PrimitiveType::Lines,      // GX_DRAW_LINE_STRIP
    PrimitiveType::Points,     // GX_DRAW_POINTS
};

// GX primitive -> RenderState primitive, using primitive restart
constexpr Common::EnumMap<PrimitiveType, Primitive::GX_DRAW_POINTS> primitive_from_gx_pr{
    PrimitiveType::TriangleStrip,  // GX_DRAW_QUADS
    PrimitiveType::TriangleStrip,  // GX_DRAW_QUADS_2
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLES
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLE_STRIP
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLE_FAN
    PrimitiveType::Lines,          // GX_DRAW_LINES
    PrimitiveType::Lines,          // GX_DRAW_LINE_STRIP
    PrimitiveType::Points,         // GX_DRAW_POINTS
};

// Due to the BT.601 standard which the GameCube is based on being a compromise
// between PAL and NTSC, neither standard gets square pixels. They are each off
// by ~9% in opposite directions.
// Just in case any game decides to take this into account, we do both these
// tests with a large amount of slop.

static float CalculateProjectionViewportRatio(const Projection::Raw& projection,
                                              const Viewport& viewport)
{
  const float projection_ar = projection[2] / projection[0];
  const float viewport_ar = viewport.wd / viewport.ht;

  return std::abs(projection_ar / viewport_ar);
}

static bool IsAnamorphicProjection(const Projection::Raw& projection, const Viewport& viewport,
                                   const VideoConfig& config)
{
  // If ratio between our projection and viewport aspect ratios is similar to 16:9 / 4:3
  // we have an anamorphic projection. This value can be overridden by a GameINI.
  // Game cheats that change the aspect ratio to natively unsupported ones
  // won't be automatically recognized here.

  return std::abs(CalculateProjectionViewportRatio(projection, viewport) -
                  config.widescreen_heuristic_widescreen_ratio) <
         config.widescreen_heuristic_aspect_ratio_slop;
}

static bool IsNormalProjection(const Projection::Raw& projection, const Viewport& viewport,
                               const VideoConfig& config)
{
  return std::abs(CalculateProjectionViewportRatio(projection, viewport) -
                  config.widescreen_heuristic_standard_ratio) <
         config.widescreen_heuristic_aspect_ratio_slop;
}

VertexManagerBase::VertexManagerBase()
    : m_cpu_vertex_buffer(MAXVBUFFERSIZE), m_cpu_index_buffer(MAXIBUFFERSIZE)
{
}

VertexManagerBase::~VertexManagerBase()
{
  MOHFrontline::NativeHostRenderer::Shutdown();
}

bool VertexManagerBase::Initialize()
{
  auto& video_events = GetVideoEvents();

  m_frame_end_event =
      video_events.after_frame_event.Register([this](Core::System&) { OnEndFrame(); });
  m_after_present_event = video_events.after_present_event.Register(
      [this](const PresentInfo& pi) { m_ticks_elapsed = pi.emulated_timestamp; });
  m_index_generator.Init();
  m_custom_shader_cache = std::make_unique<CustomShaderCache>();
  m_cpu_cull.Init();
  MOHFrontline::NativeHostRenderer::Initialize();
  return true;
}

u32 VertexManagerBase::GetRemainingSize() const
{
  return static_cast<u32>(m_end_buffer_pointer - m_cur_buffer_pointer);
}

void VertexManagerBase::AddIndices(OpcodeDecoder::Primitive primitive, u32 num_vertices)
{
  m_index_generator.AddIndices(primitive, num_vertices);
}

bool VertexManagerBase::AreAllVerticesCulled(VertexLoaderBase* loader,
                                             OpcodeDecoder::Primitive primitive, const u8* src,
                                             u32 count)
{
  return m_cpu_cull.AreAllVerticesCulled(loader, primitive, src, count);
}

DataReader VertexManagerBase::PrepareForAdditionalData(OpcodeDecoder::Primitive primitive,
                                                       u32 count, u32 stride, bool cullall)
{
  // Flush all EFB pokes. Since the buffer is shared, we can't draw pokes+primitives concurrently.
  g_framebuffer_manager->FlushEFBPokes();

  // The SSE vertex loader can write up to 4 bytes past the end
  u32 const needed_vertex_bytes = count * stride + 4;

  // We can't merge different kinds of primitives, so we have to flush here
  PrimitiveType new_primitive_type = g_backend_info.bSupportsPrimitiveRestart ?
                                         primitive_from_gx_pr[primitive] :
                                         primitive_from_gx[primitive];
  if (m_current_primitive_type != new_primitive_type) [[unlikely]]
  {
    Flush();

    // Have to update the rasterization state for point/line cull modes.
    m_current_primitive_type = new_primitive_type;
    SetRasterizationStateChanged();
  }

  u32 remaining_indices = GetRemainingIndices(primitive);
  u32 remaining_index_generator_indices = m_index_generator.GetRemainingIndices(primitive);

  // Check for size in buffer, if the buffer gets full, call Flush()
  if (!m_is_flushed && (count > remaining_index_generator_indices || count > remaining_indices ||
                        needed_vertex_bytes > GetRemainingSize())) [[unlikely]]
  {
    Flush();
  }

  m_cull_all = cullall;

  // need to alloc new buffer
  if (m_is_flushed) [[unlikely]]
  {
    if (cullall)
    {
      // This buffer isn't getting sent to the GPU. Just allocate it on the cpu.
      m_cur_buffer_pointer = m_base_buffer_pointer = m_cpu_vertex_buffer.data();
      m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
      m_index_generator.Start(m_cpu_index_buffer.data());
    }
    else
    {
      ResetBuffer(stride);
    }

    remaining_index_generator_indices = m_index_generator.GetRemainingIndices(primitive);
    remaining_indices = GetRemainingIndices(primitive);
    m_is_flushed = false;
  }

  // Now that we've reset the buffer, there should be enough space. It's possible that we still
  // won't have enough space in a few rare cases, such as vertex shader line/point expansion with a
  // ton of lines in one draw command, in which case we will either need to add support for
  // splitting a single draw command into multiple draws or using bigger indices.
  ASSERT_MSG(VIDEO, count <= remaining_index_generator_indices,
             "VertexManager: Too few remaining index values ({} > {}). "
             "32-bit indices or primitive breaking needed.",
             count, remaining_index_generator_indices);
  ASSERT_MSG(VIDEO, count <= remaining_indices,
             "VertexManager: Buffer not large enough for all indices! ({} > {}) "
             "Increase MAXIBUFFERSIZE or we need primitive breaking after all.",
             count, remaining_indices);
  ASSERT_MSG(VIDEO, needed_vertex_bytes <= GetRemainingSize(),
             "VertexManager: Buffer not large enough for all vertices! ({} > {}) "
             "Increase MAXVBUFFERSIZE or we need primitive breaking after all.",
             needed_vertex_bytes, GetRemainingSize());

  return DataReader(m_cur_buffer_pointer, m_end_buffer_pointer);
}

DataReader VertexManagerBase::DisableCullAll(u32 stride)
{
  if (m_cull_all)
  {
    m_cull_all = false;
    ResetBuffer(stride);
  }
  return DataReader(m_cur_buffer_pointer, m_end_buffer_pointer);
}

void VertexManagerBase::FlushData(u32 count, u32 stride)
{
  m_cur_buffer_pointer += count * stride;
}

u32 VertexManagerBase::GetRemainingIndices(OpcodeDecoder::Primitive primitive) const
{
  const u32 index_len = MAXIBUFFERSIZE - m_index_generator.GetIndexLen();

  if (primitive >= Primitive::GX_DRAW_LINES)
  {
    if (g_Config.UseVSForLinePointExpand())
    {
      if (g_backend_info.bSupportsPrimitiveRestart)
      {
        switch (primitive)
        {
        case Primitive::GX_DRAW_LINES:
          return index_len / 5 * 2;
        case Primitive::GX_DRAW_LINE_STRIP:
          return index_len / 5 + 1;
        case Primitive::GX_DRAW_POINTS:
          return index_len / 5;
        default:
          return 0;
        }
      }
      else
      {
        switch (primitive)
        {
        case Primitive::GX_DRAW_LINES:
          return index_len / 6 * 2;
        case Primitive::GX_DRAW_LINE_STRIP:
          return index_len / 6 + 1;
        case Primitive::GX_DRAW_POINTS:
          return index_len / 6;
        default:
          return 0;
        }
      }
    }
    else
    {
      switch (primitive)
      {
      case Primitive::GX_DRAW_LINES:
        return index_len;
      case Primitive::GX_DRAW_LINE_STRIP:
        return index_len / 2 + 1;
      case Primitive::GX_DRAW_POINTS:
        return index_len;
      default:
        return 0;
      }
    }
  }
  else if (g_backend_info.bSupportsPrimitiveRestart)
  {
    switch (primitive)
    {
    case Primitive::GX_DRAW_QUADS:
    case Primitive::GX_DRAW_QUADS_2:
      return index_len / 5 * 4;
    case Primitive::GX_DRAW_TRIANGLES:
      return index_len / 4 * 3;
    case Primitive::GX_DRAW_TRIANGLE_STRIP:
      return index_len / 1 - 1;
    case Primitive::GX_DRAW_TRIANGLE_FAN:
      return index_len / 6 * 4 + 1;
    default:
      return 0;
    }
  }
  else
  {
    switch (primitive)
    {
    case Primitive::GX_DRAW_QUADS:
    case Primitive::GX_DRAW_QUADS_2:
      return index_len / 6 * 4;
    case Primitive::GX_DRAW_TRIANGLES:
      return index_len;
    case Primitive::GX_DRAW_TRIANGLE_STRIP:
      return index_len / 3 + 2;
    case Primitive::GX_DRAW_TRIANGLE_FAN:
      return index_len / 3 + 2;
    default:
      return 0;
    }
  }
}

auto VertexManagerBase::ResetFlushAspectRatioCount() -> FlushStatistics
{
  const auto result = m_flush_statistics;
  m_flush_statistics = {};
  return result;
}

void VertexManagerBase::ResetBuffer(u32 vertex_stride)
{
  m_base_buffer_pointer = m_cpu_vertex_buffer.data();
  m_cur_buffer_pointer = m_cpu_vertex_buffer.data();
  m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
  m_index_generator.Start(m_cpu_index_buffer.data());
}

void VertexManagerBase::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                     u32* out_base_vertex, u32* out_base_index)
{
  *out_base_vertex = 0;
  *out_base_index = 0;
}

void VertexManagerBase::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  // If bounding box is enabled, we need to flush any changes first, then invalidate what we have.
  if (g_bounding_box->IsEnabled() && g_ActiveConfig.bBBoxEnable && g_backend_info.bSupportsBBox)
  {
    g_bounding_box->Flush();
  }

  // A native utility draw uploads its own vertex/index buffer.  In shadow mode
  // the original GX batch must therefore be submitted first, then the host
  // overlay can safely replace the binding.  PreferNative intentionally does
  // the opposite: if the host backend accepts the PS3 packet, GX is skipped.
  const auto native_mode = MOHFrontline::NativeRender::GetMode();
  if (native_mode == MOHFrontline::NativeRender::Mode::PreferNative &&
      MOHFrontline::NativeRender::TrySubmitCurrentDraw())
  {
    return;
  }

  g_gfx->DrawIndexed(base_index, num_indices, base_vertex);

  if (native_mode == MOHFrontline::NativeRender::Mode::Shadow)
    (void)MOHFrontline::NativeRender::TrySubmitCurrentDraw();
}

void VertexManagerBase::UploadUniforms()
{
}

void VertexManagerBase::InvalidateConstants()
{
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& pixel_shader_manager = system.GetPixelShaderManager();
  vertex_shader_manager.dirty = true;
  geometry_shader_manager.dirty = true;
  pixel_shader_manager.dirty = true;
}

void VertexManagerBase::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
}

void VertexManagerBase::UploadUtilityVertices(const void* vertices, u32 vertex_stride,
                                              u32 num_vertices, const u16* indices, u32 num_indices,
                                              u32* out_base_vertex, u32* out_base_index)
{
  // The GX vertex list should be flushed before any utility draws occur.
  ASSERT(m_is_flushed);

  // Copy into the buffers usually used for GX drawing.
  ResetBuffer(std::max(vertex_stride, 1u));
  if (vertices)
  {
    const u32 copy_size = vertex_stride * num_vertices;
    ASSERT((m_cur_buffer_pointer + copy_size) <= m_end_buffer_pointer);
    std::memcpy(m_cur_buffer_pointer, vertices, copy_size);
    m_cur_buffer_pointer += copy_size;
  }
  if (indices)
    m_index_generator.AddExternalIndices(indices, num_indices, num_vertices);

  CommitBuffer(num_vertices, vertex_stride, num_indices, out_base_vertex, out_base_index);
}

u32 VertexManagerBase::GetTexelBufferElementSize(TexelBufferFormat buffer_format)
{
  // R8 - 1, R16 - 2, RGBA8 - 4, R32G32 - 8
  return 1u << static_cast<u32>(buffer_format);
}

bool VertexManagerBase::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                          u32* out_offset)
{
  return false;
}

bool VertexManagerBase::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                          u32* out_offset, const void* palette_data,
                                          u32 palette_size, TexelBufferFormat palette_format,
                                          u32* palette_offset)
{
  return false;
}

BitSet32 VertexManagerBase::UsedTextures() const
{
  BitSet32 usedtextures;
  for (u32 i = 0; i < bpmem.genMode.numtevstages + 1u; ++i)
    if (bpmem.tevorders[i / 2].getEnable(i & 1))
      usedtextures[bpmem.tevorders[i / 2].getTexMap(i & 1)] = true;

  if (bpmem.genMode.numindstages > 0)
    for (unsigned int i = 0; i < bpmem.genMode.numtevstages + 1u; ++i)
      if (bpmem.tevind[i].IsActive() && bpmem.tevind[i].bt < bpmem.genMode.numindstages)
        usedtextures[bpmem.tevindref.getTexMap(bpmem.tevind[i].bt)] = true;

  return usedtextures;
}

void VertexManagerBase::Flush()
{
  if (m_is_flushed)
    return;

  m_is_flushed = true;

  if (m_draw_counter == 0)
  {
    // This is more or less the start of the Frame. Pump the native host audio
    // bridge here so decoding never runs on Dolphin's DVD worker thread.
    MOHFrontline::NativeAudio::Pump();
    GetVideoEvents().before_frame_event.Trigger();
  }

  if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens ||
      xfmem.numChan.numColorChans != bpmem.genMode.numcolchans)
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Mismatched configuration between XF and BP stages - {}/{} texgens, {}/{} colors. "
        "Skipping draw. Please report on the issue tracker.",
        xfmem.numTexGen.numTexGens, bpmem.genMode.numtexgens.Value(), xfmem.numChan.numColorChans,
        bpmem.genMode.numcolchans.Value());

    // Analytics reporting so we can discover which games have this problem, that way when we
    // eventually simulate the behavior we have test cases for it.
    if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens)
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUTexGensBetweenXFAndBP);
    }
    if (xfmem.numChan.numColorChans != bpmem.genMode.numcolchans)
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUColorsBetweenXFAndBP);
    }

    return;
  }

#if defined(_DEBUG) || defined(DEBUGFAST)
  PRIM_LOG("frame{}:\n texgen={}, numchan={}, dualtex={}, ztex={}, cole={}, alpe={}, ze={}",
           g_ActiveConfig.iSaveTargetId, xfmem.numTexGen.numTexGens, xfmem.numChan.numColorChans,
           xfmem.dualTexTrans.enabled, bpmem.ztex2.op.Value(), bpmem.blendmode.color_update.Value(),
           bpmem.blendmode.alpha_update.Value(), bpmem.zmode.update_enable.Value());

  for (u32 i = 0; i < xfmem.numChan.numColorChans; ++i)
  {
    LitChannel* ch = &xfmem.color[i];
    PRIM_LOG("colchan{}: matsrc={}, light={:#x}, ambsrc={}, diffunc={}, attfunc={}", i,
             ch->matsource.Value(), ch->GetFullLightMask(), ch->ambsource.Value(),
             ch->diffusefunc.Value(), ch->attnfunc.Value());
    ch = &xfmem.alpha[i];
    PRIM_LOG("alpchan{}: matsrc={}, light={:#x}, ambsrc={}, diffunc={}, attfunc={}", i,
             ch->matsource.Value(), ch->GetFullLightMask(), ch->ambsource.Value(),
             ch->diffusefunc.Value(), ch->attnfunc.Value());
  }

  for (u32 i = 0; i < xfmem.numTexGen.numTexGens; ++i)
  {
    TexMtxInfo tinfo = xfmem.texMtxInfo[i];
    if (tinfo.texgentype != TexGenType::EmbossMap)
      tinfo.hex &= 0x7ff;
    if (tinfo.texgentype != TexGenType::Regular)
      tinfo.projection = TexSize::ST;

    PRIM_LOG("txgen{}: proj={}, input={}, gentype={}, srcrow={}, embsrc={}, emblght={}, "
             "postmtx={}, postnorm={}",
             i, tinfo.projection.Value(), tinfo.inputform.Value(), tinfo.texgentype.Value(),
             tinfo.sourcerow.Value(), tinfo.embosssourceshift.Value(),
             tinfo.embosslightshift.Value(), xfmem.postMtxInfo[i].index.Value(),
             xfmem.postMtxInfo[i].normalize.Value());
  }

  PRIM_LOG("pixel: tev={}, ind={}, texgen={}, dstalpha={}, alphatest={:#x}",
           bpmem.genMode.numtevstages.Value() + 1, bpmem.genMode.numindstages.Value(),
           bpmem.genMode.numtexgens.Value(), bpmem.dstalpha.enable.Value(),
           (bpmem.alpha_test.hex >> 16) & 0xff);
#endif

  // Track some stats used elsewhere by the anamorphic widescreen heuristic.
  auto& system = Core::System::GetInstance();
  if (!system.IsWii())
  {
    const bool is_perspective = xfmem.projection.type == ProjectionType::Perspective;

    auto& counts =
        is_perspective ? m_flush_statistics.perspective : m_flush_statistics.orthographic;

    const auto& projection = xfmem.projection.rawProjection;
    // TODO: Potentially the viewport size could be used as weight for the flush count average.
    // This way a small minimap would have less effect than a fullscreen projection.
    const auto& viewport = xfmem.viewport;

    // FYI: This average is based on flushes.
    // It doesn't look at vertex counts like the heuristic does.
    counts.average_ratio.Push(CalculateProjectionViewportRatio(projection, viewport));

    if (IsAnamorphicProjection(projection, viewport, g_ActiveConfig))
    {
      ++counts.anamorphic_flush_count;
      counts.anamorphic_vertex_count += m_index_generator.GetIndexLen();
    }
    else if (IsNormalProjection(projection, viewport, g_ActiveConfig))
    {
      ++counts.normal_flush_count;
      counts.normal_vertex_count += m_index_generator.GetIndexLen();
    }
    else
    {
      ++counts.other_flush_count;
      counts.other_vertex_count += m_index_generator.GetIndexLen();
    }
  }

  auto& pixel_shader_manager = system.GetPixelShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& xf_state_manager = system.GetXFStateManager();

  if (g_ActiveConfig.bGraphicMods)
  {
    const double seconds_elapsed =
        static_cast<double>(m_ticks_elapsed) / system.GetSystemTimers().GetTicksPerSecond();
    pixel_shader_manager.constants.time_ms = seconds_elapsed * 1000;
  }

  CalculateNormals(VertexLoaderManager::GetCurrentVertexFormat());
  // Calculate ZSlope for zfreeze
  const auto used_textures = UsedTextures();
  std::vector<std::string> texture_names;
  Common::SmallVector<u32, 8> texture_units;
  std::array<SamplerState, 8> samplers;
  if (!m_cull_all)
  {
    if (!g_ActiveConfig.bGraphicMods)
    {
      for (const u32 i : used_textures)
      {
        const auto cache_entry = g_texture_cache->Load(i);
        if (!cache_entry)
          continue;
        const float custom_tex_scale = cache_entry->GetWidth() / float(cache_entry->native_width);
        samplers[i] = TextureCacheBase::GetSamplerState(
            i, custom_tex_scale, cache_entry->is_custom_tex, cache_entry->has_arbitrary_mips);
      }
    }
    else
    {
      for (const u32 i : used_textures)
      {
        const auto cache_entry = g_texture_cache->Load(i);
        if (cache_entry)
        {
          if (!Common::Contains(texture_names, cache_entry->texture_info_name))
          {
            texture_names.push_back(cache_entry->texture_info_name);
            texture_units.push_back(i);
          }

          const float custom_tex_scale = cache_entry->GetWidth() / float(cache_entry->native_width);
          samplers[i] = TextureCacheBase::GetSamplerState(
              i, custom_tex_scale, cache_entry->is_custom_tex, cache_entry->has_arbitrary_mips);
        }
      }
    }
  }
  vertex_shader_manager.SetConstants(texture_names, xf_state_manager);
  if (!bpmem.genMode.zfreeze)
  {
    // Must be done after VertexShaderManager::SetConstants()
    CalculateZSlope(VertexLoaderManager::GetCurrentVertexFormat());
  }
  else if (m_zslope.dirty && !m_cull_all)  // or apply any dirty ZSlopes
  {
    pixel_shader_manager.SetZSlope(m_zslope.dfdx, m_zslope.dfdy, m_zslope.f0);
    m_zslope.dirty = false;
  }

  if (!m_cull_all)
  {
    CustomPixelShaderContents custom_pixel_shader_contents;
    std::optional<CustomPixelShader> custom_pixel_shader;
    std::vector<std::string> custom_pixel_texture_names;
    std::span<u8> custom_pixel_shader_uniforms;
    bool skip = false;
    for (size_t i = 0; i < texture_names.size(); i++)
    {
      GraphicsModActionData::DrawStarted draw_started{texture_units, &skip, &custom_pixel_shader,
                                                      &custom_pixel_shader_uniforms};
      for (const auto& action : g_graphics_mod_manager->GetDrawStartedActions(texture_names[i]))
      {
        action->OnDrawStarted(&draw_started);
        if (custom_pixel_shader)
        {
          custom_pixel_shader_contents.shaders.push_back(*custom_pixel_shader);
          custom_pixel_texture_names.push_back(texture_names[i]);
        }
        custom_pixel_shader = std::nullopt;
      }
    }

    // Now the vertices can be flushed to the GPU. Everything following the CommitBuffer() call
    // must be careful to not upload any utility vertices, as the binding will be lost otherwise.
    const u32 num_indices = m_index_generator.GetIndexLen();
    if (num_indices == 0)
      return;

    // Texture loading can cause palettes to be applied (-> uniforms -> draws).
    // Palette application does not use vertices, only a full-screen quad, so this is okay.
    // Same with GPU texture decoding, which uses compute shaders.
    g_texture_cache->BindTextures(used_textures, samplers);

    if (!skip)
    {
      UpdatePipelineConfig();
      UpdatePipelineObject();
      if (m_current_pipeline_object)
      {
        const AbstractPipeline* pipeline_object = m_current_pipeline_object;
        if (!custom_pixel_shader_contents.shaders.empty())
        {
          if (const auto custom_pipeline =
                  GetCustomPipeline(custom_pixel_shader_contents, m_current_pipeline_config,
                                    m_current_uber_pipeline_config, m_current_pipeline_object))
          {
            pipeline_object = custom_pipeline;
          }
        }
        RenderDrawCall(pixel_shader_manager, geometry_shader_manager, custom_pixel_shader_contents,
                       custom_pixel_shader_uniforms, m_current_primitive_type, pipeline_object);
      }
    }

    // Even if we skip the draw, emulated state should still be impacted
    OnDraw();

    // The EFB cache is now potentially stale.
    g_framebuffer_manager->FlagPeekCacheAsOutOfDate();
  }

  if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens)
  {
    ERROR_LOG_FMT(VIDEO,
                  "xf.numtexgens ({}) does not match bp.numtexgens ({}). Error in command stream.",
                  xfmem.numTexGen.numTexGens, bpmem.genMode.numtexgens.Value());
  }
}

void VertexManagerBase::DoState(PointerWrap& p)
{
  if (p.IsReadMode())
  {
    // Flush old vertex data before loading state.
    Flush();
  }

  p.Do(m_zslope);
  p.Do(VertexLoaderManager::normal_cache);
  p.Do(VertexLoaderManager::tangent_cache);
  p.Do(VertexLoaderManager::binormal_cache);
}

void VertexManagerBase::CalculateZSlope(NativeVertexFormat* format)
{
  float out[12];
  float viewOffset[2] = {xfmem.viewport.xOrig - bpmem.scissorOffset.x * 2,
                         xfmem.viewport.yOrig - bpmem.scissorOffset.y * 2};

  if (m_current_primitive_type != PrimitiveType::Triangles &&
      m_current_primitive_type != PrimitiveType::TriangleStrip)
  {
    return;
  }

  // Global matrix ID.
  u32 mtxIdx = g_main_cp_state.matrix_index_a.PosNormalMtxIdx;
  const PortableVertexDeclaration vert_decl = format->GetVertexDeclaration();

  // Make sure the buffer contains at least 3 vertices.
  if ((m_cur_buffer_pointer - m_base_buffer_pointer) < (vert_decl.stride * 3))
    return;

  // Lookup vertices of the last rendered triangle and software-transform them
  // This allows us to determine the depth slope, which will be used if z-freeze
  // is enabled in the following flush.
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  for (unsigned int i = 0; i < 3; ++i)
  {
    // If this vertex format has per-vertex position matrix IDs, look it up.
    if (vert_decl.posmtx.enable)
      mtxIdx = VertexLoaderManager::position_matrix_index_cache[2 - i];

    if (vert_decl.position.components == 2)
      VertexLoaderManager::position_cache[2 - i][2] = 0;

    vertex_shader_manager.TransformToClipSpace(&VertexLoaderManager::position_cache[2 - i][0],
                                               &out[i * 4], mtxIdx);

    // Transform to Screenspace
    float inv_w = 1.0f / out[3 + i * 4];

    out[0 + i * 4] = out[0 + i * 4] * inv_w * xfmem.viewport.wd + viewOffset[0];
    out[1 + i * 4] = out[1 + i * 4] * inv_w * xfmem.viewport.ht + viewOffset[1];
    out[2 + i * 4] = out[2 + i * 4] * inv_w * xfmem.viewport.zRange + xfmem.viewport.farZ;
  }

  float dx31 = out[8] - out[0];
  float dx12 = out[0] - out[4];
  float dy12 = out[1] - out[5];
  float dy31 = out[9] - out[1];

  float DF31 = out[10] - out[2];
  float DF21 = out[6] - out[2];
  float a = DF31 * -dy12 - DF21 * dy31;
  float b = dx31 * DF21 + dx12 * DF31;
  float c = -dx12 * dy31 - dx31 * -dy12;

  // Sometimes we process de-generate triangles. Stop any divide by zeros
  if (c == 0)
    return;

  m_zslope.dfdx = -a / c;
  m_zslope.dfdy = -b / c;
  m_zslope.f0 = out[2] - (out[0] * m_zslope.dfdx + out[1] * m_zslope.dfdy);
  m_zslope.dirty = true;
}

void VertexManagerBase::CalculateNormals(NativeVertexFormat* format)
{
  const PortableVertexDeclaration vert_decl = format->GetVertexDeclaration();

  // Only update the binormal/tangent vertex shader constants if the vertex format lacks binormals
  // (VertexLoaderManager::binormal_cache gets updated by the vertex loader when binormals are
  // present, though)
  if (vert_decl.normals[1].enable)
    return;

  VertexLoaderManager::tangent_cache[3] = 0;
  VertexLoaderManager::binormal_cache[3] = 0;

  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  if (vertex_shader_manager.constants.cached_tangent != VertexLoaderManager::tangent_cache)
  {
    vertex_shader_manager.constants.cached_tangent = VertexLoaderManager::tangent_cache;
    vertex_shader_manager.dirty = true;
  }
  if (vertex_shader_manager.constants.cached_binormal != VertexLoaderManager::binormal_cache)
  {
    vertex_shader_manager.constants.cached_binormal = VertexLoaderManager::binormal_cache;
    vertex_shader_manager.dirty = true;
  }

  if (vert_decl.normals[0].enable)
    return;

  VertexLoaderManager::normal_cache[3] = 0;
  if (vertex_shader_manager.constants.cached_normal != VertexLoaderManager::normal_cache)
  {
    vertex_shader_manager.constants.cached_normal = VertexLoaderManager::normal_cache;
    vertex_shader_manager.dirty = true;
  }
}

void VertexManagerBase::UpdatePipelineConfig()
{
  NativeVertexFormat* vertex_format = VertexLoaderManager::GetCurrentVertexFormat();
  if (vertex_format != m_current_pipeline_config.vertex_format)
  {
    m_current_pipeline_config.vertex_format = vertex_format;
    m_current_uber_pipeline_config.vertex_format =
        VertexLoaderManager::GetUberVertexFormat(vertex_format->GetVertexDeclaration());
    m_pipeline_config_changed = true;
  }

  VertexShaderUid vs_uid = GetVertexShaderUid();
  if (vs_uid != m_current_pipeline_config.vs_uid)
  {
    m_current_pipeline_config.vs_uid = vs_uid;
    m_current_uber_pipeline_config.vs_uid = UberShader::GetVertexShaderUid();
    m_pipeline_config_changed = true;
  }

  PixelShaderUid ps_uid = GetPixelShaderUid();
  if (ps_uid != m_current_pipeline_config.ps_uid)
  {
    m_current_pipeline_config.ps_uid = ps_uid;
    m_current_uber_pipeline_config.ps_uid = UberShader::GetPixelShaderUid();
    m_pipeline_config_changed = true;
  }

  GeometryShaderUid gs_uid = GetGeometryShaderUid(GetCurrentPrimitiveType());
  if (gs_uid != m_current_pipeline_config.gs_uid)
  {
    m_current_pipeline_config.gs_uid = gs_uid;
    m_current_uber_pipeline_config.gs_uid = gs_uid;
    m_pipeline_config_changed = true;
  }

  if (m_rasterization_state_changed)
  {
    m_rasterization_state_changed = false;

    RasterizationState new_rs = {};
    new_rs.Generate(bpmem, m_current_primitive_type);
    if (new_rs != m_current_pipeline_config.rasterization_state)
    {
      m_current_pipeline_config.rasterization_state = new_rs;
      m_current_uber_pipeline_config.rasterization_state = new_rs;
      m_pipeline_config_changed = true;
    }
  }

  if (m_depth_state_changed)
  {
    m_depth_state_changed = false;

    DepthState new_ds = {};
    new_ds.Generate(bpmem);
    if (new_ds != m_current_pipeline_config.depth_state)
    {
      m_current_pipeline_config.depth_state = new_ds;
      m_current_uber_pipeline_config.depth_state = new_ds;
      m_pipeline_config_changed = true;
    }
  }

  if (m_blending_state_changed)
  {
    m_blending_state_changed = false;

    BlendingState new_bs = {};
    new_bs.Generate(bpmem);
    if (new_bs != m_current_pipeline_config.blending_state)
    {
      m_current_pipeline_config.blending_state = new_bs;
      m_current_uber_pipeline_config.blending_state = new_bs;
      m_pipeline_config_changed = true;
    }
  }
}

void VertexManagerBase::UpdatePipelineObject()
{
  if (!m_pipeline_config_changed)
    return;

  m_current_pipeline_object = nullptr;
  m_pipeline_config_changed = false;

  switch (g_ActiveConfig.iShaderCompilationMode)
  {
  case ShaderCompilationMode::Synchronous:
  {
    // Ubershaders disabled? Block and compile the specialized shader.
    m_current_pipeline_object = g_shader_cache->GetPipelineForUid(m_current_pipeline_config);
  }
  break;

  case ShaderCompilationMode::SynchronousUberShaders:
  {
    // Exclusive ubershader mode, always use ubershaders.
    m_current_pipeline_object =
        g_shader_cache->GetUberPipelineForUid(m_current_uber_pipeline_config);
  }
  break;

  case ShaderCompilationMode::AsynchronousUberShaders:
  case ShaderCompilationMode::AsynchronousSkipRendering:
  {
    // Can we background compile shaders? If so, get the pipeline asynchronously.
    auto res = g_shader_cache->GetPipelineForUidAsync(m_current_pipeline_config);
    if (res)
    {
      // Specialized shaders are ready, prefer these.
      m_current_pipeline_object = *res;
      return;
    }

    if (g_ActiveConfig.iShaderCompilationMode == ShaderCompilationMode::AsynchronousUberShaders)
    {
      // Specialized shaders not ready, use the ubershaders.
      m_current_pipeline_object =
          g_shader_cache->GetUberPipelineForUid(m_current_uber_pipeline_config);
    }
    else
    {
      // Ensure we try again next draw. Otherwise, if no registers change between frames, the
      // object will never be drawn, even when the shader is ready.
      m_pipeline_config_changed = true;
    }
  }
  break;
  }
}

void VertexManagerBase::OnConfigChange()
{
  // Reload index generator function tables in case VS expand config changed
  m_index_generator.Init();
  if (m_moh_csm)
    m_moh_csm->pipelines.clear();
}

void VertexManagerBase::OnDraw()
{
  m_draw_counter++;

  // If the last efb copy was too close to the one before it, don't forget about it until the next
  // efb copy happens (which might not be for a long time)
  u32 diff = m_draw_counter - m_last_efb_copy_draw_counter;
  if (m_unflushed_efb_copy && diff > MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
  {
    g_gfx->Flush();
    m_unflushed_efb_copy = false;
    m_last_efb_copy_draw_counter = m_draw_counter;
  }

  // If we didn't have any CPU access last frame, do nothing.
  if (m_scheduled_command_buffer_kicks.empty() || !m_allow_background_execution)
    return;

  // Check if this draw is scheduled to kick a command buffer.
  // The draw counters will always be sorted so a binary search is possible here.
  if (std::ranges::binary_search(m_scheduled_command_buffer_kicks, m_draw_counter))
  {
    // Kick a command buffer on the background thread.
    g_gfx->Flush();
    m_unflushed_efb_copy = false;
    m_last_efb_copy_draw_counter = m_draw_counter;
  }
}

void VertexManagerBase::OnCPUEFBAccess()
{
  // Check this isn't another access without any draws in between.
  if (!m_cpu_accesses_this_frame.empty() && m_cpu_accesses_this_frame.back() == m_draw_counter)
    return;

  // Store the current draw counter for scheduling in OnEndFrame.
  m_cpu_accesses_this_frame.emplace_back(m_draw_counter);
}

void VertexManagerBase::OnEFBCopyToRAM()
{
  // If we're not deferring, try to preempt it next frame.
  if (!g_ActiveConfig.bDeferEFBCopies)
  {
    OnCPUEFBAccess();
    return;
  }

  // Otherwise, only execute if we have at least 10 objects between us and the last copy.
  const u32 diff = m_draw_counter - m_last_efb_copy_draw_counter;
  m_last_efb_copy_draw_counter = m_draw_counter;
  if (diff < MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
  {
    m_unflushed_efb_copy = true;
    return;
  }

  m_unflushed_efb_copy = false;
  g_gfx->Flush();
}

void VertexManagerBase::OnEndFrame()
{
  m_draw_counter = 0;
  m_last_efb_copy_draw_counter = 0;
  m_scheduled_command_buffer_kicks.clear();

  if (m_moh_csm)
  {
    // Publish the just-rendered set for FINAL-XFB, then rotate the caster to the
    // other set. The next frame can clear/render freely without touching the
    // shadow maps which Presenter is still sampling.
    MOHCSMState& csm = *m_moh_csm;
    if (csm.frame_started && csm.wrote[csm.render_set])
    {
      csm.sample_set = csm.render_set;
      csm.sample_valid = true;
      csm.render_set = 1u - csm.render_set;
      if (!csm.logged_double_buffer)
      {
        std::fprintf(stderr,
                     "[moh-ps3-csm] DOUBLE BUFFER active: publish set=%u, next render set=%u\n",
                     csm.sample_set, csm.render_set);
        csm.logged_double_buffer = true;
      }
    }
    csm.frame_started = false;
  }

  // If we have no CPU access at all, leave everything in the one command buffer for maximum
  // parallelism between CPU/GPU, at the cost of slightly higher latency.
  if (m_cpu_accesses_this_frame.empty())
    return;

  // In order to reduce CPU readback latency, we want to kick a command buffer roughly halfway
  // between the draw counters that invoked the readback, or every 250 draws, whichever is
  // smaller.
  if (g_ActiveConfig.iCommandBufferExecuteInterval > 0)
  {
    u32 last_draw_counter = 0;
    u32 interval = static_cast<u32>(g_ActiveConfig.iCommandBufferExecuteInterval);
    for (u32 draw_counter : m_cpu_accesses_this_frame)
    {
      // We don't want to waste executing command buffers for only a few draws, so set a minimum.
      // Leave last_draw_counter as-is, so we get the correct number of draws between submissions.
      u32 draw_count = draw_counter - last_draw_counter;
      if (draw_count < MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
        continue;

      if (draw_count <= interval)
      {
        u32 mid_point = draw_count / 2;
        m_scheduled_command_buffer_kicks.emplace_back(last_draw_counter + mid_point);
      }
      else
      {
        u32 counter = interval;
        while (counter < draw_count)
        {
          m_scheduled_command_buffer_kicks.emplace_back(last_draw_counter + counter);
          counter += interval;
        }
      }

      last_draw_counter = draw_counter;
    }
  }

  m_cpu_accesses_this_frame.clear();

  // We invalidate the pipeline object at the start of the frame.
  // This is for the rare case where only a single pipeline configuration is used,
  // and hybrid ubershaders have compiled the specialized shader, but without any
  // state changes the specialized shader will not take over.
  InvalidatePipelineObject();
}

void VertexManagerBase::NotifyCustomShaderCacheOfHostChange(const ShaderHostConfig& host_config)
{
  m_custom_shader_cache->SetHostConfig(host_config);
  m_custom_shader_cache->Reload();
}

const AbstractTexture* VertexManagerBase::GetMOHCSMTexture(u32 cascade) const
{
  if (!m_moh_csm || cascade >= MOH_CSM_CASCADES || !m_moh_csm->resources_ready ||
      !m_moh_csm->sample_valid)
  {
    return nullptr;
  }
  return m_moh_csm->depth_textures[m_moh_csm->sample_set]
      [std::min(cascade, GetMohCSMQuality().cascades - 1)].get();
}

bool VertexManagerBase::PrepareMOHCSMForSampling(MOHCSMReceiverData* out_data)
{
  if (!m_moh_csm || !m_moh_csm->resources_ready || !m_moh_csm->sample_valid)
  {
    if (out_data)
      *out_data = {};
    return false;
  }

  MOHCSMState& csm = *m_moh_csm;
  const u32 set = csm.sample_set;
  if (!csm.wrote[set])
  {
    if (out_data)
      *out_data = {};
    return false;
  }

  if (!csm.finished_for_sampling[set])
  {
    for (auto& depth : csm.depth_textures[set])
      if (depth) depth->FinishedRendering();
    csm.finished_for_sampling[set] = true;
  }

  MOHCSMReceiverData receiver = csm.receivers[set];
  receiver.flags[0] = 1;
  // Debug 1..4 displays the raw corresponding shadow-map depth full-screen.
  // Debug 5 displays the final computed CSM shadow mask.
  // Debug 6/7 diagnose corrected/opposite scene-depth orientation; debug 8
  // keeps the corrected depth and tests the opposite shadow-compare polarity.
  // Zero is normal mode.
  receiver.flags[3] =
      std::clamp(static_cast<s32>(MohEnvFloat("MOH_PS3_CSM_DEBUG", 0.0f)), 0, 8);
  if (out_data)
    *out_data = receiver;

  static bool logged_receiver_ready = false;
  if (!logged_receiver_ready)
  {
    std::fprintf(stderr,
                 "[moh-ps3-csm] FINAL-XFB receiver READY: set=%u preserved %llu caster batches\n",
                 set, static_cast<unsigned long long>(csm.caster_batches[set]));
    logged_receiver_ready = true;
  }

  return true;
}

void VertexManagerBase::RenderMOHCSMCasters(VertexShaderManager& vertex_shader_manager,
                                             u32 base_index, u32 num_indices, u32 base_vertex,
                                             PrimitiveType primitive_type,
                                             const AbstractPipeline* current_pipeline,
                                             bool ps3_static_replacement)
{
  // Host CSM is deliberately disabled for MOH. Bail out before reading any
  // of the old diagnostic environment switches on every draw.
  if (!MohCSMEnabled())
    return;

  const bool camera_projection_test = MohEnvSwitch("MOH_PS3_CSM_CAMERA_TEST", false);
  // v2.7: WORLD_PROBE is intentionally independent from CAMERA_TEST.
  // This lets us replay the exact same world-only draw set through either:
  //   CAMERA_TEST=1 -> proven-good guest camera projection, or
  //   CAMERA_TEST=0 -> the real light-space CSM matrix.
  // Comparing the two is the decisive matrix-vs-overwrite test.
  const bool world_probe = MohEnvSwitch("MOH_PS3_CSM_WORLD_PROBE", false);
  const u32 world_probe_min_indices = static_cast<u32>(std::clamp(
      MohEnvFloat("MOH_PS3_CSM_WORLD_PROBE_MIN_INDICES", 12.0f), 3.0f, 1000000.0f));
  const bool triangle_primitive = primitive_type == PrimitiveType::Triangles ||
                                  primitive_type == PrimitiveType::TriangleStrip;
  const auto& guest_pipeline_cfg = current_pipeline ? current_pipeline->m_config : AbstractPipelineConfig{};
  const bool world_signature =
      current_pipeline && guest_pipeline_cfg.depth_state.hex == 0x0000000fu &&
      guest_pipeline_cfg.rasterization_state.hex == 0x00000018u &&
      guest_pipeline_cfg.framebuffer_state.hex == 0x00010c00u &&
      guest_pipeline_cfg.blending_state.hex == 0x00004889u;

  // v2.1 diagnostic: the previous caster rejected every draw whose XF projection
  // type was not tagged Perspective.  The v2.0 camera test showing ONLY the
  // weapon while the whole world stayed at the 0.25 sentinel is a strong sign
  // that MOH's world batches are not passing that tag even though they are real
  // 3D scene geometry.  In CAMERA_TEST we deliberately replay every triangle
  // batch with its exact guest projection so we can prove/disprove that filter.
  if (!current_pipeline || !triangle_primitive || num_indices == 0 ||
      (!camera_projection_test && xfmem.projection.type != ProjectionType::Perspective) ||
      (world_probe && (xfmem.projection.type != ProjectionType::Perspective ||
                       num_indices < world_probe_min_indices || !world_signature)))
  {
    return;
  }

  if (camera_projection_test && xfmem.projection.type != ProjectionType::Perspective)
  {
    static bool logged_non_perspective_world_candidate = false;
    if (!logged_non_perspective_world_candidate)
    {
      std::fprintf(stderr,
                   "[moh-ps3-csm] CAMERA TEST: accepting NON-PERSPECTIVE triangle batches "
                   "(old caster filter bypassed)\n");
      logged_non_perspective_world_candidate = true;
    }
  }

  if (!m_moh_csm)
    m_moh_csm = std::make_unique<MOHCSMState>();
  MOHCSMState& csm = *m_moh_csm;

  // v2.6 world-signature isolation.  v2.5 selected the first large perspective
  // batch, which can simply be a sky/viewmodel batch and therefore tells us
  // nothing about world rasterization.  The v2.4 gameplay trace exposed a very
  // stable world pipeline signature on MOH Frontline:
  //   depth=0000000f rast=00000018 fb=00010c00 blend=00004889
  // In WORLD_PROBE mode replay only those perspective draws.  This excludes the
  // shell/HUD/sky/viewmodel noise while keeping many actual world batches, so
  // one useless first draw can no longer poison the experiment.
  if (world_probe)
  {
    static bool logged_world_probe = false;
    static u32 logged_world_batches = 0;
    if (!logged_world_probe)
    {
      std::fprintf(stderr,
                   "[moh-ps3-csm] WORLD SIGNATURE PROBE active: "
                   "depth=0000000f rast=00000018 fb=00010c00 blend=00004889 "
                   "min_indices=%u projection=%s\n",
                   world_probe_min_indices, camera_projection_test ? "CAMERA" : "LIGHT-SPACE");
      logged_world_probe = true;
    }
    if (logged_world_batches < 12)
    {
      std::fprintf(stderr,
                   "[moh-ps3-csm] WORLD SIGNATURE accepted: idx=%u baseI=%u baseV=%u\n",
                   num_indices, base_index, base_vertex);
      ++logged_world_batches;
    }
  }

  // v2.4 diagnostic: the v2.3 trace was consumed entirely by shell/HUD
  // orthographic batches before gameplay started.  Only begin once a real
  // perspective batch with enough geometry appears, and keep logging only
  // perspective batches.  This captures sky/world/viewmodel state instead of
  // spending the trace budget on menu quads.
  if (MohEnvSwitch("MOH_PS3_CSM_TRACE", false))
  {
    static bool trace_started = false;
    static bool trace_done = false;
    static u32 trace_count = 0;
    constexpr u32 TRACE_LIMIT = 2400;

    const bool perspective = xfmem.projection.type == ProjectionType::Perspective;
    const bool start_candidate = perspective && num_indices >= 12;

    if (!trace_done && (!trace_started ? start_candidate : perspective))
    {
      if (!trace_started)
      {
        trace_started = true;
        std::fprintf(stderr,
                     "[moh-ps3-csm-trace] BEGIN gameplay perspective trace "
                     "(menu/HUD skipped, max %u batches)\n",
                     TRACE_LIMIT);
      }

      if (trace_count < TRACE_LIMIT)
      {
        const auto& p = vertex_shader_manager.constants.projection;
        const auto& pc = vertex_shader_manager.constants.pixelcentercorrection;
        const auto& cfg = current_pipeline->m_config;
        std::fprintf(
            stderr,
            "[moh-ps3-csm-trace] #%04u idx=%u baseI=%u baseV=%u prim=%u projType=%u "
            "P=(%.6f %.6f %.6f %.6f %.8f %.8f) "
            "VP=(wd=%.3f ht=%.3f zRange=%.3f farZ=%.3f x=%.3f y=%.3f) "
            "PC=(%.9f %.9f %.7f %.7f) PIPE=(depth=%08x rast=%08x fb=%08x blend=%08x)\n",
            trace_count, num_indices, base_index, base_vertex,
            static_cast<unsigned>(primitive_type),
            static_cast<unsigned>(xfmem.projection.type),
            p[0][0], p[0][2], p[1][1], p[1][2], p[2][2], p[2][3],
            xfmem.viewport.wd, xfmem.viewport.ht, xfmem.viewport.zRange,
            xfmem.viewport.farZ, xfmem.viewport.xOrig, xfmem.viewport.yOrig,
            pc[0], pc[1], pc[2], pc[3], cfg.depth_state.hex,
            cfg.rasterization_state.hex, cfg.framebuffer_state.hex, cfg.blending_state.hex);
        ++trace_count;
      }
      else
      {
        std::fprintf(stderr,
                     "[moh-ps3-csm-trace] END perspective trace limit reached (%u batches)\n",
                     TRACE_LIMIT);
        trace_done = true;
      }
    }
  }

  if (!csm.resources_ready)
  {
    const TextureConfig depth_config(GetMohCSMQuality().resolution, GetMohCSMQuality().resolution, 1, 1, 1,
                                     AbstractTextureFormat::D32F,
                                     AbstractTextureFlag_RenderTarget,
                                     AbstractTextureType::Texture_2DArray);
    for (u32 set = 0; set < MOHCSMState::SET_COUNT; ++set)
    {
      for (u32 i = 0; i < GetMohCSMQuality().cascades; ++i)
      {
        csm.depth_textures[set][i] =
            g_gfx->CreateTexture(depth_config, "MOH Frontline PS3 CSM depth");
        if (!csm.depth_textures[set][i])
        {
          std::fprintf(stderr,
                       "[moh-ps3-csm] failed to create set %u cascade %u depth texture\n",
                       set, i);
          return;
        }
        csm.framebuffers[set][i] =
            g_gfx->CreateFramebuffer(nullptr, csm.depth_textures[set][i].get());
        if (!csm.framebuffers[set][i])
        {
          std::fprintf(stderr,
                       "[moh-ps3-csm] failed to create set %u cascade %u framebuffer\n",
                       set, i);
          return;
        }
      }
    }
    csm.resources_ready = true;
  }

  if (!csm.frame_started)
  {
    MOHCSMReceiverData& receiver = csm.receivers[csm.render_set];
    const auto& p = vertex_shader_manager.constants.projection;
    const float p0 = p[0][0];
    const float p2 = p[0][2];
    const float p5 = p[1][1];
    const float p6 = p[1][2];
    const float p10 = p[2][2];
    const float p11 = p[2][3];

    if (!std::isfinite(p0) || !std::isfinite(p5) || std::fabs(p0) < 1.0e-5f ||
        std::fabs(p5) < 1.0e-5f)
      return;

    float camera_near = std::fabs(p11 / (p10 - 1.0f));
    float camera_far = std::fabs(p11 / p10);
    if (!std::isfinite(camera_near) || camera_near < 0.001f || camera_near > 100.0f)
      camera_near = 0.25f;
    if (!std::isfinite(camera_far) || camera_far <= camera_near + 1.0f)
      camera_far = 1800.0f;

    const float requested_far = MohEnvFloat("MOH_PS3_CSM_FAR", camera_far);
    camera_far = std::clamp(requested_far, camera_near + 8.0f, 8192.0f);
    const float lambda = std::clamp(MohEnvFloat("MOH_PS3_CSM_LAMBDA", 0.65f), 0.0f, 1.0f);

    std::array<float, MOH_CSM_CASCADES> splits{};
    for (u32 i = 0; i < GetMohCSMQuality().cascades; ++i)
    {
      const float t = static_cast<float>(i + 1) / static_cast<float>(GetMohCSMQuality().cascades);
      const float logarithmic = camera_near * std::pow(camera_far / camera_near, t);
      const float uniform = camera_near + (camera_far - camera_near) * t;
      splits[i] = logarithmic * lambda + uniform * (1.0f - lambda);
    }

    bool from_gx = false;
    const MohVec3 sun = FindMohSunDirection(vertex_shader_manager.constants, &from_gx);
    if (!from_gx && !csm.logged_fallback_sun)
    {
      std::fprintf(stderr,
                   "[moh-ps3-csm] GX sun direction not ready; using one-frame fallback direction\n");
      csm.logged_fallback_sun = true;
    }

    float split_near = camera_near;
    for (u32 i = 0; i < GetMohCSMQuality().cascades; ++i)
    {
      const auto matrix = BuildCascadeMatrix(p0, p2, p5, p6, split_near, splits[i], sun);
      for (u32 row = 0; row < 4; ++row)
      {
        for (u32 col = 0; col < 4; ++col)
          receiver.matrix_rows[i * 4 + row][col] = matrix[row * 4 + col];
      }
      receiver.splits[i] = splits[i];
      split_near = splits[i];
    }

    receiver.camera0 = {p0, p2, p5, p6};
    receiver.camera1 = {
        p10, p11,
        std::clamp(MohEnvFloat("MOH_PS3_CSM_BIAS", 0.0012f), 0.0f, 0.02f),
        0.000830078f};  // exact 4-tap receiver footprint recovered from the RSX capture
    // flags.z bitfield:
    //   bit0 = shadow-map Y flip
    //   bit1 = manual scene-depth flip AFTER v2.9 automatic CSM correction
    //   bit2 = diagnostic receiver compare-polarity flip
    const s32 receiver_transform_flags =
        (MohEnvSwitch("MOH_PS3_CSM_FLIP_Y", false) ? 1 : 0) |
        (MohEnvSwitch("MOH_PS3_CSM_DEPTH_FLIP", false) ? 2 : 0) |
        (MohEnvSwitch("MOH_PS3_CSM_COMPARE_FLIP", false) ? 4 : 0);
    receiver.flags = {0, static_cast<s32>(GetMohCSMQuality().cascades), receiver_transform_flags, 0};

    const s32 debug_mode =
        std::clamp(static_cast<s32>(MohEnvFloat("MOH_PS3_CSM_DEBUG", 0.0f)), 0, 5);
    const bool debug_depth_view = debug_mode >= 1 && debug_mode <= 4;
    // During raw-depth diagnostics use a non-zero sentinel clear.  This lets the
    // fullscreen debug shader distinguish three cases unambiguously:
    //   0.0  -> sampler/binding returned zero,
    //   0.25 -> texture is readable but no caster touched the texel,
    //   other -> real rasterized caster depth.
    const float clear_depth = debug_depth_view ? 0.25f : 0.0f;
    for (u32 i = 0; i < GetMohCSMQuality().cascades; ++i)
    {
      g_gfx->SetAndClearFramebuffer(csm.framebuffers[csm.render_set][i].get(), {}, clear_depth);
      g_gfx->SetViewportAndScissor(csm.depth_textures[csm.render_set][i]->GetRect(), 0.0f, 1.0f);
    }
    if (debug_depth_view)
      std::fprintf(stderr, "[moh-ps3-csm] DEBUG sentinel clear: D32F=0.25\n");
    g_gfx->EndUtilityDrawing();

    // A new caster frame starts here.  Only now invalidate the previous
    // presentation snapshot; Presenter has already had a chance to sample it.
    csm.frame_started = true;
    csm.wrote[csm.render_set] = false;
    csm.finished_for_sampling[csm.render_set] = false;
    receiver.flags[0] = 0;
    csm.caster_batches[csm.render_set] = 0;

    if (!csm.logged_active)
    {
      std::fprintf(stderr,
                   "[moh-ps3-csm] TRUE caster ON: %ux%u D32F | sun=(%.3f %.3f %.3f) "
                   "splits=(%.1f %.1f %.1f %.1f) bias=%.6f%s\n",
                   GetMohCSMQuality().cascades, GetMohCSMQuality().resolution,
                   sun.x, sun.y, sun.z, splits[0], splits[1], splits[2], splits[3],
                   receiver.camera1[2], from_gx ? " [GX view light]" : " [fallback]");
      csm.logged_active = true;
    }
  }

  // Never reuse the guest GX pixel shader for a shadow caster.  A large
  // subset of Dolphin GX pixel shaders explicitly writes gl_FragDepth using
  // the *camera/EFB* depth equation.  When such a shader is replayed through
  // the light-space vertex projection it can overwrite the D32F map with 0,
  // which is exactly the red-after-one-blue-frame failure seen by DEBUG=1.
  // The shadow pass only needs fixed-function depth from gl_Position.
  if (!csm.depth_only_pixel_shader)
  {
    constexpr std::string_view depth_only_ps = R"(
void main()
{
}
)";
    csm.depth_only_pixel_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Pixel, depth_only_ps, nullptr,
        "MOH Frontline true CSM depth-only pixel shader");
    if (!csm.depth_only_pixel_shader)
    {
      std::fprintf(stderr,
                   "[moh-ps3-csm] ERROR: failed to compile depth-only caster pixel shader\n");
      return;
    }
    std::fprintf(stderr,
                 "[moh-ps3-csm] depth-only caster shader READY: guest gl_FragDepth disabled\n");
  }

  auto pipeline_it = csm.pipelines.find(current_pipeline);
  if (pipeline_it == csm.pipelines.end())
  {
    AbstractPipelineConfig config = current_pipeline->m_config;
    config.pixel_shader = csm.depth_only_pixel_shader.get();
    config.blending_state = RenderState::GetNoColorWriteBlendState();

    // Shadow casters must not inherit the camera-facing cull mode.  From the
    // sun, a surface which was front-facing to the player can be back-facing;
    // inheriting the guest cull state can therefore remove valid casters.
    config.rasterization_state = RenderState::GetNoCullRasterizationState(primitive_type);

    config.depth_state.hex = 0;
    config.depth_state.test_enable = true;
    config.depth_state.update_enable = true;
    // Normal mode keeps reverse-Z nearest-depth selection.  FORCE_WRITE is a
    // diagnostic switch: if it makes geometry appear in DEBUG=1..4, the
    // remaining bug is depth convention/compare rather than rasterization.
    const s32 debug_mode =
        std::clamp(static_cast<s32>(MohEnvFloat("MOH_PS3_CSM_DEBUG", 0.0f)), 0, 5);
    const bool force_write = MohEnvSwitch("MOH_PS3_CSM_FORCE_WRITE", false) ||
                             (debug_mode >= 1 && debug_mode <= 4);

    // The CSM projection is authored directly in HOST reverse-Z space: the
    // closest caster produces the largest D32F value and the map is cleared to
    // 0.  We therefore need the physical host test to be GREATER_OR_EQUAL.
    //
    // Dolphin normally swaps Less/Greater when the backend cannot expose a
    // reversed depth range.  That swap is correct for ordinary GX rendering,
    // but it double-inverts this custom host-authored CSM pass: logical GEqual
    // becomes physical LessEqual, so a map cleared to 0 never receives any
    // positive caster depth. DEBUG=1..4 hid this by forcing Always.
    // Pick the abstract compare which maps to physical >= on either backend.
    const bool backend_swaps_depth_compare = !g_backend_info.bSupportsReversedDepthRange;
    const CompareMode nearest_caster_compare =
        backend_swaps_depth_compare ? CompareMode::LEqual : CompareMode::GEqual;
    config.depth_state.func = force_write ? CompareMode::Always : nearest_caster_compare;
    config.framebuffer_state = {};
    // A depth-only framebuffer has no color attachment.  Leaving the default
    // enum value here means RGBA8 and can produce a pipeline/render-pass mismatch
    // on Vulkan, silently leaving every shadow map at its clear value.
    config.framebuffer_state.color_texture_format = AbstractTextureFormat::Undefined;
    config.framebuffer_state.depth_texture_format = AbstractTextureFormat::D32F;
    config.framebuffer_state.samples = 1;

    auto shadow_pipeline = g_gfx->CreatePipeline(config);
    if (!shadow_pipeline)
    {
      if (!csm.logged_pipeline_failure)
      {
        std::fprintf(stderr,
                     "[moh-ps3-csm] ERROR: failed to create depth-only shadow pipeline\n");
        csm.logged_pipeline_failure = true;
      }
      return;
    }
    pipeline_it = csm.pipelines.emplace(current_pipeline, std::move(shadow_pipeline)).first;
    if (!csm.logged_pipeline_ready)
    {
      std::fprintf(stderr,
                   "[moh-ps3-csm] shadow pipeline READY: D32F, no-cull, depth=%s, "
                   "host-reversed=%d, fixed-function light-space depth\n",
                   force_write ? "Always(debug)" :
                       (backend_swaps_depth_compare ? "LEqual=>host-GEqual" : "GEqual"),
                   g_backend_info.bSupportsReversedDepthRange ? 1 : 0);
      csm.logged_pipeline_ready = true;
    }
  }

  // Protection against pathological draw counts; reject before touching any
  // projection, viewport, pixel-center or pipeline state for this caster.
  MOHCSMReceiverData& receiver = csm.receivers[csm.render_set];
  u32 cascade_mask = (1u << GetMohCSMQuality().cascades) - 1;
  const auto& static_draw = PS3MeshPort::CurrentStaticDraw();
  if (!camera_projection_test && ps3_static_replacement && static_draw.bounds_valid)
  {
    // Bounds were computed at preload, and the MSH stream validator proved a
    // single rigid XF matrix. Transform only eight corners, never triangles.
    const auto& decl = VertexLoaderManager::GetCurrentVertexFormat()->GetVertexDeclaration();
    const auto& constants = vertex_shader_manager.constants;
    u32 matrix_index = 0;
    if (decl.posmtx.enable)
      matrix_index = m_base_buffer_pointer[decl.posmtx.offset];
    if (!decl.posmtx.enable || matrix_index + 2 < constants.transformmatrices.size())
    {
      std::array<std::array<float, 3>, 8> view_corners{};
      for (u32 corner = 0; corner < 8; ++corner)
      {
        std::array<float, 3> point{};
        for (u32 axis = 0; axis < 3; ++axis)
          point[axis] = (corner & (1u << axis)) ? static_draw.bounds_max[axis] : static_draw.bounds_min[axis];
        for (u32 row = 0; row < 3; ++row)
        {
          const auto& m = decl.posmtx.enable ? constants.transformmatrices[matrix_index+row] : constants.posnormalmatrix[row];
          view_corners[corner][row] = m[0]*point[0]+m[1]*point[1]+m[2]*point[2]+m[3];
        }
      }
      for (u32 cascade = 0; cascade < GetMohCSMQuality().cascades; ++cascade)
      {
        std::array<float, 3> minimum{INFINITY, INFINITY, INFINITY};
        std::array<float, 3> maximum{-INFINITY, -INFINITY, -INFINITY};
        bool finite = true;
        for (const auto& point : view_corners)
          for (u32 row = 0; row < 3; ++row)
          {
            const auto& m = receiver.matrix_rows[cascade*4+row];
            const float value = m[0]*point[0]+m[1]*point[1]+m[2]*point[2]+m[3];
            finite &= std::isfinite(value);
            minimum[row] = std::min(minimum[row], value);
            maximum[row] = std::max(maximum[row], value);
          }
        // Orthographic light bounds include the full caster depth extent.
        const bool outside = maximum[0] < -1 || minimum[0] > 1 ||
                             maximum[1] < -1 || minimum[1] > 1 ||
                             maximum[2] < -1 || minimum[2] > 0;
        const float pixels = std::max(maximum[0]-minimum[0], maximum[1]-minimum[1]) *
                             0.5f * GetMohCSMQuality().resolution;
        if (finite && (outside || pixels < 0.25f))
          cascade_mask &= ~(1u << cascade);
      }
    }
  }
  if (!cascade_mask)
    return;
  static const u64 caster_budget = static_cast<u64>(std::clamp(
      MohEnvFloat("MOH_CSM_CASTER_BUDGET", 8192.0f), 64.0f, 1000000.0f));
  if (csm.caster_batches[csm.render_set] + GetMohCSMQuality().cascades > caster_budget)
    return;

  const AbstractPipeline* shadow_pipeline = pipeline_it->second.get();
  const auto saved_projection = vertex_shader_manager.constants.projection;
  const auto saved_pixel_center = vertex_shader_manager.constants.pixelcentercorrection;
  static bool logged_camera_projection_test = false;
  if (camera_projection_test && !logged_camera_projection_test)
  {
    std::fprintf(stderr,
                 "[moh-ps3-csm] CAMERA PROJECTION TEST active: caster uses guest camera matrix\n");
    logged_camera_projection_test = true;
  }

  // The CSM framebuffer uses its own viewport.  Keep X/Y NON-ZERO: the Dolphin
  // vertex shader uses sign(pixelcentercorrection.xy) to preserve viewport
  // orientation.  v1 wrote 0 here, so sign(0)==0 collapsed every caster vertex
  // to the centre of the shadow map and produced no visible shadows.
  const float csm_pixel_center =
      (7.0f / 12.0f - 0.5f) * (2.0f / static_cast<float>(GetMohCSMQuality().resolution));
  vertex_shader_manager.constants.pixelcentercorrection[0] =
      std::copysign(csm_pixel_center, saved_pixel_center[0] == 0.0f ? 1.0f : saved_pixel_center[0]);
  vertex_shader_manager.constants.pixelcentercorrection[1] =
      std::copysign(csm_pixel_center, saved_pixel_center[1] == 0.0f ? -1.0f : saved_pixel_center[1]);
  // CAMERA_TEST must preserve the guest depth-range transform too.  Dolphin's
  // vertex shader applies pixelcentercorrection.zw *after* projection, and MOH
  // uses different depth ranges for world/viewmodel/UI passes.  v2.0-v2.1 kept
  // the camera projection but forced z=1,w=0, which was not actually an exact
  // camera replay: the weapon/HUD survived, while world geometry could be
  // clipped by the wrong depth-range convention.
  if (camera_projection_test)
  {
    vertex_shader_manager.constants.pixelcentercorrection[2] = saved_pixel_center[2];
    vertex_shader_manager.constants.pixelcentercorrection[3] = saved_pixel_center[3];
  }
  else
  {
    // True light-space CSM matrices are authored directly in the GameCube
    // console clip interval [-W,0], so they intentionally use the neutral
    // reverse-Z conversion.
    vertex_shader_manager.constants.pixelcentercorrection[2] = 1.0f;
    vertex_shader_manager.constants.pixelcentercorrection[3] = 0.0f;
  }

  static bool logged_camera_depth_range = false;
  if (camera_projection_test && !logged_camera_depth_range)
  {
    std::fprintf(stderr,
                 "[moh-ps3-csm] CAMERA TEST: preserving guest depth-range correction z=%.6f w=%.6f\n",
                 saved_pixel_center[2], saved_pixel_center[3]);
    logged_camera_depth_range = true;
  }

  for (u32 cascade = 0; cascade < GetMohCSMQuality().cascades; ++cascade)
  {
    if (!(cascade_mask & (1u << cascade)))
      continue;
    // Diagnostic escape hatch: render the exact same geometry with the guest
    // camera projection into the CSM target.  If this produces a stable depth
    // image while the light-space matrix does not, the backend/pipeline/texture
    // path is proven good and BuildCascadeMatrix is the remaining fault.
    if (camera_projection_test)
    {
      vertex_shader_manager.constants.projection = saved_projection;
    }
    else
    {
      for (u32 row = 0; row < 4; ++row)
        vertex_shader_manager.constants.projection[row] = receiver.matrix_rows[cascade * 4 + row];
    }

    vertex_shader_manager.dirty = true;
    UploadUniforms();

    g_gfx->SetFramebuffer(csm.framebuffers[csm.render_set][cascade].get());
    g_gfx->SetViewportAndScissor(csm.depth_textures[csm.render_set][cascade]->GetRect(), 0.0f, 1.0f);
    g_gfx->SetPipeline(shadow_pipeline);
    DrawCurrentBatch(base_index, num_indices, base_vertex);
    ++csm.caster_batches[csm.render_set];
    if (!csm.logged_first_caster_draw)
    {
      std::fprintf(stderr,
                   "[moh-ps3-csm] first REAL caster draw submitted: cascade=%u indices=%u\n",
                   cascade, num_indices);
      csm.logged_first_caster_draw = true;
    }
  }

  if (world_probe)
  {
    static bool logged_world_write = false;
    if (!logged_world_write)
    {
      const auto& p = saved_projection;
      std::fprintf(stderr,
                   "[moh-ps3-csm] WORLD SIGNATURE first replay submitted: idx=%u "
                   "P=(%.6f %.6f %.6f %.6f %.8f %.8f)\n",
                   num_indices, p[0][0], p[0][2], p[1][1], p[1][2], p[2][2], p[2][3]);
      logged_world_write = true;
    }
  }

  vertex_shader_manager.constants.projection = saved_projection;
  vertex_shader_manager.constants.pixelcentercorrection = saved_pixel_center;
  vertex_shader_manager.dirty = true;
  csm.wrote[csm.render_set] = true;
  csm.finished_for_sampling[csm.render_set] = false;

  // Restore the EFB and the exact guest viewport/scissor before the ordinary
  // GameCube draw continues.  EndUtilityDrawing performs only that state restore;
  // unlike BeginUtilityDrawing it does not recurse into Flush().
  g_gfx->EndUtilityDrawing();
  UploadUniforms();
}

void VertexManagerBase::RenderDrawCall(
    PixelShaderManager& pixel_shader_manager, GeometryShaderManager& geometry_shader_manager,
    const CustomPixelShaderContents& custom_pixel_shader_contents,
    std::span<u8> custom_pixel_shader_uniforms, PrimitiveType primitive_type,
    const AbstractPipeline* current_pipeline)
{
  // Now we can upload uniforms, as nothing else will override them.
  geometry_shader_manager.SetConstants(primitive_type);
  pixel_shader_manager.SetConstants();
  if (!custom_pixel_shader_uniforms.empty() &&
      pixel_shader_manager.custom_constants.data() != custom_pixel_shader_uniforms.data())
  {
    pixel_shader_manager.custom_constants_dirty = true;
  }
  pixel_shader_manager.custom_constants = custom_pixel_shader_uniforms;
  UploadUniforms();

  g_gfx->SetPipeline(current_pipeline);

  bool submitted_ps3_mesh = false;
  bool submitted_ps3_skinned = false;
  PS3MeshPort::SkinnedDrawReplacement submitted_skin_replacement;

  // DMF v16.8 validation bridge.  Frontline loads the animated GameCube skin
  // groups with GXLoadPosMtxIndx(group, palette_slot * 3) immediately before
  // GXCallDisplayList.  Therefore the current XF position-matrix palette is the
  // authoritative animated pose for this exact DMF draw.  Keep this one-shot
  // diagnostic in v16.9 so the first real replacements remain auditable.
  if (const auto skin = PS3MeshPort::CurrentSkinnedDraw();
      skin && MohEnvSwitch("MOH_PS3_DMF_VERBOSE", false))
  {
    static std::unordered_map<u32, bool> s_logged_ps3_dmf_draws;
    const bool first_for_dl =
        s_logged_ps3_dmf_draws.size() < 192 &&
        s_logged_ps3_dmf_draws.emplace(skin.display_list, true).second;
    if (first_for_dl)
    {
      NativeVertexFormat* skin_format = VertexLoaderManager::GetCurrentVertexFormat();
      const PortableVertexDeclaration& skin_decl = skin_format->GetVertexDeclaration();
      const u32 skin_stride = skin_format->GetVertexStride();
      const u32 skin_vertices = m_index_generator.GetNumVerts();
      const auto analysis = PS3MeshPort::AnalyzeCurrentSkinnedPalette();

      std::size_t finite_matrix_slots = 0;
      std::array<float, 3> first_translation{};
      bool have_first_translation = false;
      for (std::size_t slot = 0; slot < skin.gc_palette_groups.size(); ++slot)
      {
        // GX destination matrix id is slot*3. XF stores each row as four
        // floats, hence 12 floats per 3x4 position matrix.
        const std::size_t base = slot * 12;
        if (base + 11 >= std::size(xfmem.posMatrices))
          break;
        bool finite = true;
        for (std::size_t element = 0; element < 12; ++element)
          finite = finite && std::isfinite(xfmem.posMatrices[base + element]);
        if (!finite)
          continue;
        ++finite_matrix_slots;
        if (!have_first_translation)
        {
          first_translation = {xfmem.posMatrices[base + 3], xfmem.posMatrices[base + 7],
                               xfmem.posMatrices[base + 11]};
          have_first_translation = true;
        }
      }

      const auto type_value = [](ComponentFormat type) {
        return static_cast<unsigned>(type);
      };
      std::fprintf(stderr,
                   "[moh-ps3-skin] GX DRAW DECL: gc=%s material=%s DL=%08x verts=%u stride=%u primitive=%u pos=(%d,t%u,c%d,o%d) nrm=(%d,t%u,c%d,o%d) uv0=(%d,t%u,c%d,o%d) posmtx=(%d,t%u,c%d,o%d)\n",
                   skin.gc_name.data(),
                   skin.gc_material_name.empty() ? "<unnamed>" : skin.gc_material_name.data(),
                   skin.display_list, skin_vertices, skin_stride,
                   static_cast<unsigned>(primitive_type), skin_decl.position.enable ? 1 : 0,
                   type_value(skin_decl.position.type), skin_decl.position.components,
                   skin_decl.position.offset, skin_decl.normals[0].enable ? 1 : 0,
                   type_value(skin_decl.normals[0].type), skin_decl.normals[0].components,
                   skin_decl.normals[0].offset, skin_decl.texcoords[0].enable ? 1 : 0,
                   type_value(skin_decl.texcoords[0].type), skin_decl.texcoords[0].components,
                   skin_decl.texcoords[0].offset, skin_decl.posmtx.enable ? 1 : 0,
                   type_value(skin_decl.posmtx.type), skin_decl.posmtx.components,
                   skin_decl.posmtx.offset);

      std::fprintf(stderr,
                   "[moh-ps3-skin] GX PALETTE ANALYSIS: gc=%s material=%s DL=%08x ps3_clusters=%zu triangles=%zu selected=%zu ambiguous=%zu unmapped=%zu vertices=%zu matrix_slots=%zu finite_matrices=%zu firstT=(%.4f %.4f %.4f) valid=%d | palette validation\n",
                   skin.gc_name.data(),
                   skin.gc_material_name.empty() ? "<unnamed>" : skin.gc_material_name.data(),
                   skin.display_list, analysis.ps3_material_clusters, analysis.total_triangles,
                   analysis.selected_triangles, analysis.ambiguous_triangles,
                   analysis.unmapped_triangles, analysis.selected_vertices,
                   analysis.matrix_slots, finite_matrix_slots,
                   have_first_translation ? first_translation[0] : 0.0f,
                   have_first_translation ? first_translation[1] : 0.0f,
                   have_first_translation ? first_translation[2] : 0.0f,
                   analysis.valid && finite_matrix_slots == skin.gc_palette_groups.size() ? 1 : 0);
    }
  }

  // v16.9 first real PS3 DMF draw.  Only replace a draw when the host bridge
  // proved a strict 1:1 authored material correspondence: one GC DL, one PS3
  // cluster, no ambiguous/unmapped triangles, and an exact PS3->GC skin-group
  // mapping.  The GameCube animation system remains authoritative: each PS3
  // vertex receives the same GX position-matrix id (slot*3) that the original
  // DMF used after GXLoadPosMtxIndx.  Anything uncertain falls through to GC.
  const bool ps3_dmf_triangle_primitive =
      primitive_type == PrimitiveType::Triangles ||
      primitive_type == PrimitiveType::TriangleStrip;
  if (ps3_dmf_triangle_primitive && xfmem.projection.type == ProjectionType::Perspective)
  {
    auto replacement = PS3MeshPort::BuildCurrentSkinnedReplacement();
    if (replacement)
    {
      NativeVertexFormat* format = VertexLoaderManager::GetCurrentVertexFormat();
      const PortableVertexDeclaration& decl = format->GetVertexDeclaration();
      const u32 vertex_stride = format->GetVertexStride();
      const u32 gc_vertex_count = m_index_generator.GetNumVerts();
      const std::size_t gc_vertex_bytes = std::size_t(gc_vertex_count) * vertex_stride;
      const auto& cluster = *replacement.cluster;

      const auto attribute_is_float = [vertex_stride](const AttributeFormat& attribute,
                                                       int minimum_components) {
        return attribute.enable && attribute.type == ComponentFormat::Float &&
               attribute.components >= minimum_components && attribute.offset >= 0 &&
               std::size_t(attribute.offset) +
                       sizeof(float) * std::size_t(minimum_components) <=
                   vertex_stride;
      };

      bool stream_ok = vertex_stride != 0 && gc_vertex_count >= 3 &&
                       m_base_buffer_pointer && m_cur_buffer_pointer &&
                       gc_vertex_bytes <= MAXVBUFFERSIZE &&
                       std::size_t(m_cur_buffer_pointer - m_base_buffer_pointer) >=
                           gc_vertex_bytes &&
                       attribute_is_float(decl.position, 3) &&
                       attribute_is_float(decl.normals[0], 3) &&
                       attribute_is_float(decl.texcoords[0], 2) &&
                       decl.posmtx.enable && decl.posmtx.type == ComponentFormat::UByte &&
                       decl.posmtx.components >= 4 && decl.posmtx.offset >= 0 &&
                       std::size_t(decl.posmtx.offset) + sizeof(u32) <= vertex_stride &&
                       !decl.normals[1].enable && !decl.normals[2].enable &&
                       cluster.positions.size() <= 65535 &&
                       cluster.positions.size() * vertex_stride <= MAXVBUFFERSIZE &&
                       cluster.positions.size() == cluster.normals.size() &&
                       cluster.positions.size() == cluster.uv0.size() &&
                       cluster.positions.size() == replacement.position_matrix_indices.size() &&
                       !replacement.model_to_gc_local.empty() &&
                       replacement.model_to_gc_local_normal.size() ==
                           replacement.model_to_gc_local.size() &&
                       !cluster.indices.empty() && (cluster.indices.size() % 3) == 0;

      for (std::size_t tex = 1; stream_ok && tex < decl.texcoords.size(); ++tex)
        if (decl.texcoords[tex].enable)
          stream_ok = false;

      // Preserve constant tint/other color state from the GC draw.  Varying
      // authored GC colors cannot be projected safely onto the PS3 ordering.
      const std::span<const u8> gc_vertices(
          stream_ok ? m_base_buffer_pointer : nullptr, stream_ok ? gc_vertex_bytes : 0);
      for (const auto& color : decl.colors)
      {
        if (!stream_ok || !color.enable)
          continue;
        const std::size_t bytes =
            std::size_t(GetElementSize(color.type)) * std::max(color.components, 1);
        if (color.offset < 0 || std::size_t(color.offset) + bytes > vertex_stride)
        {
          stream_ok = false;
          break;
        }
        for (u32 i = 1; i < gc_vertex_count; ++i)
        {
          if (std::memcmp(gc_vertices.data() + color.offset,
                          gc_vertices.data() + std::size_t(i) * vertex_stride + color.offset,
                          bytes) != 0)
          {
            stream_ok = false;
            break;
          }
        }
      }

      // Every matrix id written below must already exist in XF and be finite.
      for (const u8 matrix_id : replacement.position_matrix_indices)
      {
        if (!stream_ok || (matrix_id % 3) != 0)
          break;
        const std::size_t local_slot = matrix_id / 3u;
        if (local_slot >= replacement.model_to_gc_local.size())
        {
          stream_ok = false;
          break;
        }
        for (const float value : replacement.model_to_gc_local[local_slot])
        {
          if (!std::isfinite(value))
            stream_ok = false;
        }
        if (local_slot >= replacement.model_to_gc_local_normal.size())
          stream_ok = false;
        else
          for (const float value : replacement.model_to_gc_local_normal[local_slot])
            if (!std::isfinite(value))
              stream_ok = false;
        if (!stream_ok)
          break;
        const std::size_t base = std::size_t(matrix_id) * 4;
        if (base + 11 >= std::size(xfmem.posMatrices))
        {
          stream_ok = false;
          break;
        }
        for (std::size_t element = 0; element < 12; ++element)
        {
          if (!std::isfinite(xfmem.posMatrices[base + element]))
          {
            stream_ok = false;
            break;
          }
        }
      }

      const std::size_t expanded_index_count =
          cluster.indices.size() +
          (g_backend_info.bSupportsPrimitiveRestart ? cluster.indices.size() / 3 : 0);
      if (expanded_index_count > MAXIBUFFERSIZE)
        stream_ok = false;
      for (const u16 index : cluster.indices)
        if (index >= cluster.positions.size())
          stream_ok = false;

      // Validate the complete model-bind -> group-local conversion before
      // ResetBuffer(). A failed PS3 conversion therefore leaves the original
      // GC batch untouched and can still fall through safely.
      for (std::size_t i = 0; stream_ok && i < cluster.positions.size(); ++i)
      {
        const u32 matrix_id = replacement.position_matrix_indices[i];
        const std::size_t local_slot = matrix_id / 3u;
        if (local_slot >= replacement.model_to_gc_local.size())
        {
          stream_ok = false;
          break;
        }
        const auto& m = replacement.model_to_gc_local[local_slot];
        const auto& normal_m = replacement.model_to_gc_local_normal[local_slot];
        const auto& p = cluster.positions[i];
        const auto& n = cluster.normals[i];
        std::array<float, 3> local_position{};
        std::array<float, 3> local_normal{};
        for (std::size_t row = 0; row < 3; ++row)
        {
          local_position[row] = m[row * 4 + 0] * p[0] + m[row * 4 + 1] * p[1] +
                                m[row * 4 + 2] * p[2] + m[row * 4 + 3];
          local_normal[row] = normal_m[row * 3 + 0] * n[0] +
                              normal_m[row * 3 + 1] * n[1] +
                              normal_m[row * 3 + 2] * n[2];
          if (!std::isfinite(local_position[row]) || !std::isfinite(local_normal[row]))
            stream_ok = false;
        }
        const float normal_length = std::sqrt(local_normal[0] * local_normal[0] +
                                              local_normal[1] * local_normal[1] +
                                              local_normal[2] * local_normal[2]);
        if (!std::isfinite(normal_length) || normal_length <= 1.0e-8f)
          stream_ok = false;
      }

      if (stream_ok)
      {
        const std::vector<u8> gc_template(gc_vertices.begin(),
                                          gc_vertices.begin() + vertex_stride);
        ResetBuffer(vertex_stride);
        for (std::size_t i = 0; i < cluster.positions.size(); ++i)
        {
          u8* destination = m_cur_buffer_pointer;
          std::memcpy(destination, gc_template.data(), vertex_stride);

          const u32 matrix_id = replacement.position_matrix_indices[i];
          const std::size_t local_slot = matrix_id / 3u;
          const auto& model_to_local = replacement.model_to_gc_local[local_slot];
          const auto& normal_to_local =
              replacement.model_to_gc_local_normal[local_slot];
          const auto& model_position = cluster.positions[i];
          const auto& model_normal = cluster.normals[i];
          std::array<float, 3> local_position{};
          std::array<float, 3> local_normal{};
          for (std::size_t row = 0; row < 3; ++row)
          {
            local_position[row] =
                model_to_local[row * 4 + 0] * model_position[0] +
                model_to_local[row * 4 + 1] * model_position[1] +
                model_to_local[row * 4 + 2] * model_position[2] +
                model_to_local[row * 4 + 3];
            local_normal[row] =
                normal_to_local[row * 3 + 0] * model_normal[0] +
                normal_to_local[row * 3 + 1] * model_normal[1] +
                normal_to_local[row * 3 + 2] * model_normal[2];
          }
          const float normal_length = std::sqrt(local_normal[0] * local_normal[0] +
                                                local_normal[1] * local_normal[1] +
                                                local_normal[2] * local_normal[2]);
          for (float& component : local_normal)
            component /= normal_length;

          std::memcpy(destination + decl.position.offset, local_position.data(),
                      sizeof(float) * 3);
          std::memcpy(destination + decl.normals[0].offset, local_normal.data(),
                      sizeof(float) * 3);
          std::memcpy(destination + decl.texcoords[0].offset, cluster.uv0[i].data(),
                      sizeof(float) * 2);

          // PosMtx_ReadDirect_UByte expands the guest byte to a host u32 in the
          // portable vertex.  Reproduce that exact representation here.
          std::memcpy(destination + decl.posmtx.offset, &matrix_id, sizeof(matrix_id));
          m_cur_buffer_pointer += vertex_stride;
        }

        m_index_generator.AddExternalTriangles(
            cluster.indices.data(), static_cast<u32>(cluster.indices.size()),
            static_cast<u32>(cluster.positions.size()));
        submitted_ps3_mesh = true;
        submitted_ps3_skinned = true;
        submitted_skin_replacement = std::move(replacement);
      }
      else
      {
        static unsigned reject_logs = 0;
        if (reject_logs++ < 48)
        {
          const auto skin = PS3MeshPort::CurrentSkinnedDraw();
          std::fprintf(stderr,
                       "[moh-ps3-skin] SKINNED STREAM REJECT: gc=%s material=%s DL=%08x | strict fallback GC\n",
                       skin.gc_name.data(), skin.gc_material_name.data(), skin.display_list);
        }
      }
    }
  }

  // Exact named MSH display-list replacement:
  //
  // Replace the CPU-side rigid GameCube draw before CommitBuffer(), while all
  // current GX state (model matrix, projection, TEV/material state, .lit
  // lighting and the true CSM caster path) remains untouched.
  //
  // This intentionally starts strict:
  //   * perspective 3D only;
  //   * triangle-list pipeline only;
  //   * exact loader-registered resource/display-list identity;
  //   * compatible object-space bounds;
  //   * host float position/UV/normal declaration only.
  //
  // Anything uncertain falls through to the original GameCube buffer.
  const bool ps3_triangle_primitive =
      primitive_type == PrimitiveType::Triangles ||
      primitive_type == PrimitiveType::TriangleStrip;
  if (!submitted_ps3_mesh && PS3MeshPort::IsStaticDrawReplacementEnabled() &&
      ps3_triangle_primitive &&
      xfmem.projection.type == ProjectionType::Perspective)
  {
    NativeVertexFormat* format = VertexLoaderManager::GetCurrentVertexFormat();
    const PortableVertexDeclaration& decl = format->GetVertexDeclaration();
    const u32 vertex_stride = format->GetVertexStride();
    const u32 gc_vertex_count = m_index_generator.GetNumVerts();
    const std::size_t gc_vertex_bytes = std::size_t(gc_vertex_count) * vertex_stride;

    const bool position_ok =
        decl.position.enable &&
        decl.position.type == ComponentFormat::Float &&
        decl.position.components >= 3 &&
        decl.position.offset >= 0 &&
        std::size_t(decl.position.offset) + sizeof(float) * 3 <= vertex_stride;

    bool declaration_ok =
        position_ok &&
        gc_vertex_count >= 3 &&
        gc_vertex_count <= 65535 &&
        vertex_stride != 0 &&
        gc_vertex_bytes <= MAXVBUFFERSIZE &&
        m_base_buffer_pointer &&
        m_cur_buffer_pointer &&
        std::size_t(m_cur_buffer_pointer - m_base_buffer_pointer) >= gc_vertex_bytes;

    // Static MSH v2 reconstructs the primary normal itself. Tangent/binormal
    // streams need the later material/shader bridge, so keep those draws GC.
    if (decl.normals[1].enable || decl.normals[2].enable)
      declaration_ok = false;

    for (std::size_t tex = 2; tex < decl.texcoords.size(); ++tex)
    {
      if (decl.texcoords[tex].enable)
        declaration_ok = false;
    }

    if (declaration_ok)
    {
      static bool s_logged_ps3_msh_draw_path = false;
      if (!s_logged_ps3_msh_draw_path)
      {
        s_logged_ps3_msh_draw_path = true;
        std::fprintf(stderr,
                     "[moh-ps3-msh] DRAW PATH ACTIVE: primitive=%s verts=%u stride=%u restart=%d\n",
                     primitive_type == PrimitiveType::TriangleStrip ? "triangle-strip" : "triangles",
                     gc_vertex_count, vertex_stride, g_backend_info.bSupportsPrimitiveRestart ? 1 : 0);
      }
      const std::span<const u8> gc_vertices(m_base_buffer_pointer, gc_vertex_bytes);

      // The IndexGenerator has already converted every GX triangle primitive to
      // the backend topology.  Recover the number of authored triangles from
      // that generated stream so direct world CPT matching has a topology check
      // instead of relying on AABB equality alone.
      const u32 gc_index_count = m_index_generator.GetIndexLen();
      u32 gc_triangle_count = 0;
      if (g_backend_info.bSupportsPrimitiveRestart &&
          primitive_type == PrimitiveType::TriangleStrip && gc_index_count >= gc_vertex_count)
      {
        const u32 restart_count = gc_index_count - gc_vertex_count;
        if (restart_count <= gc_vertex_count / 2)
          gc_triangle_count = gc_vertex_count - restart_count * 2;
      }
      else if (primitive_type == PrimitiveType::Triangles)
      {
        gc_triangle_count = gc_index_count / 3;
      }

      // v10.1 full-level CPT proof-of-life. m_draw_counter is reset by
      // OnEndFrame(), so zero marks the first render batch of a new frame.
      static bool s_moh_full_cpt_level_drawn_this_frame = false;
      if (m_draw_counter == 0)
        s_moh_full_cpt_level_drawn_this_frame = false;

      PS3MeshPort::StaticDrawMatch match;
      if (!s_moh_full_cpt_level_drawn_this_frame)
      {
        match = PS3MeshPort::AcquireFullCPTLevelDraw(gc_triangle_count);
        if (match)
        {
          s_moh_full_cpt_level_drawn_this_frame = true;
          static unsigned full_level_select_logs = 0;
          if (full_level_select_logs++ < 8)
          {
            std::fprintf(stderr,
                         "[moh-ps3-world-full] DRAW SELECTED: trigger GCverts=%u GCtris=%u stride=%u | complete CPT level will replace this direct world batch\n",
                         gc_vertex_count, gc_triangle_count, vertex_stride);
          }
        }
      }
      if (!match)
      {
        match = PS3MeshPort::MatchStaticDraw(gc_vertices, gc_vertex_count, vertex_stride,
                                             static_cast<u32>(decl.position.offset),
                                             gc_triangle_count);
      }

      if (match)
      {
        const PS3MeshPort::Submesh& submesh = *match.submesh;
        const bool world_cpt_match =
            match.mesh && match.mesh->source_name.find(".cpt#cpt-") != std::string::npos;
        const bool full_cpt_level_match =
            match.mesh && match.mesh->source_name.find("#cpt-full-level") != std::string::npos;

        auto attribute_is_float = [vertex_stride](const AttributeFormat& attribute,
                                                  int minimum_components) {
          return !attribute.enable ||
                 (attribute.type == ComponentFormat::Float &&
                  attribute.components >= minimum_components &&
                  attribute.offset >= 0 &&
                  std::size_t(attribute.offset) +
                          sizeof(float) * std::size_t(minimum_components) <=
                      vertex_stride);
        };

        const std::size_t ps3_index_count =
            submesh.indices.size() +
            (g_backend_info.bSupportsPrimitiveRestart ? submesh.indices.size() / 3 : 0);

        bool stream_ok =
            submesh.vertex_count <= 65535 &&
            std::size_t(submesh.vertex_count) * vertex_stride <= MAXVBUFFERSIZE &&
            submesh.position_uv.size() == submesh.vertex_count &&
            !submesh.indices.empty() &&
            (submesh.indices.size() % 3) == 0 &&
            ps3_index_count <= MAXIBUFFERSIZE &&
            attribute_is_float(decl.normals[0], 3) &&
            attribute_is_float(decl.texcoords[0], 2) &&
            attribute_is_float(decl.texcoords[1], 2) &&
            (!decl.texcoords[0].enable || submesh.has_uv0) &&
            (!decl.texcoords[1].enable || submesh.has_uv1);

        const bool base_stream_ok = stream_ok;
        bool color_layout_ok = true;
        bool color_constant = true;

        // Ordinary MSH replacement still requires constant GC colors.  World
        // CPT is different: its PS3 vertex ordering cannot preserve a varying
        // GC color stream yet, but refusing the draw entirely prevented any CPT
        // geometry from reaching the renderer.  For the strict direct CPT path
        // only, keep the first GC color/tint as the template until the PS3 color
        // semantic is decoded. Invalid color declarations still hard-fail.
        for (const auto& color : decl.colors)
        {
          if (!stream_ok || !color.enable)
            continue;
          const std::size_t bytes = std::size_t(GetElementSize(color.type)) * color.components;
          if (color.offset < 0 || std::size_t(color.offset) + bytes > vertex_stride)
          {
            color_layout_ok = false;
            stream_ok = false;
            break;
          }
          for (u32 i = 1; i < gc_vertex_count; ++i)
          {
            if (std::memcmp(gc_vertices.data() + color.offset,
                            gc_vertices.data() + std::size_t(i) * vertex_stride + color.offset,
                            bytes) != 0)
            {
              color_constant = false;
              if (!world_cpt_match)
                stream_ok = false;
              break;
            }
          }
        }

        bool posmtx_layout_ok = true;
        bool posmtx_constant = true;
        std::size_t posmtx_bytes = 0;
        // Rigid replacement may reuse a per-vertex position-matrix index only
        // when the whole original draw used exactly the same index/value.
        if (stream_ok && decl.posmtx.enable)
        {
          const u32 element_size = GetElementSize(decl.posmtx.type);
          const std::size_t matrix_bytes =
              std::size_t(element_size) * std::max(decl.posmtx.components, 1);
          posmtx_bytes = matrix_bytes;
          if (matrix_bytes == 0 || decl.posmtx.offset < 0 ||
              std::size_t(decl.posmtx.offset) + matrix_bytes > vertex_stride)
          {
            posmtx_layout_ok = false;
            stream_ok = false;
          }
          else
          {
            const u8* first = gc_vertices.data() + decl.posmtx.offset;
            for (u32 i = 1; i < gc_vertex_count; ++i)
            {
              const u8* current =
                  gc_vertices.data() + std::size_t(i) * vertex_stride + decl.posmtx.offset;
              if (std::memcmp(first, current, matrix_bytes) != 0)
              {
                posmtx_constant = false;
                // A world CPT draw may legitimately span several GX position
                // matrix indices. Keep ordinary rigid MSH strict, but defer
                // CPT validation to the spatial matrix-index bridge below.
                if (!world_cpt_match)
                  stream_ok = false;
                break;
              }
            }
          }
        }

        // v10.0: large GC world batches can use a varying position-matrix
        // index. Preserve that live GX/XF palette by assigning each PS3 vertex
        // the matrix field of the spatially nearest GC vertex after applying
        // the already-validated CPT world translation.
        std::vector<u32> world_posmtx_source_vertices;
        float world_posmtx_max_nearest_ratio = 0.0f;
        if (stream_ok && world_cpt_match && !full_cpt_level_match &&
            decl.posmtx.enable && !posmtx_constant && posmtx_layout_ok && posmtx_bytes != 0)
        {
          std::array<float, 3> gc_min = {1.0e30f, 1.0e30f, 1.0e30f};
          std::array<float, 3> gc_max = {-1.0e30f, -1.0e30f, -1.0e30f};
          bool gc_positions_valid = true;
          for (u32 gc_i = 0; gc_i < gc_vertex_count; ++gc_i)
          {
            std::array<float, 3> p{};
            std::memcpy(p.data(),
                        gc_vertices.data() + std::size_t(gc_i) * vertex_stride +
                            decl.position.offset,
                        sizeof(float) * 3);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            {
              gc_positions_valid = false;
              break;
            }
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
              gc_min[axis] = std::min(gc_min[axis], p[axis]);
              gc_max[axis] = std::max(gc_max[axis], p[axis]);
            }
          }

          const float dx = gc_max[0] - gc_min[0];
          const float dy = gc_max[1] - gc_min[1];
          const float dz = gc_max[2] - gc_min[2];
          const float gc_diagonal2 = dx * dx + dy * dy + dz * dz;
          const float maximum_nearest_ratio =
              MohEnvFloat("MOH_PS3_CPT_POSMTX_MAX_NEAREST", 0.20f);

          if (!gc_positions_valid || !std::isfinite(gc_diagonal2) || gc_diagonal2 <= 1.0e-12f)
          {
            stream_ok = false;
          }
          else
          {
            world_posmtx_source_vertices.resize(submesh.vertex_count);
            float max_nearest_distance2 = 0.0f;
            for (u32 ps3_i = 0; ps3_i < submesh.vertex_count; ++ps3_i)
            {
              std::array<float, 3> p = submesh.position_uv[ps3_i].position;
              if (match.world_translation_valid)
              {
                for (std::size_t axis = 0; axis < 3; ++axis)
                  p[axis] += match.world_translation[axis];
              }

              u32 nearest_gc = 0;
              float nearest_distance2 = 1.0e30f;
              for (u32 gc_i = 0; gc_i < gc_vertex_count; ++gc_i)
              {
                std::array<float, 3> q{};
                std::memcpy(q.data(),
                            gc_vertices.data() + std::size_t(gc_i) * vertex_stride +
                                decl.position.offset,
                            sizeof(float) * 3);
                const float px = p[0] - q[0];
                const float py = p[1] - q[1];
                const float pz = p[2] - q[2];
                const float distance2 = px * px + py * py + pz * pz;
                if (distance2 < nearest_distance2)
                {
                  nearest_distance2 = distance2;
                  nearest_gc = gc_i;
                }
              }
              world_posmtx_source_vertices[ps3_i] = nearest_gc;
              max_nearest_distance2 = std::max(max_nearest_distance2, nearest_distance2);
            }

            world_posmtx_max_nearest_ratio = std::sqrt(max_nearest_distance2 / gc_diagonal2);
            if (!std::isfinite(world_posmtx_max_nearest_ratio) ||
                world_posmtx_max_nearest_ratio > maximum_nearest_ratio)
            {
              static unsigned world_posmtx_reject_logs = 0;
              if (world_posmtx_reject_logs++ < 32)
              {
                std::fprintf(stderr,
                             "[moh-ps3-world-geo] WORLD POSMTX BRIDGE REJECT: ps3=%s GCverts=%u PS3verts=%u bytes=%zu max-nearest=%.6f limit=%.6f -> GC\n",
                             match.mesh->source_name.c_str(), gc_vertex_count,
                             submesh.vertex_count, posmtx_bytes,
                             world_posmtx_max_nearest_ratio, maximum_nearest_ratio);
              }
              world_posmtx_source_vertices.clear();
              stream_ok = false;
            }
            else
            {
              static unsigned world_posmtx_ready_logs = 0;
              if (world_posmtx_ready_logs++ < 32)
              {
                std::fprintf(stderr,
                             "[moh-ps3-world-geo] WORLD POSMTX BRIDGE READY: ps3=%s GCverts=%u PS3verts=%u bytes=%zu max-nearest=%.6f limit=%.6f | nearest GC matrix index per PS3 vertex\n",
                             match.mesh->source_name.c_str(), gc_vertex_count,
                             submesh.vertex_count, posmtx_bytes,
                             world_posmtx_max_nearest_ratio, maximum_nearest_ratio);
              }
            }
          }
        }

        if (world_cpt_match && stream_ok && !color_constant)
        {
          static unsigned world_color_logs = 0;
          if (world_color_logs++ < 32)
          {
            std::fprintf(stderr,
                         "[moh-ps3-world-geo] WORLD COLOR BRIDGE: ps3=%s GCverts=%u GCtris=%u stride=%u | varying GC color retained as first-vertex material tint until CPT color semantic decode\n",
                         match.mesh->source_name.c_str(), gc_vertex_count, gc_triangle_count,
                         vertex_stride);
          }
        }

        if (stream_ok)
        {
          // Normals are prepared once at resource registration, not per draw.
          const auto& ps3_normals = *match.normals;
          if (stream_ok)
          {
            // Keep the validated constant per-draw GC attributes as a template,
            // then overwrite the attributes which are authoritative in PS3 MSH.
            const std::vector<u8> gc_template(gc_vertices.begin(), gc_vertices.end());
            ResetBuffer(vertex_stride);

            for (u32 i = 0; i < submesh.vertex_count; ++i)
            {
              u8* destination = m_cur_buffer_pointer;
              std::memcpy(destination,
                          gc_template.data(),
                          vertex_stride);

              const auto& source = submesh.position_uv[i];
              std::array<float, 3> ps3_position = source.position;
              if (world_cpt_match && match.world_translation_valid)
              {
                for (std::size_t axis = 0; axis < 3; ++axis)
                  ps3_position[axis] += match.world_translation[axis];
              }

              std::memcpy(destination + decl.position.offset, ps3_position.data(),
                          sizeof(float) * 3);

              if (full_cpt_level_match && decl.posmtx.enable && posmtx_bytes != 0)
              {
                // NODE70 already moved every descriptor into authored PS3
                // level/world space. Position matrix slot 0 keeps the common
                // live camera transform without a random GC sector matrix.
                std::memset(destination + decl.posmtx.offset, 0, posmtx_bytes);
              }
              else if (!world_posmtx_source_vertices.empty())
              {
                const u32 gc_source_vertex = world_posmtx_source_vertices[i];
                std::memcpy(destination + decl.posmtx.offset,
                            gc_vertices.data() +
                                std::size_t(gc_source_vertex) * vertex_stride + decl.posmtx.offset,
                            posmtx_bytes);
              }

              if (decl.normals[0].enable)
              {
                const auto& n = ps3_normals[i];
                if (n[0] != 0.0f || n[1] != 0.0f || n[2] != 0.0f)
                  std::memcpy(destination + decl.normals[0].offset, n.data(), sizeof(float) * 3);
              }

              if (decl.texcoords[0].enable)
                std::memcpy(destination + decl.texcoords[0].offset, source.uv0.data(),
                            sizeof(float) * 2);
              if (decl.texcoords[1].enable)
                std::memcpy(destination + decl.texcoords[1].offset, source.uv1.data(),
                            sizeof(float) * 2);

              m_cur_buffer_pointer += vertex_stride;
            }

            m_index_generator.AddExternalTriangles(
                submesh.indices.data(), static_cast<u32>(submesh.indices.size()),
                submesh.vertex_count);

            submitted_ps3_mesh = true;
          }
        }
        else if (world_cpt_match)
        {
          static unsigned world_stream_reject_logs = 0;
          if (world_stream_reject_logs++ < 64)
          {
            std::fprintf(stderr,
                         "[moh-ps3-world-geo] WORLD STREAM REJECT: ps3=%s GCverts=%u GCtris=%u stride=%u base=%d color-layout=%d color-constant=%d posmtx-layout=%d posmtx-constant=%d nrm=%d uv0=%d uv1=%d -> GC\n",
                         match.mesh->source_name.c_str(), gc_vertex_count, gc_triangle_count,
                         vertex_stride, base_stream_ok ? 1 : 0, color_layout_ok ? 1 : 0,
                         color_constant ? 1 : 0, posmtx_layout_ok ? 1 : 0,
                         posmtx_constant ? 1 : 0, decl.normals[0].enable ? 1 : 0,
                         decl.texcoords[0].enable ? 1 : 0, decl.texcoords[1].enable ? 1 : 0);
          }
          // v9.1 waited for the *next* MatchStaticDraw() to expire a failed
          // transient match. If this was the final draw of the run, the shared
          // owner survived into shutdown. Reject it immediately instead.
          PS3MeshPort::RejectStaticDrawCandidate();
        }
      }
    }
  }

  u32 base_vertex, base_index;
  CommitBuffer(m_index_generator.GetNumVerts(),
               VertexLoaderManager::GetCurrentVertexFormat()->GetVertexStride(),
               m_index_generator.GetIndexLen(), &base_vertex, &base_index);

  if (g_backend_info.api_type != APIType::D3D && g_ActiveConfig.UseVSForLinePointExpand() &&
      (primitive_type == PrimitiveType::Points || primitive_type == PrimitiveType::Lines))
  {
    // VS point/line expansion puts the vertex id at gl_VertexID << 2
    // That means the base vertex has to be adjusted to match
    // (The shader adds this after shifting right on D3D, so no need to do this)
    base_vertex <<= 2;
  }

  // True PS3-style CSM caster: replay this exact 3D batch into four light-space
  // depth targets before issuing the normal EFB draw.  This is geometry casting,
  // not a screen-space shadow approximation.
  auto& vertex_shader_manager = Core::System::GetInstance().GetVertexShaderManager();
  RenderMOHCSMCasters(vertex_shader_manager, base_index, m_index_generator.GetIndexLen(),
                      base_vertex, primitive_type, current_pipeline,
                      submitted_ps3_mesh && !submitted_ps3_skinned);

  g_gfx->SetPipeline(current_pipeline);

  if (PerfQueryBase::ShouldEmulate())
    g_perf_query->EnableQuery(bpmem.zcontrol.early_ztest ? PQG_ZCOMP_ZCOMPLOC : PQG_ZCOMP);

  DrawCurrentBatch(base_index, m_index_generator.GetIndexLen(), base_vertex);
  if (submitted_ps3_skinned)
    PS3MeshPort::NotifySkinnedDrawSubmitted(submitted_skin_replacement);
  else if (submitted_ps3_mesh)
    PS3MeshPort::NotifyStaticDrawSubmitted();

  // Track the total emulated state draws
  INCSTAT(g_stats.this_frame.num_draw_calls);

  if (PerfQueryBase::ShouldEmulate())
    g_perf_query->DisableQuery(bpmem.zcontrol.early_ztest ? PQG_ZCOMP_ZCOMPLOC : PQG_ZCOMP);
}

const AbstractPipeline* VertexManagerBase::GetCustomPipeline(
    const CustomPixelShaderContents& custom_pixel_shader_contents,
    const VideoCommon::GXPipelineUid& current_pipeline_config,
    const VideoCommon::GXUberPipelineUid& current_uber_pipeline_config,
    const AbstractPipeline* current_pipeline) const
{
  if (current_pipeline)
  {
    if (!custom_pixel_shader_contents.shaders.empty())
    {
      CustomShaderInstance custom_shaders;
      custom_shaders.pixel_contents = custom_pixel_shader_contents;
      switch (g_ActiveConfig.iShaderCompilationMode)
      {
      case ShaderCompilationMode::Synchronous:
      case ShaderCompilationMode::AsynchronousSkipRendering:
      {
        if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                current_pipeline_config, custom_shaders, current_pipeline->m_config))
        {
          return *pipeline;
        }
      }
      break;
      case ShaderCompilationMode::SynchronousUberShaders:
      {
        // D3D has issues compiling large custom ubershaders
        // use specialized shaders instead
        if (g_backend_info.api_type == APIType::D3D)
        {
          if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                  current_pipeline_config, custom_shaders, current_pipeline->m_config))
          {
            return *pipeline;
          }
        }
        else
        {
          if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                  current_uber_pipeline_config, custom_shaders, current_pipeline->m_config))
          {
            return *pipeline;
          }
        }
      }
      break;
      case ShaderCompilationMode::AsynchronousUberShaders:
      {
        if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                current_pipeline_config, custom_shaders, current_pipeline->m_config))
        {
          return *pipeline;
        }
        else if (auto uber_pipeline = m_custom_shader_cache->GetPipelineAsync(
                     current_uber_pipeline_config, custom_shaders, current_pipeline->m_config))
        {
          return *uber_pipeline;
        }
      }
      break;
      };
    }
  }

  return nullptr;
}
