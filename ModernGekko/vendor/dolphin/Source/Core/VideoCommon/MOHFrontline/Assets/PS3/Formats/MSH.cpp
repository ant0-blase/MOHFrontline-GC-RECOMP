#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/MSH.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
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

constexpr std::size_t MATERIAL_RECORD_SIZE = 0x90;

bool IsTokenChar(unsigned char c)
{
  return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/';
}

std::vector<std::string> ExtractStrings(std::span<const u8> record)
{
  std::vector<std::string> out;
  std::size_t p = 0;
  while (p < record.size())
  {
    while (p < record.size() && !IsTokenChar(record[p]))
      ++p;
    const std::size_t begin = p;
    bool has_alpha = false;
    while (p < record.size() && IsTokenChar(record[p]))
    {
      has_alpha |= std::isalpha(record[p]) != 0;
      ++p;
    }
    const std::size_t len = p - begin;
    if (!has_alpha || len < 3 || len > 64)
      continue;
    std::string token(reinterpret_cast<const char*>(record.data() + begin), len);
    if (std::find(out.begin(), out.end(), token) == out.end())
      out.push_back(std::move(token));
  }
  return out;
}

bool DecodeTable(std::span<const u8> bytes, u32 offset, u32 count, Meshes::MaterialTable* out)
{
  if (!out || !count || count > 4096 || offset < 0x38)
    return false;
  const std::size_t total = std::size_t(count) * MATERIAL_RECORD_SIZE;
  if (offset > bytes.size() || total > bytes.size() - offset)
    return false;

  Meshes::MaterialTable table;
  table.source_offset = offset;
  table.records.reserve(count);
  for (u32 i = 0; i < count; ++i)
  {
    const std::size_t pos = std::size_t(offset) + std::size_t(i) * MATERIAL_RECORD_SIZE;
    Meshes::MaterialRecord rec;
    rec.source_offset = static_cast<std::uint32_t>(pos);
    rec.strings = ExtractStrings(bytes.subspan(pos, MATERIAL_RECORD_SIZE));
    table.records.push_back(std::move(rec));
  }
  *out = std::move(table);
  return true;
}

bool MaterialTraceEnabled()
{
  static const bool enabled = [] {
    const char* v = std::getenv("MOH_PS3_MSH_MATERIAL_TRACE");
    if (!v || !*v)
      return false;
    const std::string_view s(v);
    return s == "1" || s == "true" || s == "on" || s == "yes";
  }();
  return enabled;
}
}
bool Decode(std::span<const u8> bytes, Meshes::StaticMesh* out)
{
  if (!out) return false;
  // PS3MeshPort sets source_name before decoding.  The old decoder cleared the
  // whole object here, silently losing the asset identity needed for material
  // binding and diagnostics.
  const std::string source_name = out->source_name;
  *out = {};
  if (bytes.size() < 0x38 || BE32(bytes.data()) != 8) return false;
  const auto count = BE32(bytes.data() + 0x24);
  if (!count || count > 65535) return false;
  Meshes::StaticMesh mesh;
  mesh.source_name = source_name;
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
    const Meshes::Attribute* normal = nullptr;
    const Meshes::Attribute* uv0 = nullptr;
    const Meshes::Attribute* uv1 = nullptr;
    for (const auto& a : sub.attributes)
    {
      if (a.offset >= sub.vertex_stride) return false;
      if (a.semantic == 0) { if (position) return false; position = &a; }
      if (a.semantic == 2) { if (normal) return false; normal = &a; }
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
      if (normal)
      {
        if (normal->type == 6 && normal->components == 1 &&
            unsigned(normal->offset) + 4 <= sub.vertex_stride)
        {
          // RSX CMP: signed normalized X11/Y11/Z10. Axis normals in crate.msh
          // are 0x3ff/+X, 0x400/-X, 0x1ff800/+Y, 0x7fc00000/+Z.
          const u32 packed = BE32(raw + normal->offset);
          auto snorm = [](u32 bits, unsigned width) {
            const int sign = 1 << (width - 1);
            const int value = int(bits & ((1u << width) - 1));
            return std::max(-1.0f, float(value >= sign ? value - 2 * sign : value) / float(sign - 1));
          };
          v.normal = {snorm(packed, 11), snorm(packed >> 11, 11), snorm(packed >> 22, 10)};
        }
        else if (!Components(raw, *normal, 3, v.normal.data(), sub.vertex_stride)) return false;
      }
      sub.position_uv.push_back(v);
    }
    sub.has_normal = normal != nullptr;
    sub.has_uv0 = uv0 != nullptr; sub.has_uv1 = uv1 != nullptr;
    mesh.submeshes.push_back(std::move(sub));
  }
  // Header table pairs observed in Frontline PS3 MSH v8.  Previous tooling
  // already validated these as 0x90-byte-record tables across the asset set;
  // keep them host-side instead of discarding everything after geometry.
  const std::array<std::pair<u32, u32>, 2> table_desc{{
      {BE32(bytes.data() + 0x10), BE32(bytes.data() + 0x14)},
      {BE32(bytes.data() + 0x18), BE32(bytes.data() + 0x1c)},
  }};

  for (const auto& [offset, records] : table_desc)
  {
    Meshes::MaterialTable table;
    if (!DecodeTable(bytes, offset, records, &table))
      continue;

    // Do not guess an arbitrary material layout.  Only associate records to
    // submeshes when the table cardinality proves a one-record-per-submesh
    // relationship.  The extracted strings remain hints until the draw bridge
    // validates them against the current-level TPK catalog.
    const bool one_to_one = table.records.size() == mesh.submeshes.size();
    if (one_to_one)
    {
      for (std::size_t i = 0; i < mesh.submeshes.size(); ++i)
        mesh.submeshes[i].material_hints = table.records[i].strings;
    }

    if (MaterialTraceEnabled())
    {
      std::fprintf(stderr,
                   "[moh-ps3-msh-mat] source=%s table=0x%08X records=%zu submeshes=%zu one_to_one=%d\n",
                   mesh.source_name.empty() ? "<unknown>" : mesh.source_name.c_str(),
                   table.source_offset, table.records.size(), mesh.submeshes.size(),
                   one_to_one ? 1 : 0);

      if (one_to_one)
      {
        static unsigned hint_logs = 0;
        for (std::size_t i = 0; i < table.records.size() && hint_logs < 256; ++i)
        {
          const auto& hints = table.records[i].strings;
          if (hints.empty())
            continue;
          ++hint_logs;
          std::fprintf(stderr, "[moh-ps3-msh-mat] source=%s submesh=%zu hints=",
                       mesh.source_name.empty() ? "<unknown>" : mesh.source_name.c_str(), i);
          for (std::size_t h = 0; h < hints.size(); ++h)
            std::fprintf(stderr, "%s%s", h ? "," : "", hints[h].c_str());
          std::fputc('\n', stderr);
        }
      }
    }

    mesh.material_tables.push_back(std::move(table));
  }

  mesh.opaque_tail.assign(bytes.begin() + p, bytes.end());
  *out = std::move(mesh);
  return true;
}
}
