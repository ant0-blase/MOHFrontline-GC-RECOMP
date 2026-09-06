#include "VideoCommon/PS3TextureDecoder.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>

namespace PS3TextureDecoder
{
namespace
{
using u8 = std::uint8_t;
using u32 = std::uint32_t;
u32 BE16(const u8* p)
{
  return (u32(p[0]) << 8) | p[1];
}
u32 BE32(const u8* p)
{
  return (BE16(p) << 16) | BE16(p + 2);
}
u32 Morton(u32 x, u32 y, u32 w, u32 h)
{
  u32 result = 0, bit = 0;
  for (u32 mask = 1; mask < std::max(w, h); mask <<= 1)
  {
    if (mask < w)
    {
      if (x & mask)
        result |= 1u << bit;
      ++bit;
    }
    if (mask < h)
    {
      if (y & mask)
        result |= 1u << bit;
      ++bit;
    }
  }
  return result;
}
std::array<u8, 4> RGB565(u32 c)
{
  const u32 r = c >> 11, g = (c >> 5) & 63, b = c & 31;
  return {u8((r << 3) | (r >> 2)), u8((g << 2) | (g >> 4)), u8((b << 3) | (b >> 2)), 255};
}
}  // namespace

bool Decode(std::span<const u8> file, std::vector<Level>* levels)
{
  if (!levels)
    return false;
  levels->clear();
  if (file.size() < 48 || BE32(file.data()) != 0x02010100 || BE32(file.data() + 8) != 1 ||
      BE32(file.data() + 16) != 20 || file[26] != 2 || file[27] != 0 ||
      BE16(file.data() + 36) != 1 || BE32(file.data() + 28) != 0xAAE4)
    return false;
  const u32 bytes = BE32(file.data() + 20), count = file[25];
  const u32 format = file[24] & ~0x60u;
  const bool linear = (file[24] & 0x20) != 0;
  u32 w = BE16(file.data() + 32), h = BE16(file.data() + 34);
  if ((format != 0x85 && format != 0x86) || !w || !h || w > 4096 || h > 4096 || !count ||
      count > std::bit_width(std::max(w, h)) || bytes != file.size() - 48 ||
      BE32(file.data() + 4) != bytes ||
      (!linear && (!std::has_single_bit(w) || !std::has_single_bit(h))))
    return false;
  std::size_t offset = 48;
  std::vector<Level> decoded;
  for (u32 mip = 0; mip < count; ++mip)
  {
    const u32 pitch = linear && mip == 0 && BE32(file.data() + 40) ? BE32(file.data() + 40) : w * 4;
    const std::size_t size =
        format == 0x85 ? std::size_t(pitch) * h : std::size_t((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (pitch < w * 4 || size > file.size() - offset)
      return false;
    Level level{w, h, std::vector<u8>(std::size_t(w) * h * 4)};
    const u8* src = file.data() + offset;
    if (format == 0x85)
    {
      for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
        {
          const auto p =
              src + (linear ? std::size_t(y) * pitch + x * 4 : std::size_t(Morton(x, y, w, h)) * 4);
          auto* d = level.rgba.data() + (std::size_t(y) * w + x) * 4;
          d[0] = p[1];
          d[1] = p[2];
          d[2] = p[3];
          d[3] = p[0];
        }
    }
    else
    {
      // RSX 2D BC1 blocks are linear, with standard little-endian BC fields,
      // even when the descriptor's LN bit is clear (unlike ARGB texels).
      for (u32 by = 0; by < (h + 3) / 4; ++by)
        for (u32 bx = 0; bx < (w + 3) / 4; ++bx)
        {
          const u8* b = src + (std::size_t(by) * ((w + 3) / 4) + bx) * 8;
          const u32 c0 = b[0] | (u32(b[1]) << 8), c1 = b[2] | (u32(b[3]) << 8);
          std::array<std::array<u8, 4>, 4> c{RGB565(c0), RGB565(c1), {}, {}};
          for (unsigned k = 0; k < 3; ++k)
          {
            c[2][k] = c0 > c1 ? (2 * c[0][k] + c[1][k]) / 3 : (c[0][k] + c[1][k]) / 2;
            c[3][k] = c0 > c1 ? (c[0][k] + 2 * c[1][k]) / 3 : 0;
          }
          c[2][3] = 255;
          c[3][3] = c0 > c1 ? 255 : 0;
          const u32 bits = b[4] | (u32(b[5]) << 8) | (u32(b[6]) << 16) | (u32(b[7]) << 24);
          for (u32 y = 0; y < 4 && by * 4 + y < h; ++y)
            for (u32 x = 0; x < 4 && bx * 4 + x < w; ++x)
            {
              const auto& color = c[(bits >> (2 * (y * 4 + x))) & 3];
              std::copy(color.begin(), color.end(),
                        level.rgba.begin() + ((std::size_t(by * 4 + y) * w) + bx * 4 + x) * 4);
            }
        }
    }
    decoded.push_back(std::move(level));
    offset += size;
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  if (offset != file.size())
    return false;
  *levels = std::move(decoded);
  return true;
}
}  // namespace PS3TextureDecoder
