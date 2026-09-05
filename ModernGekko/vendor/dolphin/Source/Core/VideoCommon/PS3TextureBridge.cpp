#include "VideoCommon/PS3TextureBridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/Buffer.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/PS3SshDecoder.h"

namespace PS3TextureBridge
{
namespace
{
constexpr int GRID = 8;
constexpr int CELLS = GRID * GRID;

struct Fingerprint
{
  std::array<float, CELLS> values{};

  float mean = 0.0f;
  float variance = 0.0f;
};

struct Candidate
{
  std::filesystem::path file;

  std::string tag;

  u32 width = 0;
  u32 height = 0;

  std::uint8_t source_type = 0;

  Fingerprint fp;

  std::shared_ptr<VideoCommon::CustomTextureData>
      cached_texture;
};

struct Match
{
  int candidate = -1;
  float score = 999.0f;
};

bool s_enabled = false;
float s_threshold = 0.12f;

std::filesystem::path s_root;

std::vector<Candidate> s_candidates;

std::unordered_map<u64, Match> s_match_cache;

std::string Lower(std::string value)
{
  std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](unsigned char c)
      {
        return static_cast<char>(
            std::tolower(c));
      });

  return value;
}

bool Contains(
    const std::string& value,
    const std::string& needle)
{
  return value.find(needle) !=
         std::string::npos;
}

bool IsFalse(const char* value)
{
  if (!value || !*value)
    return false;

  const std::string v =
      Lower(value);

  return
      v == "0" ||
      v == "off" ||
      v == "false" ||
      v == "no";
}

Fingerprint MakeFingerprint(
    const u8* rgba,
    u32 width,
    u32 height,
    u32 row_length)
{
  Fingerprint fp;

  if (!rgba ||
      width == 0 ||
      height == 0 ||
      row_length < width)
  {
    return fp;
  }

  for (int gy = 0; gy < GRID; ++gy)
  {
    for (int gx = 0; gx < GRID; ++gx)
    {
      const u32 x0 =
          static_cast<u32>(
              (std::uint64_t(gx) *
               width) /
              GRID);

      const u32 x1 =
          std::max<u32>(
              x0 + 1,
              static_cast<u32>(
                  (std::uint64_t(gx + 1) *
                   width) /
                  GRID));

      const u32 y0 =
          static_cast<u32>(
              (std::uint64_t(gy) *
               height) /
              GRID);

      const u32 y1 =
          std::max<u32>(
              y0 + 1,
              static_cast<u32>(
                  (std::uint64_t(gy + 1) *
                   height) /
                  GRID));

      double total = 0.0;
      std::uint64_t count = 0;

      // Sample instead of processing every high-resolution PS3 pixel.
      const u32 step_x =
          std::max<u32>(
              1,
              (x1 - x0) / 4);

      const u32 step_y =
          std::max<u32>(
              1,
              (y1 - y0) / 4);

      for (u32 y = y0;
           y < std::min(y1, height);
           y += step_y)
      {
        for (u32 x = x0;
             x < std::min(x1, width);
             x += step_x)
        {
          const std::size_t p =
              (static_cast<std::size_t>(y) *
                   row_length +
               x) *
              4;

          // Average RGB means the fingerprint is insensitive
          // to R/B channel ordering differences.
          total +=
              (double(rgba[p + 0]) +
               double(rgba[p + 1]) +
               double(rgba[p + 2])) /
              (3.0 * 255.0);

          ++count;
        }
      }

      const int cell =
          gy * GRID + gx;

      fp.values[cell] =
          count ?
              static_cast<float>(
                  total /
                  double(count)) :
              0.0f;
    }
  }

  for (float v : fp.values)
    fp.mean += v;

  fp.mean /= float(CELLS);

  for (float v : fp.values)
  {
    const float d =
        v - fp.mean;

    fp.variance +=
        d * d;
  }

  fp.variance /= float(CELLS);

  return fp;
}

float Score(
    const Fingerprint& a,
    const Fingerprint& b)
{
  float raw = 0.0f;
  float structure = 0.0f;

  for (int i = 0; i < CELLS; ++i)
  {
    raw +=
        std::abs(
            a.values[i] -
            b.values[i]);

    structure +=
        std::abs(
            (a.values[i] - a.mean) -
            (b.values[i] - b.mean));
  }

  raw /= float(CELLS);
  structure /= float(CELLS);

  const float mean_delta =
      std::abs(a.mean - b.mean);

  // Structure matters more than brightness because the remaster
  // changes contrast/lighting while preserving the underlying art.
  return
      structure * 0.67f +
      raw * 0.25f +
      mean_delta * 0.08f;
}

bool IsReplacementCompatible(
    u32 gc_width,
    u32 gc_height,
    u32 ps3_width,
    u32 ps3_height)
{
  if (!gc_width ||
      !gc_height ||
      !ps3_width ||
      !ps3_height)
  {
    return false;
  }

  if (ps3_width < gc_width ||
      ps3_height < gc_height)
  {
    return false;
  }

  if ((std::uint64_t(gc_width) *
       ps3_height) !=
      (std::uint64_t(gc_height) *
       ps3_width))
  {
    return false;
  }

  if ((ps3_width % gc_width) != 0 ||
      (ps3_height % gc_height) != 0)
  {
    return false;
  }

  const u32 sx =
      ps3_width / gc_width;

  const u32 sy =
      ps3_height / gc_height;

  return
      sx == sy &&
      sx >= 1 &&
      sx <= 16;
}

bool IgnoreForAutomaticReplacement(
    const std::filesystem::path& file)
{
  std::string p =
      Lower(
          file.generic_string());

  // These are additive material/render resources,
  // not diffuse texture replacements.
  return
      Contains(p, "/detailmaps/") ||
      Contains(p, "_normal.ssh") ||
      Contains(p, "wavesbump.ssh") ||
      Contains(p, "ocean_bump.ssh") ||
      Contains(p, "oceangrad.ssh") ||
      Contains(p, "/light_") ||
      Contains(p, "diffusecube.ssh");
}

std::shared_ptr<VideoCommon::CustomTextureData>
LoadCandidateTexture(Candidate* c)
{
  if (!c)
    return nullptr;

  if (c->cached_texture)
    return c->cached_texture;

  std::vector<PS3SshDecoder::Image> images;

  if (!PS3SshDecoder::DecodeFile(
          c->file,
          &images,
          nullptr))
  {
    return nullptr;
  }

  for (const auto& image : images)
  {
    if (image.tag != c->tag ||
        image.width != c->width ||
        image.height != c->height ||
        image.source_type != c->source_type)
    {
      continue;
    }

    if (image.rgba.empty())
      continue;

    auto texture =
        std::make_shared<
            VideoCommon::CustomTextureData>();

    texture->m_slices.resize(1);
    texture->m_slices[0].m_levels.resize(1);

    auto& level =
        texture->m_slices[0].m_levels[0];

    level.width =
        image.width;

    level.height =
        image.height;

    level.row_length =
        image.width;

    level.format =
        AbstractTextureFormat::RGBA8;

    level.data =
        Common::UniqueBuffer<u8>(
            image.rgba.size());

    std::memcpy(
        level.data.data(),
        image.rgba.data(),
        image.rgba.size());

    c->cached_texture =
        texture;

    return texture;
  }

  return nullptr;
}

void BuildIndex()
{
  s_candidates.clear();
  s_match_cache.clear();

  std::error_code ec;

  std::size_t ssh_files = 0;
  std::size_t decoded_files = 0;
  std::size_t decoded_images = 0;

  for (std::filesystem::recursive_directory_iterator it(
           s_root,
           std::filesystem::directory_options::
               skip_permission_denied,
           ec),
       end;
       !ec && it != end;
       it.increment(ec))
  {
    std::error_code file_ec;

    if (!it->is_regular_file(file_ec))
      continue;

    const auto& file =
        it->path();

    if (Lower(file.extension().string()) !=
        ".ssh")
    {
      continue;
    }

    ++ssh_files;

    if (IgnoreForAutomaticReplacement(file))
      continue;

    std::vector<PS3SshDecoder::Image> images;

    if (!PS3SshDecoder::DecodeFile(
            file,
            &images,
            nullptr))
    {
      continue;
    }

    ++decoded_files;

    for (const auto& image : images)
    {
      if (image.rgba.empty() ||
          image.width == 0 ||
          image.height == 0 ||
          image.width > 8192 ||
          image.height > 8192)
      {
        continue;
      }

      Candidate c;

      c.file = file;
      c.tag = image.tag;
      c.width = image.width;
      c.height = image.height;
      c.source_type = image.source_type;

      c.fp =
          MakeFingerprint(
              image.rgba.data(),
              image.width,
              image.height,
              image.width);

      // Flat/blank textures are too ambiguous for automatic
      // visual matching.
      if (c.fp.variance < 0.0007f)
        continue;

      s_candidates.push_back(
          std::move(c));

      ++decoded_images;
    }
  }

  std::fprintf(
      stderr,
      "[moh-ps3] texture bridge: %zu SSH files, "
      "%zu decoded files, %zu match candidates "
      "(threshold %.3f)\n",
      ssh_files,
      decoded_files,
      decoded_images,
      s_threshold);
}
}

void Initialize(
    const std::filesystem::path& root)
{
  Shutdown();

  if (root.empty())
    return;

  if (IsFalse(
          std::getenv(
              "MOH_PS3_AUTO_TEXTURES")))
  {
    std::fprintf(
        stderr,
        "[moh-ps3] automatic texture bridge disabled\n");

    return;
  }

  s_root = root;

  std::error_code ec;

  if (!std::filesystem::exists(
          s_root, ec) ||
      !std::filesystem::is_directory(
          s_root, ec))
  {
    return;
  }

  if (const char* env =
          std::getenv(
              "MOH_PS3_MATCH_THRESHOLD"))
  {
    char* end = nullptr;

    const float value =
        std::strtof(env, &end);

    if (end != env &&
        std::isfinite(value))
    {
      s_threshold =
          std::clamp(
              value,
              0.02f,
              0.30f);
    }
  }

  s_enabled = true;

  BuildIndex();
}

void Shutdown()
{
  s_enabled = false;

  s_candidates.clear();
  s_match_cache.clear();

  s_root.clear();

  s_threshold = 0.12f;
}

bool IsEnabled()
{
  return
      s_enabled &&
      !s_candidates.empty();
}

std::shared_ptr<VideoCommon::CustomTextureData>
FindReplacement(
    const u8* rgba,
    u32 width,
    u32 height,
    u32 row_length,
    u64 texture_hash,
    std::string* source_name,
    float* match_score)
{
  if (!IsEnabled() ||
      !rgba ||
      !width ||
      !height ||
      row_length < width)
  {
    return nullptr;
  }

  auto cached =
      s_match_cache.find(texture_hash);

  Match match;

  if (cached != s_match_cache.end())
  {
    match = cached->second;
  }
  else
  {
    const Fingerprint source_fp =
        MakeFingerprint(
            rgba,
            width,
            height,
            row_length);

    if (source_fp.variance < 0.0007f)
    {
      s_match_cache.emplace(
          texture_hash,
          Match{});

      return nullptr;
    }

    int best_index = -1;
    float best_score =
        std::numeric_limits<float>::max();

    for (std::size_t i = 0;
         i < s_candidates.size();
         ++i)
    {
      const Candidate& candidate =
          s_candidates[i];

      if (!IsReplacementCompatible(
              width,
              height,
              candidate.width,
              candidate.height))
      {
        continue;
      }

      const float score =
          Score(
              source_fp,
              candidate.fp);

      if (score < best_score)
      {
        best_score = score;
        best_index =
            static_cast<int>(i);
      }
    }

    if (best_index >= 0 &&
        best_score <= s_threshold)
    {
      match.candidate = best_index;
      match.score = best_score;
    }

    s_match_cache.emplace(
        texture_hash,
        match);
  }

  if (match.candidate < 0 ||
      static_cast<std::size_t>(
          match.candidate) >=
          s_candidates.size())
  {
    return nullptr;
  }

  Candidate& candidate =
      s_candidates[
          static_cast<std::size_t>(
              match.candidate)];

  auto texture =
      LoadCandidateTexture(
          &candidate);

  if (!texture)
    return nullptr;

  if (source_name)
  {
    std::error_code ec;

    auto rel =
        std::filesystem::relative(
            candidate.file,
            s_root,
            ec);

    *source_name =
        ec ?
            candidate.file.string() :
            rel.generic_string();

    if (!candidate.tag.empty())
    {
      *source_name +=
          ":" +
          candidate.tag;
    }
  }

  if (match_score)
    *match_score = match.score;

  static std::unordered_map<u64, bool>
      logged;

  if (!logged.contains(texture_hash))
  {
    logged.emplace(
        texture_hash,
        true);

    std::string relative;

    std::error_code ec;

    auto rel =
        std::filesystem::relative(
            candidate.file,
            s_root,
            ec);

    relative =
        ec ?
            candidate.file.string() :
            rel.generic_string();

    std::fprintf(
        stderr,
        "[moh-ps3] texture replace hash=%016llx "
        "%ux%u -> %ux%u %s:%s score=%.4f\n",
        static_cast<unsigned long long>(
            texture_hash),
        width,
        height,
        candidate.width,
        candidate.height,
        relative.c_str(),
        candidate.tag.c_str(),
        match.score);
  }

  return texture;
}
}
