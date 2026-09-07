#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/MSH.h"
#include <algorithm>
#include <bit>
#include <cmath>
namespace MOHFrontline::PS3::MSH
{
namespace
{
using u8 = std::uint8_t;
using u32 = std::uint32_t;
u32 BE16(const u8* p) { return (u32(p[0]) << 8) | p[1]; }
u32 BE32(const u8* p) { return (BE16(p) << 16) | BE16(p + 2); }
float Half(u32 h)
{
  const int exponent = (h >> 10) & 31;
  const int mantissa = h & 1023;
  const float value = exponent == 0 ? std::ldexp(float(mantissa), -24) :
      exponent == 31 ? INFINITY : std::ldexp(float(1024 + mantissa), exponent - 25);
  return h & 32768 ? -value : value;
}
// RSX F=2 and SF=3; verified against the position/UV declarations in PS3 MSH samples.
bool Components(const u8* vertex, const Meshes::Attribute& a, unsigned n, float* out, u32 stride)
{
  const unsigned size = a.type == 2 ? 4 : a.type == 3 ? 2 : 0;
  if (!size || a.components != n || a.offset > stride || n * size > stride - a.offset) return false;
  for (unsigned i = 0; i < n; ++i)
  {
    const auto* p = vertex + a.offset + i * size;
    out[i] = size == 4 ? std::bit_cast<float>(BE32(p)) : Half(BE16(p));
    if (!std::isfinite(out[i])) return false;
  }
  return true;
}
}
bool Decode(std::span<const u8> bytes, Meshes::StaticMesh* out)
{
  if (!out) return false;
  *out = {};
  if (bytes.size() < 0x38 || BE32(bytes.data()) != 8) return false;
  const auto count = BE32(bytes.data() + 0x24);
  if (!count || count > 65535) return false;
  Meshes::StaticMesh mesh;
  std::size_t p = 0x38;
  for (u32 m = 0; m < count; ++m)
  {
    if (p > bytes.size() || bytes.size() - p < 12) return false;
    Meshes::Submesh sub;
    sub.index_count = BE32(bytes.data() + p);
    sub.vertex_count = BE32(bytes.data() + p + 4);
    sub.vertex_stride = bytes[p + 8];
    const unsigned attributes = bytes[p + 9];
    p += 12;
    if (!sub.vertex_count || sub.vertex_count > 65536 || !sub.vertex_stride ||
        !attributes || attributes > 32 || attributes > (bytes.size() - p) / 4) return false;
    for (unsigned a = 0; a < attributes; ++a, p += 4)
      sub.attributes.push_back({bytes[p], bytes[p+1], bytes[p+2], bytes[p+3]});
    if (sub.vertex_count > (bytes.size() - p) / sub.vertex_stride) return false;
    const auto size = std::size_t(sub.vertex_count) * sub.vertex_stride;
    if (sub.index_count > (bytes.size() - p - size) / 2) return false;
    sub.vertices.assign(bytes.begin() + p, bytes.begin() + p + size);
    p += size;
    for (u32 i = 0; i < sub.index_count; ++i, p += 2)
    {
      const auto index = BE16(bytes.data() + p);
      if (index >= sub.vertex_count) return false;
      sub.indices.push_back(static_cast<std::uint16_t>(index));
    }
    const Meshes::Attribute* position = nullptr;
    const Meshes::Attribute* uv0 = nullptr;
    const Meshes::Attribute* uv1 = nullptr;
    for (const auto& a : sub.attributes)
    {
      if (a.offset >= sub.vertex_stride) return false;
      if (a.semantic == 0) { if (position) return false; position = &a; }
      if (a.semantic == 8) { if (uv0) return false; uv0 = &a; }
      if (a.semantic == 9) { if (uv1) return false; uv1 = &a; }
    }
    if (!position) return false;
    for (u32 i = 0; i < sub.vertex_count; ++i)
    {
      Meshes::PositionUVVertex v;
      const auto* raw = sub.vertices.data() + std::size_t(i) * sub.vertex_stride;
      if (!Components(raw, *position, 3, v.position.data(), sub.vertex_stride) ||
          (uv0 && !Components(raw, *uv0, 2, v.uv0.data(), sub.vertex_stride)) ||
          (uv1 && !Components(raw, *uv1, 2, v.uv1.data(), sub.vertex_stride))) return false;
      sub.position_uv.push_back(v);
    }
    sub.has_uv0 = uv0 != nullptr; sub.has_uv1 = uv1 != nullptr;
    mesh.submeshes.push_back(std::move(sub));
  }
  mesh.opaque_tail.assign(bytes.begin() + p, bytes.end());
  *out = std::move(mesh);
  return true;
}
}
