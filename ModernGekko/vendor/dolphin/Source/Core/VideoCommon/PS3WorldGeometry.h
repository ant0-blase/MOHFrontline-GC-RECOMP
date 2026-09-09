#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "VideoCommon/MOHFrontline/Assets/Formats/WorldFormats.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/Meshes/StaticMesh.h"
#include "VideoCommon/PS3RemasterAssets.h"

// Runtime PS3 CPT/RSX world-geometry probe/bridge.
//
// Frontline's PS3 *_ART_cN.cpt files are treated as metadata containers.  The
// actual RSX payload may be inline or referenced from the level rsx.viv. This
// decoder follows the version-9 CPT chunk/node/material pointer graph to the
// compact RSX vertex declarations also used by PS3 MSH,
// validates each declaration against the referenced vertex/index ranges, and
// exports only host-renderable one-submesh StaticMesh objects.  Those objects
// are then consumed by the existing PS3MeshPort renderer/matcher.
namespace PS3WorldGeometry
{
using Attribute = MOHFrontline::Meshes::Attribute;
using PositionUVVertex = MOHFrontline::Meshes::PositionUVVertex;
using StaticMesh = MOHFrontline::Meshes::StaticMesh;
using Submesh = MOHFrontline::Meshes::Submesh;
using AssetInfo = PS3RemasterAssets::AssetInfo;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct DecodeStats
{
  std::size_t descriptor_candidates = 0;
  std::size_t inline_candidates = 0;
  std::size_t rsx_candidates = 0;
  std::size_t accepted = 0;
  std::size_t duplicate = 0;
  std::size_t rejected = 0;
};

inline bool EnvSwitch(const char* name, bool fallback)
{
  const char* raw = std::getenv(name);
  if (!raw || !*raw)
    return fallback;
  const std::string_view v(raw);
  if (v == "0" || v == "false" || v == "off" || v == "no")
    return false;
  if (v == "1" || v == "true" || v == "on" || v == "yes")
    return true;
  return fallback;
}

inline bool Enabled()
{
  static const bool enabled = EnvSwitch("MOH_PS3_WORLD_GEOMETRY", true);
  return enabled;
}

inline bool TraceEnabled()
{
  static const bool enabled = EnvSwitch("MOH_PS3_WORLD_GEOMETRY_TRACE", false);
  return enabled;
}

inline bool MaterialTraceEnabled()
{
  static const bool enabled = EnvSwitch("MOH_PS3_WORLD_MATERIAL_TRACE", false);
  return enabled;
}

inline u16 BE16(const u8* p)
{
  return static_cast<u16>((u16(p[0]) << 8) | u16(p[1]));
}
inline u32 BE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}
inline float BEFloat(const u8* p)
{
  return std::bit_cast<float>(BE32(p));
}
inline float Half(u16 h)
{
  const int exponent = (h >> 10) & 31;
  const int mantissa = h & 1023;
  const float value = exponent == 0 ? std::ldexp(float(mantissa), -24) :
      exponent == 31 ? INFINITY : std::ldexp(float(1024 + mantissa), exponent - 25);
  return h & 0x8000 ? -value : value;
}

inline bool DecodeComponents(const u8* vertex, const Attribute& a, unsigned n, float* out,
                             u32 stride)
{
  if (!vertex || !out || a.components != n)
    return false;
  const unsigned element_size = a.type == 2 ? 4u : a.type == 3 ? 2u : 0u;
  if (!element_size || a.offset > stride || n * element_size > stride - a.offset)
    return false;
  for (unsigned i = 0; i < n; ++i)
  {
    const u8* p = vertex + a.offset + i * element_size;
    out[i] = element_size == 4 ? BEFloat(p) : Half(BE16(p));
    if (!std::isfinite(out[i]))
      return false;
  }
  return true;
}

inline bool DecodeNormal(const u8* vertex, const Attribute& a, std::array<float, 3>* out,
                         u32 stride)
{
  if (!vertex || !out)
    return false;
  if (a.type == 6 && a.components == 1)
  {
    if (a.offset > stride || 4 > stride - a.offset)
      return false;
    const u32 packed = BE32(vertex + a.offset);
    const auto snorm = [](u32 bits, unsigned width) {
      const int sign = 1 << (width - 1);
      const int value = int(bits & ((1u << width) - 1));
      return std::max(-1.0f,
                      float(value >= sign ? value - 2 * sign : value) / float(sign - 1));
    };
    *out = {snorm(packed, 11), snorm(packed >> 11, 11), snorm(packed >> 22, 10)};
    return true;
  }
  return DecodeComponents(vertex, a, 3, out->data(), stride);
}

inline std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

inline bool IsWorldChunk(const AssetInfo& asset)
{
  const std::string filename = Lower(asset.filename);
  return filename.ends_with(".cpt") && filename.find("_art_c") != std::string::npos;
}

inline const AssetInfo* FindRSX(const AssetInfo& cpt)
{
  std::string rel = cpt.relative_path;
  std::replace(rel.begin(), rel.end(), '\\', '/');
  const std::string lower = Lower(rel);
  const std::string marker = "/level.viv::";
  const std::size_t pos = lower.find(marker);
  if (pos != std::string::npos)
    return PS3RemasterAssets::FindByRelativePath(rel.substr(0, pos) + "/rsx.viv");

  const std::size_t slash = rel.find_last_of('/');
  if (slash != std::string::npos)
    return PS3RemasterAssets::FindByRelativePath(rel.substr(0, slash + 1) + "rsx.viv");
  return nullptr;
}

inline u64 FNV1a(std::span<const u8> bytes, u64 seed = 1469598103934665603ULL)
{
  u64 h = seed;
  for (const u8 b : bytes)
  {
    h ^= b;
    h *= 1099511628211ULL;
  }
  return h;
}

struct Descriptor
{
  std::size_t source_offset = 0;
  u32 index_count = 0;
  u32 vertex_count = 0;
  u32 stride = 0;
  std::vector<Attribute> attributes;
  const Attribute* position = nullptr;
  const Attribute* normal = nullptr;
  const Attribute* uv0 = nullptr;
  const Attribute* uv1 = nullptr;
  std::size_t tail = 0;
};

inline bool ParseDescriptor(std::span<const u8> bytes, std::size_t p, Descriptor* out)
{
  if (!out || p > bytes.size() || bytes.size() - p < 16)
    return false;

  Descriptor d;
  d.source_offset = p;
  d.index_count = BE32(bytes.data() + p);
  d.vertex_count = BE32(bytes.data() + p + 4);
  d.stride = bytes[p + 8];
  const unsigned attribute_count = bytes[p + 9];

  // World chunks are triangle lists in the PS3 host renderer path.  Restrict
  // this aggressively to avoid interpreting texture/resource tables as meshes.
  if (d.index_count < 3 || d.index_count > 300000 || d.index_count % 3 != 0 ||
      d.vertex_count < 3 || d.vertex_count > 65535 || d.stride < 8 || d.stride > 96 ||
      attribute_count == 0 || attribute_count > 16)
    return false;

  std::size_t q = p + 12;
  if (std::size_t(attribute_count) * 4 > bytes.size() - q)
    return false;
  d.attributes.reserve(attribute_count);
  for (unsigned i = 0; i < attribute_count; ++i, q += 4)
  {
    Attribute a{bytes[q], bytes[q + 1], bytes[q + 2], bytes[q + 3]};
    if (a.offset >= d.stride || a.components == 0 || a.components > 4)
      return false;
    d.attributes.push_back(a);
  }

  for (const auto& a : d.attributes)
  {
    if (a.semantic == 0)
    {
      if (d.position)
        return false;
      d.position = &a;
    }
    else if (a.semantic == 2)
    {
      if (d.normal)
        return false;
      d.normal = &a;
    }
    else if (a.semantic == 8)
    {
      if (d.uv0)
        return false;
      d.uv0 = &a;
    }
    else if (a.semantic == 9)
    {
      if (d.uv1)
        return false;
      d.uv1 = &a;
    }
  }

  if (!d.position || d.position->components != 3 ||
      (d.position->type != 2 && d.position->type != 3))
    return false;
  if (d.uv0 && (d.uv0->components != 2 || (d.uv0->type != 2 && d.uv0->type != 3)))
    return false;
  if (d.uv1 && (d.uv1->components != 2 || (d.uv1->type != 2 && d.uv1->type != 3)))
    return false;

  d.tail = q;
  *out = std::move(d);
  // Rebind pointers after the vector move.
  out->position = out->normal = out->uv0 = out->uv1 = nullptr;
  for (const auto& a : out->attributes)
  {
    if (a.semantic == 0) out->position = &a;
    else if (a.semantic == 2) out->normal = &a;
    else if (a.semantic == 8) out->uv0 = &a;
    else if (a.semantic == 9) out->uv1 = &a;
  }
  return out->position != nullptr;
}

inline bool MaterialTokenChar(unsigned char c)
{
  return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/';
}

// Diagnostic-only material discovery. CPT world geometry is structurally
// decoded now, but its material table is not yet understood. Preserve printable
// identifiers immediately preceding a proven geometry descriptor so the next
// bridge can correlate them with TPK/RSX resources without guessing offsets.
inline std::vector<std::string> ProbeMaterialHints(std::span<const u8> bytes,
                                                   const Descriptor& d)
{
  std::vector<std::string> out;
  const std::size_t begin = d.source_offset > 0x180 ? d.source_offset - 0x180 : 0;
  const std::size_t end = d.source_offset;
  std::size_t p = begin;
  while (p < end)
  {
    while (p < end && !MaterialTokenChar(bytes[p]))
      ++p;
    const std::size_t token_begin = p;
    bool has_alpha = false;
    while (p < end && MaterialTokenChar(bytes[p]))
    {
      has_alpha |= std::isalpha(bytes[p]) != 0;
      ++p;
    }
    const std::size_t length = p - token_begin;
    if (!has_alpha || length < 3 || length > 48)
      continue;
    std::string token(reinterpret_cast<const char*>(bytes.data() + token_begin), length);
    if (std::find(out.begin(), out.end(), token) == out.end())
      out.push_back(std::move(token));
    if (out.size() >= 12)
      break;
  }
  return out;
}

inline void TraceMaterialProbe(std::span<const u8> bytes, const Descriptor& d,
                               const StaticMesh& mesh)
{
  if (!MaterialTraceEnabled())
    return;
  static unsigned logs = 0;
  if (logs++ >= 256)
    return;

  const auto word_before = [&](std::size_t distance) -> u32 {
    return d.source_offset >= distance ? BE32(bytes.data() + d.source_offset - distance) : 0;
  };
  const u8 b10 = d.source_offset + 10 < bytes.size() ? bytes[d.source_offset + 10] : 0;
  const u8 b11 = d.source_offset + 11 < bytes.size() ? bytes[d.source_offset + 11] : 0;
  std::fprintf(stderr,
               "[moh-ps3-world-mat] desc=%s off=0x%zX pre=%08X/%08X/%08X/%08X bytes10-11=%02X/%02X hints=",
               mesh.source_name.c_str(), d.source_offset, word_before(16), word_before(12),
               word_before(8), word_before(4), b10, b11);
  const auto& hints = mesh.submeshes.front().material_hints;
  if (hints.empty())
  {
    std::fputs("<none>", stderr);
  }
  else
  {
    for (std::size_t i = 0; i < hints.size(); ++i)
      std::fprintf(stderr, "%s%s", i ? "," : "", hints[i].c_str());
  }
  std::fputc('\n', stderr);
}


struct CPTHeaderTable
{
  u32 offset = 0;
  u32 count = 0;
  std::size_t span = 0;
  std::size_t record_size = 0;
  std::size_t remainder = 0;
  bool valid = false;
};

inline std::vector<std::string> ExtractRecordTokens(std::span<const u8> record,
                                                    std::size_t max_tokens = 12)
{
  std::vector<std::string> out;
  std::size_t p = 0;
  while (p < record.size())
  {
    while (p < record.size() && !MaterialTokenChar(record[p]))
      ++p;
    const std::size_t begin = p;
    bool has_alpha = false;
    while (p < record.size() && MaterialTokenChar(record[p]))
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
    if (out.size() >= max_tokens)
      break;
  }
  return out;
}

inline std::array<CPTHeaderTable, 4> DescribeCPTHeaderTables(std::span<const u8> bytes)
{
  std::array<CPTHeaderTable, 4> tables{};
  if (bytes.size() < 0x30)
    return tables;

  for (std::size_t i = 0; i < tables.size(); ++i)
  {
    const std::size_t pair = 0x10 + i * 8;
    tables[i].offset = BE32(bytes.data() + pair);
    tables[i].count = BE32(bytes.data() + pair + 4);
    if (!tables[i].offset || !tables[i].count || tables[i].offset >= bytes.size())
      continue;

    std::size_t boundary = bytes.size();
    for (std::size_t j = 0; j < tables.size(); ++j)
    {
      if (i == j)
        continue;
      const std::size_t other_pair = 0x10 + j * 8;
      const u32 other_offset = BE32(bytes.data() + other_pair);
      if (other_offset > tables[i].offset && other_offset < boundary)
        boundary = other_offset;
    }
    if (boundary <= tables[i].offset)
      continue;

    tables[i].span = boundary - tables[i].offset;
    tables[i].record_size = tables[i].span / tables[i].count;
    tables[i].remainder = tables[i].span % tables[i].count;
    tables[i].valid = tables[i].record_size != 0;
  }
  return tables;
}

struct CPTDescriptorLink
{
  bool valid = false;
  std::size_t link_record = 0;
  std::size_t node_record = 0;
  std::size_t material_table = 0;
  std::size_t material_record = 0;
  u32 node_offset = 0;
  u32 material_offset = 0;
  u32 descriptor_offset = 0;
  u32 word1 = 0;
  u32 word4 = 0;
};

inline bool ResolveCPTDescriptorLink(std::span<const u8> bytes, std::size_t descriptor_offset,
                                     CPTDescriptorLink* out)
{
  if (!out || descriptor_offset > std::numeric_limits<u32>::max())
    return false;

  // One authoritative parser for CPT table bounds/pointer ownership.  The old
  // local implementation inferred table spans from neighbouring offsets, which
  // is fragile when retail chunks contain alignment padding after TABLE2.
  const auto parsed = MOHFrontline::WorldFormats::PS3CPT::Parse(bytes);
  if (!parsed)
    return false;
  const auto link = parsed->ResolveDescriptor(static_cast<u32>(descriptor_offset));
  if (!link)
    return false;

  out->valid = true;
  out->link_record = link->record;
  out->node_record = link->node_index;
  out->material_table = link->material_table;
  out->material_record = link->material_index;
  out->node_offset = link->node_offset;
  out->material_offset = link->material_offset;
  out->descriptor_offset = link->descriptor_offset;
  out->word1 = link->word1;
  out->word4 = link->word4;
  return true;
}

struct CPTMaterialTextureRef
{
  bool valid = false;
  unsigned slot = 0;
  u32 size = 0;
  u32 raw_offset = 0;
  u32 offset = 0;
  std::array<u8, 24> descriptor{};

  u32 Width() const { return (u32(descriptor[8]) << 8) | descriptor[9]; }
  u32 Height() const { return (u32(descriptor[10]) << 8) | descriptor[11]; }
  u32 Format() const { return descriptor[0]; }
  u32 Mips() const { return descriptor[1]; }
};

inline bool DecodeCPTMaterialTextureRef(std::span<const u8> bytes,
                                        const CPTDescriptorLink& link, unsigned slot,
                                        std::uint64_t rsx_size, CPTMaterialTextureRef* out)
{
  if (!out || !link.valid || slot > 1 || !link.material_offset)
    return false;

  // MAT94 contains two exact RSX resource records.  Their bases are +0x18 and
  // +0x40; each uses the same shape already proven by the runtime CPT texture
  // parser: size@+4, raw rsx offset@+8, 24-byte descriptor@+16.
  const std::size_t record = std::size_t(link.material_offset) + (slot == 0 ? 0x18 : 0x40);
  if (record > bytes.size() || 40 > bytes.size() - record)
    return false;

  CPTMaterialTextureRef ref;
  ref.slot = slot;
  ref.size = BE32(bytes.data() + record + 4);
  ref.raw_offset = BE32(bytes.data() + record + 8);
  if (!ref.size || !ref.raw_offset || (ref.raw_offset & 0x7Fu) > 1u)
    return false;
  ref.offset = ref.raw_offset & ~1u;
  if (!ref.offset || (ref.offset & 0x7Fu) != 0)
    return false;
  std::copy_n(bytes.data() + record + 16, ref.descriptor.size(), ref.descriptor.begin());

  const u8 base_format = static_cast<u8>(ref.descriptor[0] & ~0x20u);
  if ((base_format != 0x86 && base_format != 0x87 && base_format != 0x88) ||
      !ref.Mips() || ref.Mips() > 16 || !ref.Width() || !ref.Height() ||
      ref.Width() > 8192 || ref.Height() > 8192 ||
      ref.descriptor[6] != 0xAA || ref.descriptor[7] != 0xE4)
    return false;
  if (rsx_size && (ref.offset > rsx_size || ref.size > rsx_size - ref.offset))
    return false;

  ref.valid = true;
  *out = ref;
  return true;
}

inline void AttachCPTMaterialTextures(Submesh* submesh,
                                      const std::array<CPTMaterialTextureRef, 2>& textures)
{
  if (!submesh)
    return;
  for (const auto& texture : textures)
  {
    if (!texture.valid)
      continue;
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string descriptor_hex;
    descriptor_hex.reserve(texture.descriptor.size() * 2);
    for (const u8 byte : texture.descriptor)
    {
      descriptor_hex.push_back(hex[(byte >> 4) & 0x0F]);
      descriptor_hex.push_back(hex[byte & 0x0F]);
    }
    submesh->material_hints.push_back(
        "@cpt-tex=slot:" + std::to_string(texture.slot) +
        ";offset:" + std::to_string(texture.offset) +
        ";size:" + std::to_string(texture.size) +
        ";format:" + std::to_string(texture.Format()) +
        ";mips:" + std::to_string(texture.Mips()) +
        ";width:" + std::to_string(texture.Width()) +
        ";height:" + std::to_string(texture.Height()) +
        ";desc:" + descriptor_hex);
  }
}

inline void TraceCPTResolvedState(const AssetInfo& cpt, const CPTDescriptorLink& link,
                                  const MOHFrontline::WorldFormats::Bounds3& bounds,
                                  const std::array<CPTMaterialTextureRef, 2>& textures)
{
  if (!MaterialTraceEnabled() || !link.valid)
    return;

  static std::unordered_set<std::string> dumped_nodes;
  static std::unordered_set<std::string> dumped_textures;
  const std::string node_key = cpt.relative_path + "#node:" + std::to_string(link.node_offset);
  if (bounds.valid && dumped_nodes.insert(node_key).second)
  {
    const auto center = bounds.Center();
    const auto extent = bounds.Extent();
    std::fprintf(stderr,
                 "[moh-ps3-world-mat] NODE BOUNDS: source=%s node=%zu off=0x%08X "
                 "center=(%.6f %.6f %.6f) extent=(%.6f %.6f %.6f)\n",
                 cpt.relative_path.c_str(), link.node_record, link.node_offset,
                 center[0], center[1], center[2], extent[0], extent[1], extent[2]);
  }

  for (const auto& texture : textures)
  {
    if (!texture.valid)
      continue;
    const std::string key = cpt.relative_path + "#mat:" +
                            std::to_string(link.material_offset) + ":" +
                            std::to_string(texture.slot);
    if (!dumped_textures.insert(key).second)
      continue;
    std::fprintf(stderr,
                 "[moh-ps3-world-mat] TEXTURE EXACT: source=%s mat=%zu:%zu slot=%u rsx=0x%08X raw=0x%08X size=%u %ux%u fmt=0x%02X mips=%u remap=%02X%02X\n",
                 cpt.relative_path.c_str(), link.material_table, link.material_record,
                 texture.slot, texture.offset, texture.raw_offset, texture.size,
                 texture.Width(), texture.Height(), texture.Format(), texture.Mips(),
                 texture.descriptor[6], texture.descriptor[7]);
  }
}

inline void AttachCPTDescriptorLink(Submesh* submesh, const CPTDescriptorLink& link)
{
  if (!submesh || !link.valid)
    return;
  submesh->cpt_material_offset = link.material_offset;
  submesh->material_hints.clear();
  submesh->material_hints.push_back(
      "@cpt-link=" + std::to_string(link.link_record) +
      ";node=" + std::to_string(link.node_record) +
      ";mat=" + std::to_string(link.material_table) + ":" +
      std::to_string(link.material_record) +
      ";desc=" + std::to_string(link.descriptor_offset) +
      ";w1=" + std::to_string(link.word1) +
      ";w4=" + std::to_string(link.word4));
}

inline void TraceExactCPTDescriptorLink(std::span<const u8> bytes, const AssetInfo& cpt,
                                        const Descriptor& d, const CPTDescriptorLink& link)
{
  if (!MaterialTraceEnabled() || !link.valid)
    return;

  static unsigned link_logs = 0;
  if (link_logs < 256)
  {
    ++link_logs;
    std::fprintf(stderr,
                 "[moh-ps3-world-mat] EXACT LINK: source=%s desc=0x%zX link=%zu node=%zu@0x%08X mat=%zu:%zu@0x%08X w1=%08X w4=%08X\n",
                 cpt.relative_path.c_str(), d.source_offset, link.link_record,
                 link.node_record, link.node_offset, link.material_table,
                 link.material_record, link.material_offset, link.word1, link.word4);
  }

  // Dump full records only once per exact pointer target.  This keeps the log
  // useful while exposing enough structure to identify PS3 texture/sampler
  // fields and the NODE70 affine transform in the next bridge.
  static std::unordered_set<std::string> dumped_materials;
  static std::unordered_set<std::string> dumped_nodes;
  static unsigned material_dumps = 0;
  static unsigned node_dumps = 0;

  const std::string material_key = cpt.relative_path + "#" + std::to_string(link.material_offset);
  if (material_dumps < 96 && dumped_materials.insert(material_key).second &&
      std::size_t(link.material_offset) <= bytes.size() &&
      0x94 <= bytes.size() - std::size_t(link.material_offset))
  {
    ++material_dumps;
    std::fprintf(stderr,
                 "[moh-ps3-world-mat] MAT94 EXACT: source=%s mat=%zu:%zu off=0x%08X words=",
                 cpt.relative_path.c_str(), link.material_table, link.material_record,
                 link.material_offset);
    for (std::size_t off = 0; off < 0x94; off += 4)
      std::fprintf(stderr, "%s%08X", off ? "/" : "", BE32(bytes.data() + link.material_offset + off));
    std::fputc('\n', stderr);
  }

  const std::string node_key = cpt.relative_path + "#" + std::to_string(link.node_offset);
  if (node_dumps < 96 && dumped_nodes.insert(node_key).second &&
      std::size_t(link.node_offset) <= bytes.size() &&
      0x70 <= bytes.size() - std::size_t(link.node_offset))
  {
    ++node_dumps;
    std::fprintf(stderr,
                 "[moh-ps3-world-mat] NODE70 EXACT: source=%s node=%zu off=0x%08X words=",
                 cpt.relative_path.c_str(), link.node_record, link.node_offset);
    for (std::size_t off = 0; off < 0x70; off += 4)
      std::fprintf(stderr, "%s%08X", off ? "/" : "", BE32(bytes.data() + link.node_offset + off));
    std::fputc('\n', stderr);
  }
}

inline void TraceCPTHeaderTables(std::span<const u8> bytes, const AssetInfo& cpt)
{
  if (!MaterialTraceEnabled())
    return;

  const auto tables = DescribeCPTHeaderTables(bytes);
  for (std::size_t i = 0; i < tables.size(); ++i)
  {
    const auto& t = tables[i];
    const bool exact_material = i < 2 && t.valid && t.record_size == 0x94;
    const bool fixed_link = i == 2 && t.valid && t.offset && t.count &&
                            std::size_t(t.count) <= (bytes.size() - t.offset) / 0x14;
    const bool exact_node = i == 3 && t.valid && t.record_size == 0x70;
    std::fprintf(stderr,
                 "[moh-ps3-world-mat] TABLE%zu: source=%s off=0x%08X count=%u span=0x%zX rec=0x%zX rem=%zu%s\n",
                 i, cpt.relative_path.c_str(), t.offset, t.count, t.span, t.record_size,
                 t.remainder,
                 exact_material ? " [material 0x94]" :
                 fixed_link ? " [link 0x14 + tail padding]" :
                 exact_node ? " [node 0x70]" : "");
  }
  std::fprintf(stderr,
               "[moh-ps3-world-mat] POINTER GRAPH: source=%s TABLE2.W3=descriptor TABLE2.W2=MAT94 TABLE2.W0=NODE70 | exact-link decode enabled\n",
               cpt.relative_path.c_str());
}

inline bool QuickProbe(const Descriptor& d, std::span<const u8> vertices,
                       std::span<const u8> index_bytes)
{
  const u32 vertex_probe_count = std::min<u32>(d.vertex_count, 16);
  const u32 index_probe_count = std::min<u32>(d.index_count, 128);
  if (vertices.size() < std::size_t(vertex_probe_count) * d.stride ||
      index_bytes.size() < std::size_t(index_probe_count) * 2)
    return false;

  std::array<float, 3> first{};
  bool have_first = false;
  bool different = false;
  for (u32 i = 0; i < vertex_probe_count; ++i)
  {
    std::array<float, 3> p{};
    if (!DecodeComponents(vertices.data() + std::size_t(i) * d.stride, *d.position, 3, p.data(),
                          d.stride))
      return false;
    if (std::fabs(p[0]) > 1000000.0f || std::fabs(p[1]) > 1000000.0f ||
        std::fabs(p[2]) > 1000000.0f)
      return false;
    if (!have_first)
    {
      first = p;
      have_first = true;
    }
    else if (std::fabs(p[0] - first[0]) + std::fabs(p[1] - first[1]) +
                 std::fabs(p[2] - first[2]) >
             1.0e-5f)
    {
      different = true;
    }
  }
  if (!different)
    return false;

  for (u32 i = 0; i < index_probe_count; ++i)
    if (BE16(index_bytes.data() + std::size_t(i) * 2) >= d.vertex_count)
      return false;
  return true;
}

inline bool ValidateAndBuild(const Descriptor& d, std::span<const u8> vertices,
                             std::span<const u8> index_bytes, std::string source_name,
                             std::shared_ptr<StaticMesh>* out)
{
  if (!out)
    return false;
  const std::size_t vertex_bytes = std::size_t(d.vertex_count) * d.stride;
  const std::size_t index_size = std::size_t(d.index_count) * 2;
  if (vertices.size() < vertex_bytes || index_bytes.size() < index_size)
    return false;

  Submesh sub;
  sub.vertex_count = d.vertex_count;
  sub.index_count = d.index_count;
  sub.vertex_stride = d.stride;
  sub.attributes = d.attributes;
  sub.vertices.assign(vertices.begin(), vertices.begin() + vertex_bytes);
  sub.indices.reserve(d.index_count);
  for (u32 i = 0; i < d.index_count; ++i)
  {
    const u16 index = BE16(index_bytes.data() + std::size_t(i) * 2);
    if (index >= d.vertex_count)
      return false;
    sub.indices.push_back(index);
  }

  std::array<float, 3> minimum{std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::infinity()};
  std::array<float, 3> maximum{-std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()};
  sub.position_uv.reserve(d.vertex_count);
  bool normals_ok = d.normal != nullptr;
  bool uv0_ok = d.uv0 != nullptr;
  bool uv1_ok = d.uv1 != nullptr;

  for (u32 i = 0; i < d.vertex_count; ++i)
  {
    const u8* raw = vertices.data() + std::size_t(i) * d.stride;
    PositionUVVertex v;
    if (!DecodeComponents(raw, *d.position, 3, v.position.data(), d.stride))
      return false;
    for (unsigned axis = 0; axis < 3; ++axis)
    {
      if (std::fabs(v.position[axis]) > 1000000.0f)
        return false;
      minimum[axis] = std::min(minimum[axis], v.position[axis]);
      maximum[axis] = std::max(maximum[axis], v.position[axis]);
    }
    if (normals_ok && !DecodeNormal(raw, *d.normal, &v.normal, d.stride))
      normals_ok = false;
    if (uv0_ok && (!DecodeComponents(raw, *d.uv0, 2, v.uv0.data(), d.stride) ||
                   std::fabs(v.uv0[0]) > 65536.0f || std::fabs(v.uv0[1]) > 65536.0f))
      uv0_ok = false;
    if (uv1_ok && (!DecodeComponents(raw, *d.uv1, 2, v.uv1.data(), d.stride) ||
                   std::fabs(v.uv1[0]) > 65536.0f || std::fabs(v.uv1[1]) > 65536.0f))
      uv1_ok = false;
    sub.position_uv.push_back(v);
  }

  const float ex = maximum[0] - minimum[0];
  const float ey = maximum[1] - minimum[1];
  const float ez = maximum[2] - minimum[2];
  const float largest_extent = std::max({ex, ey, ez});
  if (!std::isfinite(largest_extent) || largest_extent < 0.01f || largest_extent > 250000.0f)
    return false;

  // Reject index streams that are mostly degenerate.  Sampling keeps preload
  // cheap on very large sectors while still killing accidental descriptors.
  std::size_t tested = 0;
  std::size_t degenerate = 0;
  const std::size_t triangle_count = sub.indices.size() / 3;
  const std::size_t step = std::max<std::size_t>(1, triangle_count / 256);
  for (std::size_t t = 0; t < triangle_count; t += step)
  {
    const u16 ia = sub.indices[t * 3 + 0];
    const u16 ib = sub.indices[t * 3 + 1];
    const u16 ic = sub.indices[t * 3 + 2];
    const auto& a = sub.position_uv[ia].position;
    const auto& b = sub.position_uv[ib].position;
    const auto& c = sub.position_uv[ic].position;
    const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
    const float acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
    const float nx = aby * acz - abz * acy;
    const float ny = abz * acx - abx * acz;
    const float nz = abx * acy - aby * acx;
    const float area2 = nx * nx + ny * ny + nz * nz;
    ++tested;
    if (!std::isfinite(area2) || area2 < 1.0e-12f)
      ++degenerate;
  }
  if (tested && degenerate * 4 > tested * 3)
    return false;

  sub.has_normal = normals_ok;
  sub.has_uv0 = uv0_ok;
  sub.has_uv1 = uv1_ok;

  auto mesh = std::make_shared<StaticMesh>();
  mesh->source_name = std::move(source_name);
  mesh->submeshes.push_back(std::move(sub));
  *out = std::move(mesh);
  return true;
}

inline std::vector<u32> OffsetVariants(u32 raw)
{
  std::vector<u32> out;
  for (u32 value : {raw, raw & ~1u, raw & ~0xFu, raw & ~0x7Fu})
  {
    if (std::find(out.begin(), out.end(), value) == out.end())
      out.push_back(value);
  }
  return out;
}

inline std::vector<std::shared_ptr<StaticMesh>> Decode(const AssetInfo& cpt, DecodeStats* stats)
{
  std::vector<std::shared_ptr<StaticMesh>> meshes;
  if (!Enabled() || !IsWorldChunk(cpt))
    return meshes;

  const AssetInfo* rsx = FindRSX(cpt);
  const std::vector<u8> bytes = PS3RemasterAssets::ReadBinary(cpt);
  if (bytes.size() < 32)
    return meshes;

  if (MaterialTraceEnabled())
  {
    std::fprintf(stderr, "[moh-ps3-world-mat] CPT HEADER: %s size=%zu words=",
                 cpt.relative_path.c_str(), bytes.size());
    const std::size_t words = std::min<std::size_t>(22, bytes.size() / 4);
    for (std::size_t i = 0; i < words; ++i)
      std::fprintf(stderr, "%s%08X", i ? "," : "", BE32(bytes.data() + i * 4));
    std::fputc('\n', stderr);
    TraceCPTHeaderTables(bytes, cpt);
  }

  DecodeStats local;
  std::unordered_set<u64> accepted_keys;
  const auto graph = MOHFrontline::WorldFormats::PS3CPT::Parse(bytes);
  if (!graph)
    return meshes;

  // Walk authored chunk records, not arbitrary payload words. Retail sectors
  // contain far more than 64 meshes, and a descriptor may have several node
  // instances. Keep each link so those instances retain their own transform.
  for (u32 record = 0; record < graph->tables[2].count; ++record)
  {
    const auto link = graph->ResolveLink(record);
    if (!link)
    {
      ++local.rejected;
      continue;
    }
    const std::size_t p = link->descriptor_offset;
    Descriptor d;
    if (!ParseDescriptor(bytes, p, &d))
      continue;
    ++local.descriptor_candidates;

    const std::size_t vertex_size = std::size_t(d.vertex_count) * d.stride;
    const std::size_t index_size = std::size_t(d.index_count) * 2;
    std::shared_ptr<StaticMesh> decoded;

    // Variant A: MSH-style payload embedded directly after the declaration.
    if (d.tail <= bytes.size() && vertex_size <= bytes.size() - d.tail &&
        index_size <= bytes.size() - d.tail - vertex_size)
    {
      const auto v = std::span<const u8>(bytes).subspan(d.tail, vertex_size);
      const auto i = std::span<const u8>(bytes).subspan(d.tail + vertex_size, index_size);
      if (ValidateAndBuild(d, v, i,
                           cpt.relative_path + "#cpt-inline-" + std::to_string(p), &decoded))
      {
        ++local.inline_candidates;
      }
    }

    // Variant B: declaration followed by one or more RSX vertex/index pointers.
    if (!decoded && rsx && d.tail + 24 <= bytes.size())
    {
      std::array<u32, 6> words{};
      for (std::size_t w = 0; w < words.size(); ++w)
        words[w] = BE32(bytes.data() + d.tail + w * 4);

      bool found = false;
      unsigned range_attempts = 0;
      for (std::size_t pair = 0; pair + 1 < words.size() && !found && range_attempts < 24; ++pair)
      {
        for (const bool swap : {false, true})
        {
          const u32 raw_v = words[pair + (swap ? 1 : 0)];
          const u32 raw_i = words[pair + (swap ? 0 : 1)];
          for (const u32 vertex_offset : OffsetVariants(raw_v))
          {
            if (vertex_offset >= rsx->size || vertex_size > rsx->size - vertex_offset)
              continue;
            for (const u32 index_offset : OffsetVariants(raw_i))
            {
              if (range_attempts++ >= 24)
                break;
              if (index_offset >= rsx->size || index_size > rsx->size - index_offset)
                continue;

              const std::size_t probe_v_size =
                  std::min(vertex_size, std::size_t(d.stride) * std::min<u32>(d.vertex_count, 16));
              const std::size_t probe_i_size =
                  std::min(index_size, std::size_t(2) * std::min<u32>(d.index_count, 128));
              const std::vector<u8> pv =
                  PS3RemasterAssets::ReadRange(*rsx, vertex_offset, probe_v_size);
              const std::vector<u8> pi =
                  PS3RemasterAssets::ReadRange(*rsx, index_offset, probe_i_size);
              if (!QuickProbe(d, pv, pi))
                continue;

              const std::vector<u8> v =
                  PS3RemasterAssets::ReadRange(*rsx, vertex_offset, vertex_size);
              const std::vector<u8> i =
                  PS3RemasterAssets::ReadRange(*rsx, index_offset, index_size);
              if (v.size() != vertex_size || i.size() != index_size)
                continue;
              if (ValidateAndBuild(d, v, i,
                                   cpt.relative_path + "#cpt-rsx-" + std::to_string(p) + "-" +
                                       std::to_string(vertex_offset) + "-" + std::to_string(index_offset),
                                   &decoded))
              {
                ++local.rsx_candidates;
                found = true;
                break;
              }
            }
            if (found || range_attempts >= 24)
              break;
          }
          if (found || range_attempts >= 24)
            break;
        }
      }
    }

    if (!decoded)
    {
      ++local.rejected;
      continue;
    }

    CPTDescriptorLink descriptor_link;
    descriptor_link.valid = true;
    descriptor_link.link_record = link->record;
    descriptor_link.node_record = link->node_index;
    descriptor_link.material_table = link->material_table;
    descriptor_link.material_record = link->material_index;
    descriptor_link.node_offset = link->node_offset;
    descriptor_link.material_offset = link->material_offset;
    descriptor_link.descriptor_offset = link->descriptor_offset;
    descriptor_link.word1 = link->word1;
    descriptor_link.word4 = link->word4;
    {
      Submesh& linked_submesh = decoded->submeshes.front();
      AttachCPTDescriptorLink(&linked_submesh, descriptor_link);

      const auto bounds = graph->NodeBounds(*link);
      linked_submesh.material_hints.push_back(
          "@cpt-node=record:" + std::to_string(link->node_index) +
          ";offset:" + std::to_string(link->node_offset) + ";applied:0");

      std::array<CPTMaterialTextureRef, 2> material_textures{};
      const std::uint64_t rsx_size = rsx ? static_cast<std::uint64_t>(rsx->size) : 0;
      for (unsigned slot = 0; slot < material_textures.size(); ++slot)
        DecodeCPTMaterialTextureRef(bytes, descriptor_link, slot, rsx_size,
                                    &material_textures[slot]);
      AttachCPTMaterialTextures(&linked_submesh, material_textures);
      TraceCPTResolvedState(cpt, descriptor_link,
                            bounds.value_or(MOHFrontline::WorldFormats::Bounds3{}),
                            material_textures);
      TraceExactCPTDescriptorLink(bytes, cpt, d, descriptor_link);
    }
    const Submesh& sub = decoded->submeshes.front();
    u64 key = FNV1a(sub.vertices);
    key = FNV1a(std::span<const u8>(reinterpret_cast<const u8*>(sub.indices.data()),
                                   sub.indices.size() * sizeof(u16)), key);
    key ^= u64(sub.vertex_count) << 32;
    key ^= sub.index_count;
    // Preserve distinct authored links/materials even when their mesh bytes match.
    if (descriptor_link.valid)
    {
      key ^= u64(descriptor_link.node_offset) * 0x9E3779B185EBCA87ULL;
      key ^= u64(descriptor_link.material_offset) * 0xC2B2AE3D27D4EB4FULL;
    }
    if (!accepted_keys.insert(key).second)
    {
      ++local.duplicate;
      continue;
    }

    if (TraceEnabled() && meshes.size() < 64)
    {
      std::fprintf(stderr,
                   "[moh-ps3-world-geo] READY: %s verts=%u indices=%u stride=%u attrs=%zu uv0=%d uv1=%d nrm=%d\n",
                   decoded->source_name.c_str(), sub.vertex_count, sub.index_count,
                   sub.vertex_stride, sub.attributes.size(), sub.has_uv0 ? 1 : 0,
                   sub.has_uv1 ? 1 : 0, sub.has_normal ? 1 : 0);
    }
    meshes.push_back(std::move(decoded));
    ++local.accepted;
  }

  if (TraceEnabled() || !meshes.empty())
  {
    std::fprintf(stderr,
                 "[moh-ps3-world-geo] CPT decoded: %s descriptors=%zu inline=%zu rsx=%zu accepted=%zu duplicate=%zu rejected=%zu rsx_source=%s\n",
                 cpt.relative_path.c_str(), local.descriptor_candidates, local.inline_candidates,
                 local.rsx_candidates, local.accepted, local.duplicate, local.rejected,
                 rsx ? rsx->relative_path.c_str() : "<missing>");
  }

  if (stats)
  {
    stats->descriptor_candidates += local.descriptor_candidates;
    stats->inline_candidates += local.inline_candidates;
    stats->rsx_candidates += local.rsx_candidates;
    stats->accepted += local.accepted;
    stats->duplicate += local.duplicate;
    stats->rejected += local.rejected;
  }
  return meshes;
}
}  // namespace PS3WorldGeometry
