#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "VideoCommon/MOHFrontline/Assets/Formats/WorldFormats.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/Meshes/StaticMesh.h"
#include "VideoCommon/PS3RemasterAssets.h"

// Cross-format level bridge for Frontline's static world data.
//
// These files are complementary, not interchangeable:
//   CPT  = remaster world draw descriptors/materials/textures
//   CDB  = collision triangles and collision bounds in world coordinates
//   BPD  = GC property/level data (little endian)
//   XPD  = PS3 property/level data (big endian)
//   PSP  = property/pathfinding BSP (GC LE / PS3 BE)
//
// CPT vertices already use world coordinates. NODE70 describes visibility bounds,
// not a model transform. The optional CDB recentering experiment is disabled by
// default: collision and visual geometry need not have identical bounds.
namespace MOHFrontline::WorldLevelRuntime
{
using WorldFormats::Bounds3;
using MOHFrontline::Meshes::StaticMesh;
using AssetInfo = PS3RemasterAssets::AssetInfo;

struct Sector
{
  int chunk = -1;
  std::string source;
  Bounds3 bounds;
  std::size_t vertices = 0;
  std::size_t triangles = 0;
};

struct Summary
{
  std::uint64_t generation = 0;
  std::string level;
  std::size_t cpt_files = 0;
  std::size_t cpt_links = 0;
  std::size_t cpt_textures = 0;
  std::size_t cpt_rejected = 0;
  std::size_t xpd_files = 0;
  std::size_t xpd_properties = 0;
  std::size_t xpd_rejected = 0;
  std::size_t bpd_files = 0;
  std::size_t bpd_properties = 0;
  std::size_t bpd_rejected = 0;
  std::size_t cdb_files = 0;
  std::size_t cdb_vertices = 0;
  std::size_t cdb_triangles = 0;
  std::size_t cdb_rejected = 0;
  std::size_t psp_files = 0;
  std::size_t psp_nodes = 0;
  std::size_t psp_leaves = 0;
  std::size_t psp_rejected = 0;
  Bounds3 cdb_bounds;
  std::vector<Sector> cdb_sectors;
};

inline Summary g_summary;

inline bool EnvEnabled(const char* name, bool fallback)
{
  const char* raw = std::getenv(name);
  if (!raw || !*raw)
    return fallback;
  std::string value(raw);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (value == "0" || value == "false" || value == "off" || value == "no")
    return false;
  if (value == "1" || value == "true" || value == "on" || value == "yes")
    return true;
  return fallback;
}

inline bool Enabled()
{
  static const bool enabled =
      EnvEnabled("MOH_PS3_WORLD_DEBUG", false) ||
      EnvEnabled("MOH_PS3_WORLD_FORMAT_TRACE", false);
  return enabled;
}

inline std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

inline std::string Normalize(std::string value)
{
  std::replace(value.begin(), value.end(), '\\', '/');
  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());
  return Lower(std::move(value));
}

inline bool BelongsToLevel(const AssetInfo& asset, std::string_view level)
{
  const auto underscore = level.find('_');
  if (underscore == std::string_view::npos || underscore == 0)
    return false;
  const std::string scope = "data/" + std::string(level.substr(0, underscore)) + "/" +
                            std::string(level) + "/";
  return Normalize(asset.relative_path).find(scope) != std::string::npos;
}

inline int ChunkIndex(std::string_view name)
{
  std::string lower = Lower(std::string(name));
  const auto dot = lower.find_last_of('.');
  if (dot == std::string::npos)
    return -1;
  const auto marker = lower.rfind("_c", dot);
  if (marker == std::string::npos || marker + 2 >= dot)
    return -1;
  int value = 0;
  for (std::size_t p = marker + 2; p < dot; ++p)
  {
    if (!std::isdigit(static_cast<unsigned char>(lower[p])))
      return -1;
    value = value * 10 + (lower[p] - '0');
    if (value > 4096)
      return -1;
  }
  return value;
}

inline Bounds3 MeshBounds(const std::vector<std::shared_ptr<StaticMesh>>& meshes)
{
  Bounds3 out;
  for (const auto& mesh : meshes)
  {
    if (!mesh)
      continue;
    for (const auto& sub : mesh->submeshes)
      for (const auto& vertex : sub.position_uv)
        out.Include(vertex.position);
  }
  return out;
}

inline float Diagonal(const Bounds3& bounds)
{
  if (!bounds.valid)
    return 0.0f;
  const auto e = bounds.Extent();
  return std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
}

inline void Rebuild(std::string_view level)
{
  if (!Enabled())
  {
    g_summary = {};
    return;
  }

  Summary next;
  next.generation = PS3RemasterAssets::GetIndexGeneration();
  next.level = Lower(std::string(level));
  if (next.level.empty() || !PS3RemasterAssets::IsReady())
  {
    g_summary = std::move(next);
    return;
  }

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    if (!BelongsToLevel(asset, next.level))
      continue;
    const std::string name = Lower(asset.filename);
    const bool cpt = name.ends_with(".cpt") && name.find("_art") != std::string::npos;
    const bool xpd = name.ends_with(".xpd");
    const bool bpd = name.ends_with(".bpd");
    const bool cdb = name.ends_with(".cdb");
    const bool psp = name.ends_with(".psp");
    if (!cpt && !xpd && !bpd && !cdb && !psp)
      continue;

    const auto bytes = PS3RemasterAssets::ReadBinary(asset);
    if (bytes.empty())
    {
      if (cpt) ++next.cpt_rejected;
      if (xpd) ++next.xpd_rejected;
      if (bpd) ++next.bpd_rejected;
      if (cdb) ++next.cdb_rejected;
      if (psp) ++next.psp_rejected;
      continue;
    }

    if (cpt)
    {
      if (const auto parsed = WorldFormats::PS3CPT::Parse(bytes))
      {
        ++next.cpt_files;
        for (std::uint32_t link_index = 0; link_index < parsed->tables[2].count; ++link_index)
        {
          const auto link = parsed->ResolveLink(link_index);
          if (!link)
            continue;
          ++next.cpt_links;
          for (unsigned slot = 0; slot < 2; ++slot)
            if (parsed->MaterialTexture(*link, slot))
              ++next.cpt_textures;
        }
      }
      else
      {
        ++next.cpt_rejected;
      }
      continue;
    }

    if (xpd)
    {
      if (const auto parsed = WorldFormats::XPD::Parse(bytes))
      {
        ++next.xpd_files;
        next.xpd_properties += parsed->layout.properties.size();
      }
      else
      {
        ++next.xpd_rejected;
      }
      continue;
    }

    if (bpd)
    {
      if (const auto parsed = WorldFormats::BPD::Parse(bytes))
      {
        ++next.bpd_files;
        next.bpd_properties += parsed->properties.size();
      }
      else
      {
        ++next.bpd_rejected;
      }
      continue;
    }

    if (cdb)
    {
      if (const auto parsed = WorldFormats::CDB::Parse(bytes))
      {
        ++next.cdb_files;
        next.cdb_vertices += parsed->vertices.count;
        next.cdb_triangles += parsed->triangles.count;
        const Bounds3 bounds = WorldFormats::CDBBounds(*parsed);
        next.cdb_bounds.Include(bounds);
        if (bounds.valid)
          next.cdb_sectors.push_back(
              {ChunkIndex(name), asset.relative_path, bounds, parsed->vertices.count,
               parsed->triangles.count});
      }
      else
      {
        ++next.cdb_rejected;
      }
      continue;
    }

    if (psp)
    {
      if (const auto parsed = WorldFormats::PSP::ParseAuto(bytes))
      {
        ++next.psp_files;
        next.psp_nodes += parsed->nodes.size();
        next.psp_leaves += parsed->leaves.size();
      }
      else
      {
        ++next.psp_rejected;
      }
    }
  }

  std::sort(next.cdb_sectors.begin(), next.cdb_sectors.end(),
            [](const Sector& a, const Sector& b) { return a.chunk < b.chunk; });
  g_summary = std::move(next);

  std::fprintf(stderr,
               "[moh-ps3-world-set] READY: level=%s CPT=%zu links=%zu texrefs=%zu reject=%zu "
               "XPD=%zu props=%zu reject=%zu BPD=%zu props=%zu reject=%zu "
               "CDB=%zu verts=%zu tris=%zu sectors=%zu reject=%zu "
               "PSP=%zu nodes=%zu leaves=%zu reject=%zu\n",
               g_summary.level.c_str(), g_summary.cpt_files, g_summary.cpt_links,
               g_summary.cpt_textures, g_summary.cpt_rejected, g_summary.xpd_files,
               g_summary.xpd_properties, g_summary.xpd_rejected, g_summary.bpd_files,
               g_summary.bpd_properties, g_summary.bpd_rejected, g_summary.cdb_files,
               g_summary.cdb_vertices, g_summary.cdb_triangles, g_summary.cdb_sectors.size(),
               g_summary.cdb_rejected, g_summary.psp_files, g_summary.psp_nodes,
               g_summary.psp_leaves, g_summary.psp_rejected);
}

inline void Clear()
{
  g_summary = {};
}

inline const Sector* FindCDBSector(int chunk)
{
  if (chunk < 0)
    return nullptr;
  for (const auto& sector : g_summary.cdb_sectors)
    if (sector.chunk == chunk)
      return &sector;
  return nullptr;
}

// Diagnostic only: visual and collision envelopes need not share a center.
// Retail CPT vertices already use world space; recentering moves valid geometry.
inline bool AnchorCPTChunk(const AssetInfo& cpt, std::vector<std::shared_ptr<StaticMesh>>* meshes)
{
  if (!meshes || meshes->empty() || !EnvEnabled("MOH_PS3_WORLD_CDB_ANCHOR", false))
    return false;
  const int chunk = ChunkIndex(cpt.filename);
  const Sector* sector = FindCDBSector(chunk);
  if (!sector || !sector->bounds.valid)
    return false;

  const Bounds3 source = MeshBounds(*meshes);
  if (!source.valid)
    return false;
  const float cpt_diag = Diagonal(source);
  const float cdb_diag = Diagonal(sector->bounds);
  if (!std::isfinite(cpt_diag) || !std::isfinite(cdb_diag) || cpt_diag < 0.001f ||
      cdb_diag < 0.001f)
    return false;

  // Collision and visual geometry do not have identical envelopes, but a
  // wildly different diagonal means this is not a safe sector anchor.
  const float ratio = cpt_diag > cdb_diag ? cpt_diag / cdb_diag : cdb_diag / cpt_diag;
  if (ratio > 4.0f)
  {
    static unsigned reject_logs = 0;
    if (reject_logs++ < 32)
      std::fprintf(stderr,
                   "[moh-ps3-world-set] CPT/CDB ANCHOR REJECT: cpt=%s cdb=%s chunk=%d "
                   "diag=%.3f/%.3f ratio=%.3f\n",
                   cpt.relative_path.c_str(), sector->source.c_str(), chunk, cpt_diag, cdb_diag,
                   ratio);
    return false;
  }

  const auto cpt_center = source.Center();
  const auto cdb_center = sector->bounds.Center();
  const std::array<float, 3> translation{
      cdb_center[0] - cpt_center[0], cdb_center[1] - cpt_center[1],
      cdb_center[2] - cpt_center[2]};
  const float distance = std::sqrt(translation[0] * translation[0] +
                                   translation[1] * translation[1] +
                                   translation[2] * translation[2]);
  if (!std::isfinite(distance) || distance > 10000.0f)
    return false;

  for (auto& mesh : *meshes)
  {
    if (!mesh)
      continue;
    for (auto& sub : mesh->submeshes)
    {
      for (auto& vertex : sub.position_uv)
      {
        for (unsigned axis = 0; axis < 3; ++axis)
          vertex.position[axis] += translation[axis];
      }
      sub.material_hints.push_back(
          "@cdb-sector-anchor=" + std::to_string(chunk) + ";dx=" +
          std::to_string(translation[0]) + ";dy=" + std::to_string(translation[1]) +
          ";dz=" + std::to_string(translation[2]));
    }
  }

  static unsigned logs = 0;
  if (logs++ < 64)
  {
    std::fprintf(stderr,
                 "[moh-ps3-world-set] CPT/CDB ANCHOR: cpt=%s cdb=%s chunk=%d meshes=%zu "
                 "diag=%.3f/%.3f translation=(%.3f %.3f %.3f)\n",
                 cpt.relative_path.c_str(), sector->source.c_str(), chunk, meshes->size(),
                 cpt_diag, cdb_diag, translation[0], translation[1], translation[2]);
  }
  return true;
}
}  // namespace MOHFrontline::WorldLevelRuntime
