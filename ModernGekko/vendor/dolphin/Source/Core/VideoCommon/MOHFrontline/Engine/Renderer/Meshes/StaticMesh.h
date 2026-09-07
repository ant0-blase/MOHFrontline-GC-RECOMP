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
  std::array<float, 3> position{};
  std::array<float, 2> uv0{}, uv1{};
};
struct Submesh
{
  std::uint32_t vertex_count = 0, index_count = 0, vertex_stride = 0;
  std::vector<Attribute> attributes;
  // Preserve declarations and packed attributes not yet interpreted by the native renderer.
  std::vector<std::uint8_t> vertices;
  std::vector<std::uint16_t> indices;
  std::vector<PositionUVVertex> position_uv;
  bool has_uv0 = false, has_uv1 = false;
};
struct StaticMesh
{
  std::string source_name;
  std::vector<Submesh> submeshes;
  // Material/transform metadata must be validated before native scene activation.
  std::vector<std::uint8_t> opaque_tail;
};
}
