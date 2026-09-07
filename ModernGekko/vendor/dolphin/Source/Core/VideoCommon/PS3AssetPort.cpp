#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include "VideoCommon/PS3MeshPort.h"
#include "VideoCommon/XFMemory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace PS3AssetPort
{
namespace Native = MOHFrontline::NativeAssets;
namespace
{
bool EnvSwitch(const char* name, bool fallback)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  std::string lower(value);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lower == "0" || lower == "false" || lower == "off" || lower == "no")
    return false;
  if (lower == "1" || lower == "true" || lower == "on" || lower == "yes" ||
      lower == "experimental" || lower == "unsafe")
    return true;
  return fallback;
}

float EnvFloat(const char* name, float fallback)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  return end != value && end && *end == '\0' && std::isfinite(parsed) ? parsed : fallback;
}

struct LitVec3
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct LitScene
{
  std::string level;
  std::string source;
  LitVec3 sun_direction{};
  LitVec3 sun_color{1.0f, 1.0f, 1.0f};
  LitVec3 ambient{1.0f, 1.0f, 1.0f};
  u32 point_count = 0;
  u32 spot_count = 0;
};

std::atomic<std::shared_ptr<const LitScene>> s_lit_scene;
std::mutex s_lit_load_mutex;
std::string s_lit_attempted_level;
bool s_lighting_enabled = true;
float s_lighting_strength = 1.0f;

u32 ReadBE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

float ReadBEFloat(const u8* p)
{
  const u32 bits = ReadBE32(p);
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

LitVec3 ReadVec3(const std::vector<u8>& data, std::size_t offset)
{
  return {
      ReadBEFloat(data.data() + offset + 0),
      ReadBEFloat(data.data() + offset + 4),
      ReadBEFloat(data.data() + offset + 8),
  };
}

bool Finite(const LitVec3& v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float Length(const LitVec3& v)
{
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

std::string LowerCopy(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool Contains(std::string_view value, std::string_view needle)
{
  return value.find(needle) != std::string_view::npos;
}

std::string LevelKey(std::string_view level)
{
  std::string key(level);
  std::replace(key.begin(), key.end(), '\\', '/');

  const std::size_t slash = key.find_last_of('/');
  if (slash != std::string::npos)
    key.erase(0, slash + 1);

  const std::size_t dot = key.find_last_of('.');
  if (dot != std::string::npos)
    key.erase(dot);

  key = LowerCopy(std::move(key));

  // Accept host-side aliases such as M1L1 in addition to the original 1_1.
  if (key.size() >= 4 && key[0] == 'm')
  {
    const std::size_t l = key.find('l', 1);
    if (l != std::string::npos && l > 1 && l + 1 < key.size())
    {
      const std::string mission = key.substr(1, l - 1);
      const std::string level_number = key.substr(l + 1);
      const bool digits =
          !mission.empty() && !level_number.empty() &&
          std::all_of(mission.begin(), mission.end(), [](unsigned char c) { return std::isdigit(c); }) &&
          std::all_of(level_number.begin(), level_number.end(),
                      [](unsigned char c) { return std::isdigit(c); });
      if (digits)
        key = mission + "_" + level_number;
    }
  }

  return key;
}

const PS3RemasterAssets::AssetInfo* FindLevelLit(std::string_view requested_level)
{
  const std::string key = LevelKey(requested_level);
  if (key.empty())
    return nullptr;

  const std::string exact_name = key + ".lit";
  if (const auto* exact = PS3RemasterAssets::FindLevelLighting(key, exact_name))
    return exact;

  // Fallback for dumps whose directory naming differs from the runtime level
  // alias. Prefer the non-flickering base file.
  const PS3RemasterAssets::AssetInfo* fallback = nullptr;
  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    if (asset.kind != PS3RemasterAssets::Kind::LightingData)
      continue;

    const std::string filename = LowerCopy(asset.filename);
    const std::string relative = LowerCopy(asset.relative_path);
    if (!Contains(relative, key))
      continue;

    if (filename == exact_name)
      return &asset;

    if (!Contains(filename, "flicker"))
      fallback = &asset;
  }
  return fallback;
}

std::shared_ptr<const LitScene> ParseLevelLit(const PS3RemasterAssets::AssetInfo& asset,
                                             std::string_view level)
{
  const std::vector<u8> data = PS3RemasterAssets::ReadBinary(asset);

  // Empty .lit files exist and contain a single BE zero count.
  if (data.size() == 4 && ReadBE32(data.data()) == 0)
    return {};

  // Level .lit layout observed in the PS3 remaster:
  //   0x00 vec3 global direction
  //   0x0c vec3 sun/directional colour
  //   0x18 vec3 global ambient colour
  //   0x24 be32 point-count
  //        point_count * 36-byte records
  //        be32 spot-count
  //        spot_count * 56-byte records
  //
  // Additional categories can follow. v1 validates/skips them rather than
  // guessing their semantics.
  if (data.size() < 40)
    return {};

  auto scene = std::make_shared<LitScene>();
  scene->level = LevelKey(level);
  scene->source = asset.relative_path;
  scene->sun_direction = ReadVec3(data, 0);
  scene->sun_color = ReadVec3(data, 12);
  scene->ambient = ReadVec3(data, 24);

  const float dir_len = Length(scene->sun_direction);
  if (!Finite(scene->sun_direction) || !Finite(scene->sun_color) || !Finite(scene->ambient) ||
      dir_len < 0.75f || dir_len > 1.25f)
  {
    return {};
  }

  std::size_t offset = 36;
  scene->point_count = ReadBE32(data.data() + offset);
  offset += 4;

  constexpr u32 MAX_REASONABLE_LIGHTS = 4096;
  constexpr std::size_t POINT_RECORD_SIZE = 36;
  constexpr std::size_t SPOT_RECORD_SIZE = 56;

  if (scene->point_count > MAX_REASONABLE_LIGHTS ||
      static_cast<std::size_t>(scene->point_count) > (data.size() - offset) / POINT_RECORD_SIZE)
  {
    return {};
  }

  offset += static_cast<std::size_t>(scene->point_count) * POINT_RECORD_SIZE;
  if (offset + 4 > data.size())
    return {};

  scene->spot_count = ReadBE32(data.data() + offset);
  offset += 4;

  if (scene->spot_count > MAX_REASONABLE_LIGHTS ||
      static_cast<std::size_t>(scene->spot_count) > (data.size() - offset) / SPOT_RECORD_SIZE)
  {
    return {};
  }

  return scene;
}

void LoadLevelLighting(std::string_view level)
{
  if (!s_lighting_enabled || level.empty() || !PS3RemasterAssets::IsReady())
  {
    s_lit_scene.store(std::shared_ptr<const LitScene>{}, std::memory_order_release);
    return;
  }

  const auto* asset = FindLevelLit(level);
  if (!asset)
  {
    s_lit_scene.store(std::shared_ptr<const LitScene>{}, std::memory_order_release);
    std::fprintf(stderr, "[moh-ps3-lit] no .lit for level '%.*s'\n",
                 static_cast<int>(level.size()), level.data());
    return;
  }

  std::shared_ptr<const LitScene> parsed = ParseLevelLit(*asset, level);
  s_lit_scene.store(parsed, std::memory_order_release);

  if (!parsed)
  {
    std::fprintf(stderr, "[moh-ps3-lit] rejected/empty %s\n", asset->relative_path.c_str());
    return;
  }

  std::fprintf(stderr,
               "[moh-ps3-lit] loaded %s | dir=(%.3f %.3f %.3f) "
               "sun=(%.3f %.3f %.3f) ambient=(%.3f %.3f %.3f) "
               "points=%u spots=%u strength=%.2f | GX spatial transforms preserved\n",
               parsed->source.c_str(), parsed->sun_direction.x, parsed->sun_direction.y,
               parsed->sun_direction.z, parsed->sun_color.x, parsed->sun_color.y,
               parsed->sun_color.z, parsed->ambient.x, parsed->ambient.y, parsed->ambient.z,
               parsed->point_count, parsed->spot_count, s_lighting_strength);
}

void SyncLevelLighting(std::string_view level)
{
  const std::string key = LevelKey(level);
  std::scoped_lock lock(s_lit_load_mutex);

  // NativeAssetResolver discovers the active level from ordinary asset loads.
  // Only touch disk once when that resolver changes level; the render thread
  // only sees the atomically published immutable scene.
  if (key == s_lit_attempted_level)
    return;

  s_lit_attempted_level = key;
  LoadLevelLighting(level);
}

int ClampByte(float value)
{
  return static_cast<int>(std::lround(std::clamp(value, 0.0f, 255.0f)));
}

int BlendByte(int original, float target, float strength)
{
  return ClampByte(static_cast<float>(original) +
                   (target - static_cast<float>(original)) * strength);
}

bool SetComponent(s32& dst, int value)
{
  if (dst == value)
    return false;
  dst = value;
  return true;
}

bool RestoreNativeLightColors(VertexShaderConstants& constants, const XFMemory& xf)
{
  bool changed = false;

  // Restore the raw GX ambient registers on every draw. This makes the bridge
  // idempotent and guarantees that orthographic HUD/menu draws never inherit
  // the previous perspective draw's PS3 tint.
  for (int channel = 0; channel < 2; ++channel)
  {
    const u32 data = xf.ambColor[channel];
    changed |= SetComponent(constants.materials[channel][0], (data >> 24) & 0xFF);
    changed |= SetComponent(constants.materials[channel][1], (data >> 16) & 0xFF);
    changed |= SetComponent(constants.materials[channel][2], (data >> 8) & 0xFF);
    changed |= SetComponent(constants.materials[channel][3], data & 0xFF);
  }

  for (int i = 0; i < 8; ++i)
  {
    const Light& light = xf.lights[i];
    changed |= SetComponent(constants.lights[i].color[0], light.color[3]);
    changed |= SetComponent(constants.lights[i].color[1], light.color[2]);
    changed |= SetComponent(constants.lights[i].color[2], light.color[1]);
    changed |= SetComponent(constants.lights[i].color[3], light.color[0]);
  }

  return changed;
}
}  // namespace

bool IsTPKRSXEnabled() { return EnvSwitch("MOH_PS3_TPK_RSX", false); }
bool IsMSHEnabled() { return EnvSwitch("MOH_PS3_MSH", true); }
bool IsDMFEnabled() { return EnvSwitch("MOH_PS3_DMF", false); }
bool IsLightingEnabled() { return s_lighting_enabled; }

void Initialize()
{
  Native::Initialize();
  s_lighting_enabled = EnvSwitch("MOH_PS3_LIGHTING", true);
  s_lighting_strength = std::clamp(EnvFloat("MOH_PS3_LIGHTING_STRENGTH", 1.0f), 0.0f, 1.0f);
  {
    std::scoped_lock lock(s_lit_load_mutex);
    s_lit_attempted_level.clear();
  }
  s_lit_scene.store(std::shared_ptr<const LitScene>{}, std::memory_order_release);
  PS3MeshPort::ClearMSHCache();

  std::fprintf(stderr, "[moh-ps3-lit] bridge %s (strength=%.2f)\n",
               s_lighting_enabled ? "ON" : "OFF", s_lighting_strength);
}

void Shutdown()
{
  {
    std::scoped_lock lock(s_lit_load_mutex);
    s_lit_attempted_level.clear();
  }
  s_lit_scene.store(std::shared_ptr<const LitScene>{}, std::memory_order_release);
  PS3MeshPort::ClearMSHCache();
  Native::Shutdown();
}

void SetCurrentLevel(std::string_view level)
{
  Native::SetCurrentLevel(level);
  SyncLevelLighting(level);
  PS3MeshPort::PreloadCurrentLevelMSH(level);
}

bool ApplyLightingToGX(VertexShaderConstants& constants, const XFMemory& xf, bool perspective)
{
  // Always restore raw GX colours first so this function is reversible and
  // repeated calls never accumulate tint.
  bool changed = RestoreNativeLightColors(constants, xf);

  const std::shared_ptr<const LitScene> scene = s_lit_scene.load(std::memory_order_acquire);

  if (!s_lighting_enabled || !scene || !perspective || s_lighting_strength <= 0.0f)
    return changed;

  const float strength = s_lighting_strength;
  const std::array<float, 3> ambient = {scene->ambient.x, scene->ambient.y, scene->ambient.z};
  const std::array<float, 3> sun = {scene->sun_color.x, scene->sun_color.y, scene->sun_color.z};

  // Import the level-wide PS3 ambient registers directly. Alpha remains the
  // original GX value because .lit stores RGB triplets here.
  for (int channel = 0; channel < 2; ++channel)
  {
    for (int c = 0; c < 3; ++c)
    {
      const int original = constants.materials[channel][c];
      const float target = std::clamp(ambient[c], 0.0f, 2.0f) * 255.0f;
      changed |= SetComponent(constants.materials[channel][c],
                              BlendByte(original, target, strength));
    }
  }

  // Keep GameCube light positions/directions/attenuation (already in the
  // correct GX view space), but apply the PS3 directional-light colour as a
  // multiplicative tint to every light selected by either RGB channel.
  const u32 light_mask =
      xf.color[0].GetFullLightMask() | xf.color[1].GetFullLightMask();

  for (int i = 0; i < 8; ++i)
  {
    if ((light_mask & (1u << i)) == 0)
      continue;

    for (int c = 0; c < 3; ++c)
    {
      const int original = constants.lights[i].color[c];
      const float target =
          std::clamp(static_cast<float>(original) * std::clamp(sun[c], 0.0f, 2.0f),
                     0.0f, 255.0f);
      changed |= SetComponent(constants.lights[i].color[c],
                              BlendByte(original, target, strength));
    }
  }

  return changed;
}

std::string GetCurrentLevel() { return Native::GetCurrentLevel(); }
Class Classify(std::string_view path) { return static_cast<Class>(Native::Classify(path)); }
Match Resolve(std::string_view path, Class wanted)
{
  if (wanted == Class::StaticMesh && !IsMSHEnabled())
    return {};
  if (wanted == Class::SkinnedMesh && !IsDMFEnabled())
    return {};

  const auto found = Native::Resolve(path, static_cast<Native::Domain>(wanted));
  SyncLevelLighting(Native::GetCurrentLevel());
  Match result;
  result.guest_name = path;
  if (found)
  {
    result.asset = found.asset;
    result.asset_class = static_cast<Class>(found.domain);
    result.ps3_name = found.asset->filename;
    result.normalized_path = found.normalized_path;
    result.score = found.score;
  }
  return result;
}
Match ResolveTexture(std::string_view p) { return Resolve(p, Class::Texture); }
Match ResolveFont(std::string_view p) { return Resolve(p, Class::Font); }
Match ResolveAudio(std::string_view p) { return Resolve(p, Class::Audio); }
Match ResolveStaticMesh(std::string_view p) { return Resolve(p, Class::StaticMesh); }
Match ResolveSkinnedMesh(std::string_view p) { return Resolve(p, Class::SkinnedMesh); }
Match ResolveSkeleton(std::string_view p) { return Resolve(p, Class::Skeleton); }
Match ResolveAnimation(std::string_view p) { return Resolve(p, Class::Animation); }
Match ResolveWorld(std::string_view p) { return Resolve(p, Class::World); }
std::vector<u8> Read(const Match& m) { return m ? PS3RemasterAssets::ReadBinary(*m.asset) : std::vector<u8>{}; }
bool CanRawReplace(const Match&) { return false; }
std::string Describe(const Match& m) { return m ? m.guest_name + " -> " + m.normalized_path : "<no PS3 match>"; }
}
