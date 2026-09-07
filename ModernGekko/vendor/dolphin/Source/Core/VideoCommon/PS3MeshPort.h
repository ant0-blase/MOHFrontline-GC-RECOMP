#pragma once

#include <memory>
#include <cstdint>
#include <span>
#include <string_view>
#include <cstddef>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/Meshes/StaticMesh.h"

namespace PS3MeshPort
{
using Attribute = MOHFrontline::Meshes::Attribute;
using Submesh = MOHFrontline::Meshes::Submesh;
using StaticMesh = MOHFrontline::Meshes::StaticMesh;

struct DMFInfo
{
  bool valid = false;
  u32 version = 0;
  u32 mesh_count = 0;
  u32 material_count = 0;
  u32 bone_ref_count = 0;
  std::string model_name;
};

struct DMFResource
{
  DMFInfo info;
  std::string source_name;
  std::shared_ptr<const std::vector<u8>> bytes;
};

struct SKLInfo
{
  bool valid = false;
  bool big_endian = false;
  u32 bone_count = 0;
  u32 bone_data_offset = 0;
  u32 names_offset = 0;
  u32 names_end = 0;
  std::string source_name;
  std::vector<std::string> bone_names;
};

enum class ReplacementKind
{
  None,
  StaticMSH,
  SkinnedDMF,
  SkeletonSKL,
};

// Host-side model replacement selected from the name requested by the GC game.
// Nothing here is injected byte-for-byte into the original GC model loader.
// The GC renderer keeps its current GX transforms/state and consumes the
// converted PS3 resource through this bridge.
struct Replacement
{
  ReplacementKind kind = ReplacementKind::None;
  const StaticMesh* static_mesh = nullptr;
  const DMFResource* skinned_mesh = nullptr;
  const SKLInfo* skeleton = nullptr;

  explicit operator bool() const { return kind != ReplacementKind::None; }
};

// Strict renderer-side match for a GameCube rigid draw and one PS3 MSH
// submesh.  The bridge deliberately starts with exact vertex-count matching
// plus object-space bounds, so an uncertain match always stays GameCube.
struct StaticDrawMatch
{
  std::shared_ptr<const StaticMesh> owner;
  std::shared_ptr<const std::vector<std::array<float, 3>>> normals;
  u32 guest_resource = 0;
  u32 display_list = 0;
  const StaticMesh* mesh = nullptr;
  const Submesh* submesh = nullptr;
  std::size_t submesh_index = 0;
  float score = 0.0f;

  explicit operator bool() const { return mesh != nullptr && submesh != nullptr; }
};

// CPU load metadata -> exact GPU display-list identity. No GPU calls on CPU.
void RegisterGuestStaticMesh(std::string_view name, u32 address, std::span<const u8> bytes);
StaticDrawMatch FindDisplayList(u32 address, std::span<const u8> commands);
void SetDisplayListMatch(StaticDrawMatch match);
void NotifyStaticDrawSubmitted();
void PrintDrawStatistics();

bool IsStaticDrawReplacementEnabled();
StaticDrawMatch MatchStaticDraw(std::span<const u8> gc_vertices,
                                u32 gc_vertex_count,
                                u32 gc_vertex_stride,
                                u32 gc_position_offset);

bool ParseMSHv8(std::span<const u8> bytes, StaticMesh* out);
DMFInfo InspectDMF(std::span<const u8> bytes);
SKLInfo InspectSKL(std::span<const u8> bytes);

// Static MSH can be consumed by a host renderer. It must NOT be copied into
// the GameCube MSH loader because GC and PS3 layouts are different.
bool IsHostRenderable(const StaticMesh& mesh);

// v14.0: decode/cache every PS3 static MSH for the active level in one pass.
// This makes the complete PS3 geometry set available to the native renderer
// instead of resolving one guessed prop at a time.
void PreloadCurrentLevelMSH(std::string_view level);
void ClearMSHCache();
void PreloadCurrentLevelDMF(std::string_view level);
void ClearDMFCache();
void PreloadCurrentLevelSKL(std::string_view level);
void ClearSKLCache();
const StaticMesh* FindCachedMSH(std::string_view name_or_path);
const DMFResource* FindCachedDMF(std::string_view name_or_path);
const SKLInfo* FindCachedSKL(std::string_view name_or_path);
std::size_t CachedMSHCount();
std::size_t CachedDMFCount();
std::size_t CachedSKLCount();

// Main GC -> PS3 model bridge. .msf is accepted as an alias of .msh.
Replacement ResolveReplacement(std::string_view gc_resource_name);
}  // namespace PS3MeshPort
