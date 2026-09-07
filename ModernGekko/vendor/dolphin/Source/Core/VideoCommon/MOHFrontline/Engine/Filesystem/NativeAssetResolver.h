#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "VideoCommon/PS3RemasterAssets.h"

namespace MOHFrontline::NativeAssets
{
enum class Domain
{
  Texture, Font, Audio, StaticMesh, SkinnedMesh, Skeleton, Animation, World,
  Script, Container, Unknown
};
struct Match
{
  const PS3RemasterAssets::AssetInfo* asset = nullptr;
  Domain domain = Domain::Unknown;
  std::string normalized_path;
  int score = 0;
  explicit operator bool() const { return asset != nullptr; }
};
struct Statistics
{
  std::uint64_t requests = 0, resolved = 0, missing = 0, ambiguous = 0;
  std::size_t indexed = 0, levels = 0;
};
void Initialize();
void Shutdown();
Domain Classify(std::string_view path);
void SetCurrentLevel(std::string_view level);
std::string GetCurrentLevel();
std::vector<std::string> GetLevels();
Statistics GetStatistics();
Match Resolve(std::string_view guest_path, Domain wanted = Domain::Unknown);
std::vector<std::uint8_t> Read(const Match& match);
// Identity is not evidence of binary compatibility. No native format is injected into GC.
bool CanRawReplace(const Match& match);
}
