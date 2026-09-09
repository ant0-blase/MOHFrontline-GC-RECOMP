#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCTexture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

#include "Common/Buffer.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/TextureConfig.h"

namespace MOHFrontline::GCTexture
{
namespace
{
struct RGBA
{
  u8 r = 0, g = 0, b = 0, a = 255;
};

u16 BE16(const u8* p) { return static_cast<u16>((u16(p[0]) << 8) | u16(p[1])); }

u8 Expand3(u32 v) { return static_cast<u8>((v << 5) | (v << 2) | (v >> 1)); }
u8 Expand4(u32 v) { return static_cast<u8>((v << 4) | v); }
u8 Expand5(u32 v) { return static_cast<u8>((v << 3) | (v >> 2)); }
u8 Expand6(u32 v) { return static_cast<u8>((v << 2) | (v >> 4)); }

RGBA Decode565(u16 v)
{
  return {Expand5((v >> 11) & 31), Expand6((v >> 5) & 63), Expand5(v & 31), 255};
}

RGBA Decode5A3(u16 v)
{
  if (v & 0x8000)
    return {Expand5((v >> 10) & 31), Expand5((v >> 5) & 31), Expand5(v & 31), 255};
  return {Expand4((v >> 8) & 15), Expand4((v >> 4) & 15), Expand4(v & 15),
          Expand3((v >> 12) & 7)};
}

RGBA DecodePalette(u32 format, std::span<const u8> palette, u32 index)
{
  const std::size_t p = static_cast<std::size_t>(index) * 2u;
  if (p + 2 > palette.size())
    return {};
  const u16 v = BE16(palette.data() + p);
  switch (format)
  {
  case 0:  // GX_TL_IA8
    return {static_cast<u8>(v & 0xff), static_cast<u8>(v & 0xff),
            static_cast<u8>(v & 0xff), static_cast<u8>(v >> 8)};
  case 1:  // GX_TL_RGB565
    return Decode565(v);
  case 2:  // GX_TL_RGB5A3
    return Decode5A3(v);
  // SHPG/FNTG attachment type values seen in Frontline.
  case 0x30:
    return {static_cast<u8>(v >> 8), static_cast<u8>(v >> 8),
            static_cast<u8>(v >> 8), static_cast<u8>(v & 0xff)};
  case 0x31:
    return Decode565(v);
  case 0x32:
    return Decode5A3(v);
  default:
    return {};
  }
}

void Put(std::vector<u8>* rgba, u32 width, u32 height, u32 x, u32 y, RGBA c)
{
  if (!rgba || x >= width || y >= height)
    return;
  const std::size_t p = (static_cast<std::size_t>(y) * width + x) * 4u;
  (*rgba)[p + 0] = c.r;
  (*rgba)[p + 1] = c.g;
  (*rgba)[p + 2] = c.b;
  (*rgba)[p + 3] = c.a;
}

bool DecodeI4(u32 width, u32 height, std::span<const u8> src, std::vector<u8>* rgba)
{
  const u32 blocks_x = (width + 7) / 8;
  const u32 blocks_y = (height + 7) / 8;
  if (src.size() < static_cast<std::size_t>(blocks_x) * blocks_y * 32u) return false;
  for (u32 y = 0; y < height; ++y) for (u32 x = 0; x < width; ++x)
  {
    const u32 block = (y / 8) * blocks_x + x / 8;
    const u32 inside = (y & 7) * 4 + ((x & 7) >> 1);
    const u8 packed = src[static_cast<std::size_t>(block) * 32u + inside];
    const u8 i4 = (x & 1) ? (packed & 0xf) : (packed >> 4);
    const u8 i = Expand4(i4);
    Put(rgba, width, height, x, y, {i, i, i, 255});
  }
  return true;
}

bool Decode8x4(u32 width, u32 height, std::span<const u8> src, std::vector<u8>* rgba,
               u32 mode, u32 palette_format, std::span<const u8> palette)
{
  const u32 blocks_x = (width + 7) / 8;
  const u32 blocks_y = (height + 3) / 4;
  if (src.size() < static_cast<std::size_t>(blocks_x) * blocks_y * 32u) return false;
  for (u32 y = 0; y < height; ++y) for (u32 x = 0; x < width; ++x)
  {
    const u32 block = (y / 4) * blocks_x + x / 8;
    const u8 v = src[static_cast<std::size_t>(block) * 32u + (y & 3) * 8u + (x & 7)];
    RGBA c{};
    if (mode == 1) c = {v, v, v, 255};                         // I8
    else if (mode == 2) { const u8 i=Expand4(v&15), a=Expand4(v>>4); c={i,i,i,a}; } // IA4
    else c = DecodePalette(palette_format, palette, v);        // C8
    Put(rgba, width, height, x, y, c);
  }
  return true;
}

bool Decode4x4_16(u32 width, u32 height, std::span<const u8> src, std::vector<u8>* rgba,
                  u32 mode, u32 palette_format, std::span<const u8> palette)
{
  const u32 blocks_x = (width + 3) / 4;
  const u32 blocks_y = (height + 3) / 4;
  if (src.size() < static_cast<std::size_t>(blocks_x) * blocks_y * 32u) return false;
  for (u32 y = 0; y < height; ++y) for (u32 x = 0; x < width; ++x)
  {
    const u32 block = (y / 4) * blocks_x + x / 4;
    const std::size_t p = static_cast<std::size_t>(block) * 32u + ((y & 3) * 4u + (x & 3)) * 2u;
    const u16 v = BE16(src.data() + p);
    RGBA c{};
    if (mode == 3) { const u8 a=src[p], i=src[p+1]; c={i,i,i,a}; } // IA8: A,I
    else if (mode == 4) c = Decode565(v);
    else if (mode == 5) c = Decode5A3(v);
    else c = DecodePalette(palette_format, palette, v & 0x3fff); // C14X2
    Put(rgba, width, height, x, y, c);
  }
  return true;
}

bool DecodeRGBA8(u32 width, u32 height, std::span<const u8> src, std::vector<u8>* rgba)
{
  const u32 blocks_x = (width + 3) / 4;
  const u32 blocks_y = (height + 3) / 4;
  if (src.size() < static_cast<std::size_t>(blocks_x) * blocks_y * 64u) return false;
  for (u32 y = 0; y < height; ++y) for (u32 x = 0; x < width; ++x)
  {
    const u32 block = (y / 4) * blocks_x + x / 4;
    const std::size_t i = static_cast<std::size_t>(block) * 64u + ((y & 3) * 4u + (x & 3)) * 2u;
    Put(rgba, width, height, x, y, {src[i+1], src[i+32], src[i+33], src[i]});
  }
  return true;
}

bool DecodeC4(u32 width, u32 height, std::span<const u8> src, std::vector<u8>* rgba,
              u32 palette_format, std::span<const u8> palette)
{
  const u32 blocks_x = (width + 7) / 8;
  const u32 blocks_y = (height + 7) / 8;
  if (src.size() < static_cast<std::size_t>(blocks_x) * blocks_y * 32u) return false;
  for (u32 y=0; y<height; ++y) for (u32 x=0; x<width; ++x)
  {
    const u32 block=(y/8)*blocks_x+x/8;
    const u8 packed=src[static_cast<std::size_t>(block)*32u+(y&7)*4u+((x&7)>>1)];
    const u8 idx=(x&1)?(packed&15):(packed>>4);
    Put(rgba,width,height,x,y,DecodePalette(palette_format,palette,idx));
  }
  return true;
}

bool DecodeCMPR(u32 width, u32 height, std::span<const u8> src, std::vector<u8>* rgba)
{
  const u32 macro_x = (width + 7) / 8;
  const u32 macro_y = (height + 7) / 8;
  if (src.size() < static_cast<std::size_t>(macro_x) * macro_y * 32u) return false;
  for (u32 my=0; my<macro_y; ++my) for (u32 mx=0; mx<macro_x; ++mx)
  {
    const std::size_t macro = (static_cast<std::size_t>(my)*macro_x+mx)*32u;
    for (u32 sub=0; sub<4; ++sub)
    {
      const u8* b=src.data()+macro+sub*8u;
      const u16 c0v=BE16(b), c1v=BE16(b+2);
      const RGBA c0=Decode565(c0v), c1=Decode565(c1v);
      std::array<RGBA,4> colors{c0,c1,{},{}};
      if (c0v > c1v)
      {
        colors[2] = {static_cast<u8>((2*c0.r+c1.r)/3), static_cast<u8>((2*c0.g+c1.g)/3), static_cast<u8>((2*c0.b+c1.b)/3),255};
        colors[3] = {static_cast<u8>((c0.r+2*c1.r)/3), static_cast<u8>((c0.g+2*c1.g)/3), static_cast<u8>((c0.b+2*c1.b)/3),255};
      }
      else
      {
        colors[2] = {static_cast<u8>((c0.r+c1.r)/2), static_cast<u8>((c0.g+c1.g)/2), static_cast<u8>((c0.b+c1.b)/2),255};
        colors[3] = {0,0,0,0};
      }
      const u32 ox=(sub&1)*4, oy=(sub>>1)*4;
      for (u32 py=0; py<4; ++py)
      {
        const u8 bits=b[4+py];
        for (u32 px=0; px<4; ++px)
          Put(rgba,width,height,mx*8+ox+px,my*8+oy+py,colors[(bits >> (6-2*px)) & 3]);
      }
    }
  }
  return true;
}

std::shared_ptr<VideoCommon::CustomTextureData> Build(u32 width,u32 height,std::vector<u8> rgba)
{
  if (rgba.size() != static_cast<std::size_t>(width)*height*4u) return nullptr;
  auto out=std::make_shared<VideoCommon::CustomTextureData>();
  out->m_slices.emplace_back();
  auto& level=out->m_slices.back().m_levels.emplace_back();
  level.format=AbstractTextureFormat::RGBA8;
  level.width=width; level.height=height; level.row_length=width;
  level.data=Common::UniqueBuffer<u8>(rgba.size());
  std::memcpy(level.data.data(),rgba.data(),rgba.size());
  return out;
}
}  // namespace

bool ReplacementEnabled()
{
  const char* value = std::getenv("MOH_NATIVE_GC_TEXTURES");
  if (!value || !*value)
    return false;
  const std::string_view v(value);
  return v != "0" && v != "false" && v != "off" && v != "no";
}

std::shared_ptr<VideoCommon::CustomTextureData>
DecodeUpload(u32 width, u32 height, u32 format, std::span<const u8> texture,
             u32 palette_format, std::span<const u8> palette)
{
  if (!width || !height || width > 8192 || height > 8192 || texture.empty()) return nullptr;
  std::vector<u8> rgba(static_cast<std::size_t>(width)*height*4u,0);
  bool ok=false;
  switch (format)
  {
  case 0: ok=DecodeI4(width,height,texture,&rgba); break;
  case 1: ok=Decode8x4(width,height,texture,&rgba,1,palette_format,palette); break;
  case 2: ok=Decode8x4(width,height,texture,&rgba,2,palette_format,palette); break;
  case 3: ok=Decode4x4_16(width,height,texture,&rgba,3,palette_format,palette); break;
  case 4: ok=Decode4x4_16(width,height,texture,&rgba,4,palette_format,palette); break;
  case 5: ok=Decode4x4_16(width,height,texture,&rgba,5,palette_format,palette); break;
  case 6: ok=DecodeRGBA8(width,height,texture,&rgba); break;
  case 8: ok=DecodeC4(width,height,texture,&rgba,palette_format,palette); break;
  case 9: ok=Decode8x4(width,height,texture,&rgba,9,palette_format,palette); break;
  case 10: ok=Decode4x4_16(width,height,texture,&rgba,10,palette_format,palette); break;
  case 14: ok=DecodeCMPR(width,height,texture,&rgba); break;
  default: break;
  }
  return ok ? Build(width,height,std::move(rgba)) : nullptr;
}
}  // namespace MOHFrontline::GCTexture
