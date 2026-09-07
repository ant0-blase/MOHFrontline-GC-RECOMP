#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace MOHFrontline::Meshes
{
struct Attribute
{
  std::uint8_t semantic = 0, components = 0, type = 0, offset = 0;
};
struct PositionUVVertex
{
  std::array<float, 3> position{}, normal{};
  std::array<float, 2> uv0{}, uv1{};
};
struct MaterialRecord
{
  std::uint32_t source_offset = 0;
  std::vector<std::string> strings;
};
struct MaterialTable
{
  std::uint32_t source_offset = 0;
  std::vector<MaterialRecord> records;
};
struct Submesh
{
  std::uint32_t vertex_count = 0, index_count = 0, vertex_stride = 0;
  std::vector<Attribute> attributes;
  // Preserve declarations and packed attributes not yet interpreted by the native renderer.
  std::vector<std::uint8_t> vertices;
  std::vector<std::uint16_t> indices;
  std::vector<PositionUVVertex> position_uv;
  bool has_uv0 = false, has_uv1 = false, has_normal = false;
  // v4: material-table strings associated by submesh index when a PS3 MSH
  // table has exactly the same record count as the geometry section table.
  // The renderer must still validate/use these names before native submission.
  std::vector<std::string> material_hints;
};
struct StaticMesh
{
  std::string source_name;
  std::vector<Submesh> submeshes;
  // Frontline PS3 MSH v8 exposes two header table pairs at +0x10/+0x14 and
  // +0x18/+0x1c.  Samples use fixed 0x90-byte records.  Preserve their
  // printable identifiers instead of throwing the tables away.
  std::vector<MaterialTable> material_tables;
  // Material/transform metadata must be validated before native scene activation.
  std::vector<std::uint8_t> opaque_tail;
};
}
