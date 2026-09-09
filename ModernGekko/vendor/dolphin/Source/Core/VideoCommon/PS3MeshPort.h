#pragma once
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/SkinBind.h"

#include <memory>
#include <array>
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

struct DMFSkinGroup
{
  u8 bone_a = 0;
  u8 bone_b = 0;
  float blend = 0.0f;
  std::array<float, 4> auxiliary{};
};

struct DMFCluster
{
  u32 material_index = 0;
  u32 texture_index = 0;
  u32 material_cluster_index = 0;
  std::string material_name;
  u8 vertex_stride = 0;
  u8 attribute_word_count = 0;
  std::vector<Attribute> attributes;
  std::vector<u16> palette_groups;
  std::vector<u16> vertex_palette_slots;
  std::vector<u16> indices;
  std::vector<u8> vertices;
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 3>> normals;
  std::vector<std::array<float, 2>> uv0;
  bool has_position = false;
  bool has_normal = false;
  bool has_uv0 = false;
};

struct DMFDecoded
{
  bool valid = false;
  std::vector<std::string> bone_refs;
  // Authored PS3 DMF inverse-bind matrices, indexed directly by DMF bone-ref.
  // These tables live in the DMF itself and therefore do not require a .skl
  // just to convert model/bind-space vertices back to GC skin-group local space.
  std::vector<std::array<float, 16>> inverse_bind_by_ref;
  bool bind_tables_valid = false;
  std::vector<DMFSkinGroup> skin_groups;
  std::vector<DMFCluster> clusters;
  std::size_t total_vertices = 0;
  std::size_t total_indices = 0;
};

struct DMFResource
{
  DMFInfo info;
  std::string source_name;
  std::shared_ptr<const std::vector<u8>> bytes;
  std::shared_ptr<const DMFDecoded> decoded;
};

struct EMTInfo
{
  bool valid = false;
  bool big_endian = false;
  u32 version = 0;
  u32 entity_count = 0;
  u32 section_a = 0;
  u32 section_b = 0;
  u32 section_c = 0;
  std::size_t leks_blocks = 0;
};

struct EMTResource
{
  EMTInfo info;
  std::string source_name;
  std::shared_ptr<const std::vector<u8>> bytes;
};



struct SkinnedPaletteAnalysis
{
  bool valid = false;
  // GC and PS3 DMF material-table indices are platform-local.  The strict
  // bridge resolves a PS3 material slot by exact skin-palette identity and
  // records the winning PS3 index here; 0xffffffff means unresolved.
  u32 ps3_material_index = 0xffffffffu;
  // Global ordinal in DMFDecoded::clusters for a structurally proven
  // GC material-cluster -> PS3 cluster match.  This is separate from
  // material_index because remaster material tables are platform-local.
  u32 ps3_cluster_ordinal = 0xffffffffu;
  bool exact_cluster_identity = false;

  // v12: the PS3 material can be split into a completely different number
  // of clusters from the GC material. In that case all PS3 triangles are
  // partitioned exactly once across the authored GC DLs by:
  //   * exact material identity;
  //   * exact total triangle count;
  //   * PS3 skin-group -> GC skin-group compatibility;
  //   * exact per-GC-DL triangle quota.
  bool exact_triangle_partition = false;
  std::size_t ps3_material_candidates = 0;
  std::size_t compatible_ps3_materials = 0;
  std::size_t ps3_material_clusters = 0;
  std::size_t total_triangles = 0;
  std::size_t selected_triangles = 0;
  std::size_t ambiguous_triangles = 0;
  std::size_t unmapped_triangles = 0;
  std::size_t selected_vertices = 0;
  std::size_t matrix_slots = 0;
};

struct DMFReplacementReadiness
{
  bool geometry_valid = false;
  bool skeleton_valid = false;
  bool bind_valid = false;
  bool palettes_valid = false;
  bool materials_valid = false;
  bool all_required_parts_mapped = false;
  bool Ready() const
  {
    return geometry_valid && skeleton_valid && bind_valid && palettes_valid &&
           materials_valid && all_required_parts_mapped;
  }
};

struct PreparedDMFDraw
{
  std::string gc_name, material_name, skeleton_name;
  std::vector<u8> palette;
  std::vector<s16> group_map;
  SkinnedPaletteAnalysis analysis;
  DMFReplacementReadiness readiness;
};

struct SkinnedDrawMatch
{
  std::shared_ptr<const DMFResource> owner;
  std::string_view gc_name;
  std::shared_ptr<const PreparedDMFDraw> prepared;
  u32 display_list = 0;
  u32 material_index = 0;
  u32 cluster_index = 0;
  std::string_view gc_material_name;
  std::span<const u8> gc_palette_groups;
  std::span<const s16> ps3_group_to_gc;
  std::string_view skeleton_name;

  explicit operator bool() const { return owner != nullptr; }
};


// Strict v16.9 proof-of-life replacement.  It is returned only when the
// current GC DMF material has exactly one authored GX display list, exactly
// one PS3 material cluster, every PS3 triangle maps to that palette, and all
// required vertex attributes were decoded from the RSX declaration.
struct SkinnedDrawReplacement
{
  std::shared_ptr<const DMFResource> owner;
  const DMFCluster* cluster = nullptr;
  std::vector<u8> position_matrix_indices;
  // PS3 0x0502 DMF positions are stored in model bind space. Retail GC DMF
  // display lists feed group-local positions to the live GX/XF skin matrix.
  // One inverse-bind affine matrix per GC palette slot converts PS3 model-space
  // vertices back into the exact local space expected by that GC matrix slot.
  // UVs remain authored PS3 UV0 and are never modified here.
  std::vector<std::array<float, 12>> model_to_gc_local;
  // Non-rigid skin groups are not orthonormal.  Positions use inverse(group
  // bind), while normals require transpose(group bind).  Keep the normal
  // transform separate so generic two-bone groups do not inherit the rigid
  // weapon shortcut.
  std::vector<std::array<float, 9>> model_to_gc_local_normal;
  std::size_t gc_material_draws = 0;

  explicit operator bool() const
  {
    return owner != nullptr && cluster != nullptr &&
           position_matrix_indices.size() == cluster->positions.size() &&
           !model_to_gc_local.empty() &&
           model_to_gc_local_normal.size() == model_to_gc_local.size();
  }
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
  MOHFrontline::PS3::SkinBind::Hierarchy hierarchy;
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
  bool bounds_valid = false;
  std::array<float, 3> bounds_min{}, bounds_max{};

  // v9.3 world-CPT local-origin bridge.  Some PS3 CPT clusters keep the same
  // authored shape/scale as the GC batch but use a different local origin.
  // Only strict centered-bounds matches set this translation.
  bool world_translation_valid = false;
  std::array<float, 3> world_translation{};

  explicit operator bool() const { return mesh != nullptr && submesh != nullptr; }
};

// CPU load metadata -> exact GPU display-list identity. No GPU calls on CPU.
void RegisterGuestStaticMesh(std::string_view name, u32 address, std::span<const u8> bytes);
StaticDrawMatch FindDisplayList(u32 address, std::span<const u8> commands);
void SetDisplayListContext(u32 address, std::span<const u8> commands);
void SetDisplayListMatch(StaticDrawMatch match);
const StaticDrawMatch& CurrentStaticDraw();
SkinnedDrawMatch CurrentSkinnedDraw();
void SetSkinnedDrawMatch(SkinnedDrawMatch match);
bool IsStaticBootstrapEnabled();
SkinnedPaletteAnalysis AnalyzeCurrentSkinnedPalette();
SkinnedDrawReplacement BuildCurrentSkinnedReplacement();
void NotifyStaticDrawSubmitted();
void RejectStaticDrawCandidate();
void NotifySkinnedDrawSubmitted(const SkinnedDrawReplacement& replacement);
void PrintDrawStatistics();

bool IsStaticDrawReplacementEnabled();

// v10.1 experimental complete CPT world proof-of-life. The renderer builds a
// single host mesh from every decoded *_ART_cN.cpt descriptor after NODE70
// transforms and may submit it once per frame from a direct GC world draw.
bool IsFullCPTLevelRenderEnabled();
StaticDrawMatch AcquireFullCPTLevelDraw(u32 gc_triangle_count);

StaticDrawMatch MatchStaticDraw(std::span<const u8> gc_vertices,
                                u32 gc_vertex_count,
                                u32 gc_vertex_stride,
                                u32 gc_position_offset,
                                u32 gc_triangle_count = 0);

bool ParseMSHv8(std::span<const u8> bytes, StaticMesh* out);
DMFInfo InspectDMF(std::span<const u8> bytes);
bool DecodeDMF0502(std::span<const u8> bytes, DMFDecoded* out);
EMTInfo InspectEMT(std::span<const u8> bytes);
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
void PreloadCurrentLevelEMT(std::string_view level);
void ClearEMTCache();
const StaticMesh* FindCachedMSH(std::string_view name_or_path);
const DMFResource* FindCachedDMF(std::string_view name_or_path);
const SKLInfo* FindCachedSKL(std::string_view name_or_path);
const EMTResource* FindCachedEMT(std::string_view name_or_path);
std::size_t CachedMSHCount();
std::size_t CachedDMFCount();
std::size_t CachedSKLCount();
std::size_t CachedEMTCount();

// Main GC -> PS3 model bridge. .msf is accepted as an alias of .msh.
Replacement ResolveReplacement(std::string_view gc_resource_name);
}  // namespace PS3MeshPort
