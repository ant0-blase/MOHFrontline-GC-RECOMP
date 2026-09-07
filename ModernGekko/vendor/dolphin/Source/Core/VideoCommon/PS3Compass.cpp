#include "VideoCommon/PS3Compass.h"
#include "VideoCommon/PS3NamedSky.h"
#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/Materials/PS3MaterialCatalog.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <mutex>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Common/Hash.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/MohPcLayer.h"
#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/PS3TextureDecoder.h"
#include "VideoCommon/TextureDecoder.h"
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
std::unordered_set<u32> named_sky_addresses;
std::unordered_set<u32> uploaded_named_sky;
std::unordered_set<u32> named_sky_invalidations;
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


std::string RequestedPS3Locale()
{
  const char* value =
      std::getenv("MOH_PS3_LOCALE");

  if (!value || !*value)
    return "usa";

  std::string locale = Lower(value);

  if (locale == "us" ||
      locale == "en" ||
      locale == "eng" ||
      locale == "english")
  {
    return "usa";
  }

  if (locale == "fr" || locale == "fra")
    return "french";

  if (locale == "de" || locale == "ger")
    return "german";

  if (locale == "es" || locale == "spa")
    return "spanish";

  if (locale == "it" || locale == "ita")
    return "italian";

  return locale;
}

bool ContainsLocaleSegment(
    std::string_view path,
    std::string_view locale)
{
  const std::string needle =
      "/_" + std::string(locale) + "/";

  return path.find(needle) !=
         std::string_view::npos;
}

bool HasAnyForeignLocale(
    std::string_view path,
    std::string_view wanted)
{
  static constexpr std::string_view locales[] = {
      "french", "german", "spanish", "italian",
      "japanese", "korean", "russian", "polish",
      "dutch", "portuguese",
  };

  for (const auto locale : locales)
  {
    if (locale != wanted &&
        ContainsLocaleSegment(path, locale))
    {
      return true;
    }
  }

  return false;
}

int AssetLocalePriority(
    std::string_view relative_path)
{
  const std::string path =
      Normalize(std::string(relative_path));

  const std::string wanted =
      RequestedPS3Locale();

  if (wanted == "usa")
  {
    if (ContainsLocaleSegment(path, "usa") ||
        ContainsLocaleSegment(path, "english"))
    {
      return 4;
    }

    if (HasAnyForeignLocale(path, "usa"))
      return 0;

    // Generic shell/shell.viv is the English/USA fallback.
    return 3;
  }

  if (ContainsLocaleSegment(path, wanted))
    return 4;

  if (HasAnyForeignLocale(path, wanted))
    return 0;

  return 2;
}

std::string SemanticStem(std::string_view path)
{
  std::string key = StemKey(path);

  auto erase_all =
      [&](std::string_view token)
      {
        for (;;)
        {
          const auto pos = key.find(token);

          if (pos == std::string::npos)
            break;

          key.erase(pos, token.size());
        }
      };

  erase_all("master");
  erase_all("txt");
  erase_all("highlight");

  if (key.ends_with("hl"))
    key.resize(key.size() - 2);

  if (key.starts_with("mp") &&
      key.size() > 2 &&
      std::isdigit(
          static_cast<unsigned char>(key[2])))
  {
    key = "multiplayer" + key.substr(2);
  }

  return key;
}

std::size_t LocaleAwareScore(
    const PS3RemasterAssets::AssetInfo& asset,
    std::string_view wanted_path)
{
  const int locale =
      AssetLocalePriority(asset.relative_path);

  if (!locale)
    return 0;

  return
      static_cast<std::size_t>(locale) *
          1000000u +
      CommonSuffixScore(
          wanted_path,
          Normalize(asset.relative_path));
}

const PS3RemasterAssets::AssetInfo* FindBestAsset(std::string_view guest_name)
{
  if (!PS3RemasterAssets::IsReady())
    return nullptr;

  // v14.7 FIXED2: exact named GameCube sky -> exact PS3 sky.
  // No fingerprint/fuzzy matching for named sky faces.
  const std::string guest_file = Filename(guest_name);

  static constexpr std::array<std::string_view, 6> sky_suffixes = {
      "_fr.gsh", "_lf.gsh", "_bk.gsh",
      "_rt.gsh", "_up.gsh", "_dn.gsh",
  };

  bool named_sky_face = false;

  for (const auto suffix : sky_suffixes)
  {
    if (guest_file.ends_with(suffix))
    {
      named_sky_face = true;
      break;
    }
  }

  if (named_sky_face)
  {
    std::string level;

    if (guest_file.starts_with("level1test_"))
    {
      level = "1_1";
    }
    else if (guest_file.size() >= 7 &&
             std::isdigit(static_cast<unsigned char>(guest_file[0])) &&
             guest_file[1] == '_' &&
             std::isdigit(static_cast<unsigned char>(guest_file[2])) &&
             guest_file[3] == '_')
    {
      level = guest_file.substr(0, 3);
    }

    if (!level.empty())
    {
      std::string ps3_file = guest_file;
      ps3_file.replace(ps3_file.size() - 4, 4, ".ssh");

      const std::string relative =
          "data/" + level.substr(0, 1) + "/" +
          level + "/" + ps3_file;

      if (const auto* exact =
              PS3RemasterAssets::FindByRelativePath(relative))
      {
        std::fprintf(
            stderr,
            "[moh-ps3-sky] EXACT GSH->SSH: %s -> %s\n",
            guest_file.c_str(),
            relative.c_str());

        return exact;
      }

      std::fprintf(
          stderr,
          "[moh-ps3-sky] EXACT GSH->SSH missing: %s -> %s (keeping GC)\n",
          guest_file.c_str(),
          relative.c_str());

      return nullptr;
    }
  }

  // Native-PC resolver stays first: exact level/locale-aware identities should
  // win whenever the new subsystem knows the asset.
  const auto native_match =
      MOHFrontline::NativeAssets::Resolve(
          guest_name,
          MOHFrontline::NativeAssets::Domain::Texture);

  if (native_match.asset)
  {
    static unsigned native_logs = 0;

    if (native_logs < 160)
    {
      ++native_logs;

      std::fprintf(
          stderr,
          "[moh-native-assets] texture: %.*s -> %s score=%d\n",
          static_cast<int>(
              guest_name.size()),
          guest_name.data(),
          native_match.normalized_path.c_str(),
          native_match.score);
    }

    return native_match.asset;
  }

  // IMPORTANT:
  // The native resolver is intentionally conservative. A native miss must NOT
  // disable the mature v8-v11 frontend/HUD matcher. Codex previously returned
  // nullptr here, which regressed PS3 pause/controller buttons and other 2D
  // assets (for example X_button / Tri_button) even though those assets still
  // exist in the PS3 pack.
  //
  // MOH_PS3_NATIVE_STRICT=1 is available only for resolver-development tests.
  if (const char* strict =
          std::getenv("MOH_PS3_NATIVE_STRICT");
      strict && *strict)
  {
    const std::string value =
        Lower(strict);

    if (value == "1" ||
        value == "true" ||
        value == "on" ||
        value == "yes")
    {
      return nullptr;
    }
  }

  static bool legacy_fallback_logged = false;

  if (!legacy_fallback_logged)
  {
    legacy_fallback_logged = true;

    std::fprintf(
        stderr,
        "[moh-native-assets] compatibility texture resolver enabled "
        "after native misses (set MOH_PS3_NATIVE_STRICT=1 to test strict mode)\n");
  }

  static bool locale_logged = false;

  if (!locale_logged)
  {
    locale_logged = true;

    std::fprintf(
        stderr,
        "[moh-ps3-texture] locale policy: %s "
        "(foreign localized assets rejected)\n",
        RequestedPS3Locale().c_str());
  }

  const std::string wanted_filename =
      CanonicalPS3Filename(guest_name);

  if (!wanted_filename.ends_with(".ssh"))
    return nullptr;

  const std::string wanted_path =
      CanonicalGuestPath(guest_name);

  const std::string wanted_stem =
      StemKey(wanted_filename);

  const std::string wanted_semantic =
      SemanticStem(wanted_filename);

  const PS3RemasterAssets::AssetInfo* best =
      nullptr;

  std::size_t best_score = 0;

  auto consider =
      [&](const PS3RemasterAssets::AssetInfo& asset)
      {
        const std::size_t score =
            LocaleAwareScore(
                asset,
                wanted_path);

        if (!score)
          return;

        if (!best ||
            score > best_score)
        {
          best =
              &asset;

          best_score =
              score;
        }
      };

  // 1) Exact basename. This restores deterministic PS3 frontend assets such as
  //    data/pausescreen/tri_button.ssh without weakening native level scoping.
  for (const auto& asset :
       PS3RemasterAssets::GetAssets())
  {
    if (Lower(asset.filename) ==
        wanted_filename)
    {
      consider(asset);
    }
  }

  if (best)
  {
    static unsigned exact_fallback_logs = 0;

    if (exact_fallback_logs < 64)
    {
      ++exact_fallback_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-texture] compatibility exact: %.*s -> %s\n",
          static_cast<int>(
              guest_name.size()),
          guest_name.data(),
          best->relative_path.c_str());
    }

    return best;
  }

  // 2) Same normalized stem. This intentionally ignores punctuation/case
  // differences while still requiring a complete stem identity.
  if (!wanted_stem.empty())
  {
    for (const auto& asset :
         PS3RemasterAssets::GetAssets())
    {
      const std::string asset_name =
          Lower(asset.filename);

      if (!asset_name.ends_with(".ssh"))
        continue;

      if (StemKey(asset_name) ==
          wanted_stem)
      {
        consider(asset);
      }
    }
  }

  if (best)
  {
    static unsigned stem_logs = 0;

    if (stem_logs < 64)
    {
      ++stem_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-texture] compatibility stem: %.*s -> %s\n",
          static_cast<int>(
              guest_name.size()),
          guest_name.data(),
          best->relative_path.c_str());
    }

    return best;
  }

  // 3) Conservative frontend semantic alias retained from the known-good
  //    v8-v11 path (master/txt/highlight/hl aliases).
  if (wanted_semantic.size() >= 5)
  {
    for (const auto& asset :
         PS3RemasterAssets::GetAssets())
    {
      const std::string asset_name =
          Lower(asset.filename);

      if (!asset_name.ends_with(".ssh"))
        continue;

      if (SemanticStem(asset_name) ==
          wanted_semantic)
      {
        consider(asset);
      }
    }
  }

  if (best)
  {
    static unsigned semantic_logs = 0;

    if (semantic_logs < 64)
    {
      ++semantic_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-texture] semantic alias: %.*s -> %s\n",
          static_cast<int>(
              guest_name.size()),
          guest_name.data(),
          best->relative_path.c_str());
    }

    return best;
  }

  // 4) Last compatibility fallback is substring-based but ONLY accepts
  //    frontend/UI-ish PS3 assets. It never becomes the 3D material matcher.
  if (wanted_stem.size() >= 5)
  {
    for (const auto& asset :
         PS3RemasterAssets::GetAssets())
    {
      if (!AssetLocalePriority(
              asset.relative_path))
      {
        continue;
      }

      const std::string asset_name =
          Lower(asset.filename);

      if (!asset_name.ends_with(".ssh"))
        continue;

      const std::string asset_key =
          StemKey(asset_name);

      if (asset_key.size() < 5)
        continue;

      const bool related =
          wanted_stem.find(asset_key) !=
              std::string::npos ||
          asset_key.find(wanted_stem) !=
              std::string::npos;

      if (!related)
        continue;

      const std::string relative =
          Normalize(
              asset.relative_path);

      const bool frontend =
          relative.find("shell") !=
              std::string::npos ||
          relative.find("menu") !=
              std::string::npos ||
          relative.find("pause") !=
              std::string::npos ||
          relative.find("frontend") !=
              std::string::npos ||
          relative.find("loading") !=
              std::string::npos ||
          relative.find("bitmaps") !=
              std::string::npos ||
          relative.find("_usa") !=
              std::string::npos;

      if (!frontend)
        continue;

      consider(asset);
    }
  }

  if (best)
  {
    static unsigned fallback_logs = 0;

    if (fallback_logs < 64)
    {
      ++fallback_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-texture] locale-safe frontend fallback: %.*s -> %s\n",
          static_cast<int>(
              guest_name.size()),
          guest_name.data(),
          best->relative_path.c_str());
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

  const auto* asset =
      PS3RemasterAssets::FindByRelativePath(
          resource->relative_path);

  if (!asset)
  {
    std::fprintf(
        stderr,
        "[moh-ps3-texture] asset disappeared from index: %s\n",
        resource->relative_path.c_str());

    return nullptr;
  }

  const std::vector<u8> binary =
      PS3RemasterAssets::ReadBinary(
          *asset);

  std::vector<PS3TextureDecoder::Level>
      levels;

  auto try_decode =
      [&](std::span<const u8> bytes,
          std::size_t wrapper_offset)
      {
        levels.clear();

        if (!PS3TextureDecoder::Decode(
                bytes,
                &levels) ||
            levels.empty())
        {
          return false;
        }

        if (wrapper_offset)
        {
          std::fprintf(
              stderr,
              "[moh-ps3-texture] embedded texture payload found: "
              "%s +0x%zX (%zu -> %zu bytes)\n",
              resource->relative_path.c_str(),
              wrapper_offset,
              binary.size(),
              bytes.size());
        }

        return true;
      };

  bool decoded_ok =
      !binary.empty() &&
      try_decode(
          std::span<const u8>(
              binary.data(),
              binary.size()),
          0);

  // A number of EA assets are wrappers: the VIV entry itself starts with
  // metadata and the actual GTF/SHPS payload follows at a small aligned
  // offset. Scan only the beginning of the entry and let the real decoder
  // validate every candidate before accepting it.
  if (!decoded_ok &&
      binary.size() >= 8)
  {
    const std::size_t scan_limit =
        std::min<std::size_t>(
            binary.size() - 4,
            0x4000);

    for (std::size_t offset = 1;
         offset <= scan_limit;
         ++offset)
    {
      const u8* p =
          binary.data() +
          offset;

      const bool gtf =
          p[0] == 0x02 &&
          p[1] == 0x01 &&
          p[2] == 0x01 &&
          p[3] == 0x00;

      const bool shps =
          (p[0] == 'S' &&
           p[1] == 'H' &&
           p[2] == 'P' &&
           (p[3] == 'S' ||
            p[3] == 'I')) ||
          (p[0] == 'S' &&
           p[1] == 'h' &&
           p[2] == 'p' &&
           p[3] == 'S');

      if (!gtf &&
          !shps)
      {
        continue;
      }

      if (try_decode(
              std::span<const u8>(
                  binary.data() +
                      offset,
                  binary.size() -
                      offset),
              offset))
      {
        decoded_ok =
            true;

        break;
      }
    }
  }

  if (!decoded_ok)
  {
    static unsigned failure_logs = 0;

    if (failure_logs < 96)
    {
      ++failure_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-texture-debug] FAIL %s "
          "bytes=%zu embedded=%d archive_off=0x%llX packed=%llu "
          "logical=%ju refpack=%d prefix=",
          resource->relative_path.c_str(),
          binary.size(),
          asset->embedded ? 1 : 0,
          static_cast<unsigned long long>(
              asset->archive_offset),
          static_cast<unsigned long long>(
              asset->packed_size),
          static_cast<std::uintmax_t>(
              asset->size),
          asset->refpack ? 1 : 0);

      const std::size_t dump =
          std::min<std::size_t>(
              binary.size(),
              64);

      for (std::size_t i = 0;
           i < dump;
           ++i)
      {
        std::fprintf(
            stderr,
            "%02X",
            static_cast<unsigned>(
                binary[i]));

        if ((i & 3u) == 3u)
          std::fputc(' ', stderr);
      }

      if (!dump)
        std::fprintf(stderr, "<empty>");

      std::fputc('\n', stderr);
    }

    std::fprintf(
        stderr,
        "[moh-ps3-texture] unsupported/malformed PS3 texture: %s; keeping GC texture\n",
        resource->relative_path.c_str());

    return nullptr;
  }

  auto decoded =
      std::make_shared<
          VideoCommon::CustomTextureData>();

  decoded->m_slices.emplace_back();

  for (const auto& source :
       levels)
  {
    VideoCommon::CustomTextureData::
        ArraySlice::Level level;

    level.width =
        source.width;

    level.height =
        source.height;

    level.row_length =
        source.width;

    level.data.reset(
        source.rgba.size());

    std::copy(
        source.rgba.begin(),
        source.rgba.end(),
        level.data.begin());

    decoded->m_slices[0]
        .m_levels.push_back(
            std::move(level));
  }

  resource->decoded =
      decoded;

  std::fprintf(
      stderr,
      "[moh-ps3-texture] loaded %s: %ux%u, %zu mip levels\n",
      resource->relative_path.c_str(),
      levels[0].width,
      levels[0].height,
      levels.size());

  return resource->decoded;
}


struct Auto3DFingerprint
{
  std::array<float, 64> shape{};
  float mean_r = 0.0f;
  float mean_g = 0.0f;
  float mean_b = 0.0f;
  float mean_a = 0.0f;
  float contrast = 0.0f;
};

struct Auto3DCandidate
{
  std::string relative_path;
  u32 width = 0;
  u32 height = 0;
  Auto3DFingerprint fingerprint;
};

std::mutex auto3d_mutex;
std::string auto3d_level_scope;
std::string auto3d_built_scope;
std::vector<Auto3DCandidate> auto3d_candidates;

std::unordered_map<
    u64,
    std::shared_ptr<VideoCommon::CustomTextureData>>
    auto3d_matches;

std::unordered_set<u64> auto3d_rejected;

std::unordered_map<
    u64,
    std::shared_ptr<VideoCommon::CustomTextureData>>
    strict_level_matches;
std::unordered_set<u64> strict_level_rejected;

struct SkyGCProbe
{
  u64 key = 0;
  Auto3DFingerprint fingerprint;
};

std::vector<SkyGCProbe> sky_gc_probes;

std::unordered_map<
    u64,
    std::string>
    sky_exact_assignments;

bool sky_assignment_logged = false;
bool sky_cache_invalidation_pending = false;

bool StrictLevelTexturesEnabled()
{
  const char* value = std::getenv("MOH_PS3_STRICT_LEVEL_TEXTURES");
  if (!value || !*value)
    return true;
  const std::string lower = Lower(std::string(value));
  return lower != "0" && lower != "false" && lower != "off" && lower != "no";
}

bool SkyTexturesEnabled()
{
  const char* value = std::getenv("MOH_PS3_SKY");

  if (!value || !*value)
    return true;

  const std::string lower = Lower(std::string(value));

  return lower != "0" &&
         lower != "false" &&
         lower != "off" &&
         lower != "no";
}

bool Auto3DEnabled()
{
  const char* value = std::getenv("MOH_PS3_AUTO_3D");

  if (!value || !*value)
    return true;

  const std::string lower = Lower(value);

  return lower != "0" &&
         lower != "false" &&
         lower != "off" &&
         lower != "no";
}

double Auto3DThreshold()
{
  constexpr double fallback = 0.46;
  const char* value = std::getenv("MOH_PS3_AUTO_3D_THRESHOLD");

  if (!value || !*value)
    return fallback;

  char* end = nullptr;
  const double parsed = std::strtod(value, &end);

  if (end == value || !std::isfinite(parsed))
    return fallback;

  return std::clamp(parsed, 0.20, 1.20);
}

double Auto3DMargin()
{
  constexpr double fallback = 0.080;
  const char* value = std::getenv("MOH_PS3_AUTO_3D_MARGIN");

  if (!value || !*value)
    return fallback;

  char* end = nullptr;
  const double parsed = std::strtod(value, &end);

  if (end == value || !std::isfinite(parsed))
    return fallback;

  return std::clamp(parsed, 0.0, 0.50);
}

Auto3DFingerprint MakeAuto3DFingerprint(
    const u8* rgba,
    u32 width,
    u32 height)
{
  Auto3DFingerprint result;

  if (!rgba || !width || !height)
    return result;

  float mean_luma = 0.0f;

  for (u32 gy = 0; gy < 8; ++gy)
  {
    for (u32 gx = 0; gx < 8; ++gx)
    {
      float r = 0.0f;
      float g = 0.0f;
      float b = 0.0f;
      float a = 0.0f;

      for (u32 sy = 0; sy < 2; ++sy)
      {
        for (u32 sx = 0; sx < 2; ++sx)
        {
          const float fx =
              (static_cast<float>(gx) +
               (static_cast<float>(sx) + 0.5f) / 2.0f) /
              8.0f;

          const float fy =
              (static_cast<float>(gy) +
               (static_cast<float>(sy) + 0.5f) / 2.0f) /
              8.0f;

          const u32 x =
              std::min(
                  width - 1,
                  static_cast<u32>(
                      fx * static_cast<float>(width)));

          const u32 y =
              std::min(
                  height - 1,
                  static_cast<u32>(
                      fy * static_cast<float>(height)));

          const u8* p =
              rgba +
              (std::size_t(y) * width + x) * 4u;

          r += static_cast<float>(p[0]) / 255.0f;
          g += static_cast<float>(p[1]) / 255.0f;
          b += static_cast<float>(p[2]) / 255.0f;
          a += static_cast<float>(p[3]) / 255.0f;
        }
      }

      r *= 0.25f;
      g *= 0.25f;
      b *= 0.25f;
      a *= 0.25f;

      const float luma =
          r * 0.2126f +
          g * 0.7152f +
          b * 0.0722f;

      const std::size_t index =
          std::size_t(gy) * 8u + gx;

      result.shape[index] = luma;
      mean_luma += luma;
      result.mean_r += r;
      result.mean_g += g;
      result.mean_b += b;
      result.mean_a += a;
    }
  }

  constexpr float inv_cells = 1.0f / 64.0f;

  mean_luma *= inv_cells;
  result.mean_r *= inv_cells;
  result.mean_g *= inv_cells;
  result.mean_b *= inv_cells;
  result.mean_a *= inv_cells;

  float variance = 0.0f;

  for (float value : result.shape)
  {
    const float centered = value - mean_luma;
    variance += centered * centered;
  }

  variance *= inv_cells;
  result.contrast = std::sqrt(variance);

  const float normalization =
      result.contrast > 0.035f ?
          result.contrast :
          1.0f;

  for (float& value : result.shape)
    value = (value - mean_luma) / normalization;

  return result;
}

float Auto3DDistance(
    const Auto3DFingerprint& a,
    const Auto3DFingerprint& b)
{
  float structure_mse = 0.0f;

  for (std::size_t i = 0; i < a.shape.size(); ++i)
  {
    const float d = a.shape[i] - b.shape[i];
    structure_mse += d * d;
  }

  const float structure =
      std::sqrt(
          structure_mse /
          static_cast<float>(a.shape.size()));

  const float color =
      (std::abs(a.mean_r - b.mean_r) +
       std::abs(a.mean_g - b.mean_g) +
       std::abs(a.mean_b - b.mean_b)) /
      3.0f;

  const float alpha =
      std::abs(a.mean_a - b.mean_a);

  const float contrast =
      std::abs(a.contrast - b.contrast);

  return structure * 0.76f +
         color * 0.14f +
         alpha * 0.04f +
         contrast * 0.06f;
}

void ResetAuto3DCacheLocked()
{
  auto3d_built_scope.clear();
  auto3d_candidates.clear();
  auto3d_matches.clear();
  auto3d_rejected.clear();
  strict_level_matches.clear();
  strict_level_rejected.clear();
  sky_gc_probes.clear();
  sky_exact_assignments.clear();
  sky_assignment_logged = false;
  sky_cache_invalidation_pending = false;
}

void SetAuto3DLevelScope(
    std::string scope,
    const char* reason)
{
  scope = Normalize(std::move(scope));

  if (!scope.empty() && !scope.ends_with('/'))
    scope.push_back('/');

  std::scoped_lock lock(auto3d_mutex);

  if (auto3d_level_scope == scope)
    return;

  PS3AssetPort::SetCurrentLevel(scope);
  auto3d_level_scope = std::move(scope);
  ResetAuto3DCacheLocked();
  {
    std::scoped_lock registrations_lock(mutex);
    for (u32 address : named_sky_addresses)
    {
      registrations.erase(address);
      named_sky_invalidations.insert(address);
    }
    named_sky_addresses.clear();
    uploaded_named_sky.clear();
  }

  std::fprintf(
      stderr,
      "[moh-ps3-auto3d] level scope: %s (%s)\n",
      auto3d_level_scope.empty() ?
          "<frontend>" :
          auto3d_level_scope.c_str(),
      reason ? reason : "unknown");
}

void UpdateLevelScopeFromGuestName(std::string_view guest_name)
{
  const std::string path = Normalize(std::string(guest_name));
  const std::string filename = Filename(guest_name);

  if (filename == "start.gsh" || filename == "return.gsh")
  {
    SetAuto3DLevelScope({}, "frontend guest request");
    return;
  }

  for (const std::string& level : MOHFrontline::NativeAssets::GetLevels())
  {
    const std::string load_gsh = "load" + level + ".gsh";
    const std::string load_ssh = "load" + level + ".ssh";
    const std::string level_component = "/" + level + "/";

    if (filename != load_gsh && filename != load_ssh &&
        path.find(level_component) == std::string::npos)
      continue;

    const auto underscore = level.find('_');
    if (underscore == std::string::npos || underscore == 0)
      continue;

    SetAuto3DLevelScope("data/" + level.substr(0, underscore) + "/" + level + "/",
                        "guest level identity");
    return;
  }
}

void UpdateAuto3DScope(
    std::string_view guest_name,
    const PS3RemasterAssets::AssetInfo& asset)
{
  const std::string relative =
      Normalize(asset.relative_path);

  const auto level_viv =
      relative.find("level.viv::");

  if (level_viv != std::string::npos)
  {
    SetAuto3DLevelScope(
        relative.substr(0, level_viv),
        "level.viv named asset");
    return;
  }

  const std::string stem =
      StemKey(asset.filename);

  if (stem.starts_with("load") &&
      stem.size() >= 6 &&
      std::isdigit(static_cast<unsigned char>(stem[4])) &&
      std::isdigit(static_cast<unsigned char>(stem[5])))
  {
    const char mission = stem[4];
    const char stage = stem[5];

    std::string scope = "data/";
    scope.push_back(mission);
    scope.push_back('/');
    scope.push_back(mission);
    scope.push_back('_');
    scope.push_back(stage);
    scope.push_back('/');

    SetAuto3DLevelScope(
        std::move(scope),
        "loading screen");
    return;
  }

  const std::string guest =
      Filename(guest_name);

  if (guest == "start.gsh" ||
      guest == "return.gsh")
  {
    SetAuto3DLevelScope({}, "frontend");
  }
}


bool ContainsAuto3DUnsafeToken(
    std::string_view filename)
{
  const std::string key =
      StemKey(filename);

  static constexpr std::string_view unsafe[] = {
      "compass",
      "health",
      "hitmeter",
      "crosshair",
      "checkbox",
      "popup",
      "pause",
      "button",
      "arrow",
      "controller",
      "hud",
      "frontend",
      "loading",
      "loadbar",
      "menu",
      "overlay",
      "scope",
      "reticle",
      "ammo",
      "icon",
      "meter",
      "prototype",
      "debug",
      "dummy",
      "temp",
  };

  for (const auto token : unsafe)
  {
    if (key.find(token) !=
        std::string::npos)
    {
      return true;
    }
  }

  return false;
}

bool IsLikelyCubeFaceCandidate(
    std::string_view relative_path)
{
  const std::string stem =
      StemKey(relative_path);

  if (stem.size() < 3)
    return false;

  static constexpr std::string_view suffixes[] = {
      "fr",
      "bk",
      "lf",
      "rt",
      "up",
      "dn",
      "front",
      "back",
      "left",
      "right",
      "top",
      "bottom",
  };

  for (const auto suffix : suffixes)
  {
    if (stem.ends_with(suffix))
      return true;
  }

  return false;
}

std::string CurrentLevelIdFromScope(
    std::string_view scope)
{
  std::string level =
      Normalize(std::string(scope));

  while (!level.empty() &&
         level.back() == '/')
  {
    level.pop_back();
  }

  const auto slash =
      level.find_last_of('/');

  if (slash != std::string::npos)
    level.erase(0, slash + 1);

  return level;
}

std::string DirectSkyStemForLevel(std::string_view level)
{
  if (level == "1_1")
    return "level1test";

  return std::string(level);
}

bool IsExactCurrentLevelSkyFacePath(
    std::string_view relative,
    std::string_view scope)
{
  if (!SkyTexturesEnabled() ||
      scope.empty())
  {
    return false;
  }

  std::string normalized_scope =
      Normalize(std::string(scope));

  if (!normalized_scope.ends_with('/'))
    normalized_scope.push_back('/');

  const std::string path =
      Normalize(std::string(relative));

  // Sky faces in the PS3 remaster are standalone files:
  // data/2/2_1/2_1_fr.ssh, ... not level.viv:: entries.
  if (!path.starts_with(normalized_scope) ||
      path.find("::") != std::string::npos)
  {
    return false;
  }

  const std::string level =
      CurrentLevelIdFromScope(
          normalized_scope);

  if (level.empty())
    return false;

  const std::string file =
      Filename(path);

  static constexpr std::string_view faces[] = {
      "fr", "bk", "lf", "rt", "up", "dn",
  };

  const std::string sky_stem =
      DirectSkyStemForLevel(level);

  for (const auto face : faces)
  {
    if (file ==
        sky_stem + "_" +
            std::string(face) +
            ".ssh")
    {
      return true;
    }
  }

  return false;
}


[[maybe_unused]] bool UpdateDeterministicSkyAssignmentLocked(
    u64 key,
    const Auto3DFingerprint& fingerprint,
    u32 width,
    u32 height,
    std::string* assigned_path,
    bool* potential_sky)
{
  if (assigned_path)
    assigned_path->clear();

  if (potential_sky)
    *potential_sky = false;

  // v14.6: the GameCube level files already name the six sky resources
  // explicitly (FR/LF/BK/RT/UP/DN). Do not guess sky identity from anonymous
  // texture fingerprints anymore. The old matcher is opt-in only for debugging.
  const char* legacy_sky_match =
      std::getenv("MOH_PS3_LEGACY_SKY_MATCH");

  if (!legacy_sky_match ||
      std::string_view(legacy_sky_match) != "1")
  {
    return false;
  }

  const bool supported_sky_probe =
      width == height &&
      (width == 128 ||
       width == 256);

  if (!SkyTexturesEnabled() ||
      auto3d_level_scope.empty() ||
      !supported_sky_probe)
  {
    return false;
  }

  // v14.5: GameCube sky faces are not assumed to all be 128x128.
  // Keep 128x128 sides, but also admit 256x256 square sky uploads.
  // v14.4: once the global sky assignment has been solved, freeze it.
  // Re-solving for every later 128x128 texture can make unrelated
  // world/floor textures steal a sky face such as DN or UP.
  if (!sky_exact_assignments.empty())
  {
    const auto it =
        sky_exact_assignments.find(key);

    if (it != sky_exact_assignments.end())
    {
      if (assigned_path)
        *assigned_path = it->second;

      if (potential_sky)
        *potential_sky = true;

      return true;
    }

    // Only the hashes already present in sky_exact_assignments are sky.
    // Any other 128x128 anonymous GC texture must stay untouched.
    if (potential_sky)
      *potential_sky = true;

    return false;
  }

  /*
   * v13.9.2
   *
   * The PS3 remaster has six canonical cube faces:
   *
   *   FR BK LF RT UP DN
   *
   * Do NOT require auto3d_candidates.size() to contain exactly six
   * decoded sky entries. Asset aliases and unsupported SSH variants
   * can make the decoded set smaller/larger.
   *
   * Deduplicate by canonical face identity and solve using whatever
   * usable subset exists.
   */

  static constexpr
      std::array<std::string_view, 6>
      face_suffixes = {
          "fr",
          "bk",
          "lf",
          "rt",
          "up",
          "dn",
      };

  std::array<
      const Auto3DCandidate*,
      6>
      face_slots = {
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
      };

  const std::string level =
      CurrentLevelIdFromScope(
          auto3d_level_scope);

  const std::string sky_stem =
      DirectSkyStemForLevel(level);

  for (const auto& candidate :
       auto3d_candidates)
  {
    if (!IsExactCurrentLevelSkyFacePath(
            candidate.relative_path,
            auto3d_level_scope))
    {
      continue;
    }

    const std::string file =
        Filename(
            candidate.relative_path);

    for (std::size_t slot = 0;
         slot < face_suffixes.size();
         ++slot)
    {
      const std::string expected =
          sky_stem +
          "_" +
          std::string(
              face_suffixes[slot]) +
          ".ssh";

      if (file != expected)
        continue;

      const Auto3DCandidate* old =
          face_slots[slot];

      if (!old ||
          static_cast<u64>(
              candidate.width) *
                  candidate.height >
              static_cast<u64>(
                  old->width) *
                  old->height ||
          (candidate.width ==
               old->width &&
           candidate.height ==
               old->height &&
           candidate.relative_path <
               old->relative_path))
      {
        face_slots[slot] =
            &candidate;
      }

      break;
    }
  }


  std::vector<
      const Auto3DCandidate*>
      faces;

  std::vector<std::size_t>
      canonical_slots;

  for (std::size_t slot = 0;
       slot < face_slots.size();
       ++slot)
  {
    if (!face_slots[slot])
      continue;

    faces.push_back(
        face_slots[slot]);

    canonical_slots.push_back(
        slot);
  }


  static unsigned inventory_logs = 0;

  if (inventory_logs++ < 16)
  {
    std::fprintf(
        stderr,
        "[moh-ps3-sky] FACE INVENTORY: "
        "scope=%s decoded=%zu/6 "
        "FR=%s BK=%s LF=%s RT=%s UP=%s DN=%s\n",
        auto3d_level_scope.c_str(),
        faces.size(),
        face_slots[0] ? "yes" : "NO",
        face_slots[1] ? "yes" : "NO",
        face_slots[2] ? "yes" : "NO",
        face_slots[3] ? "yes" : "NO",
        face_slots[4] ? "yes" : "NO",
        face_slots[5] ? "yes" : "NO");
  }


  /*
   * Require four decoded PS3 faces minimum.
   *
   * This avoids accidentally assigning two random 128x128 world
   * textures if only one/two malformed SSH files entered the index.
   */
  if (faces.size() < 4)
    return false;


  float nearest =
      std::numeric_limits<float>
          ::infinity();

  for (const auto* face :
       faces)
  {
    nearest =
        std::min(
            nearest,
            Auto3DDistance(
                fingerprint,
                face->fingerprint));
  }


  if (potential_sky)
  {
    *potential_sky =
        std::isfinite(nearest) &&
        nearest <= 0.75f;
  }


  bool known = false;

  for (auto& probe :
       sky_gc_probes)
  {
    if (probe.key == key)
    {
      probe.fingerprint =
          fingerprint;

      known = true;

      break;
    }
  }


  if (!known &&
      sky_gc_probes.size() < 96)
  {
    SkyGCProbe probe;

    probe.key =
        key;

    probe.fingerprint =
        fingerprint;

    sky_gc_probes.push_back(
        std::move(probe));
  }


  const std::size_t face_count =
      faces.size();

  /*
   * Do not freeze an assignment immediately on the first four uploads.
   * Collect enough anonymous GC textures for the global assignment to
   * choose the real sky faces instead of nearby world/HUD textures.
   */
  const std::size_t minimum_probes =
      std::max<std::size_t>(
          face_count,
          32);

  if (sky_gc_probes.size() <
      minimum_probes)
  {
    return false;
  }


  struct AssignmentState
  {
    float cost =
        std::numeric_limits<float>
            ::infinity();

    std::array<int, 6>
        probe_for_face = {
            -1,
            -1,
            -1,
            -1,
            -1,
            -1,
        };
  };


  std::array<
      AssignmentState,
      64>
      dp;

  dp[0].cost =
      0.0f;


  const unsigned full_mask =
      (1u <<
       static_cast<unsigned>(
           face_count)) -
      1u;


  for (std::size_t probe_index = 0;
       probe_index <
           sky_gc_probes.size();
       ++probe_index)
  {
    auto next =
        dp;

    for (unsigned mask = 0;
         mask <= full_mask;
         ++mask)
    {
      if (!std::isfinite(
              dp[mask].cost))
      {
        continue;
      }

      for (std::size_t face_index = 0;
           face_index <
               face_count;
           ++face_index)
      {
        const unsigned bit =
            1u <<
            static_cast<unsigned>(
                face_index);

        if (mask & bit)
          continue;

        const float score =
            Auto3DDistance(
                sky_gc_probes[
                    probe_index]
                    .fingerprint,
                faces[
                    face_index]
                    ->fingerprint);

        if (!std::isfinite(score) ||
            score > 1.10f)
        {
          continue;
        }

        const unsigned new_mask =
            mask | bit;

        const float new_cost =
            dp[mask].cost +
            score;

        if (new_cost >=
            next[new_mask].cost)
        {
          continue;
        }

        next[new_mask] =
            dp[mask];

        next[new_mask].cost =
            new_cost;

        next[new_mask]
            .probe_for_face[
                face_index] =
            static_cast<int>(
                probe_index);
      }
    }

    dp =
        std::move(next);
  }


  const AssignmentState&
      solution =
          dp[full_mask];


  if (!std::isfinite(
          solution.cost))
  {
    return false;
  }


  float max_edge =
      0.0f;

  unsigned strong_edges =
      0;


  for (std::size_t face_index = 0;
       face_index <
           face_count;
       ++face_index)
  {
    const int probe_index =
        solution
            .probe_for_face[
                face_index];

    if (probe_index < 0)
      return false;

    const std::size_t probe =
        static_cast<std::size_t>(
            probe_index);

    if (probe >=
        sky_gc_probes.size())
    {
      return false;
    }

    const float score =
        Auto3DDistance(
            sky_gc_probes[
                probe]
                .fingerprint,
            faces[
                face_index]
                ->fingerprint);

    max_edge =
        std::max(
            max_edge,
            score);

    if (score <= 0.72f)
    {
      ++strong_edges;
    }
  }


  const float average =
      solution.cost /
      static_cast<float>(
          face_count);


  const unsigned strong_needed =
      std::max(
          3u,
          static_cast<unsigned>(
              (face_count * 2u + 2u) /
              3u));


  if (average > 0.70f ||
      max_edge > 1.10f ||
      strong_edges <
          strong_needed)
  {
    return false;
  }


  /*
   * Once solved, install an exact GC texture hash -> PS3 face map.
   */
  const bool assignment_was_ready =
      !sky_exact_assignments.empty();

  sky_exact_assignments.clear();


  for (std::size_t face_index = 0;
       face_index <
           face_count;
       ++face_index)
  {
    const std::size_t probe =
        static_cast<std::size_t>(
            solution
                .probe_for_face[
                    face_index]);

    const u64 gc_key =
        sky_gc_probes[
            probe].key;

    sky_exact_assignments[
        gc_key] =
        faces[
            face_index]
            ->relative_path;

    // v14.1: these hashes may have been rejected by the old per-texture
    // matcher before the global sky assignment became ready. Unblock them
    // so every mapped cube face can use its deterministic PS3 replacement.
    strict_level_rejected.erase(gc_key);
  }

  if (!assignment_was_ready &&
      !sky_exact_assignments.empty())
  {
    sky_cache_invalidation_pending = true;

    std::fprintf(
        stderr,
        "[moh-ps3-sky] v14.2 assignment committed: "
        "requesting one-shot texture-cache reload\n");
  }


  if (!sky_assignment_logged)
  {
    sky_assignment_logged =
        true;

    std::fprintf(
        stderr,
        "[moh-ps3-sky] PARTIAL-6FACE ASSIGNMENT READY: "
        "scope=%s ps3=%zu/6 probes=%zu "
        "avg=%.3f max=%.3f strong=%u/%u\n",
        auto3d_level_scope.c_str(),
        face_count,
        sky_gc_probes.size(),
        average,
        max_edge,
        strong_edges,
        strong_needed);


    for (std::size_t face_index = 0;
         face_index <
             face_count;
         ++face_index)
    {
      const std::size_t probe =
          static_cast<std::size_t>(
              solution
                  .probe_for_face[
                      face_index]);

      const float score =
          Auto3DDistance(
              sky_gc_probes[
                  probe]
                  .fingerprint,
              faces[
                  face_index]
                  ->fingerprint);

      const std::size_t slot =
          canonical_slots[
              face_index];

      std::fprintf(
          stderr,
          "[moh-ps3-sky]   %s "
          "GC=%016llX -> %s score=%.3f\n",
          std::string(
              face_suffixes[
                  slot])
              .c_str(),
          static_cast<
              unsigned long long>(
              sky_gc_probes[
                  probe].key),
          faces[
              face_index]
              ->relative_path
              .c_str(),
          score);
    }
  }


  if (!assigned_path)
    return false;


  const auto it =
      sky_exact_assignments.find(
          key);


  if (it ==
      sky_exact_assignments.end())
  {
    return false;
  }


  *assigned_path =
      it->second;

  return true;
}


bool IsPowerOfTwoLikeScale(float scale)
{
  if (!std::isfinite(scale) ||
      scale < 0.95f ||
      scale > 8.5f)
  {
    return false;
  }

  const float log2_scale =
      std::log2(
          std::max(
              scale,
              0.001f));

  const float nearest =
      std::round(
          log2_scale);

  return
      std::abs(
          log2_scale -
          nearest) <=
      0.14f;
}

bool Auto3DPathAllowed(
    std::string_view relative,
    std::string_view scope)
{
  const std::string path =
      Normalize(
          std::string(
              relative));

  if (!AssetLocalePriority(path))
    return false;

  if (path.starts_with("data/bitmaps/") ||
      path.find("/shell/") != std::string::npos ||
      path.find("/pausescreen/") != std::string::npos ||
      path.find("/loading/") != std::string::npos)
  {
    return false;
  }

  if (ContainsAuto3DUnsafeToken(path))
    return false;

  if (!scope.empty() &&
      path.starts_with(scope))
  {
    // IMPORTANT v13.9:
    // PS3 sky faces are direct level files, not entries in level.viv.
    if (IsExactCurrentLevelSkyFacePath(
            path,
            scope))
    {
      return true;
    }

    if (path.find("level.viv::") !=
            std::string::npos ||
        path.find("comp.viv::") !=
            std::string::npos)
    {
      return true;
    }
  }

  // Kept exclusively for MOH_PS3_AUTO_3D_FUZZY diagnostics.
  // Strict matching below never consumes global weapon candidates.
  if (path.find("/weapons/") != std::string::npos ||
      path.find("weapons.viv::") != std::string::npos ||
      path.find("weapon.viv::") != std::string::npos)
  {
    return true;
  }

  return false;
}

void BuildAuto3DCandidatesLocked()
{
  if (auto3d_level_scope.empty())
    return;

  if (auto3d_built_scope == auto3d_level_scope)
    return;

  auto3d_candidates.clear();

  const std::string scope = auto3d_level_scope;

  std::size_t tried = 0;
  std::size_t decoded_count = 0;

  for (const auto& asset :
       PS3RemasterAssets::GetAssets())
  {
    // Named sky faces are never candidates for anonymous world textures.
    if (!PS3NamedSky::RelativePath(asset.filename).empty())
      continue;
    if (asset.kind != PS3RemasterAssets::Kind::Texture ||
        !Lower(asset.filename).ends_with(".ssh") ||
        !Auto3DPathAllowed(asset.relative_path, scope))
    {
      continue;
    }

    ++tried;

    const auto binary =
        PS3RemasterAssets::ReadBinary(asset);

    if (binary.empty())
      continue;

    std::vector<PS3TextureDecoder::Level> levels;

    if (!PS3TextureDecoder::Decode(
            std::span<const u8>(
                binary.data(),
                binary.size()),
            &levels) ||
        levels.empty())
    {
      continue;
    }

    const auto& base = levels.front();

    if (base.rgba.size() <
            std::size_t(base.width) *
                base.height *
                4u ||
        base.width < 8 ||
        base.height < 8 ||
        base.width > 4096 ||
        base.height > 4096 ||
        base.rgba.empty())
    {
      continue;
    }

    Auto3DCandidate candidate;
    candidate.relative_path =
        Normalize(asset.relative_path);
    candidate.width = base.width;
    candidate.height = base.height;
    candidate.fingerprint =
        MakeAuto3DFingerprint(
            base.rgba.data(),
            base.width,
            base.height);

    if (candidate.fingerprint.contrast < 0.018f)
      continue;

    auto3d_candidates.push_back(
        std::move(candidate));

    ++decoded_count;
  }

  auto3d_built_scope = scope;

  std::fprintf(
      stderr,
      "[moh-ps3-auto3d] SAFE candidate index: "
      "scope=%s eligible=%zu decoded=%zu\n",
      scope.c_str(),
      tried,
      decoded_count);
}

u64 Auto3DKey(const TextureInfo& info)
{
  u64 key =
      Common::GetHash64(
          info.GetData(),
          info.GetTextureSize(),
          0);

  key ^= static_cast<u64>(info.GetRawWidth()) << 40;
  key ^= static_cast<u64>(info.GetRawHeight()) << 24;
  key ^=
      static_cast<u64>(
          static_cast<u32>(
              info.GetTextureFormat())) <<
      16;

  if (const auto palette_size = info.GetPaletteSize();
      palette_size &&
      *palette_size &&
      info.GetTlutAddress())
  {
    key ^=
        Common::GetHash64(
            info.GetTlutAddress(),
            *palette_size,
            key);
  }

  return key;
}

std::shared_ptr<VideoCommon::CustomTextureData>
DecodeAuto3DWinner(
    std::string_view relative_path)
{
  const auto* asset =
      PS3RemasterAssets::FindByRelativePath(
          relative_path);

  if (!asset)
    return nullptr;

  Resource temporary;
  temporary.filename = asset->filename;
  temporary.relative_path =
      Normalize(asset->relative_path);

  return DecodeResource(&temporary);
}

std::shared_ptr<VideoCommon::CustomTextureData>
FindStrictCurrentLevelTexture(const TextureInfo& info)
{
  if (!StrictLevelTexturesEnabled() ||
      !MohPcLayer::IsPS3TextureReplacementEnabled() ||
      !PS3RemasterAssets::IsReady() ||
      !info.IsDataValid() ||
      info.IsFromTmem() ||
      info.GetTextureFormat() == TextureFormat::XFB ||
      !info.GetData() ||
      !info.GetTextureSize())
  {
    return nullptr;
  }

  const u32 width =
      info.GetRawWidth();

  const u32 height =
      info.GetRawHeight();

  if (width < 32 ||
      height < 16 ||
      width > 2048 ||
      height > 2048)
  {
    return nullptr;
  }

  const u64 key =
      Auto3DKey(info);

  std::string scope;

  {
    std::scoped_lock lock(
        auto3d_mutex);

    scope =
        auto3d_level_scope;

    if (scope.empty())
      return nullptr;

    if (const auto it =
            strict_level_matches.find(key);
        it != strict_level_matches.end())
    {
      return it->second;
    }

    if (strict_level_rejected.contains(key))
      return nullptr;

    BuildAuto3DCandidatesLocked();
  }

  std::vector<u8> gc_rgba(
      std::size_t(width) *
      height *
      4u);

  TexDecoder_Decode(
      gc_rgba.data(),
      info.GetData(),
      static_cast<int>(width),
      static_cast<int>(height),
      info.GetTextureFormat(),
      info.GetTlutAddress(),
      info.GetTlutFormat());

  const Auto3DFingerprint gc =
      MakeAuto3DFingerprint(
          gc_rgba.data(),
          width,
          height);

  if (gc.contrast < 0.018f)
    return nullptr;

  const float gc_aspect =
      static_cast<float>(width) /
      static_cast<float>(height);

  struct Winner
  {
    std::string path;
    float best =
        std::numeric_limits<float>::infinity();
    float second =
        std::numeric_limits<float>::infinity();
  };

  Winner sky;
  Winner level;

  {
    std::scoped_lock lock(
        auto3d_mutex);

    for (const auto& candidate :
         auto3d_candidates)
    {
      const bool exact_sky =
          IsExactCurrentLevelSkyFacePath(
              candidate.relative_path,
              scope);

      const bool current_archive =
          candidate.relative_path.starts_with(scope) &&
          (candidate.relative_path.find(
               "level.viv::") !=
               std::string::npos ||
           candidate.relative_path.find(
               "comp.viv::") !=
               std::string::npos);

      // This deliberately rejects global weapon/material candidates.
      if (!exact_sky &&
          !current_archive)
      {
        continue;
      }

      // Cube faces observed in Frontline are square and reasonably large.
      // This stops ordinary 32/64 px material textures selecting a sky face.
      if (exact_sky &&
          (width < 128 ||
           height < 128 ||
           gc_aspect < 0.90f ||
           gc_aspect > 1.10f))
      {
        continue;
      }

      const float candidate_aspect =
          static_cast<float>(
              candidate.width) /
          static_cast<float>(
              candidate.height);

      const float aspect_error =
          std::abs(
              std::log(
                  std::max(
                      gc_aspect,
                      0.0001f) /
                  std::max(
                      candidate_aspect,
                      0.0001f)));

      if (aspect_error > 0.08f)
        continue;

      const float scale_x =
          static_cast<float>(
              candidate.width) /
          static_cast<float>(
              width);

      const float scale_y =
          static_cast<float>(
              candidate.height) /
          static_cast<float>(
              height);

      if (!IsPowerOfTwoLikeScale(scale_x) ||
          !IsPowerOfTwoLikeScale(scale_y))
      {
        continue;
      }

      const float scale_shape =
          std::abs(
              std::log(
                  std::max(
                      scale_x,
                      0.001f) /
                  std::max(
                      scale_y,
                      0.001f)));

      if (scale_shape > 0.10f)
        continue;

      const float score =
          Auto3DDistance(
              gc,
              candidate.fingerprint) +
          aspect_error * 0.30f +
          scale_shape * 0.12f;

      Winner& target =
          exact_sky ?
              sky :
              level;

      if (score < target.best)
      {
        target.second =
            target.best;

        target.best =
            score;

        target.path =
            candidate.relative_path;
      }
      else if (score < target.second)
      {
        target.second =
            score;
      }
    }
  }

  // Slightly more tolerant than v13.8 because the PS3 remaster sky has
  // resolution/color changes, but still requires a unique direct sky face.
  const bool sky_confident =
      SkyTexturesEnabled() &&
      !sky.path.empty() &&
      sky.best <= 0.30f &&
      (!std::isfinite(sky.second) ||
       sky.second - sky.best >= 0.008f);

  const bool level_confident =
      !level.path.empty() &&
      level.best <= 0.12f &&
      (!std::isfinite(level.second) ||
       level.second - level.best >= 0.055f);

  // Bias toward the exact six-face PS3 sky set when it is a credible match.
  const bool choose_sky =
      sky_confident &&
      (!level_confident ||
       sky.best <=
           level.best + 0.030f);

  const std::string chosen =
      choose_sky ?
          sky.path :
          (level_confident ?
               level.path :
               std::string{});

  if (chosen.empty())
  {
    {
      std::scoped_lock lock(
          auto3d_mutex);

      strict_level_rejected.insert(
          key);
    }

    static unsigned sky_reject_logs = 0;

    if (std::isfinite(sky.best) &&
        sky_reject_logs++ < 32)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-sky] direct-face candidate rejected: "
          "GC=%ux%u best=%.3f second=%.3f scope=%s\n",
          width,
          height,
          sky.best,
          sky.second,
          scope.c_str());
    }

    return nullptr;
  }

  auto decoded =
      DecodeAuto3DWinner(
          chosen);

  if (!decoded)
  {
    std::scoped_lock lock(
        auto3d_mutex);

    strict_level_rejected.insert(
        key);

    return nullptr;
  }

  {
    std::scoped_lock lock(
        auto3d_mutex);

    strict_level_matches[key] =
        decoded;
  }

  static unsigned strict_logs = 0;

  if (strict_logs++ < 160)
  {
    std::fprintf(
        stderr,
        choose_sky ?
            "[moh-ps3-sky] DIRECT PS3 MATCH: "
            "GC=%ux%u fmt=%u -> %s score=%.3f\n" :
            "[moh-ps3-level] STRICT MATCH: "
            "GC=%ux%u fmt=%u -> %s score=%.3f\n",
        width,
        height,
        static_cast<unsigned>(
            info.GetTextureFormat()),
        chosen.c_str(),
        choose_sky ?
            sky.best :
            level.best);
  }

  return decoded;
}


struct ExactTPKEntry
{
  const char* name;
  u32 gc_width;
  u32 gc_height;
  u32 gc_format;
  u64 gc_fnv1a;
};

constexpr std::array<ExactTPKEntry, 28> exact_tpk_1_1 = {{
    {"BT01", 256u, 256u, 14u, 0x91AAA17C8BE3668EULL},
    {"BT01C", 256u, 256u, 14u, 0x424E462DA6474CFCULL},
    {"BT02", 256u, 256u, 14u, 0x87FB7A6A012799CEULL},
    {"BT21", 256u, 256u, 14u, 0x77E03F08BD37ACB7ULL},
    {"COL_CEN_256_5", 128u, 128u, 14u, 0x4D42F7AD84A2C713ULL},
    {"COL_SID_256_4", 128u, 128u, 14u, 0xFDF454F580635F80ULL},
    {"FRAG_BODY_256", 128u, 128u, 14u, 0x56F1983ED05C48CEULL},
    {"FRAG_HAN_256", 128u, 128u, 14u, 0x0F9ABB33391CB13FULL},
    {"GI_256", 256u, 256u, 14u, 0xF011AF037428812EULL},
    {"GR1", 128u, 128u, 14u, 0xF2C4E7B953EFCA9CULL},
    {"GR2", 128u, 128u, 14u, 0xA8C13BBE27E6B6C5ULL},
    {"GR3", 128u, 128u, 14u, 0x0662F8529D47E395ULL},
    {"GR5", 128u, 128u, 14u, 0x13CBD313C9F66E76ULL},
    {"HT01", 128u, 128u, 14u, 0xBEC54A7E8D7F7150ULL},
    {"HT01B", 128u, 128u, 14u, 0x74187B88515D0795ULL},
    {"HT02", 128u, 128u, 14u, 0x404BC45F9FFD7F7EULL},
    {"HT02A", 128u, 128u, 14u, 0xCFD78001918B8D0CULL},
    {"HT02B", 128u, 128u, 14u, 0xEC5533A189E69E31ULL},
    {"HT03", 128u, 128u, 14u, 0xFB913EE29EC39055ULL},
    {"HT03A", 128u, 128u, 14u, 0x102D78F3CD3B7DB3ULL},
    {"M1SIDE_256", 256u, 256u, 14u, 0x82CD824A3FABDB7FULL},
    {"M1TOP_256", 256u, 256u, 14u, 0xB0712EEC49CC1DECULL},
    {"PISTOL_GRAFT", 128u, 128u, 14u, 0xFF7DB7AE35A9A61BULL},
    {"US1", 128u, 128u, 14u, 0x8AFF22A604A666B9ULL},
    {"US2", 128u, 128u, 14u, 0x4901FF9A979A9ABDULL},
    {"US3", 128u, 128u, 14u, 0xAB193588A2678C2AULL},
    {"US7_LEFT", 64u, 64u, 14u, 0x34DEB128FED33D77ULL},
    {"US7_RIGHT", 32u, 32u, 14u, 0x04265D3B02F8F7F8ULL},
}};

std::unordered_map<std::string, std::shared_ptr<VideoCommon::CustomTextureData>> exact_tpk_decoded;

u32 TPKBE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

void TPKWriteBE32(std::vector<u8>* out, std::size_t offset, u32 value)
{
  if (!out || offset + 4 > out->size())
    return;
  (*out)[offset + 0] = static_cast<u8>(value >> 24);
  (*out)[offset + 1] = static_cast<u8>(value >> 16);
  (*out)[offset + 2] = static_cast<u8>(value >> 8);
  (*out)[offset + 3] = static_cast<u8>(value);
}

u64 ExactFNV1a64(const u8* data, std::size_t size)
{
  u64 hash = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < size; ++i)
  {
    hash ^= data[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

const PS3RemasterAssets::AssetInfo* FindScopedPS3Asset(std::string_view scope, std::string_view filename)
{
  const std::string wanted = Lower(std::string(filename));
  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    if (Lower(asset.filename) != wanted)
      continue;
    const std::string relative = Normalize(asset.relative_path);
    if (scope.empty() || relative.starts_with(scope))
      return &asset;
  }
  return nullptr;
}

std::shared_ptr<VideoCommon::CustomTextureData>
BuildCustomTextureFromPS3Levels(const std::vector<PS3TextureDecoder::Level>& levels)
{
  if (levels.empty())
    return nullptr;
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
  return decoded;
}

bool ParsePS3TPKRecord(std::span<const u8> tpk, std::string_view wanted_name,
                       u32* payload_offset, u32* payload_size,
                       std::array<u8, 24>* descriptor)
{
  if (!payload_offset || !payload_size || !descriptor || tpk.size() < 32 ||
      tpk[0] != 'T' || tpk[1] != 'P' || tpk[2] != 'A' || tpk[3] != 'C')
    return false;

  const u32 count = TPKBE32(tpk.data() + 8);
  const u32 names_offset = TPKBE32(tpk.data() + 12);
  const u32 pointer_table = TPKBE32(tpk.data() + 16);

  if (!count || count > 512 || names_offset >= tpk.size() ||
      pointer_table >= tpk.size() ||
      std::uint64_t(pointer_table) + std::uint64_t(count) * 4u > tpk.size())
    return false;

  for (u32 i = 0; i < count; ++i)
  {
    const std::size_t name_pos = std::size_t(names_offset) + std::size_t(i) * 32u;
    if (name_pos + 32 > tpk.size())
      return false;

    std::size_t length = 0;
    while (length < 32 && tpk[name_pos + length] != 0)
      ++length;

    const std::string_view name(reinterpret_cast<const char*>(tpk.data() + name_pos), length);
    if (name != wanted_name)
      continue;

    const u32 record_offset = TPKBE32(tpk.data() + pointer_table + i * 4u);
    if (std::uint64_t(record_offset) + 48u > tpk.size())
      return false;

    const u8* record = tpk.data() + record_offset;
    *payload_size = TPKBE32(record + 4);
    *payload_offset = TPKBE32(record + 8);
    std::copy_n(record + 16, descriptor->size(), descriptor->begin());
    return true;
  }

  return false;
}

std::shared_ptr<VideoCommon::CustomTextureData>
DecodeExactPS3TPKTexture(std::string_view scope, std::string_view name)
{
  std::string level_path(scope);
  while (level_path.ends_with('/')) level_path.pop_back();
  const auto levels = MOHFrontline::Materials::LoadTexture(Filename(level_path), name);
  return levels ? BuildCustomTextureFromPS3Levels(*levels) : nullptr;
}

std::shared_ptr<VideoCommon::CustomTextureData>
FindExactTPK1_1(const TextureInfo& info)
{
  if (!PS3AssetPort::IsTPKRSXEnabled())
    return nullptr;

  std::string scope;
  {
    std::scoped_lock lock(auto3d_mutex);
    scope = auto3d_level_scope;
  }

  if (scope != "data/1/1_1/")
    return nullptr;

  if (info.GetTextureFormat() != TextureFormat::CMPR ||
      !info.GetData() || !info.GetTextureSize())
    return nullptr;

  const u64 hash = ExactFNV1a64(info.GetData(), info.GetTextureSize());

  for (const auto& entry : exact_tpk_1_1)
  {
    if (entry.gc_width != info.GetRawWidth() ||
        entry.gc_height != info.GetRawHeight() ||
        entry.gc_format != static_cast<u32>(info.GetTextureFormat()) ||
        entry.gc_fnv1a != hash)
      continue;

    auto decoded = DecodeExactPS3TPKTexture(scope, entry.name);
    if (decoded)
    {
      std::fprintf(stderr,
                   "[moh-ps3-tpk] EXACT MATCH: GC %ux%u fmt=%u hash=%016llX -> %s\n",
                   info.GetRawWidth(), info.GetRawHeight(),
                   static_cast<unsigned>(info.GetTextureFormat()),
                   static_cast<unsigned long long>(hash), entry.name);
    }
    return decoded;
  }

  return nullptr;
}


struct LevelPortTextureEntry
{
  std::string level;
  std::string name;
  u32 width = 0;
  u32 height = 0;
  u32 gx_format = 0;
  u64 texture_hash = 0;
  u64 palette_hash = 0;
  u32 palette_format = 0;
  std::filesystem::path texture_path;
  std::filesystem::path normal_path;
};

std::mutex level_port_mutex;
bool level_port_manifest_loaded = false;
std::filesystem::path level_port_root;
std::vector<LevelPortTextureEntry> level_port_textures;

std::unordered_map<
    std::string,
    std::shared_ptr<VideoCommon::CustomTextureData>>
    level_port_decoded;

bool LevelPortEnabled()
{
  if (!PS3AssetPort::IsTPKRSXEnabled())
    return false;

  const char* value = std::getenv("MOH_PS3_LEVEL_PORT");

  // Exact all-level TPK/RSX material replacement is enabled with the TPK/RSX
  // path by default. Keep the legacy variable as an explicit kill switch.
  if (!value || !*value)
    return true;

  const std::string lower = Lower(std::string(value));
  return lower != "0" && lower != "false" && lower != "off" && lower != "no";
}

std::filesystem::path ResolveLevelPortRoot()
{
  if (const char* value =
          std::getenv("MOH_PS3_PORT_CACHE");
      value && *value)
  {
    return std::filesystem::path(value);
  }

  if (const char* value =
          std::getenv("MOH_PS3_FILES");
      value && *value)
  {
    const auto ps3 =
        std::filesystem::path(value);

    return
        ps3.parent_path() /
        "PS3_PORT_CACHE";
  }

  return {};
}

std::vector<std::string_view>
SplitTSVLine(std::string_view line)
{
  std::vector<std::string_view> fields;
  std::size_t begin = 0;

  while (begin <= line.size())
  {
    const std::size_t tab =
        line.find('\t', begin);

    if (tab == std::string_view::npos)
    {
      fields.emplace_back(line.substr(begin));
      break;
    }

    fields.emplace_back(
        line.substr(begin, tab - begin));

    begin = tab + 1;
  }

  return fields;
}

u64 ParseHex64NoThrow(std::string_view text)
{
  if (text.empty())
    return 0;

  const std::string temp(text);
  char* end = nullptr;

  return static_cast<u64>(
      std::strtoull(
          temp.c_str(),
          &end,
          16));
}

u32 ParseU32NoThrow(std::string_view text)
{
  if (text.empty())
    return 0;

  const std::string temp(text);
  char* end = nullptr;

  return static_cast<u32>(
      std::strtoul(
          temp.c_str(),
          &end,
          10));
}

void LoadLevelPortManifestLocked()
{
  if (level_port_manifest_loaded)
    return;

  level_port_manifest_loaded = true;
  level_port_root = ResolveLevelPortRoot();

  if (level_port_root.empty())
  {
    std::fprintf(
        stderr,
        "[moh-ps3-port] no cache root; "
        "set MOH_PS3_PORT_CACHE or MOH_PS3_FILES\n");

    return;
  }

  const auto manifest =
      level_port_root /
      "exact-textures.tsv";

  std::ifstream file(manifest);

  if (!file)
  {
    std::fprintf(
        stderr,
        "[moh-ps3-port] cache manifest missing: %s\n",
        manifest.string().c_str());

    return;
  }

  std::string line;

  // TSV header.
  if (!std::getline(file, line))
    return;

  std::size_t rows = 0;

  while (std::getline(file, line))
  {
    if (line.empty())
      continue;

    const auto f =
        SplitTSVLine(line);

    // level, name, width, height, gx_format,
    // texture_fnv1a64, palette_fnv1a64, palette_format,
    // ps3_texture, ps3_normal, ...
    if (f.size() < 10)
      continue;

    LevelPortTextureEntry entry;

    entry.level = std::string(f[0]);
    entry.name = std::string(f[1]);
    entry.width = ParseU32NoThrow(f[2]);
    entry.height = ParseU32NoThrow(f[3]);
    entry.gx_format = ParseU32NoThrow(f[4]);
    entry.texture_hash = ParseHex64NoThrow(f[5]);
    entry.palette_hash = ParseHex64NoThrow(f[6]);
    entry.palette_format = ParseU32NoThrow(f[7]);

    entry.texture_path =
        level_port_root /
        std::filesystem::path(
            std::string(f[8]));

    if (!f[9].empty())
    {
      entry.normal_path =
          level_port_root /
          std::filesystem::path(
              std::string(f[9]));
    }

    if (!entry.level.empty() &&
        entry.width &&
        entry.height &&
        entry.texture_hash &&
        !entry.texture_path.empty())
    {
      level_port_textures.push_back(
          std::move(entry));

      ++rows;
    }
  }

  std::fprintf(
      stderr,
      "[moh-ps3-port] all-level exact material cache: "
      "%zu entries from %s\n",
      rows,
      manifest.string().c_str());
}

std::string CurrentLevelPortId()
{
  std::scoped_lock lock(auto3d_mutex);

  std::string scope =
      auto3d_level_scope;

  if (scope.empty())
    return {};

  while (!scope.empty() &&
         scope.back() == '/')
  {
    scope.pop_back();
  }

  const auto slash =
      scope.find_last_of('/');

  if (slash != std::string::npos)
    scope.erase(0, slash + 1);

  return scope;
}

std::shared_ptr<VideoCommon::CustomTextureData>
DecodeLevelPortFile(
    const std::filesystem::path& path)
{
  const std::string key =
      path.string();

  {
    std::scoped_lock lock(level_port_mutex);

    if (const auto it =
            level_port_decoded.find(key);
        it != level_port_decoded.end())
    {
      return it->second;
    }
  }

  std::ifstream file(
      path,
      std::ios::binary |
      std::ios::ate);

  if (!file)
    return nullptr;

  const std::streamoff end =
      file.tellg();

  if (end <= 0 ||
      end >
          512ll *
              1024ll *
              1024ll)
  {
    return nullptr;
  }

  file.seekg(0, std::ios::beg);

  std::vector<u8> binary(
      static_cast<std::size_t>(end));

  if (!file.read(
          reinterpret_cast<char*>(
              binary.data()),
          static_cast<std::streamsize>(
              binary.size())))
  {
    return nullptr;
  }

  std::vector<
      PS3TextureDecoder::Level>
      levels;

  if (!PS3TextureDecoder::Decode(
          std::span<const u8>(
              binary.data(),
              binary.size()),
          &levels) ||
      levels.empty())
  {
    std::fprintf(
        stderr,
        "[moh-ps3-port] cached GTF decode failed: %s\n",
        path.string().c_str());

    return nullptr;
  }

  auto decoded =
      BuildCustomTextureFromPS3Levels(
          levels);

  if (!decoded)
    return nullptr;

  {
    std::scoped_lock lock(level_port_mutex);
    level_port_decoded[key] = decoded;
  }

  return decoded;
}

std::shared_ptr<VideoCommon::CustomTextureData>
FindExactLevelPortTexture(
    const TextureInfo& info)
{
  if (!LevelPortEnabled())
  {
    static bool uv_safe_logged = false;

    if (!uv_safe_logged)
    {
      uv_safe_logged = true;

      std::fprintf(
          stderr,
          "[moh-ps3-port] exact PS3 TPK/RSX materials are OFF "
          "(MOH_PS3_TPK_RSX=0 or MOH_PS3_LEVEL_PORT=0)\n");
    }

    return nullptr;
  }

  if (!info.GetData() ||
      !info.GetTextureSize())
  {
    return nullptr;
  }

  const std::string level =
      CurrentLevelPortId();

  if (level.empty())
    return nullptr;

  {
    std::scoped_lock lock(level_port_mutex);
    LoadLevelPortManifestLocked();
  }

  const u64 texture_hash =
      ExactFNV1a64(
          info.GetData(),
          info.GetTextureSize());

  u64 palette_hash = 0;

  if (const auto palette_size =
          info.GetPaletteSize();
      palette_size &&
      *palette_size &&
      info.GetTlutAddress())
  {
    palette_hash =
        ExactFNV1a64(
            reinterpret_cast<const u8*>(
                info.GetTlutAddress()),
            *palette_size);
  }

  const u32 width = info.GetRawWidth();
  const u32 height = info.GetRawHeight();

  const u32 format =
      static_cast<u32>(
          info.GetTextureFormat());

  LevelPortTextureEntry found;
  bool has_found = false;

  {
    std::scoped_lock lock(level_port_mutex);

    for (const auto& entry :
         level_port_textures)
    {
      if (entry.level != level ||
          entry.width != width ||
          entry.height != height ||
          entry.gx_format != format ||
          entry.texture_hash != texture_hash)
      {
        continue;
      }

      if (entry.palette_hash &&
          entry.palette_hash != palette_hash)
      {
        continue;
      }

      found = entry;
      has_found = true;
      break;
    }
  }

  if (!has_found)
    return nullptr;

  // The manifest remains useful as an exact GC identity table, but the cached
  // texture payloads were produced by an older TPK extractor which pre-dates
  // the corrected TPAC data-base handling.  Do not upload those stale files.
  //
  // Resolve the material name back through the live TPK catalog and decode the
  // texture directly from the current level's rsx.viv instead.
  const auto underscore = level.find('_');
  if (underscore == std::string::npos || underscore == 0)
    return nullptr;

  const std::string scope =
      "data/" + level.substr(0, underscore) + "/" + level + "/";

  auto decoded =
      DecodeExactPS3TPKTexture(
          scope,
          found.name);

  // Explicit debug-only escape hatch for comparing against an old generated
  // PS3_PORT_CACHE.  Production fallback is the original GameCube texture.
  if (!decoded)
  {
    const char* legacy_cache =
        std::getenv("MOH_PS3_LEGACY_PORT_CACHE");

    if (legacy_cache &&
        std::string_view(legacy_cache) == "1")
    {
      decoded =
          DecodeLevelPortFile(
              found.texture_path);
    }
  }

  if (!decoded)
  {
    static unsigned live_fail_logs = 0;
    if (live_fail_logs++ < 64)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-tpk] LIVE EXACT fallback to GC: "
          "level=%s material=%s hash=%016llX\n",
          level.c_str(),
          found.name.c_str(),
          static_cast<unsigned long long>(
              texture_hash));
    }
    return nullptr;
  }

  std::fprintf(
      stderr,
      "[moh-ps3-tpk] LIVE EXACT MATERIAL: "
      "level=%s GC=%ux%u fmt=%u hash=%016llX "
      "-> TPK::%s%s%s\n",
      level.c_str(),
      width,
      height,
      format,
      static_cast<unsigned long long>(
          texture_hash),
      found.name.c_str(),
      found.normal_path.empty() ?
          "" :
          " normal=",
      found.normal_path.empty() ?
          "" :
          found.normal_path.filename()
              .string()
              .c_str());

  return decoded;
}

bool Auto3DFuzzyEnabled()
{
  const char* value = std::getenv("MOH_PS3_AUTO_3D_FUZZY");
  if (!value || !*value)
    return false;

  const std::string lower = Lower(std::string(value));
  return lower == "1" || lower == "true" || lower == "on" || lower == "yes";
}

std::shared_ptr<VideoCommon::CustomTextureData>
FindAuto3D(const TextureInfo& info)
{
  static bool mode_logged = false;
  if (!mode_logged)
  {
    mode_logged = true;
    std::fprintf(stderr,
                 "[moh-ps3-policy] sky=%s TPK/RSX=%s MSH-decoder=%s DMF-decoder=%s strict-level=%s\n",
                 SkyTexturesEnabled() ? "ON" : "OFF",
                 PS3AssetPort::IsTPKRSXEnabled() ? "ON" : "OFF",
                 PS3AssetPort::IsMSHEnabled() ? "ON" : "OFF",
                 PS3AssetPort::IsDMFEnabled() ? "ON" : "OFF",
                 StrictLevelTexturesEnabled() ? "ON" : "OFF");
  }

  if (!Auto3DEnabled() ||
      !MohPcLayer::IsPS3TextureReplacementEnabled() ||
      !PS3RemasterAssets::IsReady() ||
      !info.IsDataValid() ||
      info.IsFromTmem() ||
      info.GetTextureFormat() == TextureFormat::XFB ||
      !info.GetData() ||
      !info.GetTextureSize())
  {
    return nullptr;
  }

  // Exact TPK/RSX material replacements are safe to run by default: both
  // paths require an exact GC identity (level + dimensions/format + hash).
  // Keeping them behind MOH_PS3_LEGACY_TPK_HASH made TPK/RSX report ON while
  // no TPK texture could reach the renderer.
  if (PS3AssetPort::IsTPKRSXEnabled())
  {
    static bool exact_tpk_logged = false;
    if (!exact_tpk_logged)
    {
      exact_tpk_logged = true;
      std::fprintf(stderr, "[moh-ps3-tpk] exact runtime bridge ON\n");
    }

    if (auto exact = FindExactLevelPortTexture(info)) return exact;
    if (auto exact = FindExactTPK1_1(info)) return exact;
  }

  // Sky/current-level SSH matching is next; broad fuzzy matching remains opt-in.
  if (auto strict = FindStrictCurrentLevelTexture(info))
    return strict;

  if (!Auto3DFuzzyEnabled())
    return nullptr;

  const u32 width = info.GetRawWidth();
  const u32 height = info.GetRawHeight();

  if (width < 8 ||
      height < 8 ||
      width > 2048 ||
      height > 2048)
  {
    return nullptr;
  }

  const u64 key = Auto3DKey(info);
  std::string scope;

  {
    std::scoped_lock lock(auto3d_mutex);

    scope = auto3d_level_scope;

    if (scope.empty())
      return nullptr;

    if (const auto it = auto3d_matches.find(key);
        it != auto3d_matches.end())
    {
      return it->second;
    }

    if (auto3d_rejected.contains(key))
      return nullptr;

    BuildAuto3DCandidatesLocked();
  }

  std::vector<u8> gc_rgba(
      std::size_t(width) *
      height *
      4u);

  TexDecoder_Decode(
      gc_rgba.data(),
      info.GetData(),
      static_cast<int>(width),
      static_cast<int>(height),
      info.GetTextureFormat(),
      info.GetTlutAddress(),
      info.GetTlutFormat());

  const Auto3DFingerprint gc =
      MakeAuto3DFingerprint(
          gc_rgba.data(),
          width,
          height);

  if (gc.contrast < 0.018f)
  {
    std::scoped_lock lock(auto3d_mutex);
    auto3d_rejected.insert(key);
    return nullptr;
  }

  const float gc_aspect =
      static_cast<float>(width) /
      static_cast<float>(height);

  std::string best_path;
  bool best_is_cube_face = false;

  float best_score =
      std::numeric_limits<float>::infinity();

  float second_score =
      std::numeric_limits<float>::infinity();

  {
    std::scoped_lock lock(auto3d_mutex);

    for (const auto& candidate :
         auto3d_candidates)
    {
      const float candidate_aspect =
          static_cast<float>(candidate.width) /
          static_cast<float>(candidate.height);

      const float aspect_error =
          std::abs(
              std::log(
                  std::max(gc_aspect, 0.0001f) /
                  std::max(candidate_aspect, 0.0001f)));

      if (aspect_error > 0.10f)
        continue;

      const float scale_x =
          static_cast<float>(candidate.width) /
          static_cast<float>(width);

      const float scale_y =
          static_cast<float>(candidate.height) /
          static_cast<float>(height);

      if (!IsPowerOfTwoLikeScale(scale_x) ||
          !IsPowerOfTwoLikeScale(scale_y))
      {
        continue;
      }

      const float scale_shape =
          std::abs(
              std::log(
                  std::max(scale_x, 0.001f) /
                  std::max(scale_y, 0.001f)));

      if (scale_shape > 0.12f)
        continue;

      const float score =
          Auto3DDistance(
              gc,
              candidate.fingerprint) +
          aspect_error * 0.30f +
          scale_shape * 0.12f;

      if (score < best_score)
      {
        second_score = best_score;
        best_score = score;
        best_path = candidate.relative_path;
        best_is_cube_face =
            IsLikelyCubeFaceCandidate(
                candidate.relative_path);
      }
      else if (score < second_score)
      {
        second_score = score;
      }
    }
  }

  const float threshold =
      static_cast<float>(
          Auto3DThreshold());

  const float margin =
      static_cast<float>(
          Auto3DMargin());

  const bool extremely_strong =
      best_score < 0.10f;

  const bool cube_confident =
      !best_is_cube_face ||
      best_score < 0.15f;

  const bool confident =
      std::isfinite(best_score) &&
      best_score <= threshold &&
      cube_confident &&
      (extremely_strong ||
       !std::isfinite(second_score) ||
       second_score - best_score >= margin);

  if (!confident ||
      best_path.empty())
  {
    {
      std::scoped_lock lock(auto3d_mutex);
      auto3d_rejected.insert(key);
    }

    static unsigned anonymous_logs = 0;

    if (anonymous_logs < 64)
    {
      ++anonymous_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-auto3d] anonymous GC: "
          "%ux%u fmt=%u stage=%u hash=%016llX scope=%s\n",
          width,
          height,
          static_cast<unsigned>(
              info.GetTextureFormat()),
          info.GetStage(),
          static_cast<unsigned long long>(
              key),
          scope.c_str());
    }

    static unsigned reject_logs = 0;

    if (reject_logs < 96)
    {
      ++reject_logs;

      std::fprintf(
          stderr,
          "[moh-ps3-auto3d] reject: "
          "GC=%ux%u fmt=%u scope=%s "
          "best=%.3f second=%.3f threshold=%.3f margin=%.3f cube=%d\n",
          width,
          height,
          static_cast<unsigned>(
              info.GetTextureFormat()),
          scope.c_str(),
          best_score,
          second_score,
          threshold,
          margin,
          best_is_cube_face ? 1 : 0);
    }

    return nullptr;
  }

  auto decoded =
      DecodeAuto3DWinner(best_path);

  if (!decoded)
  {
    std::scoped_lock lock(auto3d_mutex);
    auto3d_rejected.insert(key);
    return nullptr;
  }

  {
    std::scoped_lock lock(auto3d_mutex);
    auto3d_matches[key] = decoded;
  }

  std::fprintf(
      stderr,
      "[moh-ps3-auto3d] MATCH: "
      "GC=%ux%u fmt=%u stage=%u -> %s "
      "score=%.3f second=%.3f\n",
      width,
      height,
      static_cast<unsigned>(
          info.GetTextureFormat()),
      info.GetStage(),
      best_path.c_str(),
      best_score,
      second_score);

  return decoded;
}


}  // namespace

int NameIndex(std::string_view name)
{
  UpdateLevelScopeFromGuestName(name);

  const std::string sky_path = PS3NamedSky::RelativePath(name);
  const PS3RemasterAssets::AssetInfo* asset = nullptr;
  if (!sky_path.empty())
  {
    // Missing or disabled exact faces must not enter any other resolver.
    if (!SkyTexturesEnabled())
      return -1;
    asset = PS3RemasterAssets::FindByRelativePath(sky_path);
    if (!asset)
    {
      std::fprintf(stderr, "[moh-ps3-sky] NAMED SKY FALLBACK: %.*s -> %s (absent)\n",
                   static_cast<int>(name.size()), name.data(), sky_path.c_str());
      return -1;
    }
    SetAuto3DLevelScope(sky_path.substr(0, sky_path.find_last_of('/') + 1),
                        "named GC sky face");
  }
  else
  {
    // Prefer exact current-level TPK material data while the original GC name
    // is still available. TPKIndex() creates a decoded Resource backed by the
    // PS3 rsx.viv payload; Register()/Find() keep the normal exact GC
    // address/hash validation before the replacement reaches TextureCache.
    const std::string normalized_guest = Normalize(std::string(name));
    const std::string guest_file = Filename(name);

    const bool frontend_or_global =
        normalized_guest.find("mohfl_exports/") != std::string::npos ||
        normalized_guest.find("pausescreen/") != std::string::npos ||
        normalized_guest.find("bitmaps/") != std::string::npos ||
        normalized_guest.find("loading/") != std::string::npos ||
        normalized_guest.find("shell/") != std::string::npos;

    if (!frontend_or_global && guest_file.ends_with(".gsh"))
    {
      std::string tpk_name = guest_file;
      tpk_name.resize(tpk_name.size() - 4);

      if (const int tpk_index = TPKIndex(tpk_name); tpk_index >= 0)
      {
        static unsigned tpk_bind_logs = 0;
        if (tpk_bind_logs++ < 160)
        {
          std::fprintf(stderr,
                       "[moh-ps3-tpk] NAME BIND: guest=%.*s -> current-level TPK::%s\n",
                       static_cast<int>(name.size()), name.data(), tpk_name.c_str());
        }
        return tpk_index;
      }
    }

    asset = FindBestAsset(name);
  }

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
          "[moh-ps3-texture] no PS3 match: guest=%.*s stem=%s\n",
          static_cast<int>(
              name.size()),
          name.data(),
          StemKey(name)
              .c_str());
    }

    return -1;
  }

  UpdateAuto3DScope(
      name,
      *asset);

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

  if (!sky_path.empty())
  {
    std::fprintf(stderr, "[moh-ps3-sky] NAMED SKY GC: %.*s\n",
                 static_cast<int>(name.size()), name.data());
    std::fprintf(stderr, "[moh-ps3-sky] NAMED SKY PS3: %s\n", sky_path.c_str());
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
        "[moh-ps3-texture] map guest=%.*s -> PS3=%s\n",
        static_cast<int>(
            name.size()),
        name.data(),
        key.c_str());
  }

  return id;
}


int MaterialIndex(std::string key, std::shared_ptr<VideoCommon::CustomTextureData> data)
{
  if (!data) return -1;
  std::scoped_lock lock(mutex);
  if (auto it = resource_ids.find(key); it != resource_ids.end()) return it->second;
  const int id = next_resource_id++;
  Resource r;
  r.filename = Filename(key);
  r.relative_path = key;
  r.decoded = std::move(data);
  r.attempted = true;
  resource_ids.emplace(std::move(key), id);
  resources.emplace(id, std::move(r));
  return id;
}

int TPKIndex(std::string_view name)
{
  if (!PS3AssetPort::IsTPKRSXEnabled()) return -1;
  const auto level = MOHFrontline::NativeAssets::GetCurrentLevel();
  if (level.empty()) return -1;
  const auto decoded = MOHFrontline::Materials::LoadTexture(level, name);
  if (!decoded) return -1;
  return MaterialIndex("data/" + level.substr(0, 1) + "/" + level +
                       "/level.viv::tpk" + level + ".tpk::" + std::string(name),
                       BuildCustomTextureFromPS3Levels(*decoded));
}

void MarkNamedSkyAddress(u32 address)
{
  std::scoped_lock lock(mutex);
  address &= 0x1FFFFFFF;
  named_sky_addresses.insert(address);
  registrations.erase(address);  // A reloaded level may reuse the allocation.
  uploaded_named_sky.erase(address);
  named_sky_invalidations.insert(address);
}

void NotifyTextureUploaded(const TextureInfo& info)
{
  std::scoped_lock lock(mutex);
  const u32 address = info.GetRawAddress();
  if (uploaded_named_sky.contains(address))
    return;
  const auto it = registrations.find(address);
  if (it == registrations.end())
    return;
  const auto resource = resources.find(it->second.resource_id);
  if (resource == resources.end())
    return;
  const auto& path = resource->second.relative_path;
  if (path.find(".tpk::") != std::string::npos)
  {
    uploaded_named_sky.insert(address);
    std::fprintf(stderr, "[moh-ps3-tpk] ACTIVE GC-material=%08x -> PS3=%s\n",
                 address, path.c_str());
    return;
  }
  if (!named_sky_addresses.contains(address)) return;
  uploaded_named_sky.insert(address);
  std::string face = path.substr(path.size() - 6, 2);
  std::transform(face.begin(), face.end(), face.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  std::fprintf(stderr,
               "[moh-ps3-sky] NAMED SKY ACTIVE: GC=%08x hash=%016llx face=%s -> %s\n",
               address, static_cast<unsigned long long>(it->second.texture_hash),
               face.c_str(), path.c_str());
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

  if (PS3NamedSky::RelativePath(resource_it->second.relative_path).empty())
  {
    named_sky_addresses.erase(address & 0x1FFFFFFF);
    uploaded_named_sky.erase(address & 0x1FFFFFFF);
  }

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

  bool has_exact_registration = false;

  {
    std::scoped_lock lock(mutex);

    has_exact_registration =
        registrations.find(address) !=
        registrations.end();
  }

  if (!has_exact_registration)
  {
    {
      std::scoped_lock lock(mutex);
      if (named_sky_addresses.contains(address))
        return nullptr;
    }
    return FindAuto3D(info);
  }

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

std::vector<u32> ConsumeSkyCacheInvalidations()
{
  std::scoped_lock lock(mutex);
  std::vector<u32> addresses(named_sky_invalidations.begin(), named_sky_invalidations.end());
  named_sky_invalidations.clear();
  return addresses;
}

void Shutdown()
{
  {
    std::scoped_lock auto_lock(auto3d_mutex);
    auto3d_level_scope.clear();
    auto3d_built_scope.clear();
    auto3d_candidates.clear();
    auto3d_matches.clear();
    auto3d_rejected.clear();
    strict_level_matches.clear();
    strict_level_rejected.clear();
    sky_gc_probes.clear();
    sky_exact_assignments.clear();
    sky_assignment_logged = false;
    sky_cache_invalidation_pending = false;
    exact_tpk_decoded.clear();
    level_port_decoded.clear();
    level_port_textures.clear();
    level_port_manifest_loaded = false;
    level_port_root.clear();
  }


  std::scoped_lock lock(mutex);
  registrations.clear();
  named_sky_addresses.clear();
  uploaded_named_sky.clear();
  named_sky_invalidations.clear();
  resources.clear();
  resource_ids.clear();
  next_resource_id = 0;
}
}  // namespace PS3Compass
