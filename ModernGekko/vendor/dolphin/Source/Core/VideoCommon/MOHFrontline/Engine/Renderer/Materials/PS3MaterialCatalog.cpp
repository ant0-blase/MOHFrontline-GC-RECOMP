#include "VideoCommon/MOHFrontline/Engine/Renderer/Materials/PS3MaterialCatalog.h"
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/TPK.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
namespace MOHFrontline::Materials
{
namespace
{
std::string CanonicalTPKName(std::string_view input)
{
  std::string name(input);
  const auto slash = name.find_last_of("/\\:");
  if (slash != std::string::npos)
    name.erase(0, slash + 1);

  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (name.ends_with(".gsh") || name.ends_with(".ssh"))
    name.resize(name.size() - 4);

  return name;
}

struct Catalog
{
  const PS3RemasterAssets::AssetInfo* rsx = nullptr;
  std::map<std::string, PS3::TPK::Texture, std::less<>> records;
  std::map<std::string, std::shared_ptr<const std::vector<PS3TextureDecoder::Level>>, std::less<>> decoded;
};
std::mutex mutex;
std::map<std::string, Catalog, std::less<>> catalogs;
std::uint64_t generation = ~std::uint64_t(0);
Statistics stats;
Catalog& Get(std::string_view level)
{
  if (generation != PS3RemasterAssets::GetIndexGeneration())
  { generation = PS3RemasterAssets::GetIndexGeneration(); catalogs.clear(); stats = {}; }
  auto [it, inserted] = catalogs.try_emplace(std::string(level));
  auto& c = it->second;
  if (!inserted) return c;
  const auto levels = NativeAssets::GetLevels();
  if (std::find(levels.begin(), levels.end(), level) == levels.end()) { ++stats.failures; return c; }
  const auto mission = level.substr(0, level.find('_'));
  const auto scope = "data/" + std::string(mission) + '/' + std::string(level) + '/';
  auto tpk = NativeAssets::Resolve(scope + "tpk" + std::string(level) + ".tpk", NativeAssets::Domain::Container);
  auto rsx = NativeAssets::Resolve(scope + "rsx.viv", NativeAssets::Domain::Container);
  std::vector<PS3::TPK::Texture> records;
  if (!tpk || !rsx || !PS3::TPK::Parse(NativeAssets::Read(tpk), &records))
  { ++stats.failures;
    std::fprintf(stderr, "[moh-ps3-tpk] FAIL file=%stpk%.*s.tpk record=header reason=missing or invalid TPAC/RSX\n",
                 scope.c_str(), int(level.size()), level.data());
    return c; }
  c.rsx = rsx.asset;
  std::set<std::string> ambiguous;
  for (auto& r : records)
  {
    if (r.offset > c.rsx->size || r.size > c.rsx->size - r.offset) { ++stats.failures; continue; }
    const auto name = CanonicalTPKName(r.name);
    if (name.empty())
    {
      ++stats.failures;
      continue;
    }
    r.name = name;
    if (ambiguous.contains(name)) continue;
    if (auto existing = c.records.find(name); existing != c.records.end())
    {
      if (existing->second.offset != r.offset || existing->second.size != r.size ||
          existing->second.descriptor != r.descriptor)
      {
        c.records.erase(existing); ambiguous.insert(name); ++stats.failures;
        std::fprintf(stderr, "[moh-native][material] ambiguous TPAC name excluded: %s/%s\n",
                     std::string(level).c_str(), name.c_str());
      }
    }
    else c.records.emplace(name, std::move(r));
  }
  ++stats.catalogs; stats.records += c.records.size();
  std::fprintf(stderr, "[moh-ps3-tpk] loaded level=%.*s records=%zu (exact guest-name binding ready)\n",
               int(level.size()), level.data(), c.records.size());
  return c;
}
}
bool HasTexture(std::string_view level, std::string_view name)
{
  std::scoped_lock lock(mutex);
  return Get(level).records.contains(CanonicalTPKName(name));
}
std::shared_ptr<const std::vector<PS3TextureDecoder::Level>> LoadTexture(std::string_view level, std::string_view name)
{
  std::scoped_lock lock(mutex);
  auto& c = Get(level);
  const std::string key = CanonicalTPKName(name);
  if (key.empty()) return {};
  if (auto it = c.decoded.find(key); it != c.decoded.end()) return it->second;
  const auto found = c.records.find(key);
  if (found == c.records.end()) return {};
  const auto& r = found->second;
  auto decoded = DecodeRSXTexture(*c.rsx, r.offset, r.size, r.descriptor);
  if (!decoded)
  {
    ++stats.failures;
    std::fprintf(stderr, "[moh-ps3-tpk] FAIL level=%.*s record=%.*s reason=RSX range or texture decode\n",
                 int(level.size()), level.data(), int(key.size()), key.data());
  }
  else
  {
    ++stats.decoded;
    std::fprintf(stderr, "[moh-ps3-tpk] material=%.*s texture=%.*s size=%ux%u\n",
                 int(key.size()), key.data(), int(key.size()), key.data(),
                 decoded->front().width, decoded->front().height);
  }
  c.decoded[key] = decoded;
  return decoded;
}
std::shared_ptr<const std::vector<PS3TextureDecoder::Level>> DecodeRSXTexture(
    const PS3RemasterAssets::AssetInfo& asset, std::uint64_t offset, std::uint32_t size,
    const std::array<std::uint8_t, 24>& descriptor)
{
  const auto bytes = PS3RemasterAssets::ReadRange(asset, offset, size);
  if (!size || bytes.size() != size) return {};
  // Existing decoder accepts this 48-byte GTF header plus an unchanged RSX
  // payload. Descriptor fields and BC blocks retain their documented endian.
  std::vector<std::uint8_t> gtf(48 + bytes.size());
  auto write = [&](std::size_t p, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) gtf[p+i] = v >> (24 - 8*i);
  };
  write(0, 0x02010100); write(4, size); write(8, 1); write(16, 20); write(20, size);
  std::copy(descriptor.begin(), descriptor.end(), gtf.begin() + 24);
  std::copy(bytes.begin(), bytes.end(), gtf.begin() + 48);
  auto decoded = std::make_shared<std::vector<PS3TextureDecoder::Level>>();
  if (!PS3TextureDecoder::Decode(gtf, decoded.get()) || decoded->empty()) return {};
  return decoded;
}

Statistics GetStatistics() { std::scoped_lock lock(mutex); return stats; }
}
