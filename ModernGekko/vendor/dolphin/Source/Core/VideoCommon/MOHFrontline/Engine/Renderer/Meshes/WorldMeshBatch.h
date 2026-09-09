#pragma once
#include <algorithm>
#include <memory>
#include <string_view>
#include "StaticMesh.h"

namespace MOHFrontline::Meshes
{
inline std::shared_ptr<StaticMesh> BuildWorldCPTPack(
    const std::vector<std::shared_ptr<StaticMesh>>& meshes, std::size_t first,
    std::size_t count)
{
  if (count < 2 || first >= meshes.size() || count > meshes.size() - first)
    return {};

  // Preflight before copying geometry. One draw binds one material; merging
  // different CPT materials would paint every member with the same texture.
  const std::string_view first_source = meshes[first] ? std::string_view(meshes[first]->source_name) : std::string_view{};
  const auto source_end = first_source.find(".cpt#");
  if (source_end == std::string_view::npos)
    return {};
  const auto source_file = first_source.substr(0, source_end + 4);
  std::optional<std::uint32_t> material;
  std::size_t reserve_vertices = 0, reserve_indices = 0;
  for (std::size_t member = 0; member < count; ++member)
  {
    const auto& mesh = meshes[first + member];
    if (!mesh || mesh->submeshes.size() != 1)
      return {};
    const std::string_view source = mesh->source_name;
    if (source.substr(0, source.find('#')) != source_file)
      return {};
    const auto& sub = mesh->submeshes[0];
    if (std::any_of(sub.indices.begin(), sub.indices.end(),
                    [&](std::uint16_t index) { return index >= sub.vertex_count; }))
      return {};
    if (!sub.cpt_material_offset ||
        (member && sub.cpt_material_offset != material))
      return {};
    material = sub.cpt_material_offset;
    reserve_vertices += sub.vertex_count;
    reserve_indices += sub.indices.size();
    if (reserve_vertices > 65535u || reserve_indices > 300000u)
      return {};
  }

  auto pack = std::make_shared<StaticMesh>();
  Submesh merged;
  merged.has_uv0 = true;
  merged.has_uv1 = true;
  merged.has_normal = true;
  merged.vertex_stride = 32;
  merged.cpt_material_offset = material;
  merged.position_uv.reserve(reserve_vertices);
  merged.indices.reserve(reserve_indices);

  std::size_t vertex_base = 0;
  std::size_t total_indices = 0;
  for (std::size_t member = 0; member < count; ++member)
  {
    const auto& mesh = meshes[first + member];
    if (!mesh || mesh->submeshes.size() != 1)
      return {};
    const auto& sub = mesh->submeshes[0];
    if (!sub.vertex_count || sub.position_uv.size() != sub.vertex_count ||
        sub.indices.empty() || (sub.indices.size() % 3) != 0)
      return {};
    if (vertex_base + sub.vertex_count > 65535u ||
        total_indices + sub.indices.size() > 300000u)
      return {};

    if (member == 0)
      merged.attributes = sub.attributes;

    // v9.8: preserve the exact CPT pointer graph for every descriptor inside
    // an aggregate.  The current renderer still submits one merged draw, but
    // these span records retain the future split point needed for true
    // per-descriptor PS3 materials without re-decoding the CPT.
    const std::size_t member_first_index = total_indices;
    merged.material_hints.push_back(
        "@cpt-pack-span=member:" + std::to_string(member) +
        ";first-index:" + std::to_string(member_first_index) +
        ";index-count:" + std::to_string(sub.indices.size()) +
        ";first-vertex:" + std::to_string(vertex_base) +
        ";vertex-count:" + std::to_string(sub.vertex_count));
    for (const std::string& hint : sub.material_hints)
    {
      if (hint.rfind("@cpt-", 0) == 0)
        merged.material_hints.push_back("@cpt-pack-member=" + std::to_string(member) + ";" + hint);
    }

    merged.has_uv0 = merged.has_uv0 && sub.has_uv0;
    merged.has_uv1 = merged.has_uv1 && sub.has_uv1;
    merged.has_normal = merged.has_normal && sub.has_normal;
    merged.position_uv.insert(merged.position_uv.end(), sub.position_uv.begin(),
                              sub.position_uv.end());
    for (std::uint16_t index : sub.indices)
    {
      const std::size_t shifted = vertex_base + index;
      if (shifted > 65535u)
        return {};
      merged.indices.push_back(static_cast<std::uint16_t>(shifted));
    }
    vertex_base += sub.vertex_count;
    total_indices += sub.indices.size();
  }

  if (merged.position_uv.empty() || merged.indices.empty())
    return {};
  merged.vertex_count = static_cast<std::uint32_t>(merged.position_uv.size());
  merged.index_count = static_cast<std::uint32_t>(merged.indices.size());

  const std::string& first_name = meshes[first]->source_name;
  const std::size_t marker = first_name.find(".cpt#");
  if (marker == std::string::npos)
    return {};
  pack->source_name = first_name.substr(0, marker + 4) + "#cpt-pack-" +
                      std::to_string(first) + "x" + std::to_string(count);
  pack->submeshes.push_back(std::move(merged));
  return pack;
}

}
