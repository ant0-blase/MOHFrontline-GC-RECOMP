#include "VideoCommon/PS3FontParser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace PS3FontParser
{
namespace
{

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr std::size_t SFNH_RECORD_SIZE = 24;
constexpr std::size_t SFNH_RECORD_BASE = 0x14;

std::vector<Font> s_fonts;
std::unordered_map<std::string, std::size_t> s_by_filename;
bool s_ready = false;

u16 Read16BE(const u8* p)
{
  return
      static_cast<u16>(
          (u16(p[0]) << 8) |
           u16(p[1]));
}

u32 Read32BE(const u8* p)
{
  return
      (u32(p[0]) << 24) |
      (u32(p[1]) << 16) |
      (u32(p[2]) << 8) |
       u32(p[3]);
}

std::string Lower(std::string value)
{
  std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](unsigned char c)
      {
        return static_cast<char>(
            std::tolower(c));
      });

  return value;
}

bool ReadWholeFile(
    const std::filesystem::path& path,
    std::vector<u8>* out)
{
  std::ifstream file(
      path,
      std::ios::binary |
      std::ios::ate);

  if (!file)
    return false;

  const std::streamoff end =
      file.tellg();

  if (end <= 0 ||
      end > 128ll * 1024ll * 1024ll)
  {
    return false;
  }

  out->resize(
      static_cast<std::size_t>(end));

  file.seekg(
      0,
      std::ios::beg);

  return static_cast<bool>(
      file.read(
          reinterpret_cast<char*>(
              out->data()),
          static_cast<std::streamsize>(
              out->size())));
}

bool IsSFNH(
    const std::vector<u8>& data)
{
  return
      data.size() >= 0x20 &&
      data[0] == 'S' &&
      data[1] == 'F' &&
      data[2] == 'N' &&
      data[3] == 'H';
}

bool DecodeFrontlineGTFAtlas(Font* font)
{
  if (!font ||
      font->texture_payload.size() < 0x30)
  {
    return false;
  }

  const auto& payload =
      font->texture_payload;

  // Frontline's SFNH embeds a compact one-texture GTF:
  //
  //   00 version
  //   04 total texture bytes
  //   08 texture count
  //   0C texture id
  //   10 descriptor offset (= 0x14)
  //   14 texture byte size
  //   18 CellGcmTexture
  //
  // CellGcmTexture:
  //   +00 format
  //   +01 mipmap
  //   +02 dimension
  //   +03 cubemap
  //   +04 remap
  //   +08 width
  //   +0A height
  //   +0C depth
  //   +0E location
  //   +10 pitch
  //   +14 offset
  //
  // The payload image itself starts immediately after the 0x30-byte compact
  // header/descriptor in all observed MOH Frontline SFNH files.

  const u32 texture_size =
      Read32BE(
          payload.data() + 0x14);

  const u8 format =
      payload[0x18];

  // Strip CELL_GCM_TEXTURE_LN / UN flags.
  const u8 base_format =
      static_cast<u8>(
          format & ~0x60u);

  const bool linear =
      (format & 0x20u) != 0;

  const u32 width =
      Read16BE(
          payload.data() + 0x20);

  const u32 height =
      Read16BE(
          payload.data() + 0x22);

  u32 pitch =
      Read32BE(
          payload.data() + 0x28);

  // CELL_GCM_TEXTURE_A4R4G4B4 = 0x83.
  if (base_format != 0x83u ||
      !linear ||
      width == 0 ||
      height == 0 ||
      width > 8192 ||
      height > 8192)
  {
    std::fprintf(
        stderr,
        "[moh-ps3-font] unsupported GTF atlas %s: "
        "format=%02X base=%02X linear=%d %ux%u\\n",
        font->filename.c_str(),
        static_cast<unsigned>(format),
        static_cast<unsigned>(base_format),
        linear ? 1 : 0,
        width,
        height);

    return false;
  }

  if (pitch == 0)
    pitch = width * 2u;

  if (pitch < width * 2u)
    return false;

  if (texture_size == 0 ||
      texture_size > payload.size())
  {
    return false;
  }

  // The compact GTF has no explicit padded file-header size field.
  // texture_size precisely describes the texture bytes, so deriving the
  // start from EOF works for all observed SFNH files and also tolerates
  // descriptor padding.
  const std::size_t data_offset =
      payload.size() -
      static_cast<std::size_t>(
          texture_size);

  const std::size_t required =
      static_cast<std::size_t>(
          pitch) *
      static_cast<std::size_t>(
          height);

  if (data_offset < 0x30 ||
      required > texture_size ||
      data_offset + required >
          payload.size())
  {
    std::fprintf(
        stderr,
        "[moh-ps3-font] malformed GTF atlas %s: "
        "data=0x%zX tex=%u pitch=%u %ux%u\\n",
        font->filename.c_str(),
        data_offset,
        texture_size,
        pitch,
        width,
        height);

    return false;
  }

  std::vector<u8> rgba;

  rgba.resize(
      static_cast<std::size_t>(
          width) *
      static_cast<std::size_t>(
          height) *
      4u);

  const u8* src_base =
      payload.data() +
      data_offset;

  for (u32 y = 0;
       y < height;
       ++y)
  {
    const u8* row =
        src_base +
        static_cast<std::size_t>(y) *
        pitch;

    for (u32 x = 0;
         x < width;
         ++x)
    {
      // PS3 PPU/GTF metadata and this texture payload are big-endian.
      const u16 pixel =
          static_cast<u16>(
              (u16(row[x * 2u + 0u]) << 8) |
               u16(row[x * 2u + 1u]));

      const u8 a =
          static_cast<u8>(
              ((pixel >> 12) & 0x0Fu) *
              17u);

      const u8 r =
          static_cast<u8>(
              ((pixel >> 8) & 0x0Fu) *
              17u);

      const u8 g =
          static_cast<u8>(
              ((pixel >> 4) & 0x0Fu) *
              17u);

      const u8 b =
          static_cast<u8>(
              (pixel & 0x0Fu) *
              17u);

      const std::size_t dst =
          (static_cast<std::size_t>(y) *
               width +
           x) *
          4u;

      // Preserve the PS3 atlas colour and alpha. ImGui tint is white.
      rgba[dst + 0] = r;
      rgba[dst + 1] = g;
      rgba[dst + 2] = b;
      rgba[dst + 3] = a;
    }
  }

  font->atlas_width = width;
  font->atlas_height = height;
  font->atlas_pitch = pitch;
  font->gtf_format = format;
  font->atlas_rgba =
      std::move(rgba);
  font->atlas_rgba_ready = true;

  std::fprintf(
      stderr,
      "[moh-ps3-font] GTF atlas ready %s: "
      "%ux%u pitch=%u format=%02X "
      "A4R4G4B4-linear -> RGBA8\\n",
      font->filename.c_str(),
      width,
      height,
      pitch,
      static_cast<unsigned>(
          format));

  return true;
}

bool ParseSFNH(
    const std::filesystem::path& path,
    const std::filesystem::path& root,
    Font* out)
{
  std::vector<u8> data;

  if (!ReadWholeFile(path, &data) ||
      !IsSFNH(data))
  {
    return false;
  }

  const u32 glyph_count =
      Read32BE(data.data() + 0x04);

  const u32 canvas_width =
      Read32BE(data.data() + 0x08);

  const u32 canvas_height =
      Read32BE(data.data() + 0x0C);

  const u32 payload_offset =
      Read32BE(data.data() + 0x10);

  if (glyph_count == 0 ||
      glyph_count > 0x10000)
  {
    return false;
  }

  const std::size_t expected_payload =
      SFNH_RECORD_BASE +
      static_cast<std::size_t>(
          glyph_count) *
      SFNH_RECORD_SIZE;

  if (payload_offset < expected_payload ||
      payload_offset > data.size())
  {
    return false;
  }

  Font font;

  font.absolute_path = path;
  font.filename =
      path.filename().string();

  std::error_code ec;

  const auto relative =
      std::filesystem::relative(
          path,
          root,
          ec);

  font.relative_path =
      ec ?
          font.filename :
          relative.generic_string();

  font.glyph_count =
      glyph_count;

  font.canvas_width =
      canvas_width;

  font.canvas_height =
      canvas_height;

  font.payload_offset =
      payload_offset;

  font.glyphs.reserve(
      glyph_count);

  for (u32 i = 0;
       i < glyph_count;
       ++i)
  {
    const std::size_t off =
        SFNH_RECORD_BASE +
        static_cast<std::size_t>(i) *
        SFNH_RECORD_SIZE;

    if (off + SFNH_RECORD_SIZE >
        data.size())
    {
      return false;
    }

    const u32 code_flags =
        Read32BE(
            data.data() + off + 0x00);

    const u32 bearing_advance =
        Read32BE(
            data.data() + off + 0x14);

    Glyph glyph;

    glyph.codepoint =
        static_cast<u16>(
            code_flags >> 16);

    glyph.flags =
        static_cast<u16>(
            code_flags & 0xffffu);

    glyph.x =
        Read32BE(
            data.data() + off + 0x04);

    glyph.y =
        Read32BE(
            data.data() + off + 0x08);

    glyph.width =
        Read32BE(
            data.data() + off + 0x0C);

    glyph.height =
        Read32BE(
            data.data() + off + 0x10);

    glyph.bearing =
        static_cast<std::int16_t>(
            bearing_advance >> 16);

    glyph.advance =
        static_cast<u16>(
            bearing_advance & 0xffffu);

    font.glyphs.emplace_back(
        glyph);
  }

  font.texture_payload.assign(
      data.begin() + payload_offset,
      data.end());

  font.ps3_gtf =
      font.texture_payload.size() >= 4 &&
      font.texture_payload[0] == 0x02 &&
      font.texture_payload[1] == 0x01 &&
      font.texture_payload[2] == 0x01 &&
      font.texture_payload[3] == 0x00;

  if (font.ps3_gtf)
    DecodeFrontlineGTFAtlas(&font);

  *out =
      std::move(font);

  return true;
}

void LogGlyph(
    const Font& font,
    u16 codepoint)
{
  const Glyph* glyph = nullptr;

  for (const Glyph& g :
       font.glyphs)
  {
    if (g.codepoint ==
        codepoint)
    {
      glyph = &g;
      break;
    }
  }

  if (!glyph)
    return;

  std::fprintf(
      stderr,
      "[moh-ps3-font]   U+%04X "
      "xy=%u,%u wh=%ux%u "
      "bearing=%d advance=%u "
      "flags=%04X\n",
      static_cast<unsigned>(
          glyph->codepoint),
      glyph->x,
      glyph->y,
      glyph->width,
      glyph->height,
      static_cast<int>(
          glyph->bearing),
      static_cast<unsigned>(
          glyph->advance),
      static_cast<unsigned>(
          glyph->flags));
}

}  // namespace

void Initialize(
    const std::filesystem::path& root)
{
  Shutdown();

  std::error_code ec;

  if (root.empty() ||
      !std::filesystem::exists(root, ec) ||
      !std::filesystem::is_directory(root, ec))
  {
    return;
  }

  const auto options =
      std::filesystem::
          directory_options::
              skip_permission_denied;

  std::filesystem::
      recursive_directory_iterator it(
          root,
          options,
          ec);

  const std::filesystem::
      recursive_directory_iterator end;

  for (;
       !ec && it != end;
       it.increment(ec))
  {
    std::error_code file_ec;

    if (!it->is_regular_file(
            file_ec))
    {
      continue;
    }

    if (Lower(
            it->path()
                .extension()
                .string()) != ".sfn")
    {
      continue;
    }

    Font font;

    if (!ParseSFNH(
            it->path(),
            root,
            &font))
    {
      continue;
    }

    const std::size_t index =
        s_fonts.size();

    s_by_filename.emplace(
        Lower(font.filename),
        index);

    std::fprintf(
        stderr,
        "[moh-ps3-font] SFNH parsed %s: "
        "glyphs=%u canvas=%ux%u "
        "table=0x%X bitmap=0x%X "
        "payload=%zu kind=%s "
        "table-exact sorted\n",
        font.filename.c_str(),
        font.glyph_count,
        font.canvas_width,
        font.canvas_height,
        font.payload_offset,
        font.payload_offset,
        font.texture_payload.size(),
        font.ps3_gtf ?
            "PS3-GTF" :
            "unknown");

    if (font.texture_payload.size() >=
        32)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-font]   payload magic:");

      for (std::size_t i = 0;
           i < 32;
           ++i)
      {
        std::fprintf(
            stderr,
            " %02X",
            static_cast<unsigned>(
                font.texture_payload[i]));
      }

      std::fprintf(
          stderr,
          "\n");
    }

    LogGlyph(
        font,
        0x0020);

    LogGlyph(
        font,
        0x0041);

    s_fonts.emplace_back(
        std::move(font));
  }

  s_ready = true;

  std::fprintf(
      stderr,
      "[moh-ps3-font] parsed %zu fonts "
      "(%zu SFNH)\n",
      s_fonts.size(),
      s_fonts.size());
}

void Shutdown()
{
  s_ready = false;

  s_fonts.clear();
  s_by_filename.clear();
}

bool IsReady()
{
  return s_ready;
}

const std::vector<Font>&
GetFonts()
{
  return s_fonts;
}

const Font* FindByFilename(
    std::string_view filename)
{
  const auto it =
      s_by_filename.find(
          Lower(
              std::string(
                  filename)));

  if (it ==
      s_by_filename.end())
  {
    return nullptr;
  }

  return
      &s_fonts[it->second];
}

const Glyph* FindGlyph(
    const Font& font,
    u16 codepoint)
{
  for (const Glyph& glyph :
       font.glyphs)
  {
    if (glyph.codepoint ==
        codepoint)
    {
      return &glyph;
    }
  }

  return nullptr;
}

}  // namespace PS3FontParser
