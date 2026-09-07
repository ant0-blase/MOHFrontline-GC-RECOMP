#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/TPK.h"
#include <algorithm>
#include <set>
namespace MOHFrontline::PS3::TPK
{
bool Parse(std::span<const std::uint8_t> b, std::vector<Texture>* out)
{
  if (!out) return false;
  out->clear();
  if (b.size() < 32 || !std::equal(b.begin(), b.begin() + 4, "TPAC")) return false;
  auto u32 = [&](std::size_t p) { return (std::uint32_t(b[p]) << 24) |
      (std::uint32_t(b[p+1]) << 16) | (std::uint32_t(b[p+2]) << 8) | b[p+3]; };
  const auto count = u32(8), names = u32(12), pointers = u32(16);
  if (!count || count > 65536 || names > b.size() || pointers > b.size() ||
      count > (b.size() - names) / 32 || count > (b.size() - pointers) / 4) return false;
  std::vector<Texture> parsed;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    const auto p = std::size_t(names) + i * 32;
    auto end = std::find(b.begin() + p, b.begin() + p + 32, 0);
    const auto offset = u32(pointers + i * 4);
    if (end == b.begin() + p || offset > b.size() || b.size() - offset < 48) return false;
    Texture t;
    t.name.assign(b.begin() + p, end);
    t.size = u32(offset + 4); t.offset = u32(offset + 8);
    std::copy_n(b.begin() + offset + 16, 24, t.descriptor.begin());
    if (!t.size || t.descriptor[2] != 2 || t.descriptor[3] != 0 || !t.descriptor[1]) return false;
    parsed.push_back(std::move(t));
  }
  *out = std::move(parsed);
  return true;
}
}
