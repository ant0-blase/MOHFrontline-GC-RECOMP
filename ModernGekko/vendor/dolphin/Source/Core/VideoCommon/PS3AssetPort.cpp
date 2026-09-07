#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"

namespace PS3AssetPort
{
namespace Native = MOHFrontline::NativeAssets;
void Initialize() { Native::Initialize(); }
void Shutdown() { Native::Shutdown(); }
void SetCurrentLevel(std::string_view level) { Native::SetCurrentLevel(level); }
std::string GetCurrentLevel() { return Native::GetCurrentLevel(); }
Class Classify(std::string_view path) { return static_cast<Class>(Native::Classify(path)); }
Match Resolve(std::string_view path, Class wanted)
{
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
