#include "VideoCommon/PS3Compass.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "Common/Hash.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/MohPcLayer.h"
#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/PS3TextureDecoder.h"
#include "VideoCommon/TextureInfo.h"

namespace PS3Compass
{
namespace
{
struct Resource
{
  std::string filename;
  std::string relative_path;
  std::shared_ptr<VideoCommon::CustomTextureData> decoded;
  bool attempted = false;
};

struct Registration
{
  int resource_id = -1;
  u32 address = 0;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  std::size_t texture_size = 0;
  u64 texture_hash = 0;
  u32 palette_format = 0;
  std::size_t palette_size = 0;
  u64 palette_hash = 0;
  bool logged = false;
};

std::unordered_map<int, Resource> resources;
std::unordered_map<std::string, int> resource_ids;
std::unordered_map<u32, Registration> registrations;
int next_resource_id = 0;
std::mutex mutex;

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string Normalize(std::string value)
{
  std::replace(value.begin(), value.end(), '\\', '/');
  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());
  return Lower(std::move(value));
}

std::string Filename(std::string_view path)
{
  const auto slash = path.find_last_of("/\\:");
  std::string filename(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
  return Lower(std::move(filename));
}


std::string StemKey(
    std::string_view path)
{
  std::string filename =
      Filename(path);

  const auto dot =
      filename.find_last_of('.');

  if (dot !=
      std::string::npos)
  {
    filename.resize(dot);
  }

  std::string key;
  key.reserve(
      filename.size());

  for (unsigned char c :
       filename)
  {
    if (std::isalnum(c))
    {
      key.push_back(
          static_cast<char>(
              std::tolower(c)));
    }
  }

  return key;
}

std::string CanonicalPS3Filename(std::string_view guest_name)
{
  std::string filename = Filename(guest_name);
  if (filename.ends_with(".gsh"))
    filename.replace(filename.size() - 4, 4, ".ssh");
  return filename;
}

std::string CanonicalGuestPath(std::string_view guest_name)
{
  std::string path = Normalize(std::string(guest_name));
  if (path.ends_with(".gsh"))
    path.replace(path.size() - 4, 4, ".ssh");
  return path;
}

std::size_t CommonSuffixScore(std::string_view a, std::string_view b)
{
  std::size_t score = 0;
  while (score < a.size() && score < b.size() &&
         a[a.size() - score - 1] == b[b.size() - score - 1])
  {
    ++score;
  }
  return score;
}

const PS3RemasterAssets::AssetInfo* FindBestAsset(std::string_view guest_name)
{
  if (!PS3RemasterAssets::IsReady())
    return nullptr;

  const std::string wanted_filename =
      CanonicalPS3Filename(
          guest_name);

  if (!wanted_filename.ends_with(
          ".ssh"))
  {
    return nullptr;
  }

  const std::string wanted_path =
      CanonicalGuestPath(
          guest_name);

  const PS3RemasterAssets::AssetInfo*
      best = nullptr;

  std::size_t best_score = 0;

  // Pass 1: exact basename. This remains authoritative.
  for (const auto& asset :
       PS3RemasterAssets::GetAssets())
  {
    if (Lower(
            asset.filename) !=
        wanted_filename)
    {
      continue;
    }

    const std::size_t score =
        CommonSuffixScore(
            wanted_path,
            Normalize(
                asset.relative_path));

    if (!best ||
        score >
            best_score)
    {
      best =
          &asset;

      best_score =
          score;
    }
  }

  if (best)
    return best;

  // Pass 2: safe spelling normalization only.
  // Examples: health_bar.gsh <-> HealthBar.ssh,
  // weapon-icon.gsh <-> weapon_icon.ssh.
  const std::string wanted_stem =
      StemKey(
          wanted_filename);

  if (wanted_stem.empty())
    return nullptr;

  for (const auto& asset :
       PS3RemasterAssets::GetAssets())
  {
    const std::string asset_name =
        Lower(
            asset.filename);

    if (!asset_name.ends_with(
            ".ssh") ||
        StemKey(
            asset_name) !=
            wanted_stem)
    {
      continue;
    }

    const std::size_t score =
        CommonSuffixScore(
            wanted_path,
            Normalize(
                asset.relative_path));

    if (!best ||
        score >
            best_score)
    {
      best =
          &asset;

      best_score =
          score;
    }
  }


  if (best)
    return best;

  // menu substring fallback: Main_Master -> main, Options_TXT -> options, etc.
  const std::string wanted_key = StemKey(wanted_filename);

  if (wanted_key.size() >= 5)
  {
    for (const auto& asset : PS3RemasterAssets::GetAssets())
    {
      const std::string asset_name = Lower(asset.filename);
      if (!asset_name.ends_with(".ssh"))
        continue;

      const std::string asset_key = StemKey(asset_name);
      if (asset_key.size() < 5)
        continue;

      const bool related =
          wanted_key.find(asset_key) != std::string::npos ||
          asset_key.find(wanted_key) != std::string::npos;

      if (!related)
        continue;

      const std::string relative = Normalize(asset.relative_path);
      const bool menuish =
          relative.find("menu") != std::string::npos ||
          relative.find("pause") != std::string::npos ||
          relative.find("frontend") != std::string::npos ||
          relative.find("_usa") != std::string::npos;

      if (!menuish)
        continue;

      best = &asset;
      std::fprintf(
          stderr,
          "[moh-ps3-texture] menu substring fallback: %.*s -> %s\n",
          static_cast<int>(guest_name.size()),
          guest_name.data(),
          relative.c_str());
      break;
    }
  }

  return best;
}


std::shared_ptr<VideoCommon::CustomTextureData> DecodeResource(Resource* resource)
{
  if (!resource)
    return nullptr;

  if (resource->attempted)
    return resource->decoded;

  resource->attempted = true;

  const auto* asset = PS3RemasterAssets::FindByRelativePath(resource->relative_path);
  if (!asset)
  {
    std::fprintf(stderr, "[moh-ps3-texture] asset disappeared from index: %s\n",
                 resource->relative_path.c_str());
    return nullptr;
  }

  std::vector<PS3TextureDecoder::Level> levels;
  if (!PS3TextureDecoder::Decode(PS3RemasterAssets::ReadBinary(*asset), &levels) || levels.empty())
  {
    std::fprintf(stderr,
                 "[moh-ps3-texture] unsupported/malformed PS3 texture: %s; keeping GC texture\n",
                 resource->relative_path.c_str());
    return nullptr;
  }

  auto decoded = std::make_shared<VideoCommon::CustomTextureData>();
  decoded->m_slices.emplace_back();

  for (const auto& source : levels)
  {
    VideoCommon::CustomTextureData::ArraySlice::Level level;
    level.width = source.width;
    level.height = source.height;
    level.row_length = source.width;
    level.data.reset(source.rgba.size());
    std::copy(source.rgba.begin(), source.rgba.end(), level.data.begin());
    decoded->m_slices[0].m_levels.push_back(std::move(level));
  }

  resource->decoded = decoded;

  std::fprintf(stderr, "[moh-ps3-texture] loaded %s: %ux%u, %zu mip levels\n",
               resource->relative_path.c_str(), levels[0].width, levels[0].height, levels.size());
  return resource->decoded;
}
}  // namespace

int NameIndex(std::string_view name)
{
  const auto* asset =
      FindBestAsset(
          name);

  if (!asset)
  {
    static unsigned miss_logs = 0;

    if (miss_logs < 160 &&
        Filename(name).ends_with(
            ".gsh"))
    {
      ++miss_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-texture] no PS3 match: guest=%.*s stem=%s\\n",
          static_cast<int>(
              name.size()),
          name.data(),
          StemKey(name)
              .c_str());
    }

    return -1;
  }

  const std::string key =
      Normalize(
          asset->relative_path);

  std::scoped_lock lock(
      mutex);

  if (const auto it =
          resource_ids.find(
              key);
      it !=
          resource_ids.end())
  {
    return it->second;
  }

  const int id =
      next_resource_id++;

  Resource resource;

  resource.filename =
      asset->filename;

  resource.relative_path =
      key;

  resources.emplace(
      id,
      std::move(resource));

  resource_ids.emplace(
      key,
      id);

  static unsigned match_logs = 0;

  if (match_logs < 160)
  {
    ++match_logs;

    std::fprintf(
        stderr,
        "[moh-ps3-texture] map guest=%.*s -> PS3=%s\\n",
        static_cast<int>(
            name.size()),
        name.data(),
        key.c_str());
  }

  return id;
}


void Register(int index, u32 address, u32 width, u32 height, u32 format, std::vector<u8> original,
              u32 palette_format, std::vector<u8> palette)
{
  if (index < 0 || original.empty() || !width || !height)
    return;

  std::scoped_lock lock(mutex);

  auto resource_it = resources.find(index);
  if (resource_it == resources.end())
    return;

  auto decoded = DecodeResource(&resource_it->second);
  if (!decoded)
    return;

  Registration registration;
  registration.resource_id = index;
  registration.address = address & 0x1FFFFFFF;
  registration.width = width;
  registration.height = height;
  registration.format = format;
  registration.texture_size = original.size();
  registration.texture_hash = Common::GetHash64(original.data(), original.size(), 0);
  registration.palette_format = palette_format;
  registration.palette_size = palette.size();
  if (!palette.empty())
    registration.palette_hash = Common::GetHash64(palette.data(), palette.size(), 0);

  registrations[registration.address] = std::move(registration);
}

std::shared_ptr<VideoCommon::CustomTextureData> Find(const TextureInfo& info)
{
  if (!MohPcLayer::IsPS3TextureReplacementEnabled())
    return nullptr;


  if (info.IsFromTmem())
    return nullptr;

  const u32 address = info.GetRawAddress();
  Registration registration;
  std::shared_ptr<VideoCommon::CustomTextureData> decoded;
  std::string relative_path;

  {
    std::scoped_lock lock(mutex);
    const auto it = registrations.find(address);
    if (it == registrations.end())
      return nullptr;

    registration = it->second;

    const auto resource_it = resources.find(registration.resource_id);
    if (resource_it == resources.end() || !resource_it->second.decoded)
      return nullptr;

    decoded = resource_it->second.decoded;
    relative_path = resource_it->second.relative_path;
  }

  if (registration.width != info.GetRawWidth() ||
      registration.height != info.GetRawHeight() ||
      registration.format != static_cast<u32>(info.GetTextureFormat()) ||
      registration.texture_size != info.GetTextureSize() ||
      registration.texture_hash != Common::GetHash64(info.GetData(), info.GetTextureSize(), 0))
  {
    return nullptr;
  }

  if (registration.palette_size)
  {
    if (registration.palette_format != static_cast<u32>(info.GetTlutFormat()) ||
        info.GetPaletteSize().value_or(0) != registration.palette_size ||
        !info.GetTlutAddress() ||
        registration.palette_hash !=
            Common::GetHash64(info.GetTlutAddress(), registration.palette_size, 0))
    {
      return nullptr;
    }
  }

  {
    std::scoped_lock lock(mutex);
    auto it = registrations.find(address);
    if (it != registrations.end() && it->second.resource_id == registration.resource_id &&
        it->second.texture_hash == registration.texture_hash && !it->second.logged)
    {
      it->second.logged = true;
      std::fprintf(stderr,
                   "[moh-ps3-texture] replacement active: GC %ux%u fmt=%u @%08x -> %s\n",
                   registration.width, registration.height, registration.format, address,
                   relative_path.c_str());
    }
  }

  return decoded;
}

void Shutdown()
{
  std::scoped_lock lock(mutex);
  registrations.clear();
  resources.clear();
  resource_ids.clear();
  next_resource_id = 0;
}
}  // namespace PS3Compass
