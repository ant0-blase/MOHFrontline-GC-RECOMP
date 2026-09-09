#include "VideoCommon/MOHFrontline/Engine/Renderer/NativeRenderBridge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "VideoCommon/PS3MeshPort.h"

namespace MOHFrontline::NativeRender
{
namespace
{
std::mutex s_mutex;
Submitter s_submitter = nullptr;
void* s_userdata = nullptr;

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsBlockedCharacterDMF(std::string_view name)
{
  std::string filename =
      Lower(std::string(name));

  const auto slash =
      filename.find_last_of("/\\:");

  if (slash != std::string::npos)
    filename.erase(0, slash + 1);

  if (filename.ends_with(".dmt"))
  {
    filename.replace(
        filename.size() - 4,
        4,
        ".dmf");
  }

  if (!filename.ends_with(".dmf"))
    return false;

  if (filename.rfind("uhm", 0) == 0)
    return true;

  return filename.size() >= 4 &&
         filename[0] == 'b' &&
         filename[1] == 'm' &&
         std::isdigit(
             static_cast<unsigned char>(
                 filename[2])) &&
         std::isdigit(
             static_cast<unsigned char>(
                 filename[3]));
}

bool BuildStatic(DrawPacket* out)
{
  const auto& current = PS3MeshPort::CurrentStaticDraw();
  if (!current || !current.submesh)
    return false;

  const auto& sub = *current.submesh;
  if (sub.position_uv.size() != sub.vertex_count || sub.indices.empty())
    return false;

  DrawPacket& packet = *out;
  packet.vertices.clear();
  packet.indices.clear();
  packet.model_to_gc_local.clear();
  packet.material_name.clear();
  packet.has_texture = true;
  packet.use_vertex_color = false;
  packet.skinned = false;
  packet.source_name =
      current.mesh && !current.mesh->source_name.empty() ? current.mesh->source_name : "PS3-MSH";
  if (!sub.material_hints.empty())
    packet.material_name = sub.material_hints.front();

  packet.vertices.reserve(sub.position_uv.size());
  for (const auto& source : sub.position_uv)
  {
    Vertex vertex;
    vertex.position = source.position;
    vertex.normal = source.normal;
    vertex.uv0 = source.uv0;
    packet.vertices.push_back(vertex);
  }

  packet.indices.reserve(sub.indices.size());
  for (u16 index : sub.indices)
  {
    if (index >= packet.vertices.size())
      return false;
    packet.indices.push_back(index);
  }

  return true;
}

bool BuildSkinned(DrawPacket* out)
{
  const auto context = PS3MeshPort::CurrentSkinnedDraw();
  if (!context)
    return false;

  // v12.10:
  // Never submit PS3 animated character geometry to the host renderer.
  //
  // The original GX draw is therefore retained. Textures/materials are
  // handled by their own replacement layer and are not disabled here.
  if (context.gc_material_name == "mohf_body" ||
      IsBlockedCharacterDMF(context.gc_name))
  {
    return false;
  }

  const auto replacement =
      PS3MeshPort::BuildCurrentSkinnedReplacement();
  if (!replacement || !replacement.cluster)
    return false;

  const auto& cluster = *replacement.cluster;
  if (cluster.positions.empty() || cluster.indices.empty() ||
      replacement.position_matrix_indices.size() != cluster.positions.size())
    return false;

  DrawPacket& packet = *out;
  packet.vertices.clear();
  packet.indices.clear();
  packet.model_to_gc_local.clear();
  packet.material_name.clear();
  packet.has_texture = true;
  packet.use_vertex_color = false;
  packet.skinned = true;
  packet.source_name =
      replacement.owner ? replacement.owner->source_name : std::string("PS3-DMF");
  packet.material_name = cluster.material_name;
  packet.model_to_gc_local = replacement.model_to_gc_local;
  packet.vertices.resize(cluster.positions.size());

  for (std::size_t i = 0; i < packet.vertices.size(); ++i)
  {
    packet.vertices[i].position = cluster.positions[i];
    if (i < cluster.normals.size())
      packet.vertices[i].normal = cluster.normals[i];
    if (i < cluster.uv0.size())
      packet.vertices[i].uv0 = cluster.uv0[i];
    packet.vertices[i].matrix_index = replacement.position_matrix_indices[i];
  }

  packet.indices.reserve(cluster.indices.size());
  for (u16 index : cluster.indices)
  {
    if (index >= packet.vertices.size())
      return false;
    packet.indices.push_back(index);
  }

  return true;
}
}  // namespace

Mode GetMode()
{
  static const Mode cached = [] {
    const char* value = std::getenv("MOH_NATIVE_RENDER");
    if (!value || !*value)
      return Mode::Off;

    const std::string v = Lower(value);
    if (v == "shadow" || v == "capture" || v == "probe")
      return Mode::Shadow;
    if (v == "native" || v == "prefer" || v == "prefer-native" || v == "prefer_native")
      return Mode::PreferNative;
    return Mode::Off;
  }();
  return cached;
}

const char* ModeName(Mode mode)
{
  switch (mode)
  {
  case Mode::Shadow: return "shadow";
  case Mode::PreferNative: return "prefer-native";
  default: return "off";
  }
}

void SetSubmitter(Submitter submitter, void* userdata)
{
  std::scoped_lock lock(s_mutex);
  s_submitter = submitter;
  s_userdata = userdata;
}

bool HasSubmitter()
{
  std::scoped_lock lock(s_mutex);
  return s_submitter != nullptr;
}

bool KeepOriginalGCGeometry()
{
  static const bool cached = [] {
    const char* value = std::getenv("MOH_NATIVE_GEOMETRY_SOURCE");
    if (!value || !*value)
      value = std::getenv("MOH_NATIVE_VFS");
    if (!value || !*value)
      return false;

    const std::string v = Lower(value);
    return v == "gc" || v == "gc-first" || v == "gc_first" ||
           v == "gc-only" || v == "gc_only";
  }();
  return cached;
}

bool BuildCurrentDraw(DrawPacket* out)
{
  if (!out)
    return false;

  // Do not silently substitute PS3 MSH/DMF when GC-original was requested.
  // The original GX draw is retained as the safe GC geometry path.
  if (KeepOriginalGCGeometry())
  {
    static bool logged = false;
    if (!logged)
    {
      logged = true;
      std::fprintf(stderr,
                   "[moh-native-render] geometry source=GC-original: retaining original GX geometry\n");
    }
    return false;
  }

  if (BuildStatic(out))
    return true;
  return BuildSkinned(out);
}

bool TrySubmitCurrentDraw()
{
  const Mode mode = GetMode();
  if (mode == Mode::Off)
    return false;

  Submitter submitter = nullptr;
  void* userdata = nullptr;
  {
    std::scoped_lock lock(s_mutex);
    submitter = s_submitter;
    userdata = s_userdata;
  }

  if (!submitter)
  {
    // Shadow mode is useful even before the final Vulkan/D3D/Metal backend:
    // prove that a PS3 MSH/DMF has been converted into a complete host-neutral
    // draw packet while keeping the original GX draw for pixels.
    DrawPacket shadow;
    if (BuildCurrentDraw(&shadow))
    {
      static unsigned shadow_logs = 0;
      if (shadow_logs++ < 128)
      {
        std::fprintf(stderr,
                     "[moh-native-render] SHADOW PACKET %s source=%s material=%s "
                     "vertices=%zu indices=%zu matrices=%zu -> GX mirror\n",
                     shadow.skinned ? "DMF" : "MSH", shadow.source_name.c_str(),
                     shadow.material_name.c_str(), shadow.vertices.size(),
                     shadow.indices.size(), shadow.model_to_gc_local.size());
      }
    }
    else
    {
      static bool waiting_logged = false;
      if (!waiting_logged)
      {
        waiting_logged = true;
        std::fprintf(stderr,
                     "[moh-native-render] mode=%s waiting for a matched PS3 MSH/DMF -> GX fallback\n",
                     ModeName(mode));
      }
    }
    return false;
  }

  // Submission is synchronous; reuse capacity across draws on the render thread.
  thread_local DrawPacket packet;
  if (!BuildCurrentDraw(&packet))
    return false;

  const bool accepted = submitter(packet, userdata);

  static unsigned logs = 0;
  if (logs++ < 128)
  {
    std::fprintf(stderr,
                 "[moh-native-render] %s source=%s material=%s vertices=%zu indices=%zu accepted=%d\n",
                 packet.skinned ? "DMF" : "MSH", packet.source_name.c_str(),
                 packet.material_name.c_str(), packet.vertices.size(), packet.indices.size(),
                 accepted ? 1 : 0);
  }

  return mode == Mode::PreferNative && accepted;
}
}  // namespace MOHFrontline::NativeRender
