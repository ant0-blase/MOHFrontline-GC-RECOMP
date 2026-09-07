#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/PS3RemasterAssets.h"

namespace PS3AssetPort
{
enum class Class
{
  Texture,
  Font,
  Audio,
  StaticMesh,
  SkinnedMesh,
  Skeleton,
  Animation,
  World,
  Script,
  Container,
  Unknown
};

struct Match
{
  const PS3RemasterAssets::AssetInfo* asset = nullptr;
  Class asset_class = Class::Unknown;
  std::string guest_name;
  std::string ps3_name;
  std::string normalized_path;
  int score = 0;

  explicit operator bool() const { return asset != nullptr; }
};

void Initialize();
void Shutdown();

void SetCurrentLevel(std::string_view level);
std::string GetCurrentLevel();

Class Classify(std::string_view path);

// General resolver. This is the one entry point that every future file-load
// hook should use instead of maintaining separate hard-coded tables.
Match Resolve(std::string_view guest_path, Class wanted = Class::Unknown);

// Convenience paths used by the existing runtime bridges.
Match ResolveTexture(std::string_view guest_path);
Match ResolveFont(std::string_view guest_path);
Match ResolveAudio(std::string_view guest_path);
Match ResolveStaticMesh(std::string_view guest_path);
Match ResolveSkinnedMesh(std::string_view guest_path);
Match ResolveSkeleton(std::string_view guest_path);
Match ResolveAnimation(std::string_view guest_path);
Match ResolveWorld(std::string_view guest_path);

std::vector<u8> Read(const Match& match);

// True only for files that may safely be substituted byte-for-byte into the
// original GameCube loader. Platform-specific graphics/audio/model data must
// be consumed by a host-side converter/renderer instead.
bool CanRawReplace(const Match& match);

std::string Describe(const Match& match);
}  // namespace PS3AssetPort
