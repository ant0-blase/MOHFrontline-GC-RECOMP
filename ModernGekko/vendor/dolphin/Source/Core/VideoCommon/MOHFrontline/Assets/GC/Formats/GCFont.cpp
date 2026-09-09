#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCFont.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "VideoCommon/MOHFrontline/Engine/NativePCStatus.h"

namespace MOHFrontline::GCFont
{
namespace
{
u16 BE16(const u8* p) { return static_cast<u16>((u16(p[0]) << 8) | u16(p[1])); }
u32 BE24(const u8* p) { return (u32(p[0]) << 16) | (u32(p[1]) << 8) | u32(p[2]); }
u32 BE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}
u32 LE32(const u8* p)
{
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

const char* AtlasName(u8 type)
{
  switch (type & 0x7f)
  {
  case 0x16: return "RGBA8";
  case 0x18: return "PAL4/C4";
  case 0x19: return "PAL8/C8";
  case 0x1e: return "CMPR";
  default: return "unknown";
  }
}
}  // namespace

bool Inspect(std::string_view name, std::span<const u8> bytes)
{
  // Verified from the supplied GC disc assets and Dolphin MEM1 dumps.
  // FNTG header is little-endian for total size, while glyph/table offsets are
  // big-endian. Glyph records start at 0x80 and are 12 bytes each.
  if (bytes.size() < 0x90 || std::memcmp(bytes.data(), "FNTG", 4) != 0)
    return false;

  const u32 total = LE32(bytes.data() + 4);
  const u16 max_codepoint = BE16(bytes.data() + 8);
  const u16 glyph_count = BE16(bytes.data() + 10);
  const u32 atlas_offset = BE32(bytes.data() + 0x1c);
  if (total > bytes.size() || total < 0x90 || glyph_count == 0 || glyph_count > 8192 ||
      std::uint64_t{0x80} + std::uint64_t{glyph_count} * 12u > bytes.size() ||
      atlas_offset + 16u > bytes.size())
  {
    return false;
  }

  for (u32 i = 0; i < glyph_count; ++i)
  {
    const std::size_t p = 0x80u + std::size_t(i) * 12u;
    const u16 codepoint = BE16(bytes.data() + p);
    const u8 width = bytes[p + 2];
    const u8 height = bytes[p + 3];
    const u16 x = BE16(bytes.data() + p + 4);
    const u16 y = BE16(bytes.data() + p + 6);
    if (codepoint > max_codepoint || width > 250 || height > 250 || x > 4096 || y > 4096)
      return false;
  }

  const u8 atlas_type = bytes[atlas_offset] & 0x7f;
  const u32 atlas_block_size = BE24(bytes.data() + atlas_offset + 1);
  const u16 atlas_width = BE16(bytes.data() + atlas_offset + 4);
  const u16 atlas_height = BE16(bytes.data() + atlas_offset + 6);
  if (atlas_block_size < 16 || atlas_block_size > bytes.size() - atlas_offset ||
      atlas_width == 0 || atlas_height == 0 || atlas_width > 4096 || atlas_height > 4096)
  {
    return false;
  }

  u8 palette_type = 0;
  u32 palette_entries = 0;
  const std::size_t palette_offset = atlas_offset + atlas_block_size;
  if ((atlas_type == 0x18 || atlas_type == 0x19) && palette_offset + 16 <= bytes.size())
  {
    palette_type = bytes[palette_offset] & 0x7f;
    const u32 palette_block_size = BE24(bytes.data() + palette_offset + 1);
    if (palette_block_size >= 16 && palette_block_size <= bytes.size() - palette_offset)
      palette_entries = (palette_block_size - 16u) / 2u;
  }

  char detail[256]{};
  std::snprintf(detail, sizeof(detail),
                "FNTG glyphs=%u maxcp=%u atlas=%ux%u type=0x%02x/%s palette=0x%02x entries=%u",
                glyph_count, max_codepoint, atlas_width, atlas_height, atlas_type,
                AtlasName(atlas_type), palette_type, palette_entries);
  NativePCStatus::Native(NativePCStatus::Domain::Font, name, detail);
  return true;
}
}  // namespace MOHFrontline::GCFont
