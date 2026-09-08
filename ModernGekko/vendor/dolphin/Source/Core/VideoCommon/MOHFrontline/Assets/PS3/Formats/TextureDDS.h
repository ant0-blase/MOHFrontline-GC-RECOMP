#pragma once
#include "VideoCommon/PS3TextureDecoder.h"
#include <array>
#include <span>

namespace MOHFrontline::PS3
{
inline std::vector<std::uint8_t> DDSHeader(unsigned width, unsigned height, unsigned mips,
                                           unsigned pitch_or_size, unsigned fourcc)
{
  std::vector<std::uint8_t> bytes(128);
  auto put = [&](unsigned p, unsigned v) { for (unsigned i=0; i<4; ++i) bytes[p+i] = v>>(8*i); };
  put(0, 0x20534444); put(4, 124);
  put(8, 0x1007 | (fourcc ? 0x80000 : 8) | (mips>1 ? 0x20000 : 0));
  put(12, height); put(16, width); put(20, pitch_or_size); put(28, mips);
  put(76, 32); put(80, fourcc ? 4 : 0x41); put(84, fourcc);
  if (!fourcc)
  { put(88, 32); put(92, 0xff); put(96, 0xff00); put(100, 0xff0000); put(104, 0xff000000); }
  put(108, 0x1000 | (mips>1 ? 0x400008 : 0));
  return bytes;
}
inline std::vector<std::uint8_t> EncodeDDS(std::span<const PS3TextureDecoder::Level> levels)
{
  if (levels.empty()) return {};
  auto bytes = DDSHeader(levels[0].width, levels[0].height, levels.size(), levels[0].width*4, 0);
  for (const auto& level : levels) bytes.insert(bytes.end(), level.rgba.begin(), level.rgba.end());
  return bytes;
}
inline std::vector<std::uint8_t> EncodeDDS(std::span<const PS3TextureDecoder::CompressedLevel> levels)
{
  if (levels.empty()) return {};
  const auto format = levels[0].format;
  const unsigned fourcc = format == PS3TextureDecoder::BlockFormat::BC1 ? 0x31545844 :
      format == PS3TextureDecoder::BlockFormat::BC2 ? 0x33545844 : 0x35545844;
  auto bytes = DDSHeader(levels[0].width, levels[0].height, levels.size(), levels[0].blocks.size(), fourcc);
  for (const auto& level : levels) bytes.insert(bytes.end(), level.blocks.begin(), level.blocks.end());
  return bytes;
}
}
