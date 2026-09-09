#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCViv.h"

namespace MOHFrontline::GCCompartment
{
// File offsets, never relocated guest pointers. Layouts are from GMFE69:
// CCompartment::Init (800761c8), CCPTMatBin::Link (80073bd4),
// CCDBObject::Init (8007133c), CCDBTriangle ctor (8006e058).
struct Reader
{
  std::span<const unsigned char> bytes;
  bool Range(std::uint64_t offset, std::uint64_t count, unsigned stride = 1) const
  {
    return stride && offset <= bytes.size() && count <= (bytes.size() - offset) / stride;
  }
  std::uint32_t U32(std::size_t p) const { return GCViv::ReadBE(bytes.data() + p, 4); }
  std::uint16_t U16(std::size_t p) const { return GCViv::ReadBE(bytes.data() + p, 2); }
  float Float(std::size_t p) const { return std::bit_cast<float>(U32(p)); }
  bool Vector(std::uint64_t p, std::span<float> out, bool require_finite = true) const
  {
    if (!Range(p, out.size(), 4))
      return false;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
      out[i] = Float(p + 4 * i);
      if (require_finite && !std::isfinite(out[i]))
        return false;
    }
    return true;
  }
};

struct Table
{
  std::uint32_t offset = 0, count = 0, stride = 0;
  bool Contains(std::uint32_t pointer) const
  {
    return stride && pointer >= offset && (pointer - offset) % stride == 0 &&
           (pointer - offset) / stride < count;
  }
};

struct Vertex
{
  std::array<float, 3> position{}, normal{};
  std::array<float, 2> uv{};
  std::array<unsigned char, 4> color{};
};

struct CPT
{
  Reader data;
  std::array<Table, 5> tables{}; // opaque/alpha materials, chunks, nodes, swap pointers

  static std::optional<CPT> Parse(std::span<const unsigned char> bytes)
  {
    CPT out{{bytes}};
    if (!out.data.Range(0, 0x30) || out.data.U32(0) != 17 ||
        out.data.U32(4) != bytes.size())
      return std::nullopt;
    constexpr unsigned strides[] = {0x70, 0x70, 0x14, 0x70, 4};
    for (unsigned i = 0; i < out.tables.size(); ++i)
    {
      auto& t = out.tables[i];
      t = {out.data.U32(8 + i * 8), out.data.U32(12 + i * 8), strides[i]};
      // Empty retail master tables can retain an offset beyond EOF (5_2_ART).
      if (t.count && (t.offset < 0x30 || !out.data.Range(t.offset, t.count, t.stride)))
        return std::nullopt;
    }
    return out;
  }

  // A material with flag 2 references a material in the shared master CPT;
  // its +0x6c field is an index, not a local texture offset.
  std::optional<std::uint32_t> Shape(unsigned table, std::uint32_t index) const
  {
    if (table > 1 || index >= tables[table].count)
      return std::nullopt;
    const auto material = tables[table].offset + index * 0x70;
    const auto shape_file = data.U32(material + 0x60);
    if (!shape_file || !data.Range(shape_file, 0x18) ||
        data.U32(shape_file) != 0x53485047) // SHPG
      return std::nullopt;
    const std::uint64_t shape = std::uint64_t(shape_file) + data.U32(shape_file + 0x14);
    if (!data.Range(shape, 16))
      return std::nullopt;
    return static_cast<std::uint32_t>(shape);
  }

  std::optional<std::uint32_t> Geometry(std::uint32_t chunk) const
  {
    if (chunk >= tables[2].count)
      return std::nullopt;
    const auto record = tables[2].offset + chunk * 0x14;
    if (!tables[3].Contains(data.U32(record)))
      return std::nullopt;
    const auto material = data.U32(record + 8);
    if (!tables[0].Contains(material) && !tables[1].Contains(material))
      return std::nullopt;
    const auto geometry = data.U32(record + 12);
    if (!geometry || !data.Range(geometry, 0x18))
      return std::nullopt;
    const auto count = data.U16(geometry + 2);
    const unsigned index_width = (data.U16(geometry) & 1) ? 2 : 1;
    // On disk, 0x34 bytes are reserved for CP array commands + GXBegin.
    // Init fills those commands and changes the count into a padded DL size.
    const std::uint64_t indices = std::uint64_t(data.U32(geometry + 0x14)) + 0x34;
    if (!data.U32(geometry + 0x14) || !data.Range(indices, count, 4 * index_width))
      return std::nullopt;
    return geometry;
  }

  bool DecodeVertex(std::uint32_t geometry, std::uint32_t vertex, Vertex* out) const
  {
    if (!out || !data.Range(geometry, 0x18) || vertex >= data.U16(geometry + 2))
      return false;
    for (unsigned field : {4u, 8u, 12u, 16u, 20u})
    {
      if (!data.U32(geometry + field))
        return false;
    }
    const unsigned width = (data.U16(geometry) & 1) ? 2 : 1;
    const std::uint64_t tuple = std::uint64_t(data.U32(geometry + 0x14)) + 0x34 +
                                std::uint64_t(vertex) * width * 4;
    if (!data.Range(tuple, 4, width))
      return false;
    std::array<std::uint32_t, 4> indices{};
    for (unsigned i = 0; i < 4; ++i)
      indices[i] = GCViv::ReadBE(data.bytes.data() + tuple + width * i, width);
    const std::uint64_t position = std::uint64_t(data.U32(geometry + 4)) + indices[0] * 12ull;
    const std::uint64_t normal = std::uint64_t(data.U32(geometry + 12)) + indices[1] * 3ull;
    const std::uint64_t color = std::uint64_t(data.U32(geometry + 8)) + indices[2] * 4ull;
    const std::uint64_t uv = std::uint64_t(data.U32(geometry + 16)) + indices[3] * 8ull;
    Vertex result;
    // Some untextured retail chunks contain NaN UV sentinels. Preserve those
    // unused attributes; rejecting them would discard otherwise valid geometry.
    if (!data.Vector(position, result.position) || !data.Vector(uv, result.uv, false) ||
        !data.Range(normal, 3) || !data.Range(color, 4))
      return false;
    for (unsigned i = 0; i < 3; ++i)
      result.normal[i] = std::bit_cast<std::int8_t>(data.bytes[normal + i]) / 64.0f;
    for (unsigned i = 0; i < 4; ++i)
      result.color[i] = data.bytes[color + i];
    *out = result;
    return true;
  }
};

struct CDB
{
  Reader data;
  Table vertices, triangles;
  static std::optional<CDB> Parse(std::span<const unsigned char> bytes)
  {
    CDB out{{bytes}, {}, {}};
    if (!out.data.Range(0, 0x74) || out.data.U32(0) != 7 ||
        out.data.U32(4) != bytes.size())
      return std::nullopt;
    out.vertices = {out.data.U32(12), out.data.U32(8), 12};
    out.triangles = {out.data.U32(36), out.data.U32(16), 16};
    for (const auto& t : {out.vertices, out.triangles})
    {
      if (t.count && (t.offset < 0x74 || !out.data.Range(t.offset, t.count, t.stride)))
        return std::nullopt;
    }
    return out;
  }
  bool DecodeTriangle(std::uint32_t index, std::array<std::array<float, 3>, 3>* out) const
  {
    if (!out || index >= triangles.count)
      return false;
    std::array<std::array<float, 3>, 3> result{};
    for (unsigned i = 0; i < 3; ++i)
    {
      const auto pointer = data.U32(triangles.offset + index * 16 + i * 4);
      if (!vertices.Contains(pointer) || !data.Vector(pointer, result[i]))
        return false;
    }
    *out = result;
    return true;
  }
};
}  // namespace MOHFrontline::GCCompartment
