#include "VideoCommon/PS3MeshPort.h"
#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/MSH.h"
#include "VideoCommon/PS3RemasterAssets.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

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
std::mutex g_dmf_cache_mutex;
std::unordered_map<std::string, DMFInfo> g_dmf_cache;

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
}

void ClearDMFCache()
{
  std::scoped_lock lock(g_dmf_cache_mutex);
  g_dmf_cache.clear();
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

  std::unordered_map<std::string, DMFInfo> next;
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

    DMFInfo info = InspectDMF(bytes);
    if (!info.valid)
    {
      ++rejected;
      continue;
    }

    ++decoded;
    total_meshes += info.mesh_count;
    total_materials += info.material_count;

    if (decoded <= 32)
    {
      std::fprintf(stderr,
                   "[moh-ps3-dmf] READY: %s model=%s version=0x%X meshes=%u materials=%u bones=%u\n",
                   asset.relative_path.c_str(), info.model_name.c_str(), info.version,
                   info.mesh_count, info.material_count, info.bone_ref_count);
    }

    next[filename] = info;
    next[Lower(asset.relative_path)] = std::move(info);
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

const StaticMesh* FindCachedMSH(std::string_view name_or_path)
{
  const std::string raw = Lower(std::string(name_or_path));
  const std::string base = BaseName(name_or_path);
  std::scoped_lock lock(g_msh_cache_mutex);

  if (const auto it = g_msh_cache.find(raw); it != g_msh_cache.end())
    return it->second.get();
  if (const auto it = g_msh_cache.find(base); it != g_msh_cache.end())
    return it->second.get();
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
