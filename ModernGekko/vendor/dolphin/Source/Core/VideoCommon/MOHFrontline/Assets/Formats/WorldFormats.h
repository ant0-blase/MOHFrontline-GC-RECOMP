#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCCompartment.h"

namespace MOHFrontline::WorldFormats
{
using GCCompartment::Table;
using CDB = GCCompartment::CDB; // Version 7 has the same BE pointer layout on GC and PS3.

struct Bounds3
{
  bool valid = false;
  std::array<float, 3> minimum{}, maximum{};

  void Include(const std::array<float, 3>& p)
  {
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
      return;
    if (!valid)
    {
      valid = true;
      minimum = maximum = p;
      return;
    }
    for (unsigned axis = 0; axis < 3; ++axis)
    {
      minimum[axis] = std::min(minimum[axis], p[axis]);
      maximum[axis] = std::max(maximum[axis], p[axis]);
    }
  }

  void Include(const Bounds3& other)
  {
    if (!other.valid)
      return;
    Include(other.minimum);
    Include(other.maximum);
  }

  std::array<float, 3> Center() const
  {
    return {(minimum[0] + maximum[0]) * 0.5f,
            (minimum[1] + maximum[1]) * 0.5f,
            (minimum[2] + maximum[2]) * 0.5f};
  }

  std::array<float, 3> Extent() const
  {
    return {maximum[0] - minimum[0], maximum[1] - minimum[1], maximum[2] - minimum[2]};
  }
};

inline Bounds3 CDBBounds(const CDB& cdb)
{
  Bounds3 bounds;
  // Retail level CDB vertices are already world-space. Do not reconstruct
  // bounds from triangle planes: master/chunk databases may contain unused
  // vertices which are still useful for the authored sector envelope.
  for (std::uint32_t i = 0; i < cdb.vertices.count; ++i)
  {
    std::array<float, 3> p{};
    const std::uint64_t off = std::uint64_t(cdb.vertices.offset) + std::uint64_t(i) * 12u;
    if (cdb.data.Vector(off, p))
      bounds.Include(p);
  }
  return bounds;
}

struct PS3CPT
{
  struct Link
  {
    bool valid = false;
    std::uint32_t record = 0;
    std::uint32_t node_offset = 0;
    std::uint32_t material_offset = 0;
    std::uint32_t descriptor_offset = 0;
    std::uint32_t word1 = 0;
    std::uint32_t word4 = 0;
    std::uint32_t node_index = 0;
    std::uint32_t material_table = 0;
    std::uint32_t material_index = 0;
  };

  struct TextureRef
  {
    bool valid = false;
    unsigned slot = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::array<unsigned char, 24> descriptor{};
    std::uint32_t Width() const
    {
      return (std::uint32_t(descriptor[8]) << 8) | descriptor[9];
    }
    std::uint32_t Height() const
    {
      return (std::uint32_t(descriptor[10]) << 8) | descriptor[11];
    }
    unsigned Format() const { return descriptor[0]; }
    unsigned Mips() const { return descriptor[1]; }
  };

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

  std::optional<Link> ResolveLink(std::uint32_t chunk) const
  {
    if (chunk >= tables[2].count)
      return std::nullopt;
    const std::uint32_t p = tables[2].offset + chunk * 0x14;
    Link link;
    link.record = chunk;
    link.node_offset = data.U32(p);
    link.word1 = data.U32(p + 4);
    link.material_offset = data.U32(p + 8);
    link.descriptor_offset = data.U32(p + 12);
    link.word4 = data.U32(p + 16);

    if (!tables[3].Contains(link.node_offset) || link.descriptor_offset < 0x38 ||
        !data.Range(link.descriptor_offset, 16))
      return std::nullopt;
    link.node_index = (link.node_offset - tables[3].offset) / tables[3].stride;

    bool material_ok = false;
    for (unsigned table = 0; table < 2; ++table)
    {
      if (!tables[table].Contains(link.material_offset))
        continue;
      link.material_table = table;
      link.material_index =
          (link.material_offset - tables[table].offset) / tables[table].stride;
      material_ok = true;
      break;
    }
    if (!material_ok)
      return std::nullopt;
    link.valid = true;
    return link;
  }

  std::optional<Link> ResolveDescriptor(std::uint32_t descriptor_offset) const
  {
    if (descriptor_offset < 0x38 || !data.Range(descriptor_offset, 16))
      return std::nullopt;
    for (std::uint32_t chunk = 0; chunk < tables[2].count; ++chunk)
    {
      const std::uint32_t p = tables[2].offset + chunk * 0x14;
      if (data.U32(p + 12) != descriptor_offset)
        continue;
      return ResolveLink(chunk);
    }
    return std::nullopt;
  }

  std::optional<std::uint32_t> Descriptor(std::uint32_t chunk) const
  {
    if (const auto link = ResolveLink(chunk))
      return link->descriptor_offset;
    return std::nullopt;
  }

  std::optional<Bounds3> NodeBounds(const Link& link) const
  {
    if (!link.valid || !tables[3].Contains(link.node_offset))
      return std::nullopt;
    // NODE70 is an oriented bounding box, not a model transform. GC
    // IntersectNode (80074a78) passes rows at 0/16/32 to CVolBox::SetBasis,
    // center at +0x30 to SetCenter, and +0x0c/+0x1c/+0x2c as half extents.
    std::array<float, 3> center{}, extent{};
    if (!data.Vector(link.node_offset + 0x30, center))
      return std::nullopt;
    for (unsigned axis = 0; axis < 3; ++axis)
    {
      std::array<float, 3> basis{};
      const auto p = link.node_offset + axis * 16;
      const float half = data.Float(p + 12);
      if (!data.Vector(p, basis) || !std::isfinite(half) || half < 0)
        return std::nullopt;
      for (unsigned component = 0; component < 3; ++component)
        extent[component] += std::fabs(basis[component]) * half;
    }
    Bounds3 bounds;
    for (unsigned component = 0; component < 3; ++component)
    {
      bounds.minimum[component] = center[component] - extent[component];
      bounds.maximum[component] = center[component] + extent[component];
      if (!std::isfinite(bounds.minimum[component]) || !std::isfinite(bounds.maximum[component]))
        return std::nullopt;
    }
    bounds.valid = true;
    return bounds;
  }

  std::optional<TextureRef> MaterialTexture(const Link& link, unsigned slot) const
  {
    if (!link.valid || slot > 1 || !link.material_offset ||
        !data.Range(link.material_offset, 0x94))
      return std::nullopt;
    // Proven PS3 MAT94 texture resource records. Slot 0 starts at +0x18 and
    // slot 1 at +0x40. Each uses size@+4, flagged RSX offset@+8 and the exact
    // 24-byte GTF descriptor@+16.
    const std::uint32_t base = link.material_offset + (slot == 0 ? 0x18u : 0x40u);
    if (!data.Range(base, 40))
      return std::nullopt;
    TextureRef ref;
    ref.slot = slot;
    ref.size = data.U32(base + 4);
    ref.offset = data.U32(base + 8) & ~1u;
    for (unsigned i = 0; i < ref.descriptor.size(); ++i)
      ref.descriptor[i] = data.bytes[base + 16 + i];

    const unsigned base_format = ref.descriptor[0] & 0x9fu;
    if (!ref.size || !ref.offset || (ref.offset & 0x7fu) != 0 ||
        (base_format != 0x86 && base_format != 0x87 && base_format != 0x88) ||
        ref.descriptor[1] == 0 || ref.descriptor[1] > 16 ||
        ref.descriptor[6] != 0xaa || ref.descriptor[7] != 0xe4 ||
        ref.Width() == 0 || ref.Height() == 0 || ref.Width() > 8192 || ref.Height() > 8192)
      return std::nullopt;
    ref.valid = true;
    return ref;
  }
};

// GC BPD/PSP are little endian despite running on a big-endian CPU. PS3
// XPD/PSP are big endian. Never apply the CPT/CDB byte order blindly to them.
struct PropertyReader : GCCompartment::Reader
{
  bool little = false;
  std::uint32_t U32(std::size_t p) const
  {
    if (!little)
      return GCCompartment::Reader::U32(p);
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

  static std::optional<BPD> ParseWithEndian(std::span<const unsigned char> bytes, bool little)
  {
    BPD out;
    out.data.bytes = bytes;
    out.data.little = little;
    if (!out.data.Range(0, 0x60) || out.data.U32(0) != 9 ||
        out.data.U32(4) != bytes.size())
      return std::nullopt;
    constexpr unsigned strides[] = {12, 1, 4, 8, 240, 124, 48, 8, 8};
    for (unsigned i = 0; i < out.tables.size(); ++i)
    {
      auto& t = out.tables[i];
      t = {out.data.U32(0x10 + i * 8), out.data.U32(0x14 + i * 8), strides[i]};
      if (t.count && (t.offset < 0x60 || !out.data.Range(t.offset, t.count, t.stride)))
        return std::nullopt;
    }
    std::uint64_t p = out.tables[3].offset;
    out.properties.reserve(out.tables[3].count);
    for (std::uint32_t i = 0; i < out.tables[3].count; ++i)
    {
      if (!out.data.Range(p, 8))
        return std::nullopt;
      const auto size = out.data.U32(p + 4);
      if (size < 8 || !out.data.Range(p, size))
        return std::nullopt;
      out.properties.push_back({out.data.U32(p), static_cast<std::uint32_t>(p), size});
      p += size;
    }
    return out;
  }

  static std::optional<BPD> Parse(std::span<const unsigned char> bytes)
  {
    if (bytes.size() < 8)
      return std::nullopt;
    // Do not infer byte order from only bytes[0]. Verify both the magic and
    // embedded file-size field so corrupt files cannot select the wrong mode.
    if (auto little = ParseWithEndian(bytes, true))
      return little;
    return ParseWithEndian(bytes, false);
  }
};

// PS3 renamed the big-endian BPD property container to XPD. Keep a distinct
// type so call sites cannot accidentally parse a PS3 XPD as GC little-endian.
struct XPD
{
  BPD layout;
  static std::optional<XPD> Parse(std::span<const unsigned char> bytes)
  {
    if (auto parsed = BPD::ParseWithEndian(bytes, false))
      return XPD{std::move(*parsed)};
    return std::nullopt;
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
    struct Work
    {
      std::uint32_t offset;
      bool leaf;
      bool exit;
    };
    const std::uint32_t root = out.data.U32(0x24);
    if (root < 0x28 || root >= bytes.size())
      return std::nullopt;
    std::vector<Work> pending{{root, false, false}};
    std::unordered_set<std::uint32_t> active, visited_nodes, visited_leaves;
    while (!pending.empty())
    {
      const auto work = pending.back();
      pending.pop_back();
      const auto p = work.offset;
      if (work.exit)
      {
        active.erase(p);
        continue;
      }
      if (p < 0x28 || !out.data.Range(p, work.leaf ? 0x30 : 0x10))
        return std::nullopt;
      if (work.leaf)
      {
        if (visited_nodes.contains(p))
          return std::nullopt;
        if (!visited_leaves.insert(p).second)
          continue;
        BSPLeaf leaf;
        leaf.offset = p;
        constexpr unsigned fields[] = {0x20, 0x24, 0x28, 0x1c, 0x2c};
        for (unsigned i = 0; i < leaf.lists.size(); ++i)
        {
          const auto count = i == 0 ? out.data.U32(p) : out.data.U16(p + 2 + i * 2);
          auto& t = leaf.lists[i];
          t = {out.data.U32(p + fields[i]), count, i == 3 ? 4u : 2u};
          if (t.count && (t.offset < 0x28 || !out.data.Range(t.offset, t.count, t.stride)))
            return std::nullopt;
        }
        out.leaves.push_back(leaf);
      }
      else
      {
        if (active.contains(p) || visited_leaves.contains(p))
          return std::nullopt;
        if (!visited_nodes.insert(p).second)
          continue;
        BSPNode node{p, out.data.Float(p), {out.data.U32(p + 4), out.data.U32(p + 8)},
                     {bytes[p + 12] != 0, bytes[p + 13] != 0}};
        if (!std::isfinite(node.split))
          return std::nullopt;
        out.nodes.push_back(node);
        active.insert(p);
        pending.push_back({p, false, true});
        for (unsigned i = 0; i < 2; ++i)
          pending.push_back({node.children[i], node.leaf[i], false});
      }
    }
    return out;
  }

  static std::optional<PSP> ParseAuto(std::span<const unsigned char> bytes)
  {
    if (bytes.size() < 0x28 || std::memcmp(bytes.data(), "PBSP_HEADER", 11) != 0)
      return std::nullopt;
    // A valid root pointer is a stronger byte-order discriminator than the
    // exporter pointer garbage at 0x0c..0x1f. Prefer PS3 BE when both happen
    // to parse, because this bridge consumes PS3RemasterAssets first.
    auto big = Parse(bytes, false);
    auto little = Parse(bytes, true);
    if (big && !little)
      return big;
    if (little && !big)
      return little;
    if (big)
      return big;
    return std::nullopt;
  }
};
}  // namespace MOHFrontline::WorldFormats
