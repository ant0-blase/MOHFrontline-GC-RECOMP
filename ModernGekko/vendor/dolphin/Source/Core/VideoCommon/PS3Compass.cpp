#include "VideoCommon/PS3Compass.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <mutex>
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/PS3TextureDecoder.h"
#include "VideoCommon/TextureInfo.h"

namespace PS3Compass
{
namespace
{
constexpr const char* names[] = {"compass.ssh", "compassdial.ssh", "compassedge.ssh"};
struct Entry
{
  u32 address = 0, width = 0, height = 0, format = 0;
  std::vector<u8> original, palette;
  u32 palette_format = 0;
  std::shared_ptr<VideoCommon::CustomTextureData> decoded;
  bool attempted = false, logged = false;
};
std::array<Entry, 3> entries;
std::mutex mutex;
}  // namespace
int NameIndex(std::string_view name)
{
  name.remove_prefix(
      name.find_last_of("/\\:") == std::string_view::npos ? 0 : name.find_last_of("/\\:") + 1);
  // GMFE69 ships these same assets as .gsh, not .ssh.
  std::string canonical(name);
  std::transform(canonical.begin(), canonical.end(), canonical.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (canonical.ends_with(".gsh"))
    canonical.replace(canonical.size() - 4, 4, ".ssh");
  for (int i = 0; i < 3; ++i)
    if (canonical == names[i])
      return i;
  return -1;
}
void Register(int index, u32 address, u32 width, u32 height, u32 format, std::vector<u8> original,
              u32 palette_format, std::vector<u8> palette)
{
  if (index < 0 || index >= 3 || original.empty())
    return;
  std::scoped_lock lock(mutex);
  auto& e = entries[index];
  e.address = address & 0x1FFFFFFF;
  e.width = width;
  e.height = height;
  e.format = format;
  e.original = std::move(original);
  e.palette_format = palette_format;
  e.palette = std::move(palette);
  // Asset index is initialized before guest execution. Each file is decoded once,
  // outside rendering, including all authored mip levels.
  if (!e.attempted && PS3RemasterAssets::IsReady())
  {
    e.attempted = true;
    const auto* asset = PS3RemasterAssets::FindByFilename(names[index]);
    std::vector<PS3TextureDecoder::Level> levels;
    if (!asset || !PS3TextureDecoder::Decode(PS3RemasterAssets::ReadBinary(*asset), &levels))
    {
      std::fprintf(stderr, "[moh-ps3-compass] unavailable/unsupported: %s; keeping GC texture\n",
                   names[index]);
      return;
    }
    e.decoded = std::make_shared<VideoCommon::CustomTextureData>();
    e.decoded->m_slices.emplace_back();
    for (const auto& source : levels)
    {
      VideoCommon::CustomTextureData::ArraySlice::Level level;
      level.width = source.width;
      level.height = source.height;
      level.row_length = source.width;
      level.data.reset(source.rgba.size());
      std::copy(source.rgba.begin(), source.rgba.end(), level.data.begin());
      e.decoded->m_slices[0].m_levels.push_back(std::move(level));
    }
    std::fprintf(stderr, "[moh-ps3-compass] PS3 %s loaded: %ux%u, %zu mip levels\n", names[index],
                 levels[0].width, levels[0].height, levels.size());
  }
}
std::shared_ptr<VideoCommon::CustomTextureData> Find(const TextureInfo& info)
{
  std::scoped_lock lock(mutex);
  for (unsigned i = 0; i < entries.size(); ++i)
  {
    auto& e = entries[i];
    if (!e.decoded || info.IsFromTmem() || e.address != info.GetRawAddress() ||
        e.width != info.GetRawWidth() || e.height != info.GetRawHeight() ||
        e.format != static_cast<u32>(info.GetTextureFormat()) ||
        e.original.size() != info.GetTextureSize() ||
        !std::equal(e.original.begin(), e.original.end(), info.GetData()))
      continue;
    if (!e.palette.empty() &&
        (e.palette_format != static_cast<u32>(info.GetTlutFormat()) ||
         info.GetPaletteSize().value_or(0) != e.palette.size() ||
         !std::equal(e.palette.begin(), e.palette.end(), info.GetTlutAddress())))
      continue;
    if (!e.logged)
    {
      e.logged = true;
      std::fprintf(stderr, "[moh-ps3-compass] replacement active: %s -> %s (original GX draw)\n",
                   i == 1 ? "compassDial.ssh" :
                   i == 2 ? "compassEdge.ssh" :
                            "compass.ssh",
                   names[i]);
    }
    return e.decoded;
  }
  return nullptr;
}
void Shutdown()
{
  std::scoped_lock lock(mutex);
  entries = {};
}
}  // namespace PS3Compass
