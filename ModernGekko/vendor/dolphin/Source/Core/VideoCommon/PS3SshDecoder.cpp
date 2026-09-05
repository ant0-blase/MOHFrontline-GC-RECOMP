#include "VideoCommon/PS3SshDecoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace PS3SshDecoder
{
namespace
{
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

u16 Read16(const u8* p, bool be)
{
  if (be)
    return static_cast<u16>((u16(p[0]) << 8) | u16(p[1]));

  return static_cast<u16>(
      u16(p[0]) |
      (u16(p[1]) << 8));
}

u32 Read24(const u8* p, bool be)
{
  if (be)
  {
    return
        (u32(p[0]) << 16) |
        (u32(p[1]) << 8) |
        u32(p[2]);
  }

  return
      u32(p[0]) |
      (u32(p[1]) << 8) |
      (u32(p[2]) << 16);
}

u32 Read32(const u8* p, bool be)
{
  if (be)
  {
    return
        (u32(p[0]) << 24) |
        (u32(p[1]) << 16) |
        (u32(p[2]) << 8) |
        u32(p[3]);
  }

  return
      u32(p[0]) |
      (u32(p[1]) << 8) |
      (u32(p[2]) << 16) |
      (u32(p[3]) << 24);
}

bool ValidDimension(u32 v)
{
  return v >= 1 && v <= 8192;
}

bool ReadWholeFile(const std::filesystem::path& path,
                   std::vector<u8>* out)
{
  std::ifstream f(path, std::ios::binary | std::ios::ate);

  if (!f)
    return false;

  const std::streamoff size = f.tellg();

  if (size <= 0 || size > 1024ll * 1024ll * 1024ll)
    return false;

  out->resize(static_cast<std::size_t>(size));

  f.seekg(0, std::ios::beg);

  return static_cast<bool>(
      f.read(reinterpret_cast<char*>(out->data()),
             static_cast<std::streamsize>(out->size())));
}

void WritePixel(std::vector<u8>* out,
                u32 width,
                u32 x,
                u32 y,
                u8 r,
                u8 g,
                u8 b,
                u8 a)
{
  const std::size_t pos =
      (static_cast<std::size_t>(y) * width + x) * 4;

  if (pos + 3 >= out->size())
    return;

  (*out)[pos + 0] = r;
  (*out)[pos + 1] = g;
  (*out)[pos + 2] = b;
  (*out)[pos + 3] = a;
}

u8 Expand5(u32 v)
{
  return static_cast<u8>((v << 3) | (v >> 2));
}

u8 Expand6(u32 v)
{
  return static_cast<u8>((v << 2) | (v >> 4));
}

u8 Expand4(u32 v)
{
  return static_cast<u8>((v << 4) | v);
}

std::array<u8, 4> RGB565(u16 v)
{
  return {
      Expand5((v >> 11) & 31),
      Expand6((v >> 5) & 63),
      Expand5(v & 31),
      255};
}

void DecodeDXTColors(
    u16 c0,
    u16 c1,
    std::array<std::array<u8, 4>, 4>* colors,
    bool force_four)
{
  (*colors)[0] = RGB565(c0);
  (*colors)[1] = RGB565(c1);

  if (c0 > c1 || force_four)
  {
    for (int c = 0; c < 3; ++c)
    {
      (*colors)[2][c] =
          static_cast<u8>(
              (2 * int((*colors)[0][c]) +
               int((*colors)[1][c])) /
              3);

      (*colors)[3][c] =
          static_cast<u8>(
              (int((*colors)[0][c]) +
               2 * int((*colors)[1][c])) /
              3);
    }

    (*colors)[2][3] = 255;
    (*colors)[3][3] = 255;
  }
  else
  {
    for (int c = 0; c < 3; ++c)
    {
      (*colors)[2][c] =
          static_cast<u8>(
              (int((*colors)[0][c]) +
               int((*colors)[1][c])) /
              2);
    }

    (*colors)[2][3] = 255;
    (*colors)[3] = {0, 0, 0, 0};
  }
}

bool DecodeDXT1(
    const u8* src,
    std::size_t size,
    u32 width,
    u32 height,
    std::vector<u8>* out)
{
  const u32 bx_count = (width + 3) / 4;
  const u32 by_count = (height + 3) / 4;

  const std::size_t need =
      static_cast<std::size_t>(bx_count) *
      by_count *
      8;

  if (size < need)
    return false;

  out->assign(
      static_cast<std::size_t>(width) *
          height *
          4,
      0);

  std::size_t pos = 0;

  for (u32 by = 0; by < by_count; ++by)
  {
    for (u32 bx = 0; bx < bx_count; ++bx)
    {
      const u16 c0 = Read16(src + pos + 0, false);
      const u16 c1 = Read16(src + pos + 2, false);
      const u32 bits = Read32(src + pos + 4, false);

      std::array<std::array<u8, 4>, 4> colors{};
      DecodeDXTColors(c0, c1, &colors, false);

      for (u32 py = 0; py < 4; ++py)
      {
        for (u32 px = 0; px < 4; ++px)
        {
          const u32 x = bx * 4 + px;
          const u32 y = by * 4 + py;

          if (x >= width || y >= height)
            continue;

          const u32 index =
              (bits >> (2 * (py * 4 + px))) & 3;

          const auto& c = colors[index];

          WritePixel(
              out, width, x, y,
              c[0], c[1], c[2], c[3]);
        }
      }

      pos += 8;
    }
  }

  return true;
}

bool DecodeDXT3(
    const u8* src,
    std::size_t size,
    u32 width,
    u32 height,
    std::vector<u8>* out)
{
  const u32 bx_count = (width + 3) / 4;
  const u32 by_count = (height + 3) / 4;

  const std::size_t need =
      static_cast<std::size_t>(bx_count) *
      by_count *
      16;

  if (size < need)
    return false;

  out->assign(
      static_cast<std::size_t>(width) *
          height *
          4,
      0);

  std::size_t pos = 0;

  for (u32 by = 0; by < by_count; ++by)
  {
    for (u32 bx = 0; bx < bx_count; ++bx)
    {
      std::uint64_t alpha_bits = 0;

      for (int i = 0; i < 8; ++i)
      {
        alpha_bits |=
            std::uint64_t(src[pos + i]) <<
            (8 * i);
      }

      const u16 c0 = Read16(src + pos + 8, false);
      const u16 c1 = Read16(src + pos + 10, false);
      const u32 bits = Read32(src + pos + 12, false);

      std::array<std::array<u8, 4>, 4> colors{};
      DecodeDXTColors(c0, c1, &colors, true);

      for (u32 py = 0; py < 4; ++py)
      {
        for (u32 px = 0; px < 4; ++px)
        {
          const u32 p = py * 4 + px;
          const u32 x = bx * 4 + px;
          const u32 y = by * 4 + py;

          if (x >= width || y >= height)
            continue;

          const u32 index =
              (bits >> (2 * p)) & 3;

          const u8 alpha =
              static_cast<u8>(
                  ((alpha_bits >> (4 * p)) & 15) *
                  17);

          const auto& c = colors[index];

          WritePixel(
              out, width, x, y,
              c[0], c[1], c[2], alpha);
        }
      }

      pos += 16;
    }
  }

  return true;
}

bool DecodeDXT5(
    const u8* src,
    std::size_t size,
    u32 width,
    u32 height,
    std::vector<u8>* out)
{
  const u32 bx_count = (width + 3) / 4;
  const u32 by_count = (height + 3) / 4;

  const std::size_t need =
      static_cast<std::size_t>(bx_count) *
      by_count *
      16;

  if (size < need)
    return false;

  out->assign(
      static_cast<std::size_t>(width) *
          height *
          4,
      0);

  std::size_t pos = 0;

  for (u32 by = 0; by < by_count; ++by)
  {
    for (u32 bx = 0; bx < bx_count; ++bx)
    {
      const u8 a0 = src[pos + 0];
      const u8 a1 = src[pos + 1];

      std::array<u8, 8> alpha{};
      alpha[0] = a0;
      alpha[1] = a1;

      if (a0 > a1)
      {
        for (int i = 1; i <= 6; ++i)
        {
          alpha[i + 1] =
              static_cast<u8>(
                  ((7 - i) * int(a0) +
                   i * int(a1)) /
                  7);
        }
      }
      else
      {
        for (int i = 1; i <= 4; ++i)
        {
          alpha[i + 1] =
              static_cast<u8>(
                  ((5 - i) * int(a0) +
                   i * int(a1)) /
                  5);
        }

        alpha[6] = 0;
        alpha[7] = 255;
      }

      std::uint64_t alpha_indices = 0;

      for (int i = 0; i < 6; ++i)
      {
        alpha_indices |=
            std::uint64_t(src[pos + 2 + i]) <<
            (8 * i);
      }

      const u16 c0 = Read16(src + pos + 8, false);
      const u16 c1 = Read16(src + pos + 10, false);
      const u32 bits = Read32(src + pos + 12, false);

      std::array<std::array<u8, 4>, 4> colors{};
      DecodeDXTColors(c0, c1, &colors, true);

      for (u32 py = 0; py < 4; ++py)
      {
        for (u32 px = 0; px < 4; ++px)
        {
          const u32 p = py * 4 + px;
          const u32 x = bx * 4 + px;
          const u32 y = by * 4 + py;

          if (x >= width || y >= height)
            continue;

          const u32 ci =
              (bits >> (2 * p)) & 3;

          const u32 ai =
              static_cast<u32>(
                  (alpha_indices >> (3 * p)) & 7);

          const auto& c = colors[ci];

          WritePixel(
              out, width, x, y,
              c[0], c[1], c[2], alpha[ai]);
        }
      }

      pos += 16;
    }
  }

  return true;
}

bool DecodeRaw(
    u8 type,
    const u8* src,
    std::size_t size,
    u32 width,
    u32 height,
    bool file_be,
    std::vector<u8>* out)
{
  const std::size_t pixels =
      static_cast<std::size_t>(width) *
      height;

  out->assign(pixels * 4, 0);

  switch (type)
  {
  // RGBA8888
  case 5:
  case 91:
  {
    if (size < pixels * 4)
      return false;

    std::memcpy(
        out->data(),
        src,
        pixels * 4);

    return true;
  }

  // RGB888
  case 4:
  case 67:
  {
    if (size < pixels * 3)
      return false;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      (*out)[i * 4 + 0] = src[i * 3 + 0];
      (*out)[i * 4 + 1] = src[i * 3 + 1];
      (*out)[i * 4 + 2] = src[i * 3 + 2];
      (*out)[i * 4 + 3] = 255;
    }

    return true;
  }

  // BGR888
  case 127:
  {
    if (size < pixels * 3)
      return false;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      (*out)[i * 4 + 0] = src[i * 3 + 2];
      (*out)[i * 4 + 1] = src[i * 3 + 1];
      (*out)[i * 4 + 2] = src[i * 3 + 0];
      (*out)[i * 4 + 3] = 255;
    }

    return true;
  }

  // BGRA8888
  case 125:
  {
    if (size < pixels * 4)
      return false;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      (*out)[i * 4 + 0] = src[i * 4 + 2];
      (*out)[i * 4 + 1] = src[i * 4 + 1];
      (*out)[i * 4 + 2] = src[i * 4 + 0];
      (*out)[i * 4 + 3] = src[i * 4 + 3];
    }

    return true;
  }

  // ARGB8888
  case 22:
  {
    if (size < pixels * 4)
      return false;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      const u8* p = src + i * 4;

      (*out)[i * 4 + 0] = p[1];
      (*out)[i * 4 + 1] = p[2];
      (*out)[i * 4 + 2] = p[3];
      (*out)[i * 4 + 3] = p[0];
    }

    return true;
  }

  // A8 / grayscale
  case 100:
  {
    if (size < pixels)
      return false;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      const u8 v = src[i];

      (*out)[i * 4 + 0] = v;
      (*out)[i * 4 + 1] = v;
      (*out)[i * 4 + 2] = v;
      (*out)[i * 4 + 3] = 255;
    }

    return true;
  }

  // RGBA5551
  case 3:
  case 66:
  {
    if (size < pixels * 2)
      return false;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      const u16 v =
          Read16(src + i * 2, file_be);

      (*out)[i * 4 + 0] =
          Expand5((v >> 11) & 31);

      (*out)[i * 4 + 1] =
          Expand5((v >> 6) & 31);

      (*out)[i * 4 + 2] =
          Expand5((v >> 1) & 31);

      (*out)[i * 4 + 3] =
          (v & 1) ? 255 : 0;
    }

    return true;
  }

  // RGB565. Type 20 is explicitly big-endian in EA Shape data.
  case 20:
  case 88:
  case 89:
  {
    if (size < pixels * 2)
      return false;

    const bool be =
        type == 20 ? true : file_be;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      const auto c =
          RGB565(Read16(src + i * 2, be));

      (*out)[i * 4 + 0] = c[0];
      (*out)[i * 4 + 1] = c[1];
      (*out)[i * 4 + 2] = c[2];
      (*out)[i * 4 + 3] = 255;
    }

    return true;
  }

  // Embedded PAL8 + RGBA palette.
  case 115:
  {
    if (size < 1024 + pixels)
      return false;

    const u8* palette = src;
    const u8* indices = src + 1024;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      const u8 index = indices[i];
      const u8* c = palette + std::size_t(index) * 4;

      (*out)[i * 4 + 0] = c[0];
      (*out)[i * 4 + 1] = c[1];
      (*out)[i * 4 + 2] = c[2];
      (*out)[i * 4 + 3] = c[3];
    }

    return true;
  }

  // Embedded PAL4 + RGBA palette.
  case 119:
  {
    if (size < 64 + (pixels + 1) / 2)
      return false;

    const u8* palette = src;
    const u8* indices = src + 64;

    for (std::size_t i = 0; i < pixels; ++i)
    {
      const u8 packed = indices[i / 2];

      const u8 index =
          (i & 1) ?
              (packed & 0x0F) :
              (packed >> 4);

      const u8* c = palette + std::size_t(index) * 4;

      (*out)[i * 4 + 0] = c[0];
      (*out)[i * 4 + 1] = c[1];
      (*out)[i * 4 + 2] = c[2];
      (*out)[i * 4 + 3] = c[3];
    }

    return true;
  }

  case 96:
    return DecodeDXT1(
        src, size, width, height, out);

  case 97:
    return DecodeDXT3(
        src, size, width, height, out);

  case 98:
    return DecodeDXT5(
        src, size, width, height, out);

  default:
    return false;
  }
}

bool IsOldShape(const std::string& magic)
{
  return
      magic == "SHPI" ||
      magic == "SHPP" ||
      magic == "SHPS" ||
      magic == "SHPX" ||
      magic == "SHPM" ||
      magic == "SHPG" ||
      magic == "SHPA";
}

bool IsNewShape(const std::string& magic)
{
  return
      magic == "ShpF" ||
      magic == "ShpS" ||
      magic == "ShpX" ||
      magic == "ShpM" ||
      magic == "ShpG" ||
      magic == "ShpA";
}

struct OldDir
{
  std::string tag;
  u32 offset = 0;
};

bool ParseOldDirectory(
    const std::vector<u8>& data,
    bool be,
    u32 count,
    std::vector<OldDir>* out)
{
  if (count == 0 || count > 8192)
    return false;

  if (16ull + std::uint64_t(count) * 8ull >
      data.size())
  {
    return false;
  }

  out->clear();

  for (u32 i = 0; i < count; ++i)
  {
    const std::size_t p = 16 + std::size_t(i) * 8;

    OldDir d;

    d.tag.assign(
        reinterpret_cast<const char*>(data.data() + p),
        4);

    d.offset =
        Read32(data.data() + p + 4, be);

    if (d.offset < 16 ||
        d.offset + 16 > data.size())
    {
      return false;
    }

    out->push_back(std::move(d));
  }

  return true;
}

bool DecodeOldShape(
    const std::vector<u8>& data,
    const std::string& magic,
    std::vector<Image>* images)
{
  const u32 count_le =
      Read32(data.data() + 8, false);

  const u32 count_be =
      Read32(data.data() + 8, true);

  std::vector<OldDir> dirs;

  bool dir_be = false;
  u32 count = count_le;

  if (!ParseOldDirectory(
          data, false, count_le, &dirs))
  {
    if (!ParseOldDirectory(
            data, true, count_be, &dirs))
    {
      return false;
    }

    dir_be = true;
    count = count_be;
  }

  // SHPG is naturally big-endian. For other old Shape containers,
  // choose endianness per entry using width/height plausibility.
  const bool default_data_be =
      magic == "SHPG";

  for (u32 i = 0; i < count; ++i)
  {
    const OldDir& d = dirs[i];

    const std::size_t off = d.offset;

    if (off + 16 > data.size())
      continue;

    const u8 raw_type =
        data[off + 0];

    // RefPack-compressed entry.
    // Keep it out of the bridge until decompression is added rather than
    // feeding compressed bytes to the renderer.
    if (raw_type & 0x80)
      continue;

    const u8 type =
        raw_type & 0x7F;

    bool file_be = default_data_be;

    u32 width =
        Read16(data.data() + off + 4, file_be);

    u32 height =
        Read16(data.data() + off + 6, file_be);

    if (!ValidDimension(width) ||
        !ValidDimension(height))
    {
      file_be = !file_be;

      width =
          Read16(data.data() + off + 4, file_be);

      height =
          Read16(data.data() + off + 6, file_be);
    }

    if (!ValidDimension(width) ||
        !ValidDimension(height))
    {
      continue;
    }

    u32 block_size =
        Read24(data.data() + off + 1, file_be);

    const std::size_t next =
        i + 1 < count ?
            dirs[i + 1].offset :
            data.size();

    std::size_t data_begin =
        off + 16;

    std::size_t data_end = next;

    if (block_size >= 16 &&
        off + block_size <= data.size())
    {
      data_end =
          off + block_size;
    }

    if (data_end <= data_begin)
      continue;

    Image image;
    image.tag = d.tag;
    image.width = width;
    image.height = height;
    image.source_type = type;

    if (DecodeRaw(
            type,
            data.data() + data_begin,
            data_end - data_begin,
            width,
            height,
            file_be,
            &image.rgba))
    {
      images->push_back(
          std::move(image));
    }
  }

  return !images->empty();
}

struct NewDir
{
  std::string tag;
  u32 offset = 0;
};

bool ParseNewDirectory(
    const std::vector<u8>& data,
    bool be,
    u32 count,
    std::vector<NewDir>* out)
{
  if (count == 0 || count > 8192)
    return false;

  std::size_t p = 16;

  out->clear();

  for (u32 i = 0; i < count; ++i)
  {
    if (p + 8 > data.size())
      return false;

    NewDir d;

    d.offset =
        Read32(data.data() + p, be);

    p += 8;

    if (d.offset + 32 > data.size())
      return false;

    const std::size_t start = p;

    while (p < data.size() &&
           data[p] != 0)
    {
      ++p;
    }

    if (p >= data.size())
      return false;

    d.tag.assign(
        reinterpret_cast<const char*>(
            data.data() + start),
        p - start);

    ++p;

    out->push_back(
        std::move(d));
  }

  return true;
}

bool DecodeNewShape(
    const std::vector<u8>& data,
    std::vector<Image>* images)
{
  const u32 count_be =
      Read32(data.data() + 8, true);

  const u32 count_le =
      Read32(data.data() + 8, false);

  std::vector<NewDir> dirs;

  bool dir_be = true;
  u32 count = count_be;

  if (!ParseNewDirectory(
          data, true, count_be, &dirs))
  {
    if (!ParseNewDirectory(
            data, false, count_le, &dirs))
    {
      return false;
    }

    dir_be = false;
    count = count_le;
  }

  (void)dir_be;

  for (u32 i = 0; i < count; ++i)
  {
    const NewDir& d = dirs[i];

    const std::size_t off = d.offset;

    if (off + 32 > data.size())
      continue;

    const u8 type =
        data[off + 0] & 0x7F;

    bool file_be = false;

    auto read_dimensions =
        [&](bool be,
            u32* width,
            u32* height,
            u32* raw_offset,
            u32* raw_size)
    {
      *raw_offset =
          Read32(data.data() + off + 8, be);

      *raw_size =
          Read32(data.data() + off + 12, be);

      *width =
          Read32(data.data() + off + 24, be);

      *height =
          Read32(data.data() + off + 28, be);
    };

    u32 width = 0;
    u32 height = 0;
    u32 raw_offset = 0;
    u32 raw_size = 0;

    read_dimensions(
        false,
        &width,
        &height,
        &raw_offset,
        &raw_size);

    if (!ValidDimension(width) ||
        !ValidDimension(height) ||
        off + raw_offset > data.size())
    {
      file_be = true;

      read_dimensions(
          true,
          &width,
          &height,
          &raw_offset,
          &raw_size);
    }

    if (!ValidDimension(width) ||
        !ValidDimension(height))
    {
      continue;
    }

    const std::size_t begin =
        off + raw_offset;

    if (begin >= data.size())
      continue;

    std::size_t end =
        begin + raw_size;

    if (raw_size == 0 ||
        end > data.size())
    {
      end =
          i + 1 < count ?
              dirs[i + 1].offset :
              data.size();
    }

    if (end <= begin)
      continue;

    Image image;
    image.tag = d.tag;
    image.width = width;
    image.height = height;
    image.source_type = type;

    if (DecodeRaw(
            type,
            data.data() + begin,
            end - begin,
            width,
            height,
            file_be,
            &image.rgba))
    {
      images->push_back(
          std::move(image));
    }
  }

  return !images->empty();
}
}

bool DecodeFile(
    const std::filesystem::path& path,
    std::vector<Image>* images,
    std::string* error)
{
  if (!images)
    return false;

  images->clear();

  std::vector<u8> data;

  if (!ReadWholeFile(path, &data))
  {
    if (error)
      *error = "cannot read file";

    return false;
  }

  if (data.size() < 16)
  {
    if (error)
      *error = "file too small";

    return false;
  }

  const std::string magic(
      reinterpret_cast<const char*>(data.data()),
      4);

  bool ok = false;

  if (IsOldShape(magic))
  {
    ok =
        DecodeOldShape(
            data,
            magic,
            images);
  }
  else if (IsNewShape(magic))
  {
    ok =
        DecodeNewShape(
            data,
            images);
  }
  else
  {
    if (error)
      *error = "unsupported Shape signature: " + magic;

    return false;
  }

  if (!ok && error)
    *error = "no supported image entry";

  return ok;
}
}
