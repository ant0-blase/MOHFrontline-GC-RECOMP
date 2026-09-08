#include "VideoCommon/MOHFrontline/Engine/Renderer/Materials/PS3MaterialCatalog.h"
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/TPK.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
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

bool TraceEnabled()
{
  static const bool enabled = [] { const auto* v = std::getenv("MOH_PS3_RSX_TRACE"); return v && std::string_view(v) == "1"; }();
  return enabled;
}
// Bounded persistent file handles. Catalog and range locks are never acquired
// in reverse order; reads from a shared stream are serialized.
std::mutex range_mutex;
std::map<std::filesystem::path, std::ifstream> range_files;
std::uint64_t range_generation = ~std::uint64_t(0);
std::vector<std::uint8_t> ReadPayload(const PS3RemasterAssets::AssetInfo& asset,
                                    std::uint64_t offset, std::uint32_t size)
{
  if (!size || size > 128u*1024*1024 || offset > asset.size || size > asset.size-offset)
    return {};
  if (asset.embedded || asset.refpack) return PS3RemasterAssets::ReadRange(asset, offset, size);
  std::scoped_lock lock(range_mutex);
  if (range_generation != PS3RemasterAssets::GetIndexGeneration())
  { range_files.clear(); range_generation = PS3RemasterAssets::GetIndexGeneration(); }
  auto it = range_files.find(asset.absolute_path);
  if (it == range_files.end())
  {
    if (range_files.size() >= 8) range_files.erase(range_files.begin());
    it = range_files.try_emplace(asset.absolute_path, asset.absolute_path, std::ios::binary).first;
    if (TraceEnabled()) std::fprintf(stderr, "[PS3-RSX] opened %s size=%llu\n",
        asset.relative_path.c_str(), static_cast<unsigned long long>(asset.size));
  }
  auto& file = it->second;
  if (!file.is_open() || offset > std::uint64_t(std::numeric_limits<std::streamoff>::max())) return {};
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset));
  std::vector<std::uint8_t> bytes(size);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
  return bytes;
}
std::vector<std::uint8_t> MakeGTF(const PS3RemasterAssets::AssetInfo& asset,
    std::uint64_t offset, std::uint32_t size, const std::array<std::uint8_t, 24>& descriptor)
{
  const auto bytes = ReadPayload(asset, offset, size);
  if (!size || bytes.size() != size) return {};
  std::vector<std::uint8_t> gtf(48 + bytes.size());
  auto write = [&](std::size_t p, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) gtf[p+i] = v >> (24 - 8*i);
  };
  write(0, 0x02010100); write(4, size); write(8, 1); write(16, 20); write(20, size);
  std::copy(descriptor.begin(), descriptor.end(), gtf.begin() + 24);
  std::copy(bytes.begin(), bytes.end(), gtf.begin() + 48);
  return gtf;
}
struct Catalog
{
  const PS3RemasterAssets::AssetInfo* rsx = nullptr;
  std::string metadata_source;
  std::map<std::string, std::shared_ptr<const std::vector<PS3TextureDecoder::CompressedLevel>>, std::less<>> compressed;
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
  c.metadata_source = tpk.asset->relative_path;
  std::set<std::string> ambiguous;
  for (auto& r : records)
  {
    if (r.offset > c.rsx->size || r.size > c.rsx->size - r.offset)
    {
      ++stats.failures;
      if (TraceEnabled()) std::fprintf(stderr, "[PS3-RSX] invalid bounds: resource=%s offset=%u size=%u; using GC\n", r.name.c_str(), r.offset, r.size);
      continue;
    }
    const auto name = CanonicalTPKName(r.name);
    if (name.empty())
    {
      ++stats.failures;
      continue;
    }
    // Keep the original TPAC spelling in the descriptor; only the lookup key is canonical.
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
  if (TraceEnabled()) std::fprintf(stderr, "[PS3-RSX] metadata source=%s resources=%zu rsx=%s\n",
      c.metadata_source.c_str(), c.records.size(), c.rsx->relative_path.c_str());
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
    if (TraceEnabled()) std::fprintf(stderr, "[PS3-RSX] unsupported/invalid descriptor or payload: resource=%s format=0x%02X; using GC\n", r.name.c_str(), r.descriptor[0]);
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
  const auto gtf = MakeGTF(asset, offset, size, descriptor);
  if (gtf.empty()) return {};
  auto decoded = std::make_shared<std::vector<PS3TextureDecoder::Level>>();
  if (!PS3TextureDecoder::Decode(gtf, decoded.get()) || decoded->empty()) return {};
  return decoded;
}

std::optional<TextureResource> FindTextureResource(std::string_view level, std::string_view name)
{
  std::scoped_lock lock(mutex);
  auto& c = Get(level);
  auto it = c.records.find(CanonicalTPKName(name));
  if (it == c.records.end() || !c.rsx) return {};
  return TextureResource{it->second, c.metadata_source, c.rsx->relative_path};
}

std::vector<TextureResource> ListTextureResources(std::string_view level)
{
  std::scoped_lock lock(mutex);
  auto& c = Get(level);
  std::vector<TextureResource> resources;
  if (!c.rsx)
    return resources;

  resources.reserve(c.records.size());
  for (const auto& entry : c.records)
    resources.push_back(TextureResource{entry.second, c.metadata_source, c.rsx->relative_path});

  return resources;
}

std::vector<std::uint8_t> ReadTexturePayload(std::string_view level, std::string_view name)
{
  std::scoped_lock lock(mutex);
  auto& c = Get(level);
  auto it = c.records.find(CanonicalTPKName(name));
  if (it == c.records.end() || !c.rsx) return {};
  return ReadPayload(*c.rsx, it->second.offset, it->second.size);
}
std::shared_ptr<const std::vector<PS3TextureDecoder::CompressedLevel>> LoadCompressedTexture(
    std::string_view level, std::string_view name)
{
  std::scoped_lock lock(mutex);
  auto& c = Get(level);
  const auto key = CanonicalTPKName(name);
  if (auto it = c.compressed.find(key); it != c.compressed.end()) return it->second;
  auto it = c.records.find(key);
  if (it == c.records.end() || !c.rsx) return {};
  const auto& r = it->second;
  const unsigned format = r.descriptor[0] & ~0x60u;
  if (format < 0x86 || format > 0x88) { c.compressed[key] = {}; return {}; }
  auto result = std::make_shared<std::vector<PS3TextureDecoder::CompressedLevel>>();
  if (!PS3TextureDecoder::DecodeCompressed(MakeGTF(*c.rsx, r.offset, r.size, r.descriptor), result.get()))
    result.reset();
  if (result)
  {
    ++stats.decoded;
    if (TraceEnabled()) std::fprintf(stderr, "[PS3-RSX] native BC prepared: resource=%s format=0x%02X mips=%zu\n", r.name.c_str(), r.descriptor[0], result->size());
  }
  c.compressed[key] = result;
  return result;
}

Statistics GetStatistics() { std::scoped_lock lock(mutex); return stats; }
}
