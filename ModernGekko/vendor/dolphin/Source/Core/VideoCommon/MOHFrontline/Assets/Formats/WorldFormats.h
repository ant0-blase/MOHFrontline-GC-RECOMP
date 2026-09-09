#pragma once

#include <cstring>
#include <unordered_set>
#include <vector>

#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCCompartment.h"

namespace MOHFrontline::WorldFormats
{
using GCCompartment::Table;
using CDB = GCCompartment::CDB; // Version 7 has the same BE pointer layout on GC and PS3.

struct PS3CPT
{
  GCCompartment::Reader data;
  std::array<Table, 5> tables{};
  static std::optional<PS3CPT> Parse(std::span<const unsigned char> bytes)
  {
    PS3CPT out{{bytes}};
    if (!out.data.Range(0, 0x38) || out.data.U32(0) != 9 || out.data.U32(8) != bytes.size())
      return std::nullopt;
    constexpr unsigned strides[] = {0x94, 0x94, 0x14, 0x70, 4};
    for (unsigned i = 0; i < out.tables.size(); ++i)
    {
      auto& t = out.tables[i];
      t = {out.data.U32(0x10 + i * 8), out.data.U32(0x14 + i * 8), strides[i]};
      if (t.count && (t.offset < 0x38 || !out.data.Range(t.offset, t.count, t.stride)))
        return std::nullopt;
    }
    return out;
  }
  std::optional<std::uint32_t> Descriptor(std::uint32_t chunk) const
  {
    if (chunk >= tables[2].count)
      return std::nullopt;
    const auto p = tables[2].offset + chunk * 0x14;
    const auto node = data.U32(p), material = data.U32(p + 8), descriptor = data.U32(p + 12);
    if (!tables[3].Contains(node) ||
        (!tables[0].Contains(material) && !tables[1].Contains(material)) ||
        descriptor < 0x38 || !data.Range(descriptor, 16))
      return std::nullopt;
    return descriptor;
  }
};

// GC BPD/PSP are little endian despite running on a big-endian CPU. PS3
// XPD/PSP are big endian. Never apply the CPT/CDB byte order blindly to them.
struct PropertyReader : GCCompartment::Reader
{
  bool little = false;
  std::uint32_t U32(std::size_t p) const
  {
    if (!little) return GCCompartment::Reader::U32(p);
    return std::uint32_t(bytes[p]) | (std::uint32_t(bytes[p + 1]) << 8) |
           (std::uint32_t(bytes[p + 2]) << 16) | (std::uint32_t(bytes[p + 3]) << 24);
  }
  std::uint16_t U16(std::size_t p) const
  {
    return little ? std::uint16_t(bytes[p] | (std::uint16_t(bytes[p + 1]) << 8)) :
                    GCCompartment::Reader::U16(p);
  }
  float Float(std::size_t p) const { return std::bit_cast<float>(U32(p)); }
};

struct Property
{
  std::uint32_t type = 0, offset = 0, size = 0;
};

struct BPD
{
  PropertyReader data;
  // Paths, pathfinding metadata, texture pointers, properties, particles,
  // animated lights, light volumes, particle tweakers, system tweakers.
  std::array<Table, 9> tables{};
  std::vector<Property> properties;
  static std::optional<BPD> Parse(std::span<const unsigned char> bytes)
  {
    BPD out;
    out.data.bytes = bytes;
    if (!out.data.Range(0, 0x60)) return std::nullopt;
    out.data.little = bytes[0] == 9;
    if (out.data.U32(0) != 9 || out.data.U32(4) != bytes.size()) return std::nullopt;
    constexpr unsigned strides[] = {12, 1, 4, 8, 240, 124, 48, 8, 8};
    for (unsigned i = 0; i < out.tables.size(); ++i)
    {
      auto& t = out.tables[i];
      t = {out.data.U32(0x10 + i * 8), out.data.U32(0x14 + i * 8), strides[i]};
      if (t.count && (t.offset < 0x60 || !out.data.Range(t.offset, t.count, t.stride)))
        return std::nullopt;
    }
    // xyzProperty records carry their own byte size; they are not an array
    // of one platform-wide struct. PatchUpAllPropertyData (GC 8004372c).
    std::uint64_t p = out.tables[3].offset;
    out.properties.reserve(out.tables[3].count);
    for (std::uint32_t i = 0; i < out.tables[3].count; ++i)
    {
      if (!out.data.Range(p, 8)) return std::nullopt;
      const auto size = out.data.U32(p + 4);
      if (size < 8 || !out.data.Range(p, size)) return std::nullopt;
      out.properties.push_back({out.data.U32(p), static_cast<std::uint32_t>(p), size});
      p += size;
    }
    return out;
  }
};

struct BSPLeaf
{
  std::uint32_t offset = 0;
  // Property IDs, lights, particles, proximity pairs, and pathfinding IDs.
  std::array<Table, 5> lists{};
};

struct BSPNode
{
  std::uint32_t offset = 0;
  float split = 0;
  std::array<std::uint32_t, 2> children{};
  std::array<bool, 2> leaf{};
};

struct PSP
{
  PropertyReader data;
  std::vector<BSPNode> nodes;
  std::vector<BSPLeaf> leaves;
  static std::optional<PSP> Parse(std::span<const unsigned char> bytes, bool little)
  {
    PSP out;
    out.data.bytes = bytes;
    out.data.little = little;
    if (!out.data.Range(0, 0x28) || std::memcmp(bytes.data(), "PBSP_HEADER", 11) != 0)
      return std::nullopt;
    // Bytes 0x0c..0x1f are stale exporter pointers, not offsets or a version.
    // LoadPropBSPTree (GC 80042dc4) only uses the root at +0x24.
    struct Work { std::uint32_t offset; bool leaf; bool exit; };
    std::vector<Work> pending{{out.data.U32(0x24), false, false}};
    std::unordered_set<std::uint32_t> active, visited_nodes, visited_leaves;
    while (!pending.empty())
    {
      const auto work = pending.back();
      pending.pop_back();
      const auto p = work.offset;
      if (work.exit) { active.erase(p); continue; }
      if (p < 0x28 || !out.data.Range(p, work.leaf ? 0x30 : 0x10)) return std::nullopt;
      if (work.leaf)
      {
        if (visited_nodes.contains(p)) return std::nullopt;
        if (!visited_leaves.insert(p).second) continue;
        BSPLeaf leaf;
        leaf.offset = p;
        constexpr unsigned fields[] = {0x20, 0x24, 0x28, 0x1c, 0x2c};
        for (unsigned i = 0; i < leaf.lists.size(); ++i)
        {
          const auto count = i == 0 ? out.data.U32(p) : out.data.U16(p + 2 + i * 2);
          auto& t = leaf.lists[i];
          t = {out.data.U32(p + fields[i]), count, i == 3 ? 4u : 2u};
          // Empty leaf lists also contain uninitialised exporter values.
          if (t.count && (t.offset < 0x28 || !out.data.Range(t.offset, t.count, t.stride)))
            return std::nullopt;
        }
        out.leaves.push_back(leaf);
      }
      else
      {
        if (active.contains(p) || visited_leaves.contains(p)) return std::nullopt;
        if (!visited_nodes.insert(p).second) continue;
        BSPNode node{p, out.data.Float(p), {out.data.U32(p + 4), out.data.U32(p + 8)},
                     {bytes[p + 12] != 0, bytes[p + 13] != 0}};
        if (!std::isfinite(node.split)) return std::nullopt;
        out.nodes.push_back(node);
        active.insert(p);
        pending.push_back({p, false, true});
        for (unsigned i = 0; i < 2; ++i)
          pending.push_back({node.children[i], node.leaf[i], false});
      }
    }
    return out;
  }
};
}  // namespace MOHFrontline::WorldFormats
