#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/PS3AssetPort.h"

namespace MOHFrontline::NativeVFS
{
enum class Source
{
  None,
  GameCubeHost,
  GameCubeDisc,
  PlayStation3Host,
};

enum class Policy
{
  Auto,
  PS3First,
  GCFirst,
  PS3Only,
  GCOnly,
};

struct File
{
  Source source = Source::None;
  PS3AssetPort::Class asset_class = PS3AssetPort::Class::Unknown;
  PS3AssetPort::Match ps3;
  std::filesystem::path host_path;
  u32 disc_index = 0;
  std::string guest_path;
  std::string resolved_path;
  std::uint64_t size = 0;

  explicit operator bool() const { return source != Source::None; }
  bool IsGC() const
  {
    return source == Source::GameCubeHost || source == Source::GameCubeDisc;
  }
  bool IsGCDisc() const { return source == Source::GameCubeDisc; }
  bool IsPS3() const { return source == Source::PlayStation3Host; }
};

// Native host-side VFS for MOH Frontline.
//
// PS3 data is resolved through the existing semantic PS3AssetPort. GC data is
// read either from an extracted host directory or directly from the selected
// ISO/GCM/RVZ/WIA/WBFS/CISO/GCZ/TGC/NFS through encounter/nod. The emulated
// DVD remains the caller's fallback when Resolve() returns no native file.
void Initialize();
void Shutdown();

Policy GetPolicy();
const char* PolicyName(Policy policy);
const char* SourceName(Source source);

void SetGameCubeRoot(std::filesystem::path root);
std::filesystem::path GetGameCubeRoot();

File Resolve(std::string_view guest_path,
             PS3AssetPort::Class wanted = PS3AssetPort::Class::Unknown);

// Explicit-source variants used by bridges that must never inject PS3 binary
// layouts into a GameCube guest loader.
File ResolveGameCube(std::string_view guest_path,
                     PS3AssetPort::Class wanted = PS3AssetPort::Class::Unknown);
File ResolvePlayStation3(std::string_view guest_path,
                         PS3AssetPort::Class wanted = PS3AssetPort::Class::Unknown);

std::vector<u8> Read(const File& file);
bool ReadRange(const File& file, std::uint64_t offset, std::span<u8> destination);

std::string Describe(const File& file);
}  // namespace MOHFrontline::NativeVFS
