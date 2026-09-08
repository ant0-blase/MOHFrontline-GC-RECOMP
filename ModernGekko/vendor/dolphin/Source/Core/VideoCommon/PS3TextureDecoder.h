#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace PS3TextureDecoder
{
struct Level
{
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint8_t> rgba;
};
enum class BlockFormat { BC1, BC2, BC3 };
struct CompressedLevel
{
  std::uint32_t width = 0, height = 0;
  BlockFormat format = BlockFormat::BC1;
  std::vector<std::uint8_t> blocks;
};
// Validated Frontline BC blocks are already little endian; strip linear row
// padding, preserve mip boundaries, and reject unsupported descriptors.
bool DecodeCompressed(std::span<const std::uint8_t> file, std::vector<CompressedLevel>* levels);
// Frontline compact GTF SSH only. Rejects unsupported descriptors and truncation.
bool Decode(std::span<const std::uint8_t> file, std::vector<Level>* levels);
}  // namespace PS3TextureDecoder
