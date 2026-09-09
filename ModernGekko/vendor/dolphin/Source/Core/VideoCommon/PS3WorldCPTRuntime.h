#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/PS3MeshPort.h"
#include "VideoCommon/PS3TextureDecoder.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/TextureInfo.h"
#include "VideoCommon/VideoConfig.h"

// Runtime CPT bridge for MOH Frontline.
//
// Unlike the old PS3WorldCPT table, this file contains no level-specific
// hashes, no pre-generated RSX offsets and no PS3_PORT_CACHE dependency.
// It builds the mapping from the user's own GameCube + PS3 files at runtime:
//
//   GC level.viv::*_ART*.cpt --scan SHPG--> exact GC texture identities
//   PS3 *_ART*.cpt          --scan RSX records--> descriptors + rsx.viv ranges
//   runtime decode + image fingerprint matching (master/chunks aggregated)
//   live TextureInfo exact hash --> PS3 GTF/BC decode --> CustomTextureData
//
// The parser is deliberately conservative. Unmatched/ambiguous entries stay
// GameCube-native rather than entering the legacy fuzzy texture matcher.
namespace PS3WorldCPTRuntime
{
namespace detail
{
struct GCTexture
{
  u64 hash = 0;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  std::size_t source_offset = 0;
  std::size_t payload_offset = 0;
  std::size_t payload_size = 0;
};

struct PS3Record
{
  u32 offset = 0;
  u32 size = 0;
  std::array<u8, 24> descriptor{};
  std::size_t metadata_offset = 0;

  u32 Width() const
  {
    return (u32(descriptor[8]) << 8) | u32(descriptor[9]);
  }
  u32 Height() const
  {
    return (u32(descriptor[10]) << 8) | u32(descriptor[11]);
  }
  u32 Format() const { return descriptor[0]; }
  u32 Mips() const { return descriptor[1]; }
};

struct Entry
{
  u64 gc_hash = 0;
  u32 gc_width = 0;
  u32 gc_height = 0;
  u32 gc_format = 0;

  u32 ps3_offset = 0;
  u32 ps3_size = 0;
  std::array<u8, 24> descriptor{};

  std::string level;
  std::string chunk;
  std::string rsx_path;
  float match_score = 0.0f;
  float match_margin = 0.0f;
};

struct Blob
{
  std::string name;
  std::vector<u8> data;
};

struct Catalog
{
  std::uint64_t generation = 0;
  std::string level;
  std::string rsx_path;
  std::vector<Entry> entries;
  std::unordered_set<u64> known;
  std::size_t gc_chunks = 0;
  std::size_t ps3_chunks = 0;
  std::size_t gc_textures = 0;
  std::size_t ps3_records = 0;
};

inline bool EnvEnabled(const char* name, bool fallback)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;
  std::string text(value);
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (text == "0" || text == "false" || text == "off" || text == "no")
    return false;
  if (text == "1" || text == "true" || text == "on" || text == "yes")
    return true;
  return fallback;
}

inline bool Enabled()
{
  static const bool enabled = EnvEnabled("MOH_PS3_WORLD_CPT", true);
  return enabled;
}

inline bool TraceEnabled()
{
  static const bool enabled = EnvEnabled("MOH_PS3_WORLD_CPT_TRACE", false) ||
                              EnvEnabled("MOH_PS3_RSX_TRACE", false);
  return enabled;
}

inline std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

inline std::string Normalize(std::string value)
{
  std::replace(value.begin(), value.end(), '\\', '/');
  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());
  return Lower(std::move(value));
}

inline u16 BE16(const u8* p)
{
  return static_cast<u16>((u16(p[0]) << 8) | u16(p[1]));
}

inline u32 BE24(const u8* p)
{
  return (u32(p[0]) << 16) | (u32(p[1]) << 8) | u32(p[2]);
}

inline u32 BE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

inline void WriteBE32(std::vector<u8>* out, std::size_t offset, u32 value)
{
  (*out)[offset + 0] = static_cast<u8>(value >> 24);
  (*out)[offset + 1] = static_cast<u8>(value >> 16);
  (*out)[offset + 2] = static_cast<u8>(value >> 8);
  (*out)[offset + 3] = static_cast<u8>(value);
}

inline u64 FNV1a64(const u8* data, std::size_t size)
{
  u64 hash = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < size; ++i)
  {
    hash ^= data[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

/* MOH_PS3_RUNTIME_CPT_HASH_CACHE_V1
 *
 * ReplacementKey() is queried from TextureCacheBase::LoadImpl before the
 * normal Dolphin texture-cache fast path. The old runtime-CPT code therefore
 * re-hashed the complete guest texture for every bind.
 *
 * Keep full FNV-1a as the authoritative value on cache miss. Repeated binds
 * validate pointer/address reuse with 128 bytes sampled across the texture.
 */
struct RuntimeTextureHashCacheLine
{
  const u8* data = nullptr;
  u32 address = 0;
  std::size_t size = 0;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  u64 sample = 0;
  u64 hash = 0;
  bool valid = false;
};

inline thread_local std::array<RuntimeTextureHashCacheLine, 1024> runtime_texture_hash_cache{};

inline u64 RuntimeTextureSampleSignature(const u8* data, std::size_t size)
{
  if (!data || !size)
    return 0;

  constexpr std::size_t windows = 8;
  constexpr std::size_t bytes_per_window = 16;

  u64 hash = 0xcbf29ce484222325ULL;
  hash ^= static_cast<u64>(size);
  hash *= 0x100000001b3ULL;

  for (std::size_t window = 0; window < windows; ++window)
  {
    const std::size_t span =
        size > bytes_per_window ? size - bytes_per_window : 0;
    const std::size_t base =
        windows > 1 ? (span * window) / (windows - 1) : 0;
    const std::size_t count =
        std::min(bytes_per_window, size - base);

    for (std::size_t i = 0; i < count; ++i)
    {
      hash ^= data[base + i];
      hash *= 0x100000001b3ULL;
    }
  }

  return hash;
}

inline u64 RuntimeTextureHash(const TextureInfo& info)
{
  const u8* data = info.GetData();
  const std::size_t size = info.GetTextureSize();
  if (!data || !size)
    return 0;

  const u32 address = info.GetRawAddress();
  const u32 width = info.GetRawWidth();
  const u32 height = info.GetRawHeight();
  const u32 format = static_cast<u32>(info.GetTextureFormat());
  const u64 sample = RuntimeTextureSampleSignature(data, size);

  const std::uintptr_t pointer_bits =
      reinterpret_cast<std::uintptr_t>(data);
  u64 index_key = static_cast<u64>(pointer_bits);
  index_key ^= static_cast<u64>(address) * 0x9E3779B185EBCA87ULL;
  index_key ^= static_cast<u64>(size) * 0xC2B2AE3D27D4EB4FULL;
  index_key ^= static_cast<u64>(width) << 17;
  index_key ^= static_cast<u64>(height) << 33;
  index_key ^= static_cast<u64>(format) << 49;

  RuntimeTextureHashCacheLine& line =
      runtime_texture_hash_cache[index_key & (runtime_texture_hash_cache.size() - 1)];

  if (line.valid &&
      line.data == data &&
      line.address == address &&
      line.size == size &&
      line.width == width &&
      line.height == height &&
      line.format == format &&
      line.sample == sample)
  {
    return line.hash;
  }

  const u64 hash = FNV1a64(data, size);
  line.data = data;
  line.address = address;
  line.size = size;
  line.width = width;
  line.height = height;
  line.format = format;
  line.sample = sample;
  line.hash = hash;
  line.valid = true;
  return hash;
}

inline u64 IdentityKey(u64 hash, u32 width, u32 height, u32 format)
{
  u64 key = hash;
  key ^= static_cast<u64>(width) * 0x9E3779B185EBCA87ULL;
  key ^= static_cast<u64>(height) * 0xC2B2AE3D27D4EB4FULL;
  key ^= static_cast<u64>(format) * 0x165667B19E3779F9ULL;
  return key;
}

inline std::size_t GXTextureDataSize(u32 width, u32 height, u32 format)
{
  u32 bw = 0, bh = 0, bs = 0;
  switch (format)
  {
  case 0:
  case 8:
    bw = 8; bh = 8; bs = 32; break;
  case 1:
  case 2:
  case 9:
    bw = 8; bh = 4; bs = 32; break;
  case 3:
  case 4:
  case 5:
  case 10:
    bw = 4; bh = 4; bs = 32; break;
  case 6:
    bw = 4; bh = 4; bs = 64; break;
  case 14:
    bw = 8; bh = 8; bs = 32; break;
  default:
    return 0;
  }
  const std::uint64_t bx = (std::uint64_t(width) + bw - 1u) / bw;
  const std::uint64_t by = (std::uint64_t(height) + bh - 1u) / bh;
  const std::uint64_t size = bx * by * bs;
  return size <= std::numeric_limits<std::size_t>::max() ?
             static_cast<std::size_t>(size) : 0;
}

inline bool GSHTypeToGXFormat(u8 type, u32* out)
{
  if (!out)
    return false;
  switch (type)
  {
  case 20: *out = 4; return true;
  case 22: *out = 6; return true;
  case 24: *out = 8; return true;
  case 25: *out = 9; return true;
  case 30: *out = 14; return true;
  default: return false;
  }
}

inline bool ReadFile(const std::filesystem::path& path, std::vector<u8>* out)
{
  if (!out)
    return false;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  const std::streamoff end = file.tellg();
  if (end <= 0 || end > 1024ll * 1024ll * 1024ll)
    return false;
  file.seekg(0, std::ios::beg);
  out->resize(static_cast<std::size_t>(end));
  return bool(file.read(reinterpret_cast<char*>(out->data()),
                        static_cast<std::streamsize>(out->size())));
}

inline std::filesystem::path ResolveGCLevelVIV(std::string_view level)
{
  const auto underscore = level.find('_');
  if (underscore == std::string_view::npos || underscore == 0)
    return {};

  const std::string mission(level.substr(0, underscore));
  const std::string name(level);
  const std::array<std::filesystem::path, 4> relatives = {{
      std::filesystem::path("extracted") / "files" / "DATA" / mission / name / "level.viv",
      std::filesystem::path("extracted") / "files" / "data" / mission / name / "level.viv",
      std::filesystem::path("extracted") / "DATA" / mission / name / "level.viv",
      std::filesystem::path("extracted") / "data" / mission / name / "level.viv",
  }};

  std::vector<std::filesystem::path> candidates;
  std::error_code cwd_error;
  auto base = std::filesystem::current_path(cwd_error);
  if (!cwd_error)
  {
    for (unsigned depth = 0; depth < 7 && !base.empty(); ++depth)
    {
      for (const auto& relative : relatives)
        candidates.push_back(base / relative);
      const auto parent = base.parent_path();
      if (parent == base)
        break;
      base = parent;
    }
  }

  if (const char* value = std::getenv("MOH_GC_FILES"); value && *value)
  {
    const std::filesystem::path root(value);
    candidates.push_back(root / "DATA" / mission / name / "level.viv");
    candidates.push_back(root / "data" / mission / name / "level.viv");
    candidates.push_back(root / mission / name / "level.viv");
    for (const auto& relative : relatives)
      candidates.push_back(root / relative);
  }

  if (const char* value = std::getenv("MOH_PS3_FILES"); value && *value)
  {
    std::filesystem::path ps3(value);
    if (Lower(ps3.filename().string()) == "ps3_files")
    {
      const auto repo = ps3.parent_path().parent_path();
      for (const auto& relative : relatives)
        candidates.push_back(repo / relative);
    }
  }

  for (const auto& candidate : candidates)
  {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec) && !ec)
      return candidate;
  }
  return {};
}

inline std::vector<Blob> ExtractGCCPTs(std::span<const u8> viv)
{
  std::vector<Blob> out;
  if (viv.size() < 6 || viv[0] != 0xC0 || viv[1] != 0xFB)
    return out;

  const u32 count = BE16(viv.data() + 4);
  if (!count || count > 8192)
    return out;

  std::size_t pos = 6;
  for (u32 i = 0; i < count; ++i)
  {
    if (pos + 6 > viv.size())
      return {};
    const u32 offset = BE24(viv.data() + pos);
    const u32 size = BE24(viv.data() + pos + 3);
    pos += 6;

    const std::size_t begin = pos;
    while (pos < viv.size() && viv[pos] != 0 && pos - begin <= 255)
      ++pos;
    if (pos >= viv.size() || pos - begin > 255)
      return {};

    std::string name(reinterpret_cast<const char*>(viv.data() + begin), pos - begin);
    ++pos;
    const std::string lower = Lower(name);
    if (!lower.ends_with(".cpt") || lower.find("_art") == std::string::npos)
      continue;
    if (offset > viv.size() || size > viv.size() - offset)
      continue;

    Blob blob;
    blob.name = lower;
    blob.data.assign(viv.begin() + offset, viv.begin() + offset + size);
    out.push_back(std::move(blob));
  }
  return out;
}

inline std::vector<GCTexture> ParseGCCPT(std::span<const u8> cpt)
{
  std::vector<GCTexture> out;
  static constexpr std::array<u8, 4> tag = {'S', 'H', 'P', 'G'};

  for (std::size_t pos = 0; pos + 0x40 <= cpt.size();)
  {
    const auto it = std::search(cpt.begin() + pos, cpt.end(), tag.begin(), tag.end());
    if (it == cpt.end())
      break;
    const std::size_t shpg = static_cast<std::size_t>(it - cpt.begin());
    pos = shpg + 4;
    if (shpg + 0x40 > cpt.size())
      continue;

    const std::size_t shape = shpg + 0x30;
    u32 format = 0;
    if (!GSHTypeToGXFormat(cpt[shape], &format))
      continue;

    const u32 width = BE16(cpt.data() + shape + 4);
    const u32 height = BE16(cpt.data() + shape + 6);
    if (!width || !height || width > 8192 || height > 8192)
      continue;

    const std::size_t payload_size = GXTextureDataSize(width, height, format);
    const std::size_t payload = shape + 16;
    if (!payload_size || payload > cpt.size() || payload_size > cpt.size() - payload)
      continue;

    GCTexture texture;
    texture.hash = FNV1a64(cpt.data() + payload, payload_size);
    texture.width = width;
    texture.height = height;
    texture.format = format;
    texture.source_offset = shpg;
    texture.payload_offset = payload;
    texture.payload_size = payload_size;
    out.push_back(texture);
  }
  return out;
}

inline u8 BasePS3Format(u8 raw)
{
  return static_cast<u8>(raw & ~0x20u);
}

inline bool PlausiblePS3Descriptor(const std::array<u8, 24>& d)
{
  const u8 base = BasePS3Format(d[0]);
  if (base != 0x86 && base != 0x87 && base != 0x88)
    return false;
  if (!d[1] || d[1] > 16)
    return false;
  const u32 width = (u32(d[8]) << 8) | d[9];
  const u32 height = (u32(d[10]) << 8) | d[11];
  if (!width || !height || width > 8192 || height > 8192)
    return false;
  // EA's PS3 GTF/RSX descriptors seen in TPK/SSH/CPT consistently use this remap.
  if (d[6] != 0xAA || d[7] != 0xE4)
    return false;
  return true;
}

inline std::vector<PS3Record> ParsePS3CPT(std::span<const u8> cpt, std::uintmax_t rsx_size)
{
  std::vector<PS3Record> out;
  std::unordered_set<u64> seen;

  // PS3 CPT metadata uses the same 48-byte RSX resource-record shape as the
  // current TPK parser: size@+4, rsx offset@+8, 24-byte texture descriptor@+16.
  // Scan rather than depending on a platform-specific CPT header/pointer table.
  for (std::size_t pos = 0; pos + 40 <= cpt.size(); pos += 4)
  {
    const u32 size = BE32(cpt.data() + pos + 4);
    const u32 raw_offset = BE32(cpt.data() + pos + 8);

    // CPT RSX references carry a low-bit flag in the address. The actual RSX
    // payload is 0x80-aligned; treating raw_offset as a byte address shifts BC
    // blocks by one byte (the v7 logs showed ...01 / ...81) and corrupts both
    // the runtime texture and its fingerprint.
    if (!size || !raw_offset)
      continue;
    if ((raw_offset & 0x7Fu) > 1u)
      continue;
    const u32 offset = raw_offset & ~1u;
    if (!offset || (offset & 0x7Fu) != 0 || offset > rsx_size ||
        size > rsx_size - offset)
      continue;

    std::array<u8, 24> descriptor{};
    std::copy_n(cpt.data() + pos + 16, descriptor.size(), descriptor.begin());
    if (!PlausiblePS3Descriptor(descriptor))
      continue;

    const u64 identity = (u64(offset) << 32) | size;
    if (!seen.insert(identity).second)
      continue;

    PS3Record record;
    record.offset = offset;
    record.size = size;
    record.descriptor = descriptor;
    record.metadata_offset = pos;
    out.push_back(record);
  }

  std::sort(out.begin(), out.end(),
            [](const PS3Record& a, const PS3Record& b) {
              return a.metadata_offset < b.metadata_offset;
            });
  return out;
}

struct Fingerprint
{
  static constexpr std::size_t kSide = 12;
  std::array<float, kSide * kSide * 4> samples{};
  float contrast = 0.0f;
  bool valid = false;
};

inline Fingerprint MakeFingerprint(const u8* rgba, u32 width, u32 height)
{
  Fingerprint fp;
  if (!rgba || !width || !height)
    return fp;

  float lum_sum = 0.0f;
  float lum2_sum = 0.0f;
  for (std::size_t gy = 0; gy < Fingerprint::kSide; ++gy)
  {
    const u32 y = std::min<u32>(height - 1,
        static_cast<u32>((gy * 2 + 1) * std::uint64_t(height) /
                         (Fingerprint::kSide * 2)));
    for (std::size_t gx = 0; gx < Fingerprint::kSide; ++gx)
    {
      const u32 x = std::min<u32>(width - 1,
          static_cast<u32>((gx * 2 + 1) * std::uint64_t(width) /
                           (Fingerprint::kSide * 2)));
      const u8* px = rgba + (std::size_t(y) * width + x) * 4u;
      const std::size_t base = (gy * Fingerprint::kSide + gx) * 4u;
      const float r = px[0] / 255.0f;
      const float g = px[1] / 255.0f;
      const float b = px[2] / 255.0f;
      const float a = px[3] / 255.0f;
      fp.samples[base + 0] = r;
      fp.samples[base + 1] = g;
      fp.samples[base + 2] = b;
      fp.samples[base + 3] = a;
      const float lum = r * 0.299f + g * 0.587f + b * 0.114f;
      lum_sum += lum;
      lum2_sum += lum * lum;
    }
  }

  const float n = static_cast<float>(Fingerprint::kSide * Fingerprint::kSide);
  const float mean = lum_sum / n;
  fp.contrast = std::sqrt(std::max(0.0f, lum2_sum / n - mean * mean));
  fp.valid = true;
  return fp;
}

inline float FingerprintDistance(const Fingerprint& a, const Fingerprint& b)
{
  if (!a.valid || !b.valid)
    return std::numeric_limits<float>::infinity();

  float sum = 0.0f;
  for (std::size_t i = 0; i < a.samples.size(); i += 4)
  {
    sum += std::abs(a.samples[i + 0] - b.samples[i + 0]);
    sum += std::abs(a.samples[i + 1] - b.samples[i + 1]);
    sum += std::abs(a.samples[i + 2] - b.samples[i + 2]);
    // Alpha matters, but less than authored diffuse colour.
    sum += 0.35f * std::abs(a.samples[i + 3] - b.samples[i + 3]);
  }
  return sum / static_cast<float>(Fingerprint::kSide * Fingerprint::kSide * 3.35f);
}

inline bool CompatibleDimensions(const GCTexture& gc, const PS3Record& ps3)
{
  const u32 pw = ps3.Width();
  const u32 ph = ps3.Height();
  if (!pw || !ph || pw < gc.width || ph < gc.height)
    return false;

  const std::uint64_t lhs = std::uint64_t(gc.width) * ph;
  const std::uint64_t rhs = std::uint64_t(pw) * gc.height;
  const std::uint64_t hi = std::max(lhs, rhs);
  const std::uint64_t lo = std::min(lhs, rhs);
  // Remaster textures normally retain the same UV aspect. Small padding is OK.
  if (!lo || hi - lo > hi * 5u / 100u)
    return false;

  const float sx = static_cast<float>(pw) / static_cast<float>(gc.width);
  const float sy = static_cast<float>(ph) / static_cast<float>(gc.height);
  if (sx < 0.99f || sy < 0.99f || sx > 4.05f || sy > 4.05f)
    return false;

  // Verified Frontline remaster world textures use an isotropic 1x/2x/4x
  // scale. 8x matches seen in v7 were false positives (for example 16x32 ->
  // 128x256).
  if (std::abs(sx - sy) > std::max(sx, sy) * 0.02f)
    return false;

  auto near_scale = [](float value) {
    return std::abs(value - 1.0f) <= 0.03f ||
           std::abs(value - 2.0f) <= 0.05f ||
           std::abs(value - 4.0f) <= 0.08f;
  };
  return near_scale(sx) && near_scale(sy);
}

inline std::vector<u8> MakeGTF(const PS3Record& record, std::span<const u8> payload)
{
  if (!record.size || payload.size() != record.size ||
      !PlausiblePS3Descriptor(record.descriptor))
    return {};
  std::vector<u8> gtf(48 + payload.size());
  WriteBE32(&gtf, 0, 0x02010100u);
  WriteBE32(&gtf, 4, record.size);
  WriteBE32(&gtf, 8, 1u);
  WriteBE32(&gtf, 16, 20u);
  WriteBE32(&gtf, 20, record.size);
  std::copy(record.descriptor.begin(), record.descriptor.end(), gtf.begin() + 24);
  std::copy(payload.begin(), payload.end(), gtf.begin() + 48);
  return gtf;
}

inline Fingerprint DecodeGCFingerprint(std::span<const u8> cpt, const GCTexture& texture)
{
  // CPT world payloads observed in Frontline are overwhelmingly CMPR. Support
  // every non-paletted GX format here; indexed formats need their CPT TLUT and
  // are conservatively left GameCube-native for now.
  const auto format = static_cast<TextureFormat>(texture.format);
  if (IsColorIndexed(format) || texture.payload_offset > cpt.size() ||
      texture.payload_size > cpt.size() - texture.payload_offset)
    return {};

  std::vector<u8> rgba(std::size_t(texture.width) * texture.height * 4u);
  TexDecoder_Decode(rgba.data(), cpt.data() + texture.payload_offset,
                    static_cast<int>(texture.width), static_cast<int>(texture.height),
                    format, nullptr, TLUTFormat::IA8);
  return MakeFingerprint(rgba.data(), texture.width, texture.height);
}

inline Fingerprint DecodePS3Fingerprint(const PS3RemasterAssets::AssetInfo& rsx,
                                        const PS3Record& record)
{
  const auto payload = PS3RemasterAssets::ReadRange(rsx, record.offset, record.size);
  if (payload.size() != record.size)
    return {};
  const auto gtf = MakeGTF(record, payload);
  if (gtf.empty())
    return {};
  std::vector<PS3TextureDecoder::Level> levels;
  if (!PS3TextureDecoder::Decode(gtf, &levels) || levels.empty())
    return {};
  const auto& base = levels.front();
  if (base.rgba.size() < std::size_t(base.width) * base.height * 4u)
    return {};
  return MakeFingerprint(base.rgba.data(), base.width, base.height);
}

struct GCSource
{
  GCTexture texture;
  Fingerprint fingerprint;
  std::string chunk;
};

struct PS3Source
{
  PS3Record record;
  Fingerprint fingerprint;
  std::string chunk;
};

inline float MatchScore(const GCSource& gc, const PS3Source& ps3)
{
  if (!CompatibleDimensions(gc.texture, ps3.record) ||
      !gc.fingerprint.valid || !ps3.fingerprint.valid)
    return std::numeric_limits<float>::infinity();

  float score = FingerprintDistance(gc.fingerprint, ps3.fingerprint);

  // Reward scale patterns commonly used by the PS3 remaster (2x/4x), without
  // making scale alone strong enough to select a wrong texture.
  const float sx = static_cast<float>(ps3.record.Width()) / gc.texture.width;
  const float sy = static_cast<float>(ps3.record.Height()) / gc.texture.height;
  const float scale_shape = std::abs(std::log2(std::max(0.001f, sx / sy)));
  score += scale_shape * 0.08f;

  auto near_pow2 = [](float value) {
    const float p = std::round(std::log2(std::max(value, 0.001f)));
    return std::abs(value - std::pow(2.0f, p));
  };
  score += (near_pow2(sx) + near_pow2(sy)) * 0.015f;
  return score;
}

inline bool AcceptMutualMatch(float best, float second,
                             float reverse_best, float reverse_second)
{
  if (!std::isfinite(best) || !std::isfinite(reverse_best))
    return false;

  const float margin = std::isfinite(second) ? second - best : 1.0f;
  const float reverse_margin =
      std::isfinite(reverse_second) ? reverse_second - reverse_best : 1.0f;

  // After stripping the CPT pointer flag, real pairs should agree much more
  // closely. Require mutual nearest-neighbour evidence and strong separation
  // instead of the permissive v7 one-way threshold.
  if (best <= 0.025f && reverse_best <= 0.025f)
    return true;
  if (best <= 0.120f && reverse_best <= 0.120f &&
      margin >= 0.025f && reverse_margin >= 0.025f)
    return true;
  return best <= 0.180f && reverse_best <= 0.180f &&
         margin >= 0.080f && reverse_margin >= 0.080f;
}

inline const PS3RemasterAssets::AssetInfo* FindRSX(std::string_view level)
{
  const auto underscore = level.find('_');
  if (underscore == std::string_view::npos || underscore == 0)
    return nullptr;
  const std::string scope = "data/" + std::string(level.substr(0, underscore)) + "/" +
                            std::string(level) + "/";

  if (const auto* exact = PS3RemasterAssets::FindByRelativePath(scope + "rsx.viv"))
    return exact;

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    if (Lower(asset.filename) != "rsx.viv")
      continue;
    if (Normalize(asset.relative_path).find(scope) != std::string::npos)
      return &asset;
  }
  return nullptr;
}

inline std::vector<const PS3RemasterAssets::AssetInfo*> FindPS3CPTs(std::string_view level)
{
  std::vector<const PS3RemasterAssets::AssetInfo*> out;
  const auto underscore = level.find('_');
  if (underscore == std::string_view::npos || underscore == 0)
    return out;
  const std::string scope = "data/" + std::string(level.substr(0, underscore)) + "/" +
                            std::string(level) + "/";
  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string filename = Lower(asset.filename);
    if (!filename.ends_with(".cpt") || filename.find("_art") == std::string::npos)
      continue;
    if (Normalize(asset.relative_path).find(scope) == std::string::npos)
      continue;
    out.push_back(&asset);
  }
  std::sort(out.begin(), out.end(), [](const auto* a, const auto* b) {
    return Lower(a->filename) < Lower(b->filename);
  });
  return out;
}

inline std::shared_ptr<Catalog> BuildCatalog(std::string level)
{
  auto catalog = std::make_shared<Catalog>();
  catalog->generation = PS3RemasterAssets::GetIndexGeneration();
  catalog->level = Lower(std::move(level));

  if (!Enabled() || !PS3RemasterAssets::IsReady() || catalog->level.empty())
    return catalog;

  const auto* rsx = FindRSX(catalog->level);
  if (!rsx)
  {
    std::fprintf(stderr, "[moh-ps3-cpt] no rsx.viv for level=%s -> GC fallback\n",
                 catalog->level.c_str());
    return catalog;
  }
  catalog->rsx_path = rsx->relative_path;

  const auto gc_viv_path = ResolveGCLevelVIV(catalog->level);
  std::vector<u8> gc_viv;
  if (gc_viv_path.empty() || !ReadFile(gc_viv_path, &gc_viv))
  {
    std::fprintf(stderr,
                 "[moh-ps3-cpt] GC level.viv missing for level=%s "
                 "(set MOH_GC_FILES if needed) -> GC fallback\n",
                 catalog->level.c_str());
    return catalog;
  }

  auto gc_blobs = ExtractGCCPTs(gc_viv);
  catalog->gc_chunks = gc_blobs.size();
  std::vector<GCSource> gc_sources;
  std::unordered_set<u64> seen_gc_sources;
  for (const auto& blob : gc_blobs)
  {
    const auto textures = ParseGCCPT(blob.data);
    catalog->gc_textures += textures.size();
    for (const auto& texture : textures)
    {
      catalog->known.insert(IdentityKey(texture.hash, texture.width, texture.height,
                                         texture.format));
      GCSource source;
      source.texture = texture;
      source.chunk = blob.name;
      source.fingerprint = DecodeGCFingerprint(blob.data, texture);
      const u64 source_identity =
          IdentityKey(texture.hash, texture.width, texture.height, texture.format);
      if (source.fingerprint.valid && seen_gc_sources.insert(source_identity).second)
        gc_sources.push_back(std::move(source));
    }
    if (TraceEnabled())
      std::fprintf(stderr, "[moh-ps3-cpt] GC chunk=%s textures=%zu\n",
                   blob.name.c_str(), textures.size());
  }

  const auto ps3_assets = FindPS3CPTs(catalog->level);
  catalog->ps3_chunks = ps3_assets.size();
  std::vector<PS3Source> ps3_sources;
  std::unordered_set<u64> seen_ps3_ranges;
  for (const auto* ps3_asset : ps3_assets)
  {
    const auto bytes = PS3RemasterAssets::ReadBinary(*ps3_asset);
    const auto records = ParsePS3CPT(bytes, rsx->size);
    catalog->ps3_records += records.size();
    std::size_t decoded_records = 0;
    for (const auto& record : records)
    {
      const u64 range_identity = (u64(record.offset) << 32) | record.size;
      if (!seen_ps3_ranges.insert(range_identity).second)
        continue;

      PS3Source source;
      source.record = record;
      source.chunk = Lower(ps3_asset->filename);
      source.fingerprint = DecodePS3Fingerprint(*rsx, record);
      if (source.fingerprint.valid)
      {
        ++decoded_records;
        ps3_sources.push_back(std::move(source));
      }
    }
    if (TraceEnabled())
      std::fprintf(stderr,
                   "[moh-ps3-cpt] PS3 chunk=%s records=%zu decoded=%zu\n",
                   Lower(ps3_asset->filename).c_str(), records.size(), decoded_records);
  }

  // The GameCube often has one *_ART.cpt while PS3 splits equivalent resources
  // across *_ART_cN.cpt. Match across the complete level, but only accept
  // mutual nearest neighbours. This makes the mapping one-to-one and prevents
  // several unrelated GC textures from all selecting the same RSX range.
  const std::size_t no_index = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> gc_best_index(gc_sources.size(), no_index);
  std::vector<float> gc_best(gc_sources.size(), std::numeric_limits<float>::infinity());
  std::vector<float> gc_second(gc_sources.size(), std::numeric_limits<float>::infinity());
  std::vector<std::size_t> ps3_best_index(ps3_sources.size(), no_index);
  std::vector<float> ps3_best(ps3_sources.size(), std::numeric_limits<float>::infinity());
  std::vector<float> ps3_second(ps3_sources.size(), std::numeric_limits<float>::infinity());

  for (std::size_t gi = 0; gi < gc_sources.size(); ++gi)
  {
    for (std::size_t pi = 0; pi < ps3_sources.size(); ++pi)
    {
      const float score = MatchScore(gc_sources[gi], ps3_sources[pi]);
      if (!std::isfinite(score))
        continue;

      if (score < gc_best[gi])
      {
        gc_second[gi] = gc_best[gi];
        gc_best[gi] = score;
        gc_best_index[gi] = pi;
      }
      else if (score < gc_second[gi])
      {
        gc_second[gi] = score;
      }

      if (score < ps3_best[pi])
      {
        ps3_second[pi] = ps3_best[pi];
        ps3_best[pi] = score;
        ps3_best_index[pi] = gi;
      }
      else if (score < ps3_second[pi])
      {
        ps3_second[pi] = score;
      }
    }
  }

  unsigned reject_logs = 0;
  std::size_t non_mutual = 0;
  for (std::size_t gi = 0; gi < gc_sources.size(); ++gi)
  {
    const auto& gc = gc_sources[gi];
    const std::size_t pi = gc_best_index[gi];
    if (pi == no_index)
      continue;

    const auto& best_source = ps3_sources[pi];
    const float best = gc_best[gi];
    const float second = gc_second[gi];
    const float margin = std::isfinite(second) ? second - best : 1.0f;
    const float reverse_best = ps3_best[pi];
    const float reverse_second = ps3_second[pi];
    const float reverse_margin =
        std::isfinite(reverse_second) ? reverse_second - reverse_best : 1.0f;

    const bool mutual = ps3_best_index[pi] == gi;
    if (!mutual ||
        !AcceptMutualMatch(best, second, reverse_best, reverse_second))
    {
      if (!mutual)
        ++non_mutual;
      if (TraceEnabled() && reject_logs++ < 96)
      {
        std::fprintf(stderr,
                     "[moh-ps3-cpt] MATCH REJECT: level=%s chunk=%s "
                     "GC=%ux%u fmt=%u hash=%016llX best=%.4f second=%.4f "
                     "margin=%.4f reverse=%.4f reverse-margin=%.4f mutual=%d "
                     "-> GC fallback\n",
                     catalog->level.c_str(), gc.chunk.c_str(), gc.texture.width,
                     gc.texture.height, gc.texture.format,
                     static_cast<unsigned long long>(gc.texture.hash), best, second,
                     margin, reverse_best, reverse_margin, mutual ? 1 : 0);
      }
      continue;
    }

    Entry entry;
    entry.gc_hash = gc.texture.hash;
    entry.gc_width = gc.texture.width;
    entry.gc_height = gc.texture.height;
    entry.gc_format = gc.texture.format;
    entry.ps3_offset = best_source.record.offset;
    entry.ps3_size = best_source.record.size;
    entry.descriptor = best_source.record.descriptor;
    entry.level = catalog->level;
    entry.chunk = gc.chunk + " -> " + best_source.chunk;
    entry.rsx_path = catalog->rsx_path;
    entry.match_score = best;
    entry.match_margin = std::min(margin, reverse_margin);
    catalog->entries.push_back(std::move(entry));
  }

  if (TraceEnabled())
    std::fprintf(stderr,
                 "[moh-ps3-cpt] strict mutual mapping: level=%s unique-GC=%zu "
                 "unique-PS3=%zu non-mutual=%zu accepted=%zu\n",
                 catalog->level.c_str(), gc_sources.size(), ps3_sources.size(),
                 non_mutual, catalog->entries.size());

  std::sort(catalog->entries.begin(), catalog->entries.end(),
            [](const Entry& a, const Entry& b) {
              if (a.gc_hash != b.gc_hash) return a.gc_hash < b.gc_hash;
              if (a.gc_width != b.gc_width) return a.gc_width < b.gc_width;
              if (a.gc_height != b.gc_height) return a.gc_height < b.gc_height;
              if (a.gc_format != b.gc_format) return a.gc_format < b.gc_format;
              if (a.ps3_offset != b.ps3_offset) return a.ps3_offset < b.ps3_offset;
              return a.ps3_size < b.ps3_size;
            });

  // Duplicate use of the exact same authored texture is safe. Keep conflicting
  // mappings duplicated so Lookup detects ambiguity and falls back to GC.
  catalog->entries.erase(
      std::unique(catalog->entries.begin(), catalog->entries.end(),
                  [](const Entry& a, const Entry& b) {
                    return a.gc_hash == b.gc_hash && a.gc_width == b.gc_width &&
                           a.gc_height == b.gc_height && a.gc_format == b.gc_format &&
                           a.ps3_offset == b.ps3_offset && a.ps3_size == b.ps3_size;
                  }),
      catalog->entries.end());

  std::fprintf(stderr,
               "[moh-ps3-cpt] RUNTIME PARSER READY: level=%s "
               "GC-chunks=%zu PS3-chunks=%zu GC-textures=%zu GC-fp=%zu "
               "PS3-records=%zu PS3-fp=%zu mapped=%zu source=%s\n",
               catalog->level.c_str(), catalog->gc_chunks, catalog->ps3_chunks,
               catalog->gc_textures, gc_sources.size(), catalog->ps3_records,
               ps3_sources.size(), catalog->entries.size(), gc_viv_path.string().c_str());
  return catalog;
}

inline std::mutex catalog_mutex;
inline std::shared_ptr<Catalog> current_catalog;

inline std::shared_ptr<Catalog> GetCatalog()
{
  if (!Enabled() || !PS3RemasterAssets::IsReady())
    return {};

  const auto generation = PS3RemasterAssets::GetIndexGeneration();
  const std::string_view raw_level = MOHFrontline::NativeAssets::GetCurrentLevel();
  if (raw_level.empty())
    return {};

  // MOH_PS3_RUNTIME_CPT_RESULT_CACHE_V1
  // The video thread asks for the same level catalog thousands of times per
  // second. Avoid allocating/lowercasing a std::string and taking the global
  // catalog mutex on every texture bind.
  thread_local std::shared_ptr<Catalog> tls_catalog;

  if (tls_catalog && tls_catalog->generation == generation &&
      tls_catalog->level.size() == raw_level.size())
  {
    bool same_level = true;
    for (std::size_t i = 0; i < raw_level.size(); ++i)
    {
      if (tls_catalog->level[i] !=
          static_cast<char>(std::tolower(static_cast<unsigned char>(raw_level[i]))))
      {
        same_level = false;
        break;
      }
    }
    if (same_level)
      return tls_catalog;
  }

  const std::string level = Lower(std::string(raw_level));

  std::scoped_lock lock(catalog_mutex);
  if (current_catalog && current_catalog->generation == generation &&
      current_catalog->level == level)
  {
    tls_catalog = current_catalog;
    return tls_catalog;
  }

  current_catalog = BuildCatalog(level);
  tls_catalog = current_catalog;
  return tls_catalog;
}

inline std::vector<Entry>::const_iterator FindFirst(const Catalog& catalog, u64 hash)
{
  return std::lower_bound(catalog.entries.begin(), catalog.entries.end(), hash,
                          [](const Entry& entry, u64 wanted) {
                            return entry.gc_hash < wanted;
                          });
}

inline bool Lookup(const TextureInfo& info, Entry* out, u64* out_hash = nullptr)
{
  if (!info.GetData() || !info.GetTextureSize() || !info.IsDataValid() ||
      info.GetTextureFormat() == TextureFormat::XFB)
    return false;

  const auto catalog = GetCatalog();
  if (!catalog || catalog->entries.empty())
    return false;

  const u64 hash = RuntimeTextureHash(info);
  if (out_hash)
    *out_hash = hash;

  const u32 width = info.GetRawWidth();
  const u32 height = info.GetRawHeight();
  const u32 format = static_cast<u32>(info.GetTextureFormat());

  auto it = FindFirst(*catalog, hash);
  const Entry* match = nullptr;
  unsigned matches = 0;
  for (; it != catalog->entries.end() && it->gc_hash == hash; ++it)
  {
    if (it->gc_width != width || it->gc_height != height || it->gc_format != format)
      continue;
    match = &*it;
    ++matches;
  }

  if (matches != 1 || !match)
  {
    if (matches > 1 && TraceEnabled())
      std::fprintf(stderr,
                   "[moh-ps3-cpt] AMBIGUOUS live identity: level=%s "
                   "GC=%ux%u fmt=%u hash=%016llX matches=%u -> GC fallback\n",
                   catalog->level.c_str(), width, height, format,
                   static_cast<unsigned long long>(hash), matches);
    return false;
  }

  if (out)
    *out = *match;
  return true;
}

inline bool Known(const TextureInfo& info)
{
  if (!info.GetData() || !info.GetTextureSize() || !info.IsDataValid())
    return false;
  const auto catalog = GetCatalog();
  if (!catalog)
    return false;
  const u64 hash = RuntimeTextureHash(info);
  return catalog->known.contains(IdentityKey(hash, info.GetRawWidth(), info.GetRawHeight(),
                                              static_cast<u32>(info.GetTextureFormat())));
}

inline std::vector<u8> MakeGTF(const Entry& entry, std::span<const u8> payload)
{
  if (!entry.ps3_size || payload.size() != entry.ps3_size ||
      !PlausiblePS3Descriptor(entry.descriptor))
    return {};

  std::vector<u8> gtf(48 + payload.size());
  WriteBE32(&gtf, 0, 0x02010100u);
  WriteBE32(&gtf, 4, entry.ps3_size);
  WriteBE32(&gtf, 8, 1u);
  WriteBE32(&gtf, 16, 20u);
  WriteBE32(&gtf, 20, entry.ps3_size);
  std::copy(entry.descriptor.begin(), entry.descriptor.end(), gtf.begin() + 24);
  std::copy(payload.begin(), payload.end(), gtf.begin() + 48);
  return gtf;
}

inline std::shared_ptr<VideoCommon::CustomTextureData>
BuildCustomTexture(const std::vector<PS3TextureDecoder::Level>& levels)
{
  if (levels.empty())
    return nullptr;
  auto result = std::make_shared<VideoCommon::CustomTextureData>();
  result->m_slices.emplace_back();
  for (const auto& source : levels)
  {
    if (!source.width || !source.height ||
        source.rgba.size() < std::size_t(source.width) * source.height * 4u)
      return nullptr;
    VideoCommon::CustomTextureData::ArraySlice::Level mip;
    mip.width = source.width;
    mip.height = source.height;
    mip.row_length = source.width;
    mip.data.reset(source.rgba.size());
    std::copy(source.rgba.begin(), source.rgba.end(), mip.data.begin());
    result->m_slices[0].m_levels.push_back(std::move(mip));
  }
  return result;
}

inline u64 ReplacementKeyFor(const Entry& entry, u64 gc_hash)
{
  constexpr u64 tag = 0x43505452554E0000ULL;  // "CPTRUN"
  u64 key = tag ^ gc_hash;
  key ^= static_cast<u64>(entry.ps3_offset) * 0x9E3779B185EBCA87ULL;
  key ^= static_cast<u64>(entry.ps3_size) * 0xC2B2AE3D27D4EB4FULL;
  key ^= static_cast<u64>(entry.descriptor[8]) << 48;
  key ^= static_cast<u64>(entry.descriptor[9]) << 40;
  key ^= static_cast<u64>(entry.descriptor[10]) << 32;
  key ^= static_cast<u64>(entry.descriptor[11]) << 24;
  return key ? key : 1;
}

/* MOH_PS3_RUNTIME_CPT_RESULT_CACHE_V1
 *
 * ReplacementKey() is the hottest runtime-CPT caller. It only needs one u64,
 * but the old path called Lookup(), performed lower_bound/scanning and copied
 * the whole Entry (including std::string fields) on every bind.
 *
 * Cache both matches and misses by exact texture hash + dimensions + format +
 * catalog identity. RuntimeTextureHash() (Round16) remains responsible for
 * validating repeated guest-memory contents.
 */
struct RuntimeReplacementKeyCacheLine
{
  const Catalog* catalog = nullptr;
  std::uint64_t generation = 0;
  u64 level_tag = 0;
  u64 texture_hash = 0;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  u64 replacement_key = 0;
  bool valid = false;
};

inline thread_local std::array<RuntimeReplacementKeyCacheLine, 2048>
    runtime_replacement_key_cache{};

inline u64 CatalogLevelTag(const Catalog& catalog)
{
  u64 hash = 0xcbf29ce484222325ULL;
  for (unsigned char c : catalog.level)
  {
    hash ^= c;
    hash *= 0x100000001b3ULL;
  }
  hash ^= catalog.generation;
  hash *= 0x100000001b3ULL;
  return hash;
}

inline u64 LookupReplacementKeyFast(const TextureInfo& info)
{
  if (!info.GetData() || !info.GetTextureSize() || !info.IsDataValid() ||
      info.GetTextureFormat() == TextureFormat::XFB)
  {
    return 0;
  }

  const auto catalog = GetCatalog();
  if (!catalog || catalog->entries.empty())
    return 0;

  const u64 texture_hash = RuntimeTextureHash(info);
  const u32 width = info.GetRawWidth();
  const u32 height = info.GetRawHeight();
  const u32 format = static_cast<u32>(info.GetTextureFormat());
  const u64 level_tag = CatalogLevelTag(*catalog);

  u64 slot_key = texture_hash;
  slot_key ^= static_cast<u64>(width) * 0x9E3779B185EBCA87ULL;
  slot_key ^= static_cast<u64>(height) * 0xC2B2AE3D27D4EB4FULL;
  slot_key ^= static_cast<u64>(format) * 0x165667B19E3779F9ULL;
  slot_key ^= static_cast<u64>(reinterpret_cast<std::uintptr_t>(catalog.get()));

  RuntimeReplacementKeyCacheLine& line =
      runtime_replacement_key_cache[slot_key & (runtime_replacement_key_cache.size() - 1)];

  if (line.valid && line.catalog == catalog.get() &&
      line.generation == catalog->generation &&
      line.level_tag == level_tag &&
      line.texture_hash == texture_hash &&
      line.width == width && line.height == height && line.format == format)
  {
    return line.replacement_key;
  }

  auto it = FindFirst(*catalog, texture_hash);
  const Entry* match = nullptr;
  unsigned matches = 0;
  for (; it != catalog->entries.end() && it->gc_hash == texture_hash; ++it)
  {
    if (it->gc_width != width || it->gc_height != height || it->gc_format != format)
      continue;
    match = &*it;
    ++matches;
  }

  u64 replacement_key = 0;
  if (matches == 1 && match)
  {
    replacement_key = ReplacementKeyFor(*match, texture_hash);
  }
  else if (matches > 1 && TraceEnabled())
  {
    std::fprintf(stderr,
                 "[moh-ps3-cpt] AMBIGUOUS fast identity: level=%s "
                 "GC=%ux%u fmt=%u hash=%016llX matches=%u -> GC fallback\n",
                 catalog->level.c_str(), width, height, format,
                 static_cast<unsigned long long>(texture_hash), matches);
  }

  line.catalog = catalog.get();
  line.generation = catalog->generation;
  line.level_tag = level_tag;
  line.texture_hash = texture_hash;
  line.width = width;
  line.height = height;
  line.format = format;
  line.replacement_key = replacement_key;
  line.valid = true;
  return replacement_key;
}

inline std::mutex decode_mutex;
inline std::unordered_map<u64, std::shared_ptr<VideoCommon::CustomTextureData>> decoded_cache;
inline std::unordered_set<u64> decode_failed;
inline std::uint64_t decode_generation = ~std::uint64_t(0);

inline std::shared_ptr<VideoCommon::CustomTextureData> Decode(const Entry& entry, u64 gc_hash)
{
  std::scoped_lock lock(decode_mutex);
  const auto generation = PS3RemasterAssets::GetIndexGeneration();
  if (decode_generation != generation)
  {
    decoded_cache.clear();
    decode_failed.clear();
    decode_generation = generation;
  }

  const u64 key = ReplacementKeyFor(entry, gc_hash);
  if (const auto it = decoded_cache.find(key); it != decoded_cache.end())
    return it->second;
  if (decode_failed.contains(key))
    return nullptr;

  const auto* rsx = PS3RemasterAssets::FindByRelativePath(entry.rsx_path);
  if (!rsx || entry.ps3_offset > rsx->size || entry.ps3_size > rsx->size - entry.ps3_offset)
  {
    decode_failed.insert(key);
    return nullptr;
  }

  const auto payload = PS3RemasterAssets::ReadRange(*rsx, entry.ps3_offset, entry.ps3_size);
  if (payload.size() != entry.ps3_size)
  {
    decode_failed.insert(key);
    return nullptr;
  }

  const auto gtf = MakeGTF(entry, payload);
  if (gtf.empty())
  {
    decode_failed.insert(key);
    return nullptr;
  }

  std::shared_ptr<VideoCommon::CustomTextureData> decoded;
  std::vector<PS3TextureDecoder::CompressedLevel> blocks;
  if (g_backend_info.bSupportsST3CTextures &&
      PS3TextureDecoder::DecodeCompressed(gtf, &blocks) && !blocks.empty())
  {
    decoded = std::make_shared<VideoCommon::CustomTextureData>();
    decoded->m_slices.emplace_back();
    for (const auto& source : blocks)
    {
      VideoCommon::CustomTextureData::ArraySlice::Level mip;
      mip.width = source.width;
      mip.height = source.height;
      mip.row_length = (source.width + 3u) & ~3u;
      mip.format = source.format == PS3TextureDecoder::BlockFormat::BC1 ? AbstractTextureFormat::DXT1 :
                   source.format == PS3TextureDecoder::BlockFormat::BC2 ? AbstractTextureFormat::DXT3 :
                                                                        AbstractTextureFormat::DXT5;
      mip.data.reset(source.blocks.size());
      std::copy(source.blocks.begin(), source.blocks.end(), mip.data.begin());
      decoded->m_slices[0].m_levels.push_back(std::move(mip));
    }
  }
  else
  {
    std::vector<PS3TextureDecoder::Level> levels;
    if (PS3TextureDecoder::Decode(gtf, &levels))
      decoded = BuildCustomTexture(levels);
  }

  if (!decoded)
  {
    decode_failed.insert(key);
    return nullptr;
  }

  decoded_cache.emplace(key, decoded);
  if (TraceEnabled())
  {
    std::fprintf(stderr,
                 "[moh-ps3-cpt] DECODE READY: level=%s chunk=%s "
                 "GC=%ux%u fmt=%u hash=%016llX -> rsx.viv+0x%08X size=%u "
                 "PS3=%ux%u fmt=0x%02X mips=%u score=%.4f margin=%.4f\n",
                 entry.level.c_str(), entry.chunk.c_str(), entry.gc_width, entry.gc_height,
                 entry.gc_format, static_cast<unsigned long long>(gc_hash), entry.ps3_offset,
                 entry.ps3_size,
                 (u32(entry.descriptor[8]) << 8) | entry.descriptor[9],
                 (u32(entry.descriptor[10]) << 8) | entry.descriptor[11],
                 entry.descriptor[0], entry.descriptor[1], entry.match_score, entry.match_margin);
  }
  return decoded;
}


struct CPTDrawTextureIdentity
{
  std::string level;
  std::string source;
  u32 offset = 0;
  u32 size = 0;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  u32 mips = 0;
  std::array<u8, 24> descriptor{};
};

inline bool ParseUnsignedAfter(std::string_view text, std::string_view token, u32* out)
{
  if (!out)
    return false;
  const std::size_t pos = text.find(token);
  if (pos == std::string_view::npos)
    return false;
  std::size_t i = pos + token.size();
  if (i >= text.size() || text[i] < '0' || text[i] > '9')
    return false;
  u64 value = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9')
  {
    value = value * 10 + static_cast<unsigned>(text[i] - '0');
    if (value > std::numeric_limits<u32>::max())
      return false;
    ++i;
  }
  *out = static_cast<u32>(value);
  return true;
}

inline int HexNibble(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool ParseDescriptorHex(std::string_view text, std::array<u8, 24>* out)
{
  if (!out)
    return false;
  const std::size_t pos = text.find("desc:");
  if (pos == std::string_view::npos)
    return false;
  const std::string_view hex = text.substr(pos + 5);
  if (hex.size() < out->size() * 2)
    return false;
  for (std::size_t i = 0; i < out->size(); ++i)
  {
    const int hi = HexNibble(hex[i * 2]);
    const int lo = HexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0)
      return false;
    (*out)[i] = static_cast<u8>((hi << 4) | lo);
  }
  return true;
}

inline bool SameTexture(const CPTDrawTextureIdentity& a, const CPTDrawTextureIdentity& b)
{
  return a.offset == b.offset && a.size == b.size && a.width == b.width &&
         a.height == b.height && a.format == b.format && a.mips == b.mips &&
         a.descriptor == b.descriptor;
}

inline std::optional<CPTDrawTextureIdentity> ResolveExactCurrentDrawTexture()
{
  const auto& draw = PS3MeshPort::CurrentStaticDraw();
  if (!draw || !draw.mesh || !draw.submesh)
    return std::nullopt;

  const std::string& source = draw.mesh->source_name;
  if (source.find(".cpt#cpt-") == std::string::npos ||
      source.find("#cpt-full-level") != std::string::npos)
    return std::nullopt;

  const std::string level = MOHFrontline::NativeAssets::GetCurrentLevel();
  if (level.empty())
    return std::nullopt;

  std::unordered_set<u32> pack_members;
  std::unordered_set<u32> textured_members;
  std::optional<CPTDrawTextureIdentity> chosen;
  bool conflict = false;

  for (const std::string& owned_hint : draw.submesh->material_hints)
  {
    const std::string_view hint(owned_hint);
    if (hint.starts_with("@cpt-pack-span=member:"))
    {
      u32 member = 0;
      if (ParseUnsignedAfter(hint, "member:", &member))
        pack_members.insert(member);
      continue;
    }

    const std::size_t marker = hint.find("@cpt-tex=");
    if (marker == std::string_view::npos)
      continue;
    const std::string_view tex = hint.substr(marker + 9);

    u32 slot = 0;
    if (!ParseUnsignedAfter(tex, "slot:", &slot) || slot != 0)
      continue;

    CPTDrawTextureIdentity candidate;
    candidate.level = level;
    candidate.source = source;
    if (!ParseUnsignedAfter(tex, "offset:", &candidate.offset) ||
        !ParseUnsignedAfter(tex, "size:", &candidate.size) ||
        !ParseUnsignedAfter(tex, "format:", &candidate.format) ||
        !ParseUnsignedAfter(tex, "mips:", &candidate.mips) ||
        !ParseUnsignedAfter(tex, "width:", &candidate.width) ||
        !ParseUnsignedAfter(tex, "height:", &candidate.height) ||
        !ParseDescriptorHex(tex, &candidate.descriptor) ||
        !candidate.size || !candidate.width || !candidate.height)
    {
      conflict = true;
      break;
    }

    const std::size_t member_marker = hint.find("@cpt-pack-member=");
    if (member_marker != std::string_view::npos)
    {
      u32 member = 0;
      if (!ParseUnsignedAfter(hint, "@cpt-pack-member=", &member))
      {
        conflict = true;
        break;
      }
      textured_members.insert(member);
    }

    if (!chosen)
      chosen = candidate;
    else if (!SameTexture(*chosen, candidate))
    {
      conflict = true;
      break;
    }
  }

  if (conflict || !chosen)
    return std::nullopt;
  if (!pack_members.empty() && textured_members != pack_members)
    return std::nullopt;
  return chosen;
}

inline u64 ExactDrawTextureKey(const CPTDrawTextureIdentity& identity)
{
  constexpr u64 tag = 0x4350544D41540000ULL;  // "CPTMAT"
  u64 key = tag;
  key ^= static_cast<u64>(identity.offset) * 0x9E3779B185EBCA87ULL;
  key ^= static_cast<u64>(identity.size) * 0xC2B2AE3D27D4EB4FULL;
  key ^= static_cast<u64>(identity.width) << 32;
  key ^= static_cast<u64>(identity.height) << 16;
  for (const u8 byte : identity.descriptor)
  {
    key ^= byte;
    key *= 0x100000001b3ULL;
  }
  return key ? key : 1;
}

inline std::shared_ptr<VideoCommon::CustomTextureData>
DecodeExactCurrentDrawTexture(const CPTDrawTextureIdentity& identity)
{
  const u64 key = ExactDrawTextureKey(identity);
  static std::mutex cache_mutex;
  static std::unordered_map<u64, std::shared_ptr<VideoCommon::CustomTextureData>> cache;
  {
    std::scoped_lock lock(cache_mutex);
    if (const auto it = cache.find(key); it != cache.end())
      return it->second;
  }

  const auto* rsx = FindRSX(identity.level);
  if (!rsx)
    return nullptr;
  const std::vector<u8> payload =
      PS3RemasterAssets::ReadRange(*rsx, identity.offset, identity.size);
  if (payload.size() != identity.size)
    return nullptr;

  PS3Record record;
  record.offset = identity.offset;
  record.size = identity.size;
  record.descriptor = identity.descriptor;
  const std::vector<u8> gtf = MakeGTF(record, payload);
  if (gtf.empty())
    return nullptr;

  std::vector<PS3TextureDecoder::Level> levels;
  if (!PS3TextureDecoder::Decode(gtf, &levels) || levels.empty())
    return nullptr;
  auto decoded = BuildCustomTexture(levels);
  if (!decoded)
    return nullptr;

  {
    std::scoped_lock lock(cache_mutex);
    cache.emplace(key, decoded);
  }
  return decoded;
}
}  // namespace detail

inline u64 ReplacementKey(const TextureInfo& info)
{
  if (info.GetStage() == 0)
  {
    if (const auto exact = detail::ResolveExactCurrentDrawTexture())
      return detail::ExactDrawTextureKey(*exact);
  }
  return detail::LookupReplacementKeyFast(info);
}

inline bool IsKnownWorldTexture(const TextureInfo& info)
{
  return detail::Known(info);
}

inline std::shared_ptr<VideoCommon::CustomTextureData> Find(const TextureInfo& info)
{
  if (info.GetStage() == 0)
  {
    if (const auto exact = detail::ResolveExactCurrentDrawTexture())
    {
      auto decoded = detail::DecodeExactCurrentDrawTexture(*exact);
      if (decoded)
      {
        static std::mutex log_mutex;
        static std::unordered_set<u64> logged;
        const u64 key = detail::ExactDrawTextureKey(*exact);
        std::scoped_lock lock(log_mutex);
        if (logged.insert(key).second)
        {
          std::fprintf(stderr,
                       "[moh-ps3-material] CPT DRAW EXACT: level=%s source=%s stage=0 "
                       "GC=%ux%u fmt=%u -> rsx.viv+0x%08X size=%u "
                       "PS3=%ux%u fmt=0x%02X mips=%u\n",
                       exact->level.c_str(), exact->source.c_str(),
                       info.GetRawWidth(), info.GetRawHeight(),
                       static_cast<unsigned>(info.GetTextureFormat()),
                       exact->offset, exact->size, exact->width, exact->height,
                       exact->format, exact->mips);
        }
        return decoded;
      }
    }
  }

  detail::Entry entry;
  u64 hash = 0;
  if (!detail::Lookup(info, &entry, &hash))
    return nullptr;
  return detail::Decode(entry, hash);
}

inline void NotifyUploaded(const TextureInfo& info)
{
  if (!detail::TraceEnabled())
    return;
  detail::Entry entry;
  u64 hash = 0;
  if (!detail::Lookup(info, &entry, &hash))
    return;
  static std::mutex mutex;
  static std::unordered_set<u64> logged;
  const u64 key = detail::ReplacementKeyFor(entry, hash);
  std::scoped_lock lock(mutex);
  if (!logged.insert(key).second)
    return;
  std::fprintf(stderr,
               "[moh-ps3-cpt] UPLOAD READY: level=%s chunk=%s "
               "GC=%ux%u fmt=%u hash=%016llX -> PS3=%ux%u fmt=0x%02X mips=%u "
               "rsx.viv+0x%08X\n",
               entry.level.c_str(), entry.chunk.c_str(), entry.gc_width, entry.gc_height,
               entry.gc_format, static_cast<unsigned long long>(hash),
               (u32(entry.descriptor[8]) << 8) | entry.descriptor[9],
               (u32(entry.descriptor[10]) << 8) | entry.descriptor[11],
               entry.descriptor[0], entry.descriptor[1], entry.ps3_offset);
}
}  // namespace PS3WorldCPTRuntime
