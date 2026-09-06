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

  if (file.size() < 48 ||
      BE32(file.data()) != 0x02010100 ||
      BE32(file.data() + 8) != 1 ||
      BE32(file.data() + 16) != 20 ||
      file[26] != 2 ||
      file[27] != 0 ||
      BE16(file.data() + 36) != 1 ||
      BE32(file.data() + 28) != 0xAAE4)
  {
    return false;
  }

  const u32 bytes = BE32(file.data() + 20);
  const u32 count = file[25];
  const u32 format = file[24] & ~0x60u;
  const bool linear = (file[24] & 0x20u) != 0;

  u32 w = BE16(file.data() + 32);
  u32 h = BE16(file.data() + 34);

  constexpr u32 CELL_GCM_TEXTURE_A8R8G8B8 = 0x85;
  constexpr u32 CELL_GCM_TEXTURE_COMPRESSED_DXT1 = 0x86;
  constexpr u32 CELL_GCM_TEXTURE_COMPRESSED_DXT23 = 0x87;
  constexpr u32 CELL_GCM_TEXTURE_COMPRESSED_DXT45 = 0x88;

  if ((format != CELL_GCM_TEXTURE_A8R8G8B8 &&
       format != CELL_GCM_TEXTURE_COMPRESSED_DXT1 &&
       format != CELL_GCM_TEXTURE_COMPRESSED_DXT23 &&
       format != CELL_GCM_TEXTURE_COMPRESSED_DXT45) ||
      !w || !h || w > 4096 || h > 4096 || !count ||
      count > std::bit_width(std::max(w, h)) ||
      bytes != file.size() - 48 ||
      BE32(file.data() + 4) != bytes)
  {
    return false;
  }

  if (format == CELL_GCM_TEXTURE_A8R8G8B8 &&
      !linear &&
      (!std::has_single_bit(w) || !std::has_single_bit(h)))
  {
    return false;
  }

  std::size_t offset = 48;
  std::vector<Level> decoded;

  for (u32 mip = 0; mip < count; ++mip)
  {
    const bool rgba8 = format == CELL_GCM_TEXTURE_A8R8G8B8;
    u32 pitch = 0;
    std::size_t size = 0;

    if (rgba8)
    {
      pitch = linear && mip == 0 && BE32(file.data() + 40) ?
                  BE32(file.data() + 40) :
                  w * 4;

      if (pitch < w * 4)
        return false;

      size = std::size_t(pitch) * h;
    }
    else
    {
      const std::size_t block_bytes =
          format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 ? 8u : 16u;

      size = std::size_t((w + 3) / 4) *
             std::size_t((h + 3) / 4) *
             block_bytes;
    }

    if (size > file.size() - offset)
      return false;

    Level level{
        w,
        h,
        std::vector<u8>(std::size_t(w) * h * 4)};

    const u8* src = file.data() + offset;

    if (rgba8)
    {
      for (u32 y = 0; y < h; ++y)
      {
        for (u32 x = 0; x < w; ++x)
        {
          const auto p =
              src +
              (linear ?
                   std::size_t(y) * pitch + x * 4 :
                   std::size_t(Morton(x, y, w, h)) * 4);

          auto* d =
              level.rgba.data() +
              (std::size_t(y) * w + x) * 4;

          d[0] = p[1];
          d[1] = p[2];
          d[2] = p[3];
          d[3] = p[0];
        }
      }
    }
    else
    {
      const u32 blocks_x = (w + 3) / 4;
      const u32 blocks_y = (h + 3) / 4;

      for (u32 by = 0; by < blocks_y; ++by)
      {
        for (u32 bx = 0; bx < blocks_x; ++bx)
        {
          const bool bc1 =
              format == CELL_GCM_TEXTURE_COMPRESSED_DXT1;

          const std::size_t block_bytes =
              bc1 ? 8u : 16u;

          const u8* block =
              src +
              (std::size_t(by) * blocks_x + bx) *
                  block_bytes;

          const u8* color_block =
              bc1 ? block : block + 8;

          const u32 c0 =
              color_block[0] |
              (u32(color_block[1]) << 8);

          const u32 c1 =
              color_block[2] |
              (u32(color_block[3]) << 8);

          std::array<std::array<u8, 4>, 4> colors{
              RGB565(c0),
              RGB565(c1),
              {},
              {}};

          const bool four_color =
              !bc1 || c0 > c1;

          for (unsigned channel = 0; channel < 3; ++channel)
          {
            if (four_color)
            {
              colors[2][channel] =
                  static_cast<u8>(
                      (2 * colors[0][channel] +
                       colors[1][channel]) / 3);

              colors[3][channel] =
                  static_cast<u8>(
                      (colors[0][channel] +
                       2 * colors[1][channel]) / 3);
            }
            else
            {
              colors[2][channel] =
                  static_cast<u8>(
                      (colors[0][channel] +
                       colors[1][channel]) / 2);

              colors[3][channel] = 0;
            }
          }

          colors[2][3] = 255;
          colors[3][3] = four_color ? 255 : 0;

          const u32 color_bits =
              color_block[4] |
              (u32(color_block[5]) << 8) |
              (u32(color_block[6]) << 16) |
              (u32(color_block[7]) << 24);

          std::array<u8, 8> alpha_palette{};
          std::uint64_t alpha_bits = 0;

          if (format == CELL_GCM_TEXTURE_COMPRESSED_DXT23)
          {
            for (unsigned byte = 0; byte < 8; ++byte)
            {
              alpha_bits |=
                  std::uint64_t(block[byte]) <<
                  (byte * 8);
            }
          }
          else if (format == CELL_GCM_TEXTURE_COMPRESSED_DXT45)
          {
            const u8 a0 = block[0];
            const u8 a1 = block[1];

            alpha_palette[0] = a0;
            alpha_palette[1] = a1;

            if (a0 > a1)
            {
              alpha_palette[2] = static_cast<u8>((6 * a0 + a1) / 7);
              alpha_palette[3] = static_cast<u8>((5 * a0 + 2 * a1) / 7);
              alpha_palette[4] = static_cast<u8>((4 * a0 + 3 * a1) / 7);
              alpha_palette[5] = static_cast<u8>((3 * a0 + 4 * a1) / 7);
              alpha_palette[6] = static_cast<u8>((2 * a0 + 5 * a1) / 7);
              alpha_palette[7] = static_cast<u8>((a0 + 6 * a1) / 7);
            }
            else
            {
              alpha_palette[2] = static_cast<u8>((4 * a0 + a1) / 5);
              alpha_palette[3] = static_cast<u8>((3 * a0 + 2 * a1) / 5);
              alpha_palette[4] = static_cast<u8>((2 * a0 + 3 * a1) / 5);
              alpha_palette[5] = static_cast<u8>((a0 + 4 * a1) / 5);
              alpha_palette[6] = 0;
              alpha_palette[7] = 255;
            }

            for (unsigned byte = 0; byte < 6; ++byte)
            {
              alpha_bits |=
                  std::uint64_t(block[2 + byte]) <<
                  (byte * 8);
            }
          }

          for (u32 y = 0; y < 4 && by * 4 + y < h; ++y)
          {
            for (u32 x = 0; x < 4 && bx * 4 + x < w; ++x)
            {
              const u32 pixel = y * 4 + x;

              auto color =
                  colors[
                      (color_bits >> (2 * pixel)) &
                      3u];

              if (format == CELL_GCM_TEXTURE_COMPRESSED_DXT23)
              {
                color[3] =
                    static_cast<u8>(
                        ((alpha_bits >> (4 * pixel)) &
                         0xFu) *
                        17u);
              }
              else if (format == CELL_GCM_TEXTURE_COMPRESSED_DXT45)
              {
                color[3] =
                    alpha_palette[
                        (alpha_bits >> (3 * pixel)) &
                        7u];
              }

              std::copy(
                  color.begin(),
                  color.end(),
                  level.rgba.begin() +
                      ((std::size_t(by * 4 + y) * w) +
                       bx * 4 + x) *
                          4);
            }
          }
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
