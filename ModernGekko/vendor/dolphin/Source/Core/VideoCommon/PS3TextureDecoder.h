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
// Frontline compact GTF SSH only. Rejects unsupported descriptors and truncation.
bool Decode(std::span<const std::uint8_t> file, std::vector<Level>* levels);
}  // namespace PS3TextureDecoder
