#include "VideoCommon/PS3MeshPort.h"
#include "Common/Hash.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include <atomic>
#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/MSH.h"
#include "VideoCommon/PS3RemasterAssets.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace PS3MeshPort
{
namespace
{
u16 BE16(const u8* p)
{
  return static_cast<u16>((u16(p[0]) << 8) | u16(p[1]));
}

u32 BE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u32 LE32(const u8* p)
{
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}
std::mutex g_msh_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<StaticMesh>> g_msh_cache;
struct DisplayListIdentity
{
  StaticDrawMatch match;
  u32 size = 0;
  u64 command_hash = 0;
};
std::unordered_map<u32, DisplayListIdentity> g_display_lists;
thread_local StaticDrawMatch g_current_draw;
std::atomic<u64> g_matches{0}, g_draws{0}, g_vertices{0}, g_indices{0}, g_fallbacks{0};
std::unordered_set<std::string> g_draw_logged;

std::mutex g_dmf_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<DMFResource>> g_dmf_cache;
std::mutex g_skl_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<SKLInfo>> g_skl_cache;

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string BaseName(std::string_view path)
{
  const auto slash = path.find_last_of("/\\:");
  return Lower(std::string(slash == std::string_view::npos ? path : path.substr(slash + 1)));
}

std::string CanonicalModelName(std::string_view path)
{
  std::string name = BaseName(path);
  if (name.ends_with(".msf"))
    name.replace(name.size() - 4, 4, ".msh");
  return name;
}

std::string FixedString(const u8* p, std::size_t size)
{
  std::size_t n = 0;
  while (n < size && p[n] != 0) ++n;
  return std::string(reinterpret_cast<const char*>(p), n);
}

bool EnvSwitchLocal(const char* name, bool fallback)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  const std::string lower = Lower(value);
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
    return true;
  if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
    return false;
  return fallback;
}

[[maybe_unused]] float EnvFloatLocal(const char* name, float fallback, float minimum, float maximum)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  if (end == value || !std::isfinite(parsed))
    return fallback;
  return std::clamp(parsed, minimum, maximum);
}

struct Bounds3
{
  std::array<float, 3> minimum{
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity()};
  std::array<float, 3> maximum{
      -std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity()};
  bool valid = false;
};

bool IncludePoint(Bounds3* bounds, const std::array<float, 3>& p)
{
  if (!bounds)
    return false;

  for (float v : p)
  {
    if (!std::isfinite(v) || std::abs(v) > 10000000.0f)
      return false;
  }

  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    bounds->minimum[axis] = std::min(bounds->minimum[axis], p[axis]);
    bounds->maximum[axis] = std::max(bounds->maximum[axis], p[axis]);
  }
  bounds->valid = true;
  return true;
}

Bounds3 BoundsFromGC(std::span<const u8> vertices, u32 count, u32 stride, u32 position_offset)
{
  Bounds3 out;
  if (!count || stride < sizeof(float) * 3 ||
      position_offset > stride || sizeof(float) * 3 > stride - position_offset ||
      vertices.size() < std::size_t(count) * stride)
  {
    return out;
  }

  for (u32 i = 0; i < count; ++i)
  {
    std::array<float, 3> p{};
    std::memcpy(p.data(), vertices.data() + std::size_t(i) * stride + position_offset,
                sizeof(float) * 3);
    if (!IncludePoint(&out, p))
      return {};
  }
  return out;
}

Bounds3 BoundsFromPS3(const Submesh& submesh)
{
  Bounds3 out;
  if (submesh.position_uv.size() != submesh.vertex_count)
    return out;

  for (const auto& vertex : submesh.position_uv)
  {
    if (!IncludePoint(&out, vertex.position))
      return {};
  }
  return out;
}

float BoundsScore(const Bounds3& gc, const Bounds3& ps3)
{
  if (!gc.valid || !ps3.valid)
    return std::numeric_limits<float>::infinity();

  float extent_error = 0.0f;
  float center_error = 0.0f;
  float global_scale = 0.001f;

  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    const float ge = gc.maximum[axis] - gc.minimum[axis];
    const float pe = ps3.maximum[axis] - ps3.minimum[axis];
    global_scale = std::max(global_scale, std::max(std::abs(ge), std::abs(pe)));
    const float denom = std::max(0.001f, std::max(std::abs(ge), std::abs(pe)));
    extent_error += std::abs(ge - pe) / denom;
  }

  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    const float gc_center = (gc.minimum[axis] + gc.maximum[axis]) * 0.5f;
    const float ps3_center = (ps3.minimum[axis] + ps3.maximum[axis]) * 0.5f;
    center_error += std::abs(gc_center - ps3_center) / global_scale;
  }

  return extent_error / 3.0f + (center_error / 3.0f) * 0.20f;
}

bool BelongsToLevel(const PS3RemasterAssets::AssetInfo& asset, std::string_view level)
{
  if (level.empty())
    return false;

  std::string path = Lower(asset.relative_path);
  std::string level_l = Lower(std::string(level));
  const std::string needle = "/" + level_l + "/";
  return path.find(needle) != std::string::npos;
}
}  // namespace

void ClearMSHCache()
{
  std::scoped_lock lock(g_msh_cache_mutex);
  g_msh_cache.clear();
  g_display_lists.clear();
}

void ClearDMFCache()
{
  std::scoped_lock lock(g_dmf_cache_mutex);
  g_dmf_cache.clear();
}

void ClearSKLCache()
{
  std::scoped_lock lock(g_skl_cache_mutex);
  g_skl_cache.clear();
}

void PreloadCurrentLevelMSH(std::string_view level)
{
  if (!PS3AssetPort::IsMSHEnabled() || !PS3RemasterAssets::IsReady() || level.empty())
    return;

  std::unordered_map<std::string, std::shared_ptr<StaticMesh>> next;
  std::size_t candidates = 0;
  std::size_t decoded = 0;
  std::size_t rejected = 0;

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string filename = Lower(asset.filename);
    if (!filename.ends_with(".msh") || !BelongsToLevel(asset, level))
      continue;

    ++candidates;
    const std::vector<u8> bytes = PS3RemasterAssets::ReadBinary(asset);
    if (bytes.empty())
    {
      ++rejected;
      continue;
    }

    auto mesh = std::make_shared<StaticMesh>();
    mesh->source_name = asset.relative_path;
    if (!ParseMSHv8(bytes, mesh.get()) || !IsHostRenderable(*mesh))
    {
      ++rejected;
      continue;
    }

    ++decoded;
    if (decoded <= 32)
    {
      std::size_t vertices = 0;
      std::size_t indices = 0;
      for (const auto& sub : mesh->submeshes)
      {
        vertices += sub.vertex_count;
        indices += sub.index_count;
      }
      std::fprintf(stderr,
                   "[moh-ps3-msh] READY: %s submeshes=%zu vertices=%zu indices=%zu\n",
                   asset.relative_path.c_str(), mesh->submeshes.size(), vertices, indices);
    }

    next[filename] = mesh;
    next[Lower(asset.relative_path)] = mesh;
  }

  {
    std::scoped_lock lock(g_msh_cache_mutex);
    g_display_lists.clear();
    g_msh_cache = std::move(next);
  }

  std::fprintf(stderr,
               "[moh-ps3-msh] ALL-MSH cache ready: level=%.*s candidates=%zu decoded=%zu rejected=%zu keys=%zu\n",
               static_cast<int>(level.size()), level.data(), candidates, decoded, rejected,
               CachedMSHCount());
}

void PreloadCurrentLevelDMF(std::string_view level)
{
  if (!PS3AssetPort::IsDMFEnabled() || !PS3RemasterAssets::IsReady() || level.empty())
    return;

  std::unordered_map<std::string, std::shared_ptr<DMFResource>> next;
  std::size_t candidates = 0;
  std::size_t decoded = 0;
  std::size_t rejected = 0;
  std::size_t total_meshes = 0;
  std::size_t total_materials = 0;

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string filename = Lower(asset.filename);
    if (!filename.ends_with(".dmf") || !BelongsToLevel(asset, level))
      continue;

    ++candidates;
    const std::vector<u8> bytes = PS3RemasterAssets::ReadBinary(asset);
    if (bytes.empty())
    {
      ++rejected;
      continue;
    }

    const DMFInfo info = InspectDMF(bytes);
    if (!info.valid)
    {
      ++rejected;
      continue;
    }

    ++decoded;
    total_meshes += info.mesh_count;
    total_materials += info.material_count;

    auto resource = std::make_shared<DMFResource>();
    resource->info = info;
    resource->source_name = asset.relative_path;
    resource->bytes = std::make_shared<const std::vector<u8>>(bytes);

    if (decoded <= 32)
    {
      std::fprintf(stderr,
                   "[moh-ps3-dmf] READY: %s model=%s version=0x%X meshes=%u materials=%u bones=%u\n",
                   asset.relative_path.c_str(), info.model_name.c_str(), info.version,
                   info.mesh_count, info.material_count, info.bone_ref_count);
    }

    next[filename] = resource;
    next[Lower(asset.relative_path)] = resource;
  }

  {
    std::scoped_lock lock(g_dmf_cache_mutex);
    g_dmf_cache = std::move(next);
  }

  std::fprintf(stderr,
               "[moh-ps3-dmf] cache ready: level=%.*s candidates=%zu decoded=%zu rejected=%zu "
               "meshes=%zu materials=%zu keys=%zu\n",
               static_cast<int>(level.size()), level.data(), candidates, decoded, rejected,
               total_meshes, total_materials, CachedDMFCount());
}

void PreloadCurrentLevelSKL(std::string_view level)
{
  if (!PS3AssetPort::IsSKLEnabled() || !PS3RemasterAssets::IsReady() || level.empty())
    return;

  std::unordered_map<std::string, std::shared_ptr<SKLInfo>> next;
  std::size_t candidates = 0;
  std::size_t decoded = 0;
  std::size_t rejected = 0;

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string filename = Lower(asset.filename);
    if (!filename.ends_with(".skl") || !BelongsToLevel(asset, level))
      continue;

    ++candidates;
    const std::vector<u8> bytes = PS3RemasterAssets::ReadBinary(asset);
    if (bytes.empty())
    {
      ++rejected;
      continue;
    }

    auto info = std::make_shared<SKLInfo>(InspectSKL(bytes));
    if (!info->valid)
    {
      ++rejected;
      continue;
    }

    info->source_name = asset.relative_path;
    ++decoded;

    if (decoded <= 16)
    {
      std::fprintf(stderr,
                   "[moh-ps3-skl] READY: %s bones=%u endian=%s names=%zu\n",
                   asset.relative_path.c_str(), info->bone_count,
                   info->big_endian ? "BE" : "LE", info->bone_names.size());
    }

    next[filename] = info;
    next[Lower(asset.relative_path)] = info;
  }

  {
    std::scoped_lock lock(g_skl_cache_mutex);
    g_skl_cache = std::move(next);
  }

  std::fprintf(stderr,
               "[moh-ps3-skl] cache ready: level=%.*s candidates=%zu decoded=%zu rejected=%zu keys=%zu\n",
               static_cast<int>(level.size()), level.data(), candidates, decoded, rejected,
               CachedSKLCount());
}

const StaticMesh* FindCachedMSH(std::string_view name_or_path)
{
  const std::string raw = Lower(std::string(name_or_path));
  const std::string base = CanonicalModelName(name_or_path);
  std::scoped_lock lock(g_msh_cache_mutex);

  if (const auto it = g_msh_cache.find(raw); it != g_msh_cache.end())
    return it->second.get();
  if (const auto it = g_msh_cache.find(base); it != g_msh_cache.end())
    return it->second.get();
  return nullptr;
}

const DMFResource* FindCachedDMF(std::string_view name_or_path)
{
  const std::string raw = Lower(std::string(name_or_path));
  const std::string base = BaseName(name_or_path);
  std::scoped_lock lock(g_dmf_cache_mutex);
  if (const auto it = g_dmf_cache.find(raw); it != g_dmf_cache.end()) return it->second.get();
  if (const auto it = g_dmf_cache.find(base); it != g_dmf_cache.end()) return it->second.get();
  return nullptr;
}

const SKLInfo* FindCachedSKL(std::string_view name_or_path)
{
  const std::string raw = Lower(std::string(name_or_path));
  const std::string base = BaseName(name_or_path);
  std::scoped_lock lock(g_skl_cache_mutex);
  if (const auto it = g_skl_cache.find(raw); it != g_skl_cache.end()) return it->second.get();
  if (const auto it = g_skl_cache.find(base); it != g_skl_cache.end()) return it->second.get();
  return nullptr;
}

std::size_t CachedMSHCount()
{
  std::scoped_lock lock(g_msh_cache_mutex);
  return g_msh_cache.size();
}

std::size_t CachedDMFCount()
{
  std::scoped_lock lock(g_dmf_cache_mutex);
  return g_dmf_cache.size();
}

std::size_t CachedSKLCount()
{
  std::scoped_lock lock(g_skl_cache_mutex);
  return g_skl_cache.size();
}

bool IsStaticDrawReplacementEnabled()
{
  return PS3AssetPort::IsMSHEnabled() && EnvSwitchLocal("MOH_PS3_MSH_DRAW", true) &&
         EnvSwitchLocal("MOH_PS3_MSH_REPLACE", true);
}

void RegisterGuestStaticMesh(std::string_view name, u32 address, std::span<const u8> bytes)
{
  if (!IsStaticDrawReplacementEnabled() || bytes.size() < 0x30 || BE32(bytes.data()) != 9)
    return;
  const auto level = MOHFrontline::NativeAssets::GetCurrentLevel();
  if (level.empty()) return;
  const std::string scope = "data/" + level.substr(0, 1) + "/" + level + "/";
  const std::string filename = CanonicalModelName(name) + ".msh";
  const auto* asset = PS3RemasterAssets::FindByRelativePath(scope + "level.viv::" + filename);
  if (!asset) asset = PS3RemasterAssets::FindByRelativePath(scope + filename);
  if (!asset) return;

  // First production milestone: a single rigid node/submesh on both platforms.
  // Multi-node hierarchy/material remapping must not be guessed by array order.
  const auto u = [&](std::size_t p) { return BE32(bytes.data() + p); };
  const u32 node = u(0x18);
  if (u(0x1c) != 1 || node > bytes.size() || bytes.size() - node < 32) return;
  const u32 geometry = u(node + 8);
  if (geometry > bytes.size() || bytes.size() - geometry < 36) return;
  const u32 dl = u(geometry + 32);
  const u32 count = u(geometry + 12);
  const u32 stride = u(geometry) & 1 ? 4 : 8;
  const std::uint64_t length = (std::uint64_t(count) * stride + 52 + 31) & ~31ULL;
  if (length <= 52 || length > 0x200000 || dl > bytes.size() || length > bytes.size() - dl)
    return;

  std::shared_ptr<StaticMesh> mesh;
  {
    std::scoped_lock lock(g_msh_cache_mutex);
    if (auto it = g_msh_cache.find(asset->relative_path); it != g_msh_cache.end()) mesh = it->second;
  }
  if (!mesh)
  {
    mesh = std::make_shared<StaticMesh>();
    mesh->source_name = asset->relative_path;
    if (!ParseMSHv8(PS3RemasterAssets::ReadBinary(*asset), mesh.get())) return;
    std::scoped_lock lock(g_msh_cache_mutex);
    g_msh_cache[asset->relative_path] = mesh;
  }
  if (mesh->submeshes.size() != 1) return;
  const auto& sub = mesh->submeshes[0];
  if (!sub.vertex_count || sub.position_uv.size() != sub.vertex_count ||
      sub.indices.empty() || sub.indices.size() % 3) return;
  auto normals = std::make_shared<std::vector<std::array<float, 3>>>(sub.vertex_count);
  for (std::size_t i = 0; i < sub.indices.size(); i += 3)
  {
    const auto ia = sub.indices[i], ib = sub.indices[i+1], ic = sub.indices[i+2];
    if (ia >= sub.vertex_count || ib >= sub.vertex_count || ic >= sub.vertex_count) return;
    const auto& a = sub.position_uv[ia].position;
    const auto& b = sub.position_uv[ib].position;
    const auto& c = sub.position_uv[ic].position;
    const std::array<float, 3> ab{b[0]-a[0], b[1]-a[1], b[2]-a[2]};
    const std::array<float, 3> ac{c[0]-a[0], c[1]-a[1], c[2]-a[2]};
    const std::array<float, 3> n{ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]};
    for (u16 index : {ia, ib, ic}) for (unsigned j = 0; j < 3; ++j) (*normals)[index][j] += n[j];
  }
  for (auto& n : *normals)
  {
    const float normal_length = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (normal_length > 0.000001f) for (auto& x : n) x /= normal_length;
  }
  if (sub.has_normal)
    for (std::size_t i = 0; i < sub.position_uv.size(); ++i)
      (*normals)[i] = sub.position_uv[i].normal;
  DisplayListIdentity identity;
  identity.match.owner = mesh;
  identity.match.mesh = mesh.get();
  identity.match.submesh = &mesh->submeshes[0];
  identity.match.normals = std::move(normals);
  identity.match.guest_resource = address;
  identity.match.display_list = (address + dl) & 0x1fffffff;
  identity.size = static_cast<u32>(length);
  // Constructor patches the first 52 bytes of CP/VAT commands only. Retain
  // the authored index stream as a lifetime guard against recycled addresses.
  identity.command_hash = Common::GetHash64(bytes.data() + dl + 52, length - 52, 0);
  std::scoped_lock lock(g_msh_cache_mutex);
  g_display_lists[identity.match.display_list] = identity;
  ++g_matches;
  std::fprintf(stderr, "[moh-ps3-msh] LIVE EXACT MESH: level=%s gc=%s ps3=%s submeshes=1 vertices=%u indices=%zu DL=%08x\n",
               level.c_str(), filename.c_str(), asset->relative_path.c_str(), sub.vertex_count,
               sub.indices.size(), identity.match.display_list);
}

StaticDrawMatch FindDisplayList(u32 address, std::span<const u8> commands)
{
  if (!IsStaticDrawReplacementEnabled() || commands.size() <= 52) return {};
  std::scoped_lock lock(g_msh_cache_mutex);
  const auto it = g_display_lists.find(address & 0x1fffffff);
  if (it == g_display_lists.end()) return {};
  if (it->second.size != commands.size() || it->second.command_hash !=
      Common::GetHash64(commands.data() + 52, commands.size() - 52, 0)) return {};
  return it->second.match;
}
void SetDisplayListMatch(StaticDrawMatch match) { g_current_draw = std::move(match); }
StaticDrawMatch MatchStaticDraw(std::span<const u8> gc_vertices, u32 count, u32 stride, u32 offset)
{
  if (!g_current_draw || !IsStaticDrawReplacementEnabled()) return {};
  // Bounds validate unit scale/orientation for this already exact identity.
  // They never search for or select a different asset.
  const auto gc = BoundsFromGC(gc_vertices, count, stride, offset);
  const auto ps3 = BoundsFromPS3(*g_current_draw.submesh);
  const float score = BoundsScore(gc, ps3);
  if (!std::isfinite(score) || score > 0.075f)
  {
    if (++g_fallbacks <= 12)
      std::fprintf(stderr, "[moh-ps3-msh] fallback GC: reason=coordinate/bounds mismatch mesh=%s score=%.5f\n",
                   g_current_draw.mesh->source_name.c_str(), score);
    return {};
  }
  return g_current_draw;
}
void NotifyStaticDrawSubmitted()
{
  if (!g_current_draw) return;
  ++g_draws;
  g_vertices += g_current_draw.submesh->vertex_count;
  g_indices += g_current_draw.submesh->indices.size();
  if (g_draw_logged.insert(g_current_draw.mesh->source_name).second)
    std::fprintf(stderr, "[moh-ps3-msh] FIRST REAL PS3 DRAW: gc_resource=%08x ps3=%s DL=%08x | replacement active; current GX model/view/projection and CSM preserved\n",
                 g_current_draw.guest_resource, g_current_draw.mesh->source_name.c_str(),
                 g_current_draw.display_list);
}
void PrintDrawStatistics()
{
  std::fprintf(stderr, "[moh-ps3-msh] stats: matches=%llu replacements=%llu draws=%llu vertices=%llu indices=%llu fallbacks=%llu\n",
               (unsigned long long)g_matches.load(), (unsigned long long)g_draws.load(),
               (unsigned long long)g_draws.load(), (unsigned long long)g_vertices.load(),
               (unsigned long long)g_indices.load(), (unsigned long long)g_fallbacks.load());
}

bool ParseMSHv8(std::span<const u8> bytes, StaticMesh* out)
{
  if (!PS3AssetPort::IsMSHEnabled())
    return false;

  const bool ok = MOHFrontline::PS3::MSH::Decode(bytes, out);
  static unsigned logs = 0;
  if (ok && out && logs++ < 64)
  {
    std::fprintf(stderr, "[moh-ps3-msh] decoded static MSH: submeshes=%zu\n",
                 out->submeshes.size());
  }
  return ok;
}

DMFInfo InspectDMF(std::span<const u8> bytes)
{
  DMFInfo out;

  if (!PS3AssetPort::IsDMFEnabled())
    return out;

  // Binary magic is D M F NUL.  "DMF\\0" is five source characters and
  // comparing its first four bytes checks D M F '\\', rejecting every valid
  // Frontline DMF before the PS3 0x0502 parser can run.
  static constexpr std::array<u8, 4> dmf_magic = {'D', 'M', 'F', 0};
  if (bytes.size() < 0x5c ||
      !std::equal(dmf_magic.begin(), dmf_magic.end(), bytes.begin()))
  {
    return out;
  }

  // Frontline PS3 remaster DMF revision seen in the RPCS3 dump:
  // bytes +04 = 05 02 00 00 -> revision 0x0502 + flags 0x0000.
  const u32 ps3_revision =
      (u32(bytes[4]) << 8) |
      u32(bytes[5]);

  const u32 ps3_flags =
      (u32(bytes[6]) << 8) |
      u32(bytes[7]);

  if (ps3_revision >= 0x0100 &&
      ps3_revision <= 0x0FFF &&
      ps3_flags <= 0x00FF)
  {
    out.version =
        ps3_revision;

    out.mesh_count =
        BE32(bytes.data() + 0x20);

    out.material_count =
        BE32(bytes.data() + 0x28);

    // +0x48 = count.
    // +0x4C is the pointer/table field and MUST NOT be read as a count.
    out.bone_ref_count =
        BE32(bytes.data() + 0x48);
  }
  else
  {
    // Preserve conservative support for the older documented DMF family.
    const u32 version_le =
        LE32(bytes.data() + 4);

    const u32 version_be =
        BE32(bytes.data() + 4);

    if (version_le > 0 &&
        version_le < 64)
    {
      out.version =
          version_le;

      out.mesh_count =
          LE32(bytes.data() + 0x20);

      out.material_count =
          LE32(bytes.data() + 0x28);

      out.bone_ref_count =
          LE32(bytes.data() + 0x4c);
    }
    else if (version_be > 0 &&
             version_be < 64)
    {
      out.version =
          version_be;

      out.mesh_count =
          BE32(bytes.data() + 0x20);

      out.material_count =
          BE32(bytes.data() + 0x28);

      out.bone_ref_count =
          BE32(bytes.data() + 0x4c);
    }
    else
    {
      return out;
    }
  }

  char name[9]{};

  std::memcpy(
      name,
      bytes.data() + 0x0c,
      8);

  out.model_name =
      name;

  if (out.mesh_count > 65535 ||
      out.material_count > 65535 ||
      out.bone_ref_count > 4096)
  {
    return {};
  }

  out.valid = true;

  return out;
}

SKLInfo InspectSKL(std::span<const u8> bytes)
{
  SKLInfo out;
  if (!PS3AssetPort::IsSKLEnabled() || bytes.size() < 0x20)
    return out;

  // Frontline skeletons occur in both byte orders across platforms.
  // PS3 samples use the readable "SKL1" form, while older little-endian
  // assets may appear as "1LKS".  Parse both without raw replacement.
  const bool be =
      bytes[0] == 'S' && bytes[1] == 'K' && bytes[2] == 'L' && bytes[3] == '1';
  const bool le =
      bytes[0] == '1' && bytes[1] == 'L' && bytes[2] == 'K' && bytes[3] == 'S';
  if (!be && !le)
    return out;

  const auto U32 = [be](const u8* p) { return be ? BE32(p) : LE32(p); };
  const u32 count_word = U32(bytes.data() + 4);
  u32 bone_count = count_word >> 16;
  if (!bone_count)
    bone_count = count_word & 0xffff;

  const u32 bone_data = U32(bytes.data() + 8);
  const u32 names = U32(bytes.data() + 12);
  const u32 names_end = U32(bytes.data() + 20);

  if (!bone_count || bone_count > 512 || names < 0x20 || names >= bytes.size() ||
      bone_data > bytes.size() || names_end > bytes.size())
  {
    return out;
  }

  // Each known Frontline SKL name record is 20 bytes:
  // pointer/offset + char name[16].  The actual bone node stream is kept in
  // the original PS3 resource; only identity/hierarchy binding is decoded here.
  const std::size_t names_bytes = static_cast<std::size_t>(bone_count) * 20;
  if (names_bytes > bytes.size() - names)
    return out;

  out.big_endian = be;
  out.bone_count = bone_count;
  out.bone_data_offset = bone_data;
  out.names_offset = names;
  out.names_end = names_end;
  out.bone_names.reserve(bone_count);

  for (u32 i = 0; i < bone_count; ++i)
  {
    const std::size_t record = names + static_cast<std::size_t>(i) * 20;
    const std::string name = FixedString(bytes.data() + record + 4, 16);
    if (name.empty())
      return {};
    out.bone_names.push_back(name);
  }

  out.valid = true;
  return out;
}

Replacement ResolveReplacement(std::string_view gc_resource_name)
{
  std::string name = CanonicalModelName(gc_resource_name);

  if (name.ends_with(".msh"))
  {
    if (const StaticMesh* mesh = FindCachedMSH(name))
      return {ReplacementKind::StaticMSH, mesh, nullptr, nullptr};
  }
  else if (name.ends_with(".dmf"))
  {
    if (const DMFResource* mesh = FindCachedDMF(name))
      return {ReplacementKind::SkinnedDMF, nullptr, mesh, nullptr};
  }
  else if (name.ends_with(".skl"))
  {
    if (const SKLInfo* skeleton = FindCachedSKL(name))
      return {ReplacementKind::SkeletonSKL, nullptr, nullptr, skeleton};
  }

  return {};
}

bool IsHostRenderable(const StaticMesh& mesh)
{
  if (mesh.submeshes.empty())
    return false;

  for (const auto& sub : mesh.submeshes)
  {
    if (sub.position_uv.size() != sub.vertex_count || sub.indices.empty() || !sub.vertex_stride)
      return false;

    // Require a position declaration. Semantic 0 is the position field in
    // the PS3 MSH v8 files observed in Frontline.
    const bool has_position =
        std::any_of(sub.attributes.begin(), sub.attributes.end(),
                    [](const Attribute& a) { return a.semantic == 0 && a.components >= 3; });
    if (!has_position)
      return false;
  }

  return true;
}
}  // namespace PS3MeshPort
