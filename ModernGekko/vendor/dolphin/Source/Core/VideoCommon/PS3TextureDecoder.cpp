#include "VideoCommon/PS3TextureDecoder.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstring>

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

u32 LE16(const u8* p)
{
  return u32(p[0]) | (u32(p[1]) << 8);
}

u32 LE24(const u8* p)
{
  return
      u32(p[0]) |
      (u32(p[1]) << 8) |
      (u32(p[2]) << 16);
}

u32 LE32(const u8* p)
{
  return LE16(p) | (LE16(p + 2) << 16);
}

bool StartsWith(
    std::span<const u8> data,
    const char* magic)
{
  return
      data.size() >= 4 &&
      std::memcmp(data.data(), magic, 4) == 0;
}

bool TrailingZeroPadding(
    std::span<const u8> data,
    std::size_t used)
{
  if (used > data.size())
    return false;

  for (std::size_t i = used;
       i < data.size();
       ++i)
  {
    if (data[i] != 0)
      return false;
  }

  return true;
}

u8 Expand5(u32 value)
{
  return static_cast<u8>((value << 3) | (value >> 2));
}

u8 ExpandPS2Alpha(u8 value)
{
  if (value <= 128)
    return static_cast<u8>(std::min(255u, u32(value) * 2u));

  return value;
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

bool DecodeEAOldShape(
    std::span<const u8> file,
    std::vector<Level>* levels)
{
  if (!levels || file.size() < 24)
    return false;

  const bool old_shape =
      StartsWith(file, "SHPS") ||
      StartsWith(file, "SHPI") ||
      StartsWith(file, "ShpS");

  if (!old_shape)
    return false;

  const u32 declared_size =
      LE32(file.data() + 4);

  const u32 entry_count =
      LE32(file.data() + 8);

  if (!entry_count ||
      entry_count > 1024 ||
      16ull + std::uint64_t(entry_count) * 8ull > file.size())
  {
    return false;
  }

  const std::size_t logical_end =
      declared_size >= 16 && declared_size <= file.size() ?
          declared_size :
          file.size();

  auto palette_color =
      [](std::span<const u8> palette,
         u8 palette_type,
         std::size_t index)
      {
        std::array<u8, 4> color{255, 255, 255, 255};

        if (palette_type == 33 || palette_type == 42)
        {
          const std::size_t offset = index * 4;
          if (offset + 4 <= palette.size())
          {
            color[0] = palette[offset + 0];
            color[1] = palette[offset + 1];
            color[2] = palette[offset + 2];
            color[3] = ExpandPS2Alpha(palette[offset + 3]);
          }
        }
        else if (palette_type == 36)
        {
          const std::size_t offset = index * 3;
          if (offset + 3 <= palette.size())
          {
            color[0] = palette[offset + 0];
            color[1] = palette[offset + 1];
            color[2] = palette[offset + 2];
            color[3] = 255;
          }
        }

        return color;
      };

  for (u32 directory_index = 0;
       directory_index < entry_count;
       ++directory_index)
  {
    const std::size_t dir =
        16 + std::size_t(directory_index) * 8;

    const u32 start =
        LE32(file.data() + dir + 4);

    u32 end =
        static_cast<u32>(logical_end);

    for (u32 other = 0;
         other < entry_count;
         ++other)
    {
      const std::size_t other_dir =
          16 + std::size_t(other) * 8;

      const u32 candidate =
          LE32(file.data() + other_dir + 4);

      if (candidate > start && candidate < end)
        end = candidate;
    }

    if (start + 16 > logical_end ||
        end <= start ||
        end > logical_end)
    {
      continue;
    }

    const u8 raw_type = file[start];
    const u8 type = raw_type & 0x7Fu;
    const bool compressed = (raw_type & 0x80u) != 0;

    const u32 block_size =
        LE24(file.data() + start + 1);

    const u32 width =
        LE16(file.data() + start + 4);

    const u32 height =
        LE16(file.data() + start + 6);

    if (!width || !height ||
        width > 4096 || height > 4096 ||
        block_size < 16 ||
        std::uint64_t(start) + block_size > end)
    {
      continue;
    }

    const std::size_t image_start = start + 16;
    const std::size_t image_size = block_size - 16;

    std::span<const u8> image(
        file.data() + image_start,
        image_size);

    if (compressed)
    {
      static unsigned compressed_logs = 0;
      if (compressed_logs < 32)
      {
        ++compressed_logs;
        std::fprintf(
            stderr,
            "[moh-ps3-ssh] EA SHPS compressed entry unsupported: type=%02X %ux%u prefix=%02X%02X\n",
            raw_type,
            width,
            height,
            image.size() > 0 ? image[0] : 0,
            image.size() > 1 ? image[1] : 0);
      }
      continue;
    }

    std::span<const u8> palette;
    u8 palette_type = 0;

    std::size_t attachment =
        start + block_size;

    while (attachment + 16 <= end)
    {
      const u8 attachment_type =
          file[attachment] & 0x7Fu;

      const u32 attachment_size =
          LE24(file.data() + attachment + 1);

      if (attachment_size < 16 ||
          attachment + attachment_size > end)
      {
        break;
      }

      if (attachment_type == 33 ||
          attachment_type == 36 ||
          attachment_type == 42)
      {
        palette_type = attachment_type;
        palette = std::span<const u8>(
            file.data() + attachment + 16,
            attachment_size - 16);
        break;
      }

      attachment += attachment_size;
    }

    Level level{
        width,
        height,
        std::vector<u8>(std::size_t(width) * height * 4)};

    const std::size_t pixels =
        std::size_t(width) * height;

    bool decoded = false;

    if (type == 5 || type == 91)
    {
      if (image.size() >= pixels * 4)
      {
        for (std::size_t i = 0; i < pixels; ++i)
        {
          level.rgba[i * 4 + 0] = image[i * 4 + 0];
          level.rgba[i * 4 + 1] = image[i * 4 + 1];
          level.rgba[i * 4 + 2] = image[i * 4 + 2];
          level.rgba[i * 4 + 3] = ExpandPS2Alpha(image[i * 4 + 3]);
        }
        decoded = true;
      }
    }
    else if (type == 125)
    {
      if (image.size() >= pixels * 4)
      {
        for (std::size_t i = 0; i < pixels; ++i)
        {
          level.rgba[i * 4 + 0] = image[i * 4 + 2];
          level.rgba[i * 4 + 1] = image[i * 4 + 1];
          level.rgba[i * 4 + 2] = image[i * 4 + 0];
          level.rgba[i * 4 + 3] = ExpandPS2Alpha(image[i * 4 + 3]);
        }
        decoded = true;
      }
    }
    else if (type == 4 || type == 67)
    {
      if (image.size() >= pixels * 3)
      {
        for (std::size_t i = 0; i < pixels; ++i)
        {
          level.rgba[i * 4 + 0] = image[i * 3 + 0];
          level.rgba[i * 4 + 1] = image[i * 3 + 1];
          level.rgba[i * 4 + 2] = image[i * 3 + 2];
          level.rgba[i * 4 + 3] = 255;
        }
        decoded = true;
      }
    }
    else if (type == 127)
    {
      if (image.size() >= pixels * 3)
      {
        for (std::size_t i = 0; i < pixels; ++i)
        {
          level.rgba[i * 4 + 0] = image[i * 3 + 2];
          level.rgba[i * 4 + 1] = image[i * 3 + 1];
          level.rgba[i * 4 + 2] = image[i * 3 + 0];
          level.rgba[i * 4 + 3] = 255;
        }
        decoded = true;
      }
    }
    else if (type == 3)
    {
      if (image.size() >= pixels * 2)
      {
        for (std::size_t i = 0; i < pixels; ++i)
        {
          const u32 value = LE16(image.data() + i * 2);
          level.rgba[i * 4 + 0] = Expand5((value >> 11) & 31u);
          level.rgba[i * 4 + 1] = Expand5((value >> 6) & 31u);
          level.rgba[i * 4 + 2] = Expand5((value >> 1) & 31u);
          level.rgba[i * 4 + 3] = (value & 1u) ? 255 : 0;
        }
        decoded = true;
      }
    }
    else if (type == 2 &&
             !palette.empty() &&
             image.size() >= pixels)
    {
      const std::size_t palette_entries =
          palette_type == 36 ?
              palette.size() / 3 :
              palette.size() / 4;

      if (palette_entries >= 256)
      {
        for (std::size_t i = 0; i < pixels; ++i)
        {
          const u8 raw_index = image[i];
          const std::size_t index =
              (raw_index & 0xE7u) |
              ((raw_index & 0x08u) << 1) |
              ((raw_index & 0x10u) >> 1);

          const auto color =
              palette_color(palette, palette_type, index);

          std::copy(
              color.begin(),
              color.end(),
              level.rgba.begin() + i * 4);
        }
        decoded = true;
      }
    }
    else if (type == 1 &&
             !palette.empty() &&
             image.size() >= (pixels + 1) / 2)
    {
      const std::size_t palette_entries =
          palette_type == 36 ?
              palette.size() / 3 :
              palette.size() / 4;

      if (palette_entries >= 16)
      {
        for (std::size_t i = 0; i < pixels; ++i)
        {
          const u8 packed = image[i / 2];
          const std::size_t index =
              (i & 1) ? (packed >> 4) : (packed & 0x0F);

          const auto color =
              palette_color(palette, palette_type, index);

          std::copy(
              color.begin(),
              color.end(),
              level.rgba.begin() + i * 4);
        }
        decoded = true;
      }
    }

    if (!decoded)
    {
      static unsigned unsupported_logs = 0;
      if (unsupported_logs < 48)
      {
        ++unsupported_logs;
        std::fprintf(
            stderr,
            "[moh-ps3-ssh] EA SHPS unsupported image: type=%02X masked=%02X %ux%u image=%zu palette=%u/%zu prefix=%02X%02X\n",
            raw_type,
            type,
            width,
            height,
            image.size(),
            palette_type,
            palette.size(),
            image.size() > 0 ? image[0] : 0,
            image.size() > 1 ? image[1] : 0);
      }
      continue;
    }

    levels->clear();
    levels->push_back(std::move(level));

    static unsigned decoded_logs = 0;
    if (decoded_logs < 48)
    {
      ++decoded_logs;
      std::fprintf(
          stderr,
          "[moh-ps3-ssh] EA SHPS decoded: type=%02X %ux%u palette=%u\n",
          type,
          width,
          height,
          palette_type);
    }

    return true;
  }

  return false;
}

}  // namespace

bool Decode(std::span<const u8> file, std::vector<Level>* levels)
{
  if (!levels)
    return false;

  levels->clear();

  if (DecodeEAOldShape(
          file,
          levels))
  {
    return true;
  }

  if (file.size() >= 4 &&
      BE32(file.data()) != 0x02010100 &&
      !StartsWith(file, "SHPS") &&
      !StartsWith(file, "SHPI") &&
      !StartsWith(file, "ShpS"))
  {
    static unsigned unknown_logs = 0;

    if (unknown_logs < 48)
    {
      ++unknown_logs;
      std::fprintf(
          stderr,
          "[moh-ps3-ssh] unknown texture container: magic=%02X%02X%02X%02X size=%zu\n",
          file[0],
          file[1],
          file[2],
          file[3],
          file.size());
    }
  }

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

  const u32 gcm_pitch =
      BE32(
          file.data() +
          40);

  constexpr u32 CELL_GCM_TEXTURE_A4R4G4B4 = 0x83;
constexpr u32 CELL_GCM_TEXTURE_A8R8G8B8 = 0x85;
  constexpr u32 CELL_GCM_TEXTURE_COMPRESSED_DXT1 = 0x86;
  constexpr u32 CELL_GCM_TEXTURE_COMPRESSED_DXT23 = 0x87;
  constexpr u32 CELL_GCM_TEXTURE_COMPRESSED_DXT45 = 0x88;

  const bool supported_format =
      format == CELL_GCM_TEXTURE_A4R4G4B4 ||
      format == CELL_GCM_TEXTURE_A8R8G8B8 ||
      format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 ||
      format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 ||
      format == CELL_GCM_TEXTURE_COMPRESSED_DXT45;

  const u32 descriptor_offset =
      BE32(file.data() + 16);

  const u32 remap =
      BE32(file.data() + 28);

  const u32 pitch_field =
      BE32(file.data() + 40);

  const bool bad_gtf =
      !supported_format ||
      !w ||
      !h ||
      w > 4096 ||
      h > 4096 ||
      !count ||
      count >
          static_cast<u32>(
              std::bit_width(
                  std::max(w, h))) ||
      file.size() <
          48ull +
              bytes ||
      !TrailingZeroPadding(
          file,
          48ull + bytes) ||
      BE32(file.data() + 4) !=
          bytes;

  if (bad_gtf)
  {
    static unsigned reject_logs = 0;

    if (reject_logs < 96)
    {
      ++reject_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-gtf] reject: raw=%02X base=%02X linear=%d "
          "mips=%u dim=%u cube=%u remap=%08X "
          "%ux%u depth=%u pitch=%u "
          "texbytes=%u field04=%u desc=%u file=%zu\n",
          static_cast<unsigned>(
              file[24]),
          static_cast<unsigned>(
              format),
          linear ? 1 : 0,
          count,
          static_cast<unsigned>(
              file[26]),
          static_cast<unsigned>(
              file[27]),
          remap,
          w,
          h,
          BE16(file.data() + 36),
          pitch_field,
          bytes,
          BE32(file.data() + 4),
          descriptor_offset,
          file.size());
    }

    return false;
  }

  if (remap != 0xAAE4u)
  {
    static unsigned remap_logs = 0;

    if (remap_logs < 32)
    {
      ++remap_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-gtf] non-default remap accepted: "
          "raw=%02X base=%02X remap=%08X %ux%u\n",
          static_cast<unsigned>(
              file[24]),
          static_cast<unsigned>(
              format),
          remap,
          w,
          h);
    }
  }

  if ((format == CELL_GCM_TEXTURE_A4R4G4B4 ||
       format == CELL_GCM_TEXTURE_A8R8G8B8) &&
      !linear &&
      (!std::has_single_bit(w) ||
       !std::has_single_bit(h)))
  {
    return false;
  }

  std::size_t offset = 48;
  std::vector<Level> decoded;

  for (u32 mip = 0; mip < count; ++mip)
  {
    const bool argb4444 =
        format ==
        CELL_GCM_TEXTURE_A4R4G4B4;

    const bool rgba8 =
        format ==
        CELL_GCM_TEXTURE_A8R8G8B8;

    const bool uncompressed =
        argb4444 ||
        rgba8;

    u32 pitch = 0;
    std::size_t size = 0;

    if (uncompressed)
    {
      const u32 bytes_per_pixel =
          argb4444 ?
              2u :
              4u;

      pitch =
          linear &&
                  mip == 0 &&
                  BE32(
                      file.data() +
                      40) ?
              BE32(
                  file.data() +
                  40) :
              w *
                  bytes_per_pixel;

      if (pitch <
          w *
              bytes_per_pixel)
      {
        return false;
      }

      size =
          std::size_t(
              pitch) *
          h;
    }
    else
    {
      const std::size_t block_bytes =
          format ==
                  CELL_GCM_TEXTURE_COMPRESSED_DXT1 ?
              8u :
              16u;

      const u32 blocks_x =
          (w + 3u) /
          4u;

      const u32 blocks_y =
          (h + 3u) /
          4u;

      const std::size_t packed_row =
          std::size_t(
              blocks_x) *
          block_bytes;

      if (linear)
      {
        // Sony DDS2GTF/RSX linear BC layout keeps the mip-0 pitch for the
        // complete mip chain.
        //
        // Frontline example:
        // main_master.ssh = DXT1-LN 1280x896, pitch=2560, 11 mips.
        // Block rows:
        // 224+112+56+28+14+7+4+2+1+1+1 = 450
        // 450 * 2560 = 1,152,000 bytes exactly.
        pitch =
            gcm_pitch;

        if (!pitch ||
            std::size_t(
                pitch) <
                packed_row ||
            (pitch %
                 block_bytes) !=
                0)
        {
          return false;
        }

        size =
            std::size_t(
                pitch) *
            blocks_y;
      }
      else
      {
        pitch =
            static_cast<u32>(
                packed_row);

        size =
            packed_row *
            blocks_y;
      }
    }
    if (size > file.size() - offset)
      return false;

    Level level{
        w,
        h,
        std::vector<u8>(std::size_t(w) * h * 4)};

    const u8* src = file.data() + offset;

    if (argb4444)
    {
      for (u32 y = 0;
           y < h;
           ++y)
      {
        for (u32 x = 0;
             x < w;
             ++x)
        {
          const std::size_t texel =
              linear ?
                  std::size_t(y) *
                          (pitch / 2u) +
                      x :
                  std::size_t(
                      Morton(
                          x,
                          y,
                          w,
                          h));

          const u8* p =
              src +
              texel *
                  2u;

          const u32 pixel =
              (u32(p[0]) << 8) |
              u32(p[1]);

          auto* d =
              level.rgba.data() +
              (std::size_t(y) *
                   w +
               x) *
                  4;

          d[0] =
              static_cast<u8>(
                  ((pixel >> 8) &
                   0x0Fu) *
                  17u);

          d[1] =
              static_cast<u8>(
                  ((pixel >> 4) &
                   0x0Fu) *
                  17u);

          d[2] =
              static_cast<u8>(
                  (pixel &
                   0x0Fu) *
                  17u);

          d[3] =
              static_cast<u8>(
                  ((pixel >> 12) &
                   0x0Fu) *
                  17u);
        }
      }
    }
    else if (rgba8)
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
              (linear ?
                   std::size_t(by) *
                           pitch +
                       std::size_t(bx) *
                           block_bytes :
                   (std::size_t(by) *
                        blocks_x +
                    bx) *
                       block_bytes);

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

    if (linear &&
        !uncompressed)
    {
      static unsigned linear_bc_logs = 0;

      if (linear_bc_logs < 64)
      {
        ++linear_bc_logs;

        std::fprintf(
            stderr,
            "[moh-ps3-gtf] linear BC mip: "
            "base=%02X mip=%u %ux%u pitch=%u bytes=%zu\n",
            static_cast<unsigned>(
                format),
            mip,
            w,
            h,
            pitch,
            size);
      }
    }

    decoded.push_back(std::move(level));
    offset += size;

    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }

  if (offset > file.size() ||
      !TrailingZeroPadding(file, offset))
  {
    return false;
  }

  static bool logged_formats[256]{};

  if (format < 256 &&
      !logged_formats[format])
  {
    logged_formats[format] =
        true;

    std::fprintf(
        stderr,
        "[moh-ps3-gtf] decoded: raw=%02X base=%02X "
        "linear=%d mips=%u %ux%u remap=%08X\n",
        static_cast<unsigned>(
            file[24]),
        static_cast<unsigned>(
            format),
        linear ? 1 : 0,
        count,
        BE16(file.data() + 32),
        BE16(file.data() + 34),
        BE32(file.data() + 28));
  }

  *levels = std::move(decoded);
  return true;
}

}  // namespace PS3TextureDecoder
