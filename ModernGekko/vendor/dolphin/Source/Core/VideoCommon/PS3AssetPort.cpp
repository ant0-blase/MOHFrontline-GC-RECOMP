#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
}  // namespace

bool IsTPKRSXEnabled() { return EnvSwitch("MOH_PS3_TPK_RSX", true); }
bool IsMSHEnabled() { return EnvSwitch("MOH_PS3_MSH", true); }
bool IsDMFEnabled() { return EnvSwitch("MOH_PS3_DMF", false); }
void Initialize() { Native::Initialize(); }
void Shutdown() { Native::Shutdown(); }
void SetCurrentLevel(std::string_view level) { Native::SetCurrentLevel(level); }
std::string GetCurrentLevel() { return Native::GetCurrentLevel(); }
Class Classify(std::string_view path) { return static_cast<Class>(Native::Classify(path)); }
Match Resolve(std::string_view path, Class wanted)
{
  if (wanted == Class::StaticMesh && !IsMSHEnabled())
    return {};
  if (wanted == Class::SkinnedMesh && !IsDMFEnabled())
    return {};

  const auto found = Native::Resolve(path, static_cast<Native::Domain>(wanted));
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
