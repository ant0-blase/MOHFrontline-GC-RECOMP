#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace PS3FontParser
{

struct Glyph
{
  std::uint16_t codepoint = 0;
  std::uint16_t flags = 0;

  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  std::int16_t bearing = 0;
  std::uint16_t advance = 0;
};

struct Font
{
  std::filesystem::path absolute_path;
  std::string relative_path;
  std::string filename;

  std::uint32_t glyph_count = 0;

  // Logical PS3 UI canvas. This is NOT necessarily the GTF atlas size.
  std::uint32_t canvas_width = 0;
  std::uint32_t canvas_height = 0;

  std::uint32_t payload_offset = 0;

  bool ps3_gtf = false;

  std::vector<Glyph> glyphs;

  // Raw GTF payload kept available for the later CFont GPU bridge.
  std::vector<std::uint8_t> texture_payload;

  // Decoded RSX/GTF atlas used by the native PC CFont renderer.
  //
  // Frontline PS3 SFNH fonts use a compact GTF container followed by a
  // CellGcmTexture. The fonts observed in Frontline use linear
  // A4R4G4B4 (0x83 | CELL_GCM_TEXTURE_LN = 0xA3).
  std::uint32_t atlas_width = 0;
  std::uint32_t atlas_height = 0;
  std::uint32_t atlas_pitch = 0;
  std::uint8_t gtf_format = 0;

  bool atlas_rgba_ready = false;
  std::vector<std::uint8_t> atlas_rgba;
};

void Initialize(const std::filesystem::path& root);
void Shutdown();

bool IsReady();

const std::vector<Font>& GetFonts();

const Font* FindByFilename(std::string_view filename);
const Glyph* FindGlyph(const Font& font, std::uint16_t codepoint);

}  // namespace PS3FontParser
