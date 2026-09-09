#include "VideoCommon/PS3MeshPort.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/Meshes/WorldMeshBatch.h"
#include "VideoCommon/Fifo.h"
#include "Common/Hash.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include "VideoCommon/MOHFrontline/Engine/World/WorldLevelRuntime.h"
#include <atomic>
#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/MSH.h"
#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/PS3WorldGeometry.h"
#include "VideoCommon/PS3WorldCPTRuntime.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace PS3MeshPort
{
namespace
{
u16 BE16(const u8* p)
{
  return static_cast<u16>((u16(p[0]) << 8) | u16(p[1]));
}

u32 BE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u32 LE32(const u8* p)
{
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

float BEFloat(const u8* p)
{
  const u32 bits = BE32(p);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

float HalfFloat(u16 h)
{
  const int exponent = (h >> 10) & 31;
  const int mantissa = h & 1023;
  const float value = exponent == 0 ? std::ldexp(float(mantissa), -24) :
      exponent == 31 ? INFINITY : std::ldexp(float(1024 + mantissa), exponent - 25);
  return h & 0x8000 ? -value : value;
}

bool DecodeRSXComponents(const u8* vertex, const Attribute& attribute, unsigned components,
                         float* out, u32 stride)
{
  if (!vertex || !out || attribute.components != components)
    return false;
  const unsigned element_size = attribute.type == 2 ? 4 : attribute.type == 3 ? 2 : 0;
  if (!element_size || attribute.offset > stride ||
      components * element_size > stride - attribute.offset)
    return false;

  for (unsigned i = 0; i < components; ++i)
  {
    const u8* source = vertex + attribute.offset + i * element_size;
    out[i] = element_size == 4 ? BEFloat(source) : HalfFloat(BE16(source));
    if (!std::isfinite(out[i]))
      return false;
  }
  return true;
}

bool DecodeRSXNormal(const u8* vertex, const Attribute& attribute,
                     std::array<float, 3>* out, u32 stride)
{
  if (!vertex || !out)
    return false;
  if (attribute.type == 6 && attribute.components == 1)
  {
    if (attribute.offset > stride || 4 > stride - attribute.offset)
      return false;
    const u32 packed = BE32(vertex + attribute.offset);
    const auto snorm = [](u32 bits, unsigned width) {
      const int sign = 1 << (width - 1);
      const int value = int(bits & ((1u << width) - 1));
      return std::max(-1.0f,
                      float(value >= sign ? value - 2 * sign : value) / float(sign - 1));
    };
    *out = {snorm(packed, 11), snorm(packed >> 11, 11), snorm(packed >> 22, 10)};
    return true;
  }
  return DecodeRSXComponents(vertex, attribute, 3, out->data(), stride);
}
std::mutex g_msh_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<StaticMesh>> g_msh_cache;
// Protected by g_msh_cache_mutex, including lazy mesh insertions.
u64 g_msh_cache_revision = 0;
struct DisplayListIdentity
{
  StaticDrawMatch match;
  u32 size = 0;
  u64 command_hash = 0;
};
struct DisplayListSignatureKey
{
  u32 size = 0;
  u64 command_hash = 0;
  bool operator==(const DisplayListSignatureKey&) const = default;
};
struct DisplayListSignatureKeyHash
{
  std::size_t operator()(const DisplayListSignatureKey& key) const noexcept
  {
    return std::hash<u64>{}(key.command_hash ^ (u64(key.size) << 32) ^ key.size);
  }
};
struct OriginalGCDrawCandidate
{
  StaticDrawMatch match;
  std::string gc_name;
  u32 file_size = 0;
  u32 node_table_offset = 0;
  u32 node_count = 0;
  u32 node_index = 0;
  u32 dl_offset = 0;
  std::vector<std::pair<u32, DisplayListSignatureKey>> sibling_display_lists;
};
std::unordered_map<u32, DisplayListIdentity> g_display_lists;
std::unordered_map<DisplayListSignatureKey, std::vector<OriginalGCDrawCandidate>,
                   DisplayListSignatureKeyHash>
    g_display_list_candidates;
thread_local StaticDrawMatch g_current_draw;
// CPT world batches are often submitted directly and therefore have no
// GXCallDisplayList identity. Keep their strict geometry match for one draw
// only, and cache the result by the actual GC geometry signature.
thread_local bool g_current_draw_transient_world = false;
thread_local u64 g_current_world_direct_key = 0;
struct ActiveDisplayListContext
{
  u32 address = 0;
  u32 size = 0;
  u64 command_hash = 0;

  explicit operator bool() const { return address != 0 && size > 52; }
};
thread_local ActiveDisplayListContext g_active_display_list;

// PERF v21: FindDisplayList() and SetDisplayListContext() run back-to-back for
// the same GX display list. Reuse the authoritative tail hash instead of
// hashing commands[52..] twice on the Video thread.
struct PendingDisplayListHash
{
  u32 address = 0;
  u32 size = 0;
  const u8* data = nullptr;
  u64 hash = 0;
  bool valid = false;
};
thread_local PendingDisplayListHash g_pending_display_list_hash;

std::unordered_map<u64, StaticDrawMatch> g_world_direct_matches;
std::unordered_set<u64> g_world_direct_rejected;
// v9.7: claim authored CPT descriptor MEMBERS, not only an exact range name.
// v9.6 could still accept 0x4 and 0x5 for unrelated GC draws even though those
// ranges overlap on descriptors 0..3. Descriptor ownership makes dynamic ranges
// non-overlapping while preserving cache reuse for the exact same GC identity.
std::unordered_map<std::string, u64> g_world_dynamic_descriptor_claims;
std::unordered_map<u64, std::vector<std::string>> g_world_direct_descriptor_keys;
// A decoded CPT sector may be claimed by only one direct GC geometry identity.
// This prevents equal/near-equal AABBs from mapping dozens of unrelated batches
// to the same PS3 sector (observed on 1_1 with v9.1).
std::unordered_map<const StaticMesh*, u64> g_world_direct_claims;
std::atomic<u64> g_matches{0}, g_draws{0}, g_vertices{0}, g_indices{0}, g_fallbacks{0};
std::unordered_set<std::string> g_draw_logged;
std::atomic<u64> g_dmf_draws{0}, g_dmf_vertices{0}, g_dmf_indices{0};
std::unordered_set<std::string> g_dmf_draw_logged;

std::mutex g_dmf_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<DMFResource>> g_dmf_cache;
struct GCSkinGroupRecord
{
  u8 bone_a = 0;
  u8 bone_b = 0;
  u16 blend_q = 0;
};

struct ExactDMFPair
{
  std::shared_ptr<DMFResource> ps3;
  u32 gc_size = 0;
  u32 gc_version_word = 0;
  std::vector<s16> ps3_group_to_gc;

  std::vector<GCSkinGroupRecord> gc_skin_groups;
  bool gc_bone_order_proven = false;

  std::size_t mapped_skin_groups = 0;
  std::string skeleton_name;
  std::size_t skeleton_name_matches = 0;
  std::shared_ptr<const MOHFrontline::PS3::SkinBind::Binding> bind;
  bool bind_vertices_valid = false;
};
std::unordered_map<std::string, ExactDMFPair> g_dmf_pairs;
struct OriginalGCDMFDrawCandidate
{
  std::shared_ptr<DMFResource> ps3;
  std::string gc_name;
  u32 file_size = 0;
  std::array<u8, 8> model_tag{};
  u32 group_count = 0;
  u32 group_offset = 0;
  u32 material_count = 0;
  u32 material_offset = 0;
  u32 dl_offset = 0;
  u32 material_index = 0;
  u32 cluster_index = 0;

  // Exact number of triangles emitted by this authored GameCube DMF DL.
  // v12 uses this as the capacity of the palette-flow partition. This avoids
  // assuming that GC and PS3 use the same cluster split.
  u32 gc_triangle_count = 0;

  std::string gc_material_name;
  std::vector<u8> gc_palette_groups;
  std::shared_ptr<const PreparedDMFDraw> prepared;
};
std::unordered_map<DisplayListSignatureKey, std::vector<OriginalGCDMFDrawCandidate>,
                   DisplayListSignatureKeyHash>
    g_dmf_display_list_candidates;
// Exact first-eight-byte filter: rejects unrelated DLs without hashing their
// entire contents. Passing this filter still requires the full signature.
std::unordered_set<DisplayListSignatureKey, DisplayListSignatureKeyHash> g_dmf_prefixes;

struct DMFTopologySignatureKey
{
  u32 size = 0;
  u64 topology_hash = 0;
  bool operator==(const DMFTopologySignatureKey&) const = default;
};
struct DMFTopologySignatureKeyHash
{
  std::size_t operator()(const DMFTopologySignatureKey& key) const noexcept
  {
    return std::hash<u64>{}(key.topology_hash ^ (u64(key.size) << 32) ^ key.size);
  }
};
struct DMFTopologyLocator
{
  DisplayListSignatureKey exact_key;
  std::string gc_name;
  u32 material_index = 0;
  u32 cluster_index = 0;
};
std::unordered_map<DMFTopologySignatureKey, std::vector<DMFTopologyLocator>,
                   DMFTopologySignatureKeyHash>
    g_dmf_player_topology_candidates;
std::unordered_set<u32> g_dmf_player_topology_sizes;

// v12 generic DMF partition. These are produced only during the level-load
// phase and are immutable while the GPU thread consumes them.
std::unordered_map<std::string, std::shared_ptr<const DMFCluster>>
    g_dmf_palette_flow_clusters;
std::unordered_map<std::string, SkinnedPaletteAnalysis>
    g_dmf_palette_flow_analyses;
std::unordered_set<std::string> g_dmf_palette_flow_material_attempts;


struct DMFAddressCacheEntry
{
  u32 address = 0, size = 0;
  const OriginalGCDMFDrawCandidate* candidate = nullptr;
};
// Fixed storage: even a first runtime address does not allocate a cache node.
std::array<DMFAddressCacheEntry, 8192> g_dmf_address_cache{};
thread_local SkinnedDrawMatch g_current_dmf_draw;
std::atomic<u64> g_dmf_lookups{0}, g_dmf_hits{0}, g_dmf_misses{0};
SkinnedPaletteAnalysis AnalyzeSkinnedPaletteAtLoad(const SkinnedDrawMatch& draw);
void PrepareDMFDraws();
std::mutex g_skl_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<SKLInfo>> g_skl_cache;
std::mutex g_emt_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<EMTResource>> g_emt_cache;

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}


bool PS3RuntimeDebugEnabled()
{
  static const bool enabled = [] {
    const char* value =
        std::getenv("MOH_PS3_DEBUG");

    // Keep compatibility with every command used during v9-v12 development.
    if (!value || !*value)
      value =
          std::getenv("MOH_PS3_DMF_VERBOSE");

    if (!value || !*value)
      return false;

    const std::string lower =
        Lower(value);

    return lower == "1" ||
           lower == "true" ||
           lower == "yes" ||
           lower == "on";
  }();

  return enabled;
}


std::string BaseName(std::string_view path)
{
  const auto slash = path.find_last_of("/\\:");
  return Lower(std::string(slash == std::string_view::npos ? path : path.substr(slash + 1)));
}

std::string CanonicalModelName(std::string_view path)
{
  std::string name = BaseName(path);
  if (name.ends_with(".msf"))
    name.replace(name.size() - 4, 4, ".msh");
  return name;
}

std::string CanonicalDMFName(std::string_view path)
{
  std::string name = BaseName(path);
  // Some tools/notes call this family DMT; retail Frontline assets use DMF.
  // Accept both spellings at the resolver boundary, but always bind to .dmf.
  if (name.ends_with(".dmt"))
    name.replace(name.size() - 4, 4, ".dmf");
  return name;
}

bool IsSupportedPlayerWeaponDMF(std::string_view name)
{
  return name == "th_weapondday.dmf" || name == "th_weapon.dmf" ||
         name == "m1_weapondday.dmf" || name == "m1_weapon.dmf";
}

bool IsPlayerWeaponAtlasMaterial(std::string_view name)
{
  return name == "m1top_256" || name == "m1side_256" ||
         name == "tom_01wo256" || name == "tom_02met256";
}

// v12.9 release policy.
//
// PS3 mohf_body uses a body bind convention that has not been proven
// compatible with the live retail GameCube XF palette.  Previous
// experimental paths produced stretched/exploded characters.
//
// This is deliberately NOT controlled by an environment variable.
// Weapons and every other independently validated DMF material remain
// eligible for PS3 replacement.
bool IsHardDisabledPS3DMFMaterial(std::string_view name)
{
  return name == "mohf_body";
}

// v12.10:
//
// The PS3 animated character geometry is deliberately disabled as a whole.
//
// Previous releases blocked only material=mohf_body. Character DMFs such as
// bm14.dmf still contain gr1/gr2/gr3/etc and those independently matched
// materials could therefore enter the PS3 skinned/native renderer.
//
// Keep the original retail GameCube geometry/XF animation for the complete
// character resource. The PS3 texture/material layer remains independent.
bool IsHardDisabledPS3DMFResource(std::string_view name)
{
  const std::string canonical =
      CanonicalDMFName(name);

  if (!canonical.ends_with(".dmf"))
    return false;

  // UHM = human/character mesh family.
  if (canonical.rfind("uhm", 0) == 0)
    return true;

  // BMxx and BMxx_* are character models:
  //
  //   bm01.dmf
  //   bm14.dmf
  //   bm37_20l.dmf
  //
  // This does NOT match:
  //
  //   m1_weapon.dmf
  //   th_weapon.dmf
  //   colt_weapon.dmf
  //   mp40_weapondsgs.dmf
  //   ...
  return canonical.size() >= 4 &&
         canonical[0] == 'b' &&
         canonical[1] == 'm' &&
         std::isdigit(
             static_cast<unsigned char>(
                 canonical[2])) &&
         std::isdigit(
             static_cast<unsigned char>(
                 canonical[3]));
}

// Retail GC skinned DMF display lists use a seven-byte vertex reference:
// matrix index + position/normal/UV indices. The game may copy/reindex those
// references for animated first-person weapons, which destroys the byte hash
// even though the authored primitive topology is unchanged. Hash only the GX
// primitive opcode/count sequence; never the mutable vertex references.
bool BuildPlayerWeaponDMFTopologySignature(std::span<const u8> commands, u64* out_hash)
{
  if (!out_hash || commands.size() < 3)
    return false;

  constexpr std::size_t vertex_reference_stride = 7;
  constexpr u64 fnv_offset = 1469598103934665603ULL;
  constexpr u64 fnv_prime = 1099511628211ULL;
  u64 hash = fnv_offset;
  u32 primitives = 0;
  std::size_t offset = 0;

  const auto mix = [&](u8 value) {
    hash ^= value;
    hash *= fnv_prime;
  };

  while (offset < commands.size())
  {
    const u8 opcode = commands[offset];
    if (opcode == 0)
    {
      // DMF records are 32-byte aligned. Only zero padding is allowed after
      // the final primitive so an unrelated command stream cannot match.
      for (; offset < commands.size(); ++offset)
        if (commands[offset] != 0)
          return false;
      break;
    }

    if (opcode < 0x80 || opcode > 0xBF || commands.size() - offset < 3)
      return false;

    const u16 count = static_cast<u16>((u16(commands[offset + 1]) << 8) |
                                       u16(commands[offset + 2]));
    if (!count)
      return false;

    const std::size_t remaining = commands.size() - offset - 3;
    if (count > remaining / vertex_reference_stride)
      return false;

    mix(opcode);
    mix(static_cast<u8>(count >> 8));
    mix(static_cast<u8>(count));
    offset += 3 + static_cast<std::size_t>(count) * vertex_reference_stride;
    ++primitives;
  }

  if (!primitives)
    return false;

  mix(static_cast<u8>(primitives >> 24));
  mix(static_cast<u8>(primitives >> 16));
  mix(static_cast<u8>(primitives >> 8));
  mix(static_cast<u8>(primitives));
  *out_hash = hash;
  return true;
}


std::string DMFMaterialPartitionKey(std::string_view gc_name, u32 material_index,
                                    std::string_view material_name)
{
  return std::string(gc_name) + "\n" + std::to_string(material_index) + "\n" +
         std::string(material_name);
}

std::string DMFDrawPartitionKey(std::string_view gc_name, u32 material_index,
                                u32 cluster_index, std::string_view material_name)
{
  return DMFMaterialPartitionKey(gc_name, material_index, material_name) + "\n" +
         std::to_string(cluster_index);
}

// Retail GC skinned DMF DL vertices use the proven seven-byte reference:
//
//   pos-matrix + position-index + normal-index + uv-index
//
// Count the actual primitive topology instead of inferring it from the byte
// size. This count is the one invariant needed to split one PS3 material over
// a completely different GC cluster layout.
bool CountGCDMFTriangles(std::span<const u8> commands, u32* out_triangles)
{
  if (!out_triangles)
    return false;

  *out_triangles = 0;
  constexpr std::size_t vertex_reference_stride = 7;

  std::uint64_t triangles = 0;
  std::size_t offset = 0;
  bool saw_primitive = false;

  while (offset < commands.size())
  {
    const u8 opcode = commands[offset];

    if (opcode == 0)
    {
      // DMF display lists are padded to 32 bytes.
      for (; offset < commands.size(); ++offset)
      {
        if (commands[offset] != 0)
          return false;
      }
      break;
    }

    if (opcode < 0x80 || opcode > 0xBF || commands.size() - offset < 3)
      return false;

    const u16 count =
        static_cast<u16>((u16(commands[offset + 1]) << 8) |
                         u16(commands[offset + 2]));

    if (!count)
      return false;

    const std::size_t payload =
        static_cast<std::size_t>(count) * vertex_reference_stride;

    if (payload > commands.size() - offset - 3)
      return false;

    // VAT lives in the low three bits.
    switch (opcode & 0xF8)
    {
    case 0x80: // GX_QUADS
    case 0x88: // GX_QUADS_2
      if ((count % 4) != 0)
        return false;
      triangles += static_cast<std::uint64_t>(count / 4) * 2;
      break;

    case 0x90: // GX_TRIANGLES
      if ((count % 3) != 0)
        return false;
      triangles += count / 3;
      break;

    case 0x98: // GX_TRIANGLE_STRIP
    case 0xA0: // GX_TRIANGLE_FAN
      if (count >= 3)
        triangles += count - 2;
      break;

    default:
      // A skinned surface replacement is triangle-only. Do not guess how a
      // line/point primitive should correspond to the PS3 triangle stream.
      return false;
    }

    saw_primitive = true;
    offset += 3 + payload;
  }

  if (!saw_primitive || triangles == 0 ||
      triangles > std::numeric_limits<u32>::max())
    return false;

  *out_triangles = static_cast<u32>(triangles);
  return true;
}

// Solve the generic GC<->PS3 cluster mismatch as a capacity-constrained
// bipartite graph:
//
//       PS3 triangle ---- compatible GC palette/DL
//
// Every PS3 triangle has capacity 1. Every GC DL has capacity equal to the
// exact triangle count of its original command stream. A complete max-flow
// therefore proves simultaneously that:
//
//   * no PS3 triangle is duplicated;
//   * no PS3 triangle is omitted;
//   * every triangle is rendered only under a GC palette containing all its
//     mapped skin groups;
//   * every original GC DL receives its exact authored triangle quota.
//
// This deliberately does NOT use GC cluster ordinal == PS3 cluster ordinal.
void EnsureDMFPaletteFlowPartition(const SkinnedDrawMatch& draw)
{
  if (!draw || !draw.owner || !draw.owner->decoded ||
      draw.gc_material_name.empty() || draw.gc_palette_groups.empty() ||
      draw.ps3_group_to_gc.empty() || IsSupportedPlayerWeaponDMF(draw.gc_name) ||
      IsHardDisabledPS3DMFResource(draw.gc_name) ||
      IsHardDisabledPS3DMFMaterial(draw.gc_material_name))
  {
    return;
  }

  const std::string material_key =
      DMFMaterialPartitionKey(draw.gc_name, draw.material_index,
                              draw.gc_material_name);

  if (!g_dmf_palette_flow_material_attempts.insert(material_key).second)
    return;

  static unsigned reject_logs = 0;
  const auto reject = [&](const char* reason) {
    if (PS3RuntimeDebugEnabled() && reject_logs++ < 96)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-dmf] PALETTE-FLOW REJECT: gc=%.*s material=%.*s "
          "reason=%s -> GC\n",
          static_cast<int>(draw.gc_name.size()), draw.gc_name.data(),
          static_cast<int>(draw.gc_material_name.size()),
          draw.gc_material_name.data(), reason);
    }
  };

  // Ordered only for stable output/logging. The graph itself does not assume
  // any relation between this order and the PS3 cluster order.
  std::vector<const OriginalGCDMFDrawCandidate*> gc_draws;

  for (const auto& [signature, candidates] : g_dmf_display_list_candidates)
  {
    (void)signature;

    for (const auto& candidate : candidates)
    {
      if (candidate.gc_name != draw.gc_name ||
          candidate.material_index != draw.material_index ||
          candidate.gc_material_name != draw.gc_material_name)
      {
        continue;
      }

      if (!candidate.gc_triangle_count ||
          candidate.gc_palette_groups.empty())
      {
        reject("gc-triangle-topology-unavailable");
        return;
      }

      gc_draws.push_back(&candidate);
    }
  }

  if (gc_draws.size() < 2)
    return;

  std::sort(gc_draws.begin(), gc_draws.end(),
            [](const auto* a, const auto* b) {
              return a->cluster_index < b->cluster_index;
            });

  for (std::size_t i = 1; i < gc_draws.size(); ++i)
  {
    if (gc_draws[i - 1]->cluster_index == gc_draws[i]->cluster_index)
    {
      reject("duplicate-gc-cluster-index");
      return;
    }
  }

  std::uint64_t gc_total64 = 0;
  for (const auto* candidate : gc_draws)
    gc_total64 += candidate->gc_triangle_count;

  if (!gc_total64 || gc_total64 > 200000)
  {
    reject("gc-triangle-total-invalid");
    return;
  }

  const std::size_t gc_total = static_cast<std::size_t>(gc_total64);
  const auto& decoded = *draw.owner->decoded;

  // Material-table indices are platform-local. Pick the PS3 material only
  // when the exact material name AND complete triangle total identify one
  // unique PS3 material slot.
  std::unordered_map<u32, std::size_t> ps3_triangle_totals;
  std::unordered_map<u32, std::size_t> ps3_cluster_totals;

  for (const auto& cluster : decoded.clusters)
  {
    if (cluster.material_name != draw.gc_material_name)
      continue;

    if (cluster.indices.empty() || (cluster.indices.size() % 3) != 0)
    {
      reject("malformed-ps3-index-stream");
      return;
    }

    ps3_triangle_totals[cluster.material_index] += cluster.indices.size() / 3;
    ++ps3_cluster_totals[cluster.material_index];
  }

  std::vector<u32> matching_ps3_materials;
  for (const auto& [material_index, triangles] : ps3_triangle_totals)
  {
    if (triangles == gc_total)
      matching_ps3_materials.push_back(material_index);
  }

  if (matching_ps3_materials.size() != 1)
  {
    reject("material-triangle-total-not-unique");
    return;
  }

  const u32 ps3_material_index = matching_ps3_materials.front();

  std::vector<std::size_t> source_clusters;
  for (std::size_t i = 0; i < decoded.clusters.size(); ++i)
  {
    const auto& cluster = decoded.clusters[i];
    if (cluster.material_index == ps3_material_index &&
        cluster.material_name == draw.gc_material_name)
    {
      source_clusters.push_back(i);
    }
  }

  std::sort(source_clusters.begin(), source_clusters.end(),
            [&](std::size_t a, std::size_t b) {
              const auto& ca = decoded.clusters[a];
              const auto& cb = decoded.clusters[b];

              if (ca.material_cluster_index != cb.material_cluster_index)
                return ca.material_cluster_index < cb.material_cluster_index;

              return a < b;
            });

  if (source_clusters.empty())
  {
    reject("ps3-material-has-no-clusters");
    return;
  }

  struct TriangleRef
  {
    std::size_t cluster_ordinal = 0;
    std::size_t index_offset = 0;
    std::array<s16, 3> gc_groups{-1, -1, -1};
  };

  std::vector<TriangleRef> triangles;
  triangles.reserve(gc_total);

  for (const std::size_t cluster_ordinal : source_clusters)
  {
    const DMFCluster& cluster = decoded.clusters[cluster_ordinal];

    if (!cluster.has_position || !cluster.has_normal || !cluster.has_uv0 ||
        cluster.positions.empty() ||
        cluster.positions.size() != cluster.normals.size() ||
        cluster.positions.size() != cluster.uv0.size() ||
        cluster.positions.size() != cluster.vertex_palette_slots.size())
    {
      reject("ps3-cluster-attributes-incomplete");
      return;
    }

    for (std::size_t tri = 0; tri + 2 < cluster.indices.size(); tri += 3)
    {
      TriangleRef ref;
      ref.cluster_ordinal = cluster_ordinal;
      ref.index_offset = tri;

      for (std::size_t corner = 0; corner < 3; ++corner)
      {
        const u16 vertex = cluster.indices[tri + corner];

        if (vertex >= cluster.vertex_palette_slots.size())
        {
          reject("ps3-vertex-index-out-of-range");
          return;
        }

        const u16 local_slot = cluster.vertex_palette_slots[vertex];

        if (local_slot >= cluster.palette_groups.size())
        {
          reject("ps3-local-palette-slot-out-of-range");
          return;
        }

        const u16 ps3_group = cluster.palette_groups[local_slot];

        if (ps3_group >= draw.ps3_group_to_gc.size())
        {
          reject("ps3-group-out-of-map");
          return;
        }

        const s16 gc_group = draw.ps3_group_to_gc[ps3_group];

        if (gc_group < 0 || gc_group > 255)
        {
          reject("unmapped-ps3-skin-group");
          return;
        }

        ref.gc_groups[corner] = gc_group;
      }

      triangles.push_back(ref);
    }
  }

  if (triangles.size() != gc_total)
  {
    reject("gc-ps3-material-triangle-total-mismatch");
    return;
  }

  const auto palette_contains =
      [](const std::vector<u8>& palette,
         const std::array<s16, 3>& groups) {
        for (const s16 group : groups)
        {
          if (group < 0 || group > 255 ||
              std::find(palette.begin(), palette.end(),
                        static_cast<u8>(group)) == palette.end())
          {
            return false;
          }
        }
        return true;
      };

  // Dinic max-flow. Graph depth is only:
  // source -> triangle -> GC draw -> sink.
  struct FlowEdge
  {
    int to = 0;
    int reverse = 0;
    int capacity = 0;
    int initial_capacity = 0;
  };

  const int triangle_count = static_cast<int>(triangles.size());
  const int draw_count = static_cast<int>(gc_draws.size());
  const int source = 0;
  const int triangle_base = 1;
  const int draw_base = triangle_base + triangle_count;
  const int sink = draw_base + draw_count;
  const int node_count = sink + 1;

  std::vector<std::vector<FlowEdge>> graph(node_count);

  const auto add_edge =
      [&](int from, int to, int capacity) {
        const int from_reverse = static_cast<int>(graph[to].size());
        const int to_reverse = static_cast<int>(graph[from].size());

        graph[from].push_back(
            {to, from_reverse, capacity, capacity});
        graph[to].push_back(
            {from, to_reverse, 0, 0});
      };

  for (int tri = 0; tri < triangle_count; ++tri)
  {
    add_edge(source, triangle_base + tri, 1);

    bool has_compatible_draw = false;

    for (int gc = 0; gc < draw_count; ++gc)
    {
      if (!palette_contains(gc_draws[gc]->gc_palette_groups,
                            triangles[tri].gc_groups))
      {
        continue;
      }

      add_edge(triangle_base + tri, draw_base + gc, 1);
      has_compatible_draw = true;
    }

    if (!has_compatible_draw)
    {
      reject("triangle-has-no-compatible-gc-palette");
      return;
    }
  }

  for (int gc = 0; gc < draw_count; ++gc)
  {
    if (gc_draws[gc]->gc_triangle_count >
        static_cast<u32>(std::numeric_limits<int>::max()))
    {
      reject("gc-draw-capacity-overflow");
      return;
    }

    add_edge(draw_base + gc, sink,
             static_cast<int>(gc_draws[gc]->gc_triangle_count));
  }

  std::vector<int> level(node_count);
  std::vector<std::size_t> edge_cursor(node_count);

  const auto bfs = [&]() {
    std::fill(level.begin(), level.end(), -1);

    std::queue<int> queue;
    level[source] = 0;
    queue.push(source);

    while (!queue.empty())
    {
      const int node = queue.front();
      queue.pop();

      for (const FlowEdge& edge : graph[node])
      {
        if (edge.capacity <= 0 || level[edge.to] >= 0)
          continue;

        level[edge.to] = level[node] + 1;
        queue.push(edge.to);
      }
    }

    return level[sink] >= 0;
  };

  std::function<int(int, int)> dfs =
      [&](int node, int pushed) -> int {
        if (node == sink || pushed == 0)
          return pushed;

        for (std::size_t& cursor = edge_cursor[node];
             cursor < graph[node].size(); ++cursor)
        {
          FlowEdge& edge = graph[node][cursor];

          if (edge.capacity <= 0 ||
              level[edge.to] != level[node] + 1)
          {
            continue;
          }

          const int amount =
              dfs(edge.to, std::min(pushed, edge.capacity));

          if (!amount)
            continue;

          edge.capacity -= amount;
          graph[edge.to][edge.reverse].capacity += amount;
          return amount;
        }

        return 0;
      };

  int flow = 0;

  while (bfs())
  {
    std::fill(edge_cursor.begin(), edge_cursor.end(), 0);

    while (const int pushed =
               dfs(source, std::numeric_limits<int>::max()))
    {
      flow += pushed;
    }
  }

  if (flow != triangle_count)
  {
    reject("palette-flow-incomplete");
    return;
  }

  std::vector<int> triangle_to_draw(triangle_count, -1);
  std::vector<std::vector<std::size_t>> draw_triangles(draw_count);

  for (int tri = 0; tri < triangle_count; ++tri)
  {
    const int node = triangle_base + tri;

    for (const FlowEdge& edge : graph[node])
    {
      if (edge.initial_capacity != 1 ||
          edge.to < draw_base || edge.to >= draw_base + draw_count ||
          edge.capacity != 0)
      {
        continue;
      }

      const int gc = edge.to - draw_base;

      if (triangle_to_draw[tri] >= 0)
      {
        reject("triangle-multiply-assigned");
        return;
      }

      triangle_to_draw[tri] = gc;
      draw_triangles[gc].push_back(static_cast<std::size_t>(tri));
    }

    if (triangle_to_draw[tri] < 0)
    {
      reject("triangle-unassigned-after-flow");
      return;
    }
  }

  // Convert each flow bucket into the same host-side DMFCluster shape already
  // consumed by BuildCurrentSkinnedReplacement()/VertexManagerBase.
  for (int gc = 0; gc < draw_count; ++gc)
  {
    const OriginalGCDMFDrawCandidate& gc_draw = *gc_draws[gc];

    if (draw_triangles[gc].size() != gc_draw.gc_triangle_count)
    {
      reject("gc-draw-triangle-quota-not-saturated");
      return;
    }

    auto subset = std::make_shared<DMFCluster>();
    subset->material_index = ps3_material_index;
    subset->material_cluster_index = gc_draw.cluster_index;
    subset->material_name = gc_draw.gc_material_name;
    subset->has_position = true;
    subset->has_normal = true;
    subset->has_uv0 = true;

    std::unordered_map<u64, u16> vertex_remap;
    std::unordered_map<u16, u16> palette_remap;
    bool have_texture_index = false;

    for (const std::size_t triangle_id : draw_triangles[gc])
    {
      const TriangleRef& triangle = triangles[triangle_id];
      const DMFCluster& source_cluster =
          decoded.clusters[triangle.cluster_ordinal];

      if (!have_texture_index)
      {
        subset->texture_index = source_cluster.texture_index;
        have_texture_index = true;
      }

      for (std::size_t corner = 0; corner < 3; ++corner)
      {
        const u16 source_vertex =
            source_cluster.indices[triangle.index_offset + corner];

        const u64 vertex_key =
            (u64(triangle.cluster_ordinal) << 32) |
            u64(source_vertex);

        auto vertex_it = vertex_remap.find(vertex_key);
        u16 destination_vertex = 0;

        if (vertex_it == vertex_remap.end())
        {
          if (subset->positions.size() >= 65535)
          {
            reject("partition-vertex-count-overflow");
            return;
          }

          const u16 source_local_slot =
              source_cluster.vertex_palette_slots[source_vertex];

          if (source_local_slot >= source_cluster.palette_groups.size())
          {
            reject("partition-source-palette-slot-invalid");
            return;
          }

          const u16 ps3_group =
              source_cluster.palette_groups[source_local_slot];

          auto palette_it = palette_remap.find(ps3_group);
          u16 destination_palette_slot = 0;

          if (palette_it == palette_remap.end())
          {
            if (subset->palette_groups.size() >= 65535)
            {
              reject("partition-palette-overflow");
              return;
            }

            destination_palette_slot =
                static_cast<u16>(subset->palette_groups.size());

            subset->palette_groups.push_back(ps3_group);
            palette_remap.emplace(ps3_group,
                                  destination_palette_slot);
          }
          else
          {
            destination_palette_slot = palette_it->second;
          }

          destination_vertex =
              static_cast<u16>(subset->positions.size());

          subset->positions.push_back(
              source_cluster.positions[source_vertex]);
          subset->normals.push_back(
              source_cluster.normals[source_vertex]);
          subset->uv0.push_back(
              source_cluster.uv0[source_vertex]);
          subset->vertex_palette_slots.push_back(
              destination_palette_slot);

          vertex_remap.emplace(vertex_key, destination_vertex);
        }
        else
        {
          destination_vertex = vertex_it->second;
        }

        subset->indices.push_back(destination_vertex);
      }
    }

    if (subset->indices.size() !=
            std::size_t(gc_draw.gc_triangle_count) * 3 ||
        subset->positions.empty() ||
        subset->positions.size() != subset->normals.size() ||
        subset->positions.size() != subset->uv0.size() ||
        subset->positions.size() != subset->vertex_palette_slots.size())
    {
      reject("partition-subset-invalid");
      return;
    }

    SkinnedPaletteAnalysis analysis;
    analysis.valid = true;
    analysis.ps3_material_index = ps3_material_index;
    analysis.ps3_cluster_ordinal = 0xffffffffu;
    analysis.exact_cluster_identity = false;
    analysis.exact_triangle_partition = true;
    analysis.ps3_material_candidates = ps3_triangle_totals.size();
    analysis.compatible_ps3_materials = 1;
    analysis.ps3_material_clusters = source_clusters.size();
    analysis.total_triangles = gc_draw.gc_triangle_count;
    analysis.selected_triangles = gc_draw.gc_triangle_count;
    analysis.ambiguous_triangles = 0;
    analysis.unmapped_triangles = 0;
    analysis.selected_vertices = subset->positions.size();
    analysis.matrix_slots = gc_draw.gc_palette_groups.size();

    const std::string draw_key =
        DMFDrawPartitionKey(gc_draw.gc_name,
                            gc_draw.material_index,
                            gc_draw.cluster_index,
                            gc_draw.gc_material_name);

    g_dmf_palette_flow_clusters[draw_key] = subset;
    g_dmf_palette_flow_analyses[draw_key] = analysis;
  }

  std::fprintf(
      stderr,
      "[moh-ps3-dmf] PALETTE-FLOW PARTITION READY: "
      "gc=%.*s material=%.*s gc_draws=%zu ps3_clusters=%zu "
      "triangles=%zu ps3_material=%u | every PS3 triangle claimed once\n",
      static_cast<int>(draw.gc_name.size()), draw.gc_name.data(),
      static_cast<int>(draw.gc_material_name.size()),
      draw.gc_material_name.data(), gc_draws.size(),
      source_clusters.size(), triangles.size(), ps3_material_index);
}

std::string FixedString(const u8* p, std::size_t size)
{
  std::size_t n = 0;
  while (n < size && p[n] != 0) ++n;
  return std::string(reinterpret_cast<const char*>(p), n);
}

bool EnvSwitchLocal(const char* name, bool fallback)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  const std::string lower = Lower(value);
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
    return true;
  if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
    return false;
  return fallback;
}

[[maybe_unused]] float EnvFloatLocal(const char* name, float fallback, float minimum, float maximum)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  if (end == value || !std::isfinite(parsed))
    return fallback;
  return std::clamp(parsed, minimum, maximum);
}

struct Bounds3
{
  std::array<float, 3> minimum{
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity()};
  std::array<float, 3> maximum{
      -std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity()};
  bool valid = false;
};

bool IncludePoint(Bounds3* bounds, const std::array<float, 3>& p)
{
  if (!bounds)
    return false;

  for (float v : p)
  {
    if (!std::isfinite(v) || std::abs(v) > 10000000.0f)
      return false;
  }

  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    bounds->minimum[axis] = std::min(bounds->minimum[axis], p[axis]);
    bounds->maximum[axis] = std::max(bounds->maximum[axis], p[axis]);
  }
  bounds->valid = true;
  return true;
}

Bounds3 BoundsFromGC(std::span<const u8> vertices, u32 count, u32 stride, u32 position_offset)
{
  Bounds3 out;
  if (!count || stride < sizeof(float) * 3 ||
      position_offset > stride || sizeof(float) * 3 > stride - position_offset ||
      vertices.size() < std::size_t(count) * stride)
  {
    return out;
  }

  for (u32 i = 0; i < count; ++i)
  {
    std::array<float, 3> p{};
    std::memcpy(p.data(), vertices.data() + std::size_t(i) * stride + position_offset,
                sizeof(float) * 3);
    if (!IncludePoint(&out, p))
      return {};
  }
  return out;
}

Bounds3 BoundsFromPS3(const Submesh& submesh)
{
  Bounds3 out;
  if (submesh.position_uv.size() != submesh.vertex_count)
    return out;

  for (const auto& vertex : submesh.position_uv)
  {
    if (!IncludePoint(&out, vertex.position))
      return {};
  }
  return out;
}

float BoundsScore(const Bounds3& gc, const Bounds3& ps3)
{
  if (!gc.valid || !ps3.valid)
    return std::numeric_limits<float>::infinity();

  float extent_error = 0.0f;
  float center_error = 0.0f;
  float global_scale = 0.001f;

  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    const float ge = gc.maximum[axis] - gc.minimum[axis];
    const float pe = ps3.maximum[axis] - ps3.minimum[axis];
    global_scale = std::max(global_scale, std::max(std::abs(ge), std::abs(pe)));
    const float denom = std::max(0.001f, std::max(std::abs(ge), std::abs(pe)));
    extent_error += std::abs(ge - pe) / denom;
  }

  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    const float gc_center = (gc.minimum[axis] + gc.maximum[axis]) * 0.5f;
    const float ps3_center = (ps3.minimum[axis] + ps3.maximum[axis]) * 0.5f;
    center_error += std::abs(gc_center - ps3_center) / global_scale;
  }

  return extent_error / 3.0f + (center_error / 3.0f) * 0.20f;
}

float BoundsExtentScore(const Bounds3& gc, const Bounds3& ps3)
{
  if (!gc.valid || !ps3.valid)
    return std::numeric_limits<float>::infinity();

  float extent_error = 0.0f;
  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    const float ge = gc.maximum[axis] - gc.minimum[axis];
    const float pe = ps3.maximum[axis] - ps3.minimum[axis];
    const float denom = std::max(0.001f, std::max(std::abs(ge), std::abs(pe)));
    extent_error += std::abs(ge - pe) / denom;
  }
  return extent_error / 3.0f;
}

std::array<float, 3> BoundsCenterDelta(const Bounds3& target, const Bounds3& source)
{
  std::array<float, 3> delta{};
  if (!target.valid || !source.valid)
    return delta;

  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    const float target_center = (target.minimum[axis] + target.maximum[axis]) * 0.5f;
    const float source_center = (source.minimum[axis] + source.maximum[axis]) * 0.5f;
    delta[axis] = target_center - source_center;
  }
  return delta;
}

bool IsWorldCPTMesh(const StaticMesh& mesh)
{
  const std::string_view source(mesh.source_name);
  return source.find(".cpt#cpt-inline-") != std::string_view::npos ||
         source.find(".cpt#cpt-rsx-") != std::string_view::npos ||
         source.find(".cpt#cpt-pack-") != std::string_view::npos;
}

// v9.4: one retail GC world draw is often coarser than one PS3 CPT descriptor.
// Build a host-only aggregate from consecutive descriptors belonging to the
// SAME *_ART_cN.cpt.  The aggregate is never copied into guest memory; it only
// lets the existing strict bounds/topology matcher compare one GC batch with a
// small authored PS3 descriptor run.
using MOHFrontline::Meshes::BuildWorldCPTPack;

// v9.5: retain the original descriptor order per CPT chunk. Fixed 2/4/8/16
// packs are only probes; the exact GC batch boundary can fall anywhere inside
// the authored PS3 descriptor stream. Search contiguous ranges on demand and
// materialise only the one range that wins strict matching.
struct WorldCPTSequence
{
  std::string source_name;
  std::vector<std::shared_ptr<StaticMesh>> meshes;
};

std::vector<WorldCPTSequence> g_world_cpt_sequences;

std::shared_ptr<StaticMesh> g_world_full_level_mesh;
std::shared_ptr<std::vector<std::array<float, 3>>> g_world_full_level_normals;

// v10.1 experimental full-level proof-of-life renderer. Unlike fixed/dynamic
// packs, this aggregate is not used for geometry matching: it is a single host
// draw containing every decoded *_ART_cN.cpt descriptor for the active level.
// v9.9 has already applied each descriptor's exact NODE70 affine transform, so
// this gives us the first way to inspect the complete PS3 world independently
// of GameCube batch boundaries.
std::shared_ptr<StaticMesh> BuildWorldCPTFullLevelMesh(
    const std::vector<WorldCPTSequence>& sequences,
    std::shared_ptr<std::vector<std::array<float, 3>>>* out_normals)
{
  if (!out_normals)
    return {};

  auto full = std::make_shared<StaticMesh>();
  Submesh merged;
  merged.has_uv0 = true;
  merged.has_uv1 = true;
  merged.has_normal = true;
  merged.vertex_stride = 32;

  std::size_t vertex_base = 0;
  std::size_t total_indices = 0;
  std::size_t member_ordinal = 0;
  std::size_t synthesized_normal_members = 0;
  std::size_t synthesized_normal_vertices = 0;
  std::size_t synthesized_uv1_members = 0;
  bool have_attributes = false;

  for (const auto& sequence : sequences)
  {
    for (const auto& mesh : sequence.meshes)
    {
      if (!mesh || mesh->submeshes.size() != 1)
        continue;
      const auto& sub = mesh->submeshes[0];
      if (!sub.vertex_count || sub.position_uv.size() != sub.vertex_count ||
          sub.indices.empty() || (sub.indices.size() % 3) != 0)
        continue;

      if (vertex_base + sub.vertex_count > 65535u ||
          total_indices + sub.indices.size() > 300000u)
      {
        std::fprintf(stderr,
                     "[moh-ps3-world-full] BUILD REJECT: vertices=%zu+%u indices=%zu+%zu exceed host aggregate limits\n",
                     vertex_base, sub.vertex_count, total_indices, sub.indices.size());
        return {};
      }

      if (!have_attributes)
      {
        merged.attributes = sub.attributes;
        have_attributes = true;
      }

      // A small subset of CPT descriptors uses the compact 20-byte stream
      // (position + UV0 only).  v10.1 ANDed has_normal/has_uv1 across every
      // descriptor, so those few meshes invalidated the whole 770-mesh level.
      // For the full-level proof-of-life draw, rebuild missing vertex normals
      // from authored triangle geometry and mirror UV0 into UV1 when the
      // descriptor has no secondary/lightmap coordinate.
      auto vertices = sub.position_uv;
      if (!sub.has_normal)
      {
        std::vector<std::array<float, 3>> accumulated(vertices.size(), {0.0f, 0.0f, 0.0f});
        for (std::size_t tri = 0; tri + 2 < sub.indices.size(); tri += 3)
        {
          const u16 ia = sub.indices[tri + 0];
          const u16 ib = sub.indices[tri + 1];
          const u16 ic = sub.indices[tri + 2];
          if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size())
            continue;

          const auto& a = vertices[ia].position;
          const auto& b = vertices[ib].position;
          const auto& c = vertices[ic].position;
          const float abx = b[0] - a[0];
          const float aby = b[1] - a[1];
          const float abz = b[2] - a[2];
          const float acx = c[0] - a[0];
          const float acy = c[1] - a[1];
          const float acz = c[2] - a[2];
          const std::array<float, 3> n{
              aby * acz - abz * acy,
              abz * acx - abx * acz,
              abx * acy - aby * acx};
          const float length2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
          if (!std::isfinite(length2) || length2 <= 1.0e-18f)
            continue;
          for (const u16 index : {ia, ib, ic})
          {
            accumulated[index][0] += n[0];
            accumulated[index][1] += n[1];
            accumulated[index][2] += n[2];
          }
        }

        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
          auto n = accumulated[i];
          const float length2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
          if (std::isfinite(length2) && length2 > 1.0e-18f)
          {
            const float inverse_length = 1.0f / std::sqrt(length2);
            n[0] *= inverse_length;
            n[1] *= inverse_length;
            n[2] *= inverse_length;
          }
          else
          {
            // Degenerate/unreferenced vertices are harmless in this debug
            // aggregate; keep a finite fallback so the host stream is valid.
            n = {0.0f, 0.0f, 1.0f};
          }
          vertices[i].normal = n;
        }
        ++synthesized_normal_members;
        synthesized_normal_vertices += vertices.size();
      }

      if (!sub.has_uv1)
      {
        for (auto& vertex : vertices)
          vertex.uv1 = vertex.uv0;
        ++synthesized_uv1_members;
      }

      merged.material_hints.push_back(
          "@cpt-full-span=member:" + std::to_string(member_ordinal) +
          ";source:" + mesh->source_name +
          ";first-index:" + std::to_string(total_indices) +
          ";index-count:" + std::to_string(sub.indices.size()) +
          ";first-vertex:" + std::to_string(vertex_base) +
          ";vertex-count:" + std::to_string(sub.vertex_count));
      for (const std::string& hint : sub.material_hints)
      {
        if (hint.rfind("@cpt-", 0) == 0)
          merged.material_hints.push_back("@cpt-full-member=" +
                                          std::to_string(member_ordinal) + ";" + hint);
      }

      merged.position_uv.insert(merged.position_uv.end(), vertices.begin(), vertices.end());
      for (u16 index : sub.indices)
      {
        const std::size_t shifted = vertex_base + index;
        if (shifted > 65535u)
          return {};
        merged.indices.push_back(static_cast<u16>(shifted));
      }

      vertex_base += sub.vertex_count;
      total_indices += sub.indices.size();
      ++member_ordinal;
    }
  }

  if (!have_attributes || merged.position_uv.empty() || merged.indices.empty())
    return {};

  // Every aggregate vertex now has a finite normal and both UV channels,
  // either authored by the PS3 CPT or synthesized above.
  merged.has_uv0 = true;
  merged.has_uv1 = true;
  merged.has_normal = true;
  merged.vertex_count = static_cast<u32>(merged.position_uv.size());
  merged.index_count = static_cast<u32>(merged.indices.size());

  auto normals = std::make_shared<std::vector<std::array<float, 3>>>();
  normals->reserve(merged.position_uv.size());
  for (const auto& vertex : merged.position_uv)
    normals->push_back(vertex.normal);

  std::fprintf(stderr,
               "[moh-ps3-world-full] STREAM NORMALIZED: members=%zu vertices=%zu "
               "indices=%zu synth-normal-members=%zu synth-normal-verts=%zu "
               "synth-uv1-members=%zu\n",
               member_ordinal, merged.position_uv.size(), merged.indices.size(),
               synthesized_normal_members, synthesized_normal_vertices,
               synthesized_uv1_members);

  full->source_name = "data/@host/full_level.cpt#cpt-full-level";
  full->submeshes.push_back(std::move(merged));
  *out_normals = std::move(normals);
  return full;
}

std::string WorldDescriptorClaimKey(std::string_view source, std::size_t descriptor)
{
  std::string key;
  key.reserve(source.size() + 32);
  key.append(source);
  key.push_back('\n');
  key.append(std::to_string(descriptor));
  return key;
}

std::vector<std::string> WorldRangeDescriptorKeys(std::string_view source, std::size_t first,
                                                  std::size_t count)
{
  std::vector<std::string> keys;
  keys.reserve(count);
  for (std::size_t member = 0; member < count; ++member)
    keys.push_back(WorldDescriptorClaimKey(source, first + member));
  return keys;
}

void UnionBounds(Bounds3* target, const Bounds3& source)
{
  if (!target || !source.valid)
    return;
  if (!target->valid)
  {
    *target = source;
    return;
  }
  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    target->minimum[axis] = std::min(target->minimum[axis], source.minimum[axis]);
    target->maximum[axis] = std::max(target->maximum[axis], source.maximum[axis]);
  }
  target->valid = true;
}

u64 WorldDirectKey(std::span<const u8> vertices, u32 count, u32 stride, u32 position_offset,
                   u32 triangle_count, const Bounds3& bounds)
{
  // Runtime cache identity only -- this is not used to decide GC<->PS3
  // equivalence. Include bounds plus samples of the real portable positions
  // so two world draws with the same AABB do not alias.
  constexpr u64 offset_basis = 1469598103934665603ULL;
  constexpr u64 prime = 1099511628211ULL;
  u64 hash = offset_basis;
  const auto mix = [&](const void* data, std::size_t size) {
    const auto* p = static_cast<const u8*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
      hash ^= p[i];
      hash *= prime;
    }
  };

  mix(bounds.minimum.data(), sizeof(float) * bounds.minimum.size());
  mix(bounds.maximum.data(), sizeof(float) * bounds.maximum.size());
  mix(&count, sizeof(count));
  mix(&stride, sizeof(stride));
  mix(&triangle_count, sizeof(triangle_count));
  const u32 step = std::max(1u, count / 16u);
  for (u32 i = 0; i < count; i += step)
    mix(vertices.data() + std::size_t(i) * stride + position_offset, sizeof(float) * 3);
  return hash;
}

StaticDrawMatch BuildStaticDrawMatch(const std::shared_ptr<StaticMesh>& mesh,
                                         std::size_t submesh_index)
{
  if (!mesh || submesh_index >= mesh->submeshes.size())
    return {};

  const auto& sub = mesh->submeshes[submesh_index];
  if (!sub.vertex_count || sub.position_uv.size() != sub.vertex_count ||
      sub.indices.empty() || sub.indices.size() % 3)
    return {};

  auto normals = std::make_shared<std::vector<std::array<float, 3>>>(sub.vertex_count);
  for (std::size_t i = 0; i < sub.indices.size(); i += 3)
  {
    const auto ia = sub.indices[i], ib = sub.indices[i + 1], ic = sub.indices[i + 2];
    if (ia >= sub.vertex_count || ib >= sub.vertex_count || ic >= sub.vertex_count)
      return {};
    const auto& a = sub.position_uv[ia].position;
    const auto& b = sub.position_uv[ib].position;
    const auto& c = sub.position_uv[ic].position;
    const std::array<float, 3> ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const std::array<float, 3> ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const std::array<float, 3> n{
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0]};
    for (u16 index : {ia, ib, ic})
      for (unsigned axis = 0; axis < 3; ++axis)
        (*normals)[index][axis] += n[axis];
  }

  for (auto& n : *normals)
  {
    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (length > 0.000001f)
      for (auto& value : n)
        value /= length;
  }

  if (sub.has_normal)
    for (std::size_t i = 0; i < sub.position_uv.size(); ++i)
      (*normals)[i] = sub.position_uv[i].normal;

  StaticDrawMatch match;
  match.owner = mesh;
  match.normals = std::move(normals);
  match.mesh = mesh.get();
  match.submesh = &mesh->submeshes[submesh_index];
  match.submesh_index = submesh_index;
  const auto bounds = BoundsFromPS3(sub);
  match.bounds_valid = bounds.valid;
  match.bounds_min = bounds.minimum;
  match.bounds_max = bounds.maximum;
  return match;
}

float ReadBEFloat(const u8* p)
{
  const u32 bits = BE32(p);
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

struct OriginalGCNode
{
  u32 index = 0;
  u32 dl_offset = 0;
  DisplayListSignatureKey signature;
  Bounds3 bounds;
};

bool ParseOriginalGCMSHNodes(std::span<const u8> bytes, std::vector<OriginalGCNode>* out,
                             u32* node_table_offset_out = nullptr,
                             u32* node_count_out = nullptr)
{
  if (!out || bytes.size() < 0x30 || BE32(bytes.data()) != 9)
    return false;

  const auto u = [&](std::size_t at) -> u32 {
    return at + 4 <= bytes.size() ? BE32(bytes.data() + at) : 0;
  };
  const u32 node_table = u(0x18);
  const u32 node_count = u(0x1c);
  if (!node_count || node_count > 4096 || node_table > bytes.size() ||
      std::uint64_t(node_count) * 32u > bytes.size() - node_table)
    return false;

  std::vector<OriginalGCNode> nodes;
  nodes.reserve(node_count);
  for (u32 i = 0; i < node_count; ++i)
  {
    const u32 node = node_table + i * 32u;
    const u32 geometry = u(node + 8);
    if (geometry > bytes.size() || bytes.size() - geometry < 36)
      return false;

    const u32 dl = u(geometry + 32);
    const u32 expanded_count = u(geometry + 12);
    const u32 dl_stride = u(geometry) & 1 ? 4 : 8;
    const std::uint64_t length64 =
        (std::uint64_t(expanded_count) * dl_stride + 52u + 31u) & ~std::uint64_t(31u);
    if (length64 <= 52 || length64 > 0x200000 || dl > bytes.size() ||
        length64 > bytes.size() - dl)
      return false;

    OriginalGCNode info;
    info.index = i;
    info.dl_offset = dl;
    info.signature.size = static_cast<u32>(length64);
    info.signature.command_hash =
        Common::GetHash64(bytes.data() + dl + 52, info.signature.size - 52, 0);

    // GC MSH v9 geometry +0x04 packs the position count in the high 16 bits;
    // +0x10 points at a big-endian float3 position array.  This is only used
    // for the rare GC/PS3 node-count mismatch; equal-count files bind by exact
    // authored node order.
    const u32 position_count = u(geometry + 4) >> 16;
    const u32 position_offset = u(geometry + 16);
    if (position_count && position_count <= 65535 && position_offset <= bytes.size() &&
        std::uint64_t(position_count) * 12u <= bytes.size() - position_offset)
    {
      Bounds3 bounds;
      for (u32 v = 0; v < position_count; ++v)
      {
        const u8* p = bytes.data() + position_offset + std::size_t(v) * 12u;
        const std::array<float, 3> position{ReadBEFloat(p), ReadBEFloat(p + 4), ReadBEFloat(p + 8)};
        if (!IncludePoint(&bounds, position))
        {
          bounds = {};
          break;
        }
      }
      info.bounds = bounds;
    }
    nodes.push_back(info);
  }

  *out = std::move(nodes);
  if (node_table_offset_out)
    *node_table_offset_out = node_table;
  if (node_count_out)
    *node_count_out = node_count;
  return true;
}

std::vector<int> MapGCNodesToPS3Submeshes(const std::vector<OriginalGCNode>& nodes,
                                          const StaticMesh& mesh)
{
  std::vector<int> mapping(nodes.size(), -1);
  if (nodes.empty() || mesh.submeshes.empty())
    return mapping;

  const std::string source_name = Lower(mesh.source_name);
  const bool thompson =
      source_name.find("thompson") != std::string::npos ||
      source_name.find("tommy") != std::string::npos;

  // The platform layouts preserve authored node/submesh order for most assets.
  // Thompson is an exception: GC and PS3 can contain the same 15 parts in a
  // different material/submesh order. Mapping 1:1 by ordinal then puts the
  // correct TOM_* texture on the wrong piece of geometry. Force a bounds-based
  // permutation for Thompson even when the node counts are equal.
  if (nodes.size() == mesh.submeshes.size() && !thompson)
  {
    for (std::size_t i = 0; i < nodes.size(); ++i)
      mapping[i] = static_cast<int>(i);
    return mapping;
  }

  // A small number of assets were merged/split differently by the PS3 build.
  // Map only uniquely compatible object-space parts; unmatched GC nodes remain
  // GameCube instead of duplicating a PS3 submesh.
  struct Pair
  {
    float score;
    std::size_t gc;
    std::size_t ps3;
  };
  std::vector<Pair> pairs;
  for (std::size_t gi = 0; gi < nodes.size(); ++gi)
  {
    if (!nodes[gi].bounds.valid)
      continue;
    for (std::size_t pi = 0; pi < mesh.submeshes.size(); ++pi)
    {
      const float score = BoundsScore(nodes[gi].bounds, BoundsFromPS3(mesh.submeshes[pi]));
      if (std::isfinite(score))
        pairs.push_back({score, gi, pi});
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
    return a.score < b.score;
  });

  const float maximum_score = EnvFloatLocal(
      "MOH_PS3_MSH_NODE_SCORE", thompson ? 0.20f : 0.10f, 0.001f, 1.0f);
  std::vector<bool> used_ps3(mesh.submeshes.size(), false);
  for (const auto& pair : pairs)
  {
    if (pair.score > maximum_score)
      break;
    if (mapping[pair.gc] >= 0 || used_ps3[pair.ps3])
      continue;
    mapping[pair.gc] = static_cast<int>(pair.ps3);
    used_ps3[pair.ps3] = true;
  }

  if (thompson)
  {
    const std::size_t mapped =
        static_cast<std::size_t>(std::count_if(
            mapping.begin(), mapping.end(), [](int value) { return value >= 0; }));

    if (mapped != nodes.size() || mapped != mesh.submeshes.size())
    {
      // Never create a half-GC/half-PS3 Thompson. If the permutation cannot be
      // proven complete, retain all GC parts instead of mixing material orders.
      std::fill(mapping.begin(), mapping.end(), -1);
      std::fprintf(
          stderr,
          "[moh-ps3-msh] Thompson bounds remap REJECT: %zu/%zu parts; keeping complete GC model\n",
          mapped, nodes.size());
    }
    else
    {
      std::fprintf(
          stderr,
          "[moh-ps3-msh] Thompson bounds remap ACTIVE: %zu/%zu parts matched by geometry, not ordinal\n",
          mapped, nodes.size());
    }
  }

  return mapping;
}

bool ValidateOriginalGCCandidate(const OriginalGCDrawCandidate& candidate, u32 runtime_address)
{
  if (runtime_address < candidate.dl_offset || candidate.file_size < 0x20)
    return false;
  const u32 base = runtime_address - candidate.dl_offset;
  auto& memory = Core::System::GetInstance().GetMemory();
  const u8* header = memory.GetPointerForRange(base, 0x20);
  if (!header || BE32(header) != 9 || BE32(header + 4) != candidate.file_size ||
      BE32(header + 0x18) != candidate.node_table_offset ||
      BE32(header + 0x1c) != candidate.node_count)
    return false;

  // Validate every sibling DL tail. The loader is allowed to rewrite the first
  // 52 bytes of CP/VAT setup, so identity intentionally hashes only the stable
  // authored command/index stream after those bytes.
  for (const auto& [dl_offset, signature] : candidate.sibling_display_lists)
  {
    if (signature.size <= 52 || dl_offset > candidate.file_size ||
        signature.size > candidate.file_size - dl_offset)
      return false;
    const u8* dl = memory.GetPointerForRange(base + dl_offset, signature.size);
    if (!dl)
      return false;
    const u64 hash = Common::GetHash64(dl + 52, signature.size - 52, 0);
    if (hash != signature.command_hash)
      return false;
  }
  return true;
}


bool ValidateOriginalGCDMFCandidate(const OriginalGCDMFDrawCandidate& candidate,
                                    u32 runtime_address)
{
  if (runtime_address < candidate.dl_offset || candidate.file_size < 0x50)
    return false;
  const u32 base = runtime_address - candidate.dl_offset;
  auto& memory = Core::System::GetInstance().GetMemory();
  const u8* header = memory.GetPointerForRange(base, 0x50);
  if (!header || header[0] != 'D' || header[1] != 'M' || header[2] != 'F' || header[3] != 0)
    return false;
  const auto pointer_matches = [base](u32 value, u32 offset) {
    return value == offset || (value & 0x1fffffff) == base + offset;
  };
  return std::equal(candidate.model_tag.begin(), candidate.model_tag.end(), header + 12) &&
         BE32(header + 0x20) == candidate.group_count &&
         pointer_matches(BE32(header + 0x24), candidate.group_offset) &&
         BE32(header + 0x28) == candidate.material_count &&
         pointer_matches(BE32(header + 0x2c), candidate.material_offset);
}

void ResolveDMFDisplayList(u32 address, std::span<const u8> commands)
{
  g_current_dmf_draw = {};
  static const bool draw_enabled = PS3AssetPort::IsDMFEnabled() &&
                                   EnvSwitchLocal("MOH_PS3_DMF_DRAW", true);
  if (!draw_enabled || commands.size() < sizeof(u64))
    return;

  const u32 runtime_address = address & 0x1fffffff;
  std::scoped_lock lock(g_dmf_cache_mutex);
  if (g_dmf_display_list_candidates.empty())
    return;
  const bool runtime_debug =
      PS3RuntimeDebugEnabled();

  if (runtime_debug)
    ++g_dmf_lookups;
  auto& cached = g_dmf_address_cache[(runtime_address >> 5) % g_dmf_address_cache.size()];
  const OriginalGCDMFDrawCandidate* resolved = nullptr;
  const u32 command_size = static_cast<u32>(commands.size());
  if (cached.address == runtime_address && cached.size == command_size && cached.candidate &&
      ValidateOriginalGCDMFCandidate(*cached.candidate, runtime_address))
  {
    // Authored DMF DLs are immutable. Revalidate resource identity on every hit;
    // morph output uses different DL storage and cannot pass this header check.
    resolved = cached.candidate;
    if (runtime_debug)
      ++g_dmf_hits;
  }
  else
  {
    if (runtime_debug)
      ++g_dmf_misses;
    bool topology_fallback = false;

    // Normal path: byte-exact authored DL plus a live DMF header at
    // runtime_address - dl_offset.
    u64 prefix;
    std::memcpy(&prefix, commands.data(), sizeof(prefix));
    if (g_dmf_prefixes.contains({command_size, prefix}))
    {
      const DisplayListSignatureKey key{
          command_size, Common::GetHash64(commands.data(), commands.size(), 0)};
      if (const auto it = g_dmf_display_list_candidates.find(key);
          it != g_dmf_display_list_candidates.end())
      {
        for (const auto& candidate : it->second)
        {
          if (!ValidateOriginalGCDMFCandidate(candidate, runtime_address))
            continue;
          if (resolved)
            return;  // ambiguous identity always retains GC
          resolved = &candidate;
        }
      }
    }

    // M1/Thompson first-person skinning can copy/reindex the authored GC DL
    // away from the original DMF allocation. In that case the exact byte hash
    // and header-backtracking test cannot succeed even though the primitive
    // topology is still exactly the authored material draw. Only the four
    // known remaster-atlas materials are indexed here, and a topology key must
    // resolve to one unique original candidate before it is accepted.
    if (!resolved && g_dmf_player_topology_sizes.contains(command_size))
    {
      u64 topology_hash = 0;
      if (BuildPlayerWeaponDMFTopologySignature(commands, &topology_hash))
      {
        const DMFTopologySignatureKey topology_key{command_size, topology_hash};
        if (const auto topology_it = g_dmf_player_topology_candidates.find(topology_key);
            topology_it != g_dmf_player_topology_candidates.end() &&
            topology_it->second.size() == 1)
        {
          const auto& locator = topology_it->second.front();
          if (const auto exact_it = g_dmf_display_list_candidates.find(locator.exact_key);
              exact_it != g_dmf_display_list_candidates.end())
          {
            for (const auto& candidate : exact_it->second)
            {
              if (candidate.gc_name != locator.gc_name ||
                  candidate.material_index != locator.material_index ||
                  candidate.cluster_index != locator.cluster_index ||
                  !IsSupportedPlayerWeaponDMF(candidate.gc_name) ||
                  !IsPlayerWeaponAtlasMaterial(candidate.gc_material_name))
                continue;

              if (resolved)
              {
                resolved = nullptr;
                break;
              }
              resolved = &candidate;
            }
          }

          if (resolved)
            topology_fallback = true;
        }
      }
    }

    if (!resolved)
      return;

    if (!topology_fallback)
    {
      cached = {runtime_address, command_size, resolved};
    }
    else
    {
      static unsigned topology_logs = 0;
      if (PS3RuntimeDebugEnabled() && topology_logs++ < 32)
      {
        std::fprintf(stderr,
                     "[moh-ps3-dmf] PLAYER TOPOLOGY MATCH: gc=%s material=%s "
                     "cluster=%u DL=%08x size=%u | copied/reindexed GC DL -> "
                     "native PS3 UV0 candidate\n",
                     resolved->gc_name.c_str(),
                     resolved->gc_material_name.empty() ? "<unnamed>" :
                                                          resolved->gc_material_name.c_str(),
                     resolved->cluster_index, runtime_address, command_size);
      }
    }
  }
  if (!resolved->prepared)
    return;
  const auto& prepared = *resolved->prepared;
  g_current_dmf_draw.owner = resolved->ps3;
  g_current_dmf_draw.prepared = resolved->prepared;
  g_current_dmf_draw.gc_name = prepared.gc_name;
  g_current_dmf_draw.display_list = runtime_address;
  g_current_dmf_draw.material_index = resolved->material_index;
  g_current_dmf_draw.cluster_index = resolved->cluster_index;
  g_current_dmf_draw.gc_material_name = prepared.material_name;
  g_current_dmf_draw.gc_palette_groups = prepared.palette;
  g_current_dmf_draw.ps3_group_to_gc = prepared.group_map;
  g_current_dmf_draw.skeleton_name = prepared.skeleton_name;

  if (PS3RuntimeDebugEnabled())
  {
    std::fprintf(stderr,
                 "[moh-ps3-dmf] LIVE EXACT GC DMF: gc=%s ps3=%s material=%u(%s) cluster=%u DL=%08x size=%u palette=%zu mapped_groups=%zu skeleton=%s\n",
                 resolved->gc_name.c_str(),
                 resolved->ps3 ? resolved->ps3->source_name.c_str() : "<missing>",
                 resolved->material_index,
                 resolved->gc_material_name.empty() ? "<unnamed>" : resolved->gc_material_name.c_str(),
                 resolved->cluster_index, runtime_address, command_size,
                 resolved->gc_palette_groups.size(),
                 std::count_if(g_current_dmf_draw.ps3_group_to_gc.begin(),
                               g_current_dmf_draw.ps3_group_to_gc.end(),
                               [](s16 value) { return value >= 0; }),
                 g_current_dmf_draw.skeleton_name.empty() ? "<unbound>" : g_current_dmf_draw.skeleton_name.data());
  }
}

u32 ReadBE24Local(const u8* p)
{
  return (u32(p[0]) << 16) | (u32(p[1]) << 8) | u32(p[2]);
}

void IndexOriginalGCLevelMSHSignatures(std::string_view level)
{
  g_display_list_candidates.clear();
  if (level.empty())
    return;

  const std::string campaign(1, level.front());
  const std::array<std::filesystem::path, 3> candidates{
      std::filesystem::current_path() / "extracted" / "files" / "DATA" / campaign /
          std::string(level) / "level.viv",
      std::filesystem::current_path() / "extracted" / "files" / "data" / campaign /
          std::string(level) / "level.viv",
      std::filesystem::current_path() / "extracted" / "DATA" / campaign /
          std::string(level) / "level.viv"};

  std::filesystem::path archive_path;
  for (const auto& candidate : candidates)
  {
    if (std::filesystem::is_regular_file(candidate))
    {
      archive_path = candidate;
      break;
    }
  }

  if (archive_path.empty())
  {
    std::fprintf(stderr,
                 "[moh-ps3-msh] GC all-MSH index MISS: level=%.*s original level.viv not found under extracted/files/DATA\n",
                 static_cast<int>(level.size()), level.data());
    return;
  }

  std::ifstream file(archive_path, std::ios::binary | std::ios::ate);
  if (!file)
    return;
  const std::streamoff end = file.tellg();
  if (end < 6 || end > static_cast<std::streamoff>(128 * 1024 * 1024))
    return;

  std::vector<u8> archive(static_cast<std::size_t>(end));
  file.seekg(0, std::ios::beg);
  if (!file.read(reinterpret_cast<char*>(archive.data()),
                 static_cast<std::streamsize>(archive.size())))
    return;
  if (archive[0] != 0xC0 || archive[1] != 0xFB)
    return;

  const u32 header_size = ((u32(archive[2]) << 8) | u32(archive[3])) + 4u;
  const u32 entry_count = (u32(archive[4]) << 8) | u32(archive[5]);
  if (header_size < 6 || header_size > archive.size() || !entry_count)
    return;

  std::size_t pos = 6, msh_entries = 0, gc_nodes = 0, mapped_nodes = 0;
  std::size_t full_models = 0, partial_models = 0, missing_ps3 = 0, rejected = 0;
  unsigned ready_logs = 0;

  for (u32 entry_index = 0; entry_index < entry_count; ++entry_index)
  {
    if (pos + 6 > header_size)
      break;
    const u32 offset = ReadBE24Local(archive.data() + pos);
    const u32 packed_size = ReadBE24Local(archive.data() + pos + 3);
    pos += 6;
    const std::size_t name_start = pos;
    while (pos < header_size && archive[pos] != 0)
      ++pos;
    if (pos >= header_size)
      break;
    const std::string name(reinterpret_cast<const char*>(archive.data() + name_start),
                           pos - name_start);
    ++pos;

    const std::string filename = CanonicalModelName(name);
    if (!filename.ends_with(".msh"))
      continue;
    ++msh_entries;
    if (offset > archive.size() || packed_size > archive.size() - offset || packed_size < 0x30)
    {
      ++rejected;
      continue;
    }
    const std::span<const u8> bytes(archive.data() + offset, packed_size);

    std::vector<OriginalGCNode> nodes;
    u32 node_table = 0, node_count = 0;
    if (!ParseOriginalGCMSHNodes(bytes, &nodes, &node_table, &node_count))
    {
      ++rejected;
      continue;
    }
    gc_nodes += nodes.size();

    std::shared_ptr<StaticMesh> mesh;
    {
      std::scoped_lock lock(g_msh_cache_mutex);
      if (const auto it = g_msh_cache.find(filename); it != g_msh_cache.end())
        mesh = it->second;
    }
    if (!mesh)
    {
      ++missing_ps3;
      continue;
    }

    const std::vector<int> mapping = MapGCNodesToPS3Submeshes(nodes, *mesh);
    std::size_t model_mapped = 0;
    std::vector<std::pair<u32, DisplayListSignatureKey>> sibling_signatures;
    sibling_signatures.reserve(nodes.size());
    for (const auto& node : nodes)
      sibling_signatures.emplace_back(node.dl_offset, node.signature);

    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
      if (mapping[i] < 0)
        continue;
      StaticDrawMatch match = BuildStaticDrawMatch(mesh, static_cast<std::size_t>(mapping[i]));
      if (!match)
        continue;

      OriginalGCDrawCandidate candidate;
      candidate.match = std::move(match);
      candidate.gc_name = filename;
      candidate.file_size = packed_size;
      candidate.node_table_offset = node_table;
      candidate.node_count = node_count;
      candidate.node_index = static_cast<u32>(i);
      candidate.dl_offset = nodes[i].dl_offset;
      candidate.sibling_display_lists = sibling_signatures;
      g_display_list_candidates[nodes[i].signature].push_back(std::move(candidate));

      ++mapped_nodes;
      ++model_mapped;
      if (PS3RuntimeDebugEnabled() && ready_logs++ < 64)
      {
        std::fprintf(stderr,
                     "[moh-ps3-msh] GC SIGNATURE READY: gc=%s node=%zu/%zu ps3=%s submesh=%d/%zu DLoff=%08x DLsize=%u hash=%016llX\n",
                     filename.c_str(), i + 1, nodes.size(), mesh->source_name.c_str(), mapping[i] + 1,
                     mesh->submeshes.size(), nodes[i].dl_offset, nodes[i].signature.size,
                     static_cast<unsigned long long>(nodes[i].signature.command_hash));
      }
    }

    if (model_mapped == nodes.size())
      ++full_models;
    else if (model_mapped)
    {
      ++partial_models;
      std::fprintf(stderr,
                   "[moh-ps3-msh] GC/PS3 node mismatch PARTIAL: gc=%s nodes=%zu ps3_submeshes=%zu mapped=%zu (unmatched nodes stay GC)\n",
                   filename.c_str(), nodes.size(), mesh->submeshes.size(), model_mapped);
    }
    else
      ++rejected;
  }

  std::size_t collision_keys = 0, collision_candidates = 0;
  for (const auto& [key, list] : g_display_list_candidates)
  {
    (void)key;
    if (list.size() > 1)
    {
      ++collision_keys;
      collision_candidates += list.size();
    }
  }

  std::fprintf(stderr,
               "[moh-ps3-msh] GC all-MSH signature index ready: level=%.*s archive=%s msh=%zu full=%zu partial=%zu gc_nodes=%zu mapped_nodes=%zu signature_keys=%zu collision_keys=%zu collision_candidates=%zu missing_ps3=%zu rejected=%zu\n",
               static_cast<int>(level.size()), level.data(), archive_path.string().c_str(), msh_entries,
               full_models, partial_models, gc_nodes, mapped_nodes, g_display_list_candidates.size(),
               collision_keys, collision_candidates, missing_ps3, rejected);
}

bool BelongsToLevel(const PS3RemasterAssets::AssetInfo& asset, std::string_view level)
{
  if (level.empty())
    return false;

  std::string path = Lower(asset.relative_path);
  std::string level_l = Lower(std::string(level));
  const std::string needle = "/" + level_l + "/";
  return path.find(needle) != std::string::npos;
}



std::vector<std::string> ReadDMFBoneRefs(std::span<const u8> bytes, bool be)
{
  std::vector<std::string> out;
  if (bytes.size() < 0x50)
    return out;
  const auto U32 = [be](const u8* q) { return be ? BE32(q) : LE32(q); };
  const u32 count = U32(bytes.data() + 0x48);
  const u32 offset = U32(bytes.data() + 0x4c);
  if (!count || count > 4096 || offset > bytes.size() ||
      static_cast<std::size_t>(count) * 16 > bytes.size() - offset)
    return out;
  out.reserve(count);
  for (u32 i = 0; i < count; ++i)
    out.push_back(FixedString(bytes.data() + offset + static_cast<std::size_t>(i) * 16, 16));
  return out;
}

std::string SkinGroupKey(std::string_view a, std::string_view b, int blend_q)
{
  std::string key = Lower(std::string(a));
  key.push_back('\n');
  key += Lower(std::string(b));
  key.push_back('\n');
  key += std::to_string(blend_q);
  return key;
}



void BuildExactSkinGroupMap(std::span<const u8> gc_bytes, ExactDMFPair* pair)
{
  if (!pair || !pair->ps3 || !pair->ps3->decoded || !pair->ps3->decoded->valid ||
      gc_bytes.size() < 0x50)
    return;

  // v12.10:
  // No character PS3 geometry means no character skin-map computation.
  if (IsHardDisabledPS3DMFResource(
          pair->ps3->source_name))
  {
    return;
  }

  const u32 gc_group_count = BE32(gc_bytes.data() + 0x20);
  const u32 gc_group_offset = BE32(gc_bytes.data() + 0x24);
  if (!gc_group_count || gc_group_count > 4096 || gc_group_offset > gc_bytes.size() ||
      static_cast<std::size_t>(gc_group_count) * 4 > gc_bytes.size() - gc_group_offset)
    return;

  const std::vector<std::string> gc_bones = ReadDMFBoneRefs(gc_bytes, true);
  if (gc_bones.empty())
    return;

  pair->gc_skin_groups.clear();
  pair->gc_skin_groups.reserve(gc_group_count);

  for (u32 i = 0; i < gc_group_count; ++i)
  {
    const u8* q =
        gc_bytes.data() + gc_group_offset +
        static_cast<std::size_t>(i) * 4;

    pair->gc_skin_groups.push_back(
        {q[0], q[1], BE16(q + 2)});
  }

  const auto index_group_key = [](u32 bone_a, u32 bone_b, int blend_q) {
    return std::to_string(bone_a) + "\n" + std::to_string(bone_b) + "\n" +
           std::to_string(blend_q);
  };

  std::unordered_map<std::string, std::vector<u16>> gc_by_key;
  std::unordered_map<std::string, std::vector<u16>> gc_by_swapped_key;

  // Structural equivalent of the name maps. These are used only after bone-ref
  // ordering has independently been proven by shared name anchors.
  std::unordered_map<std::string, std::vector<u16>> gc_by_index_key;
  std::unordered_map<std::string, std::vector<u16>> gc_by_swapped_index_key;

  for (u32 i = 0; i < gc_group_count; ++i)
  {
    const u8* q = gc_bytes.data() + gc_group_offset + static_cast<std::size_t>(i) * 4;
    const u8 bone_a = q[0];
    const u8 bone_b = q[1];
    const int blend_q = BE16(q + 2);

    if (bone_a >= gc_bones.size() || bone_b >= gc_bones.size())
      continue;

    gc_by_key[SkinGroupKey(gc_bones[bone_a], gc_bones[bone_b], blend_q)].push_back(
        static_cast<u16>(i));

    gc_by_index_key[index_group_key(bone_a, bone_b, blend_q)].push_back(
        static_cast<u16>(i));

    // Exact 4.12 identity:
    //
    //     A*q + B*(4096-q)
    //       ==
    //     B*(4096-q) + A*q
    //
    // Therefore changing authored bone order is not a fuzzy remap.
    if (blend_q >= 0 && blend_q <= 4096)
    {
      gc_by_swapped_key
          [SkinGroupKey(gc_bones[bone_b], gc_bones[bone_a], 4096 - blend_q)]
              .push_back(static_cast<u16>(i));

      gc_by_swapped_index_key
          [index_group_key(bone_b, bone_a, 4096 - blend_q)]
              .push_back(static_cast<u16>(i));
    }
  }

  const auto& decoded = *pair->ps3->decoded;

  // Prove that PS3 and GC bone-reference index spaces use the same ordering.
  // We never infer this from model names or geometry. Shared non-duplicate bone
  // strings must resolve to the same numeric index on both platforms.
  std::unordered_map<std::string, std::size_t> ps3_bone_index;
  std::unordered_set<std::string> duplicate_ps3_bones;

  for (std::size_t i = 0; i < decoded.bone_refs.size(); ++i)
  {
    const std::string name = Lower(decoded.bone_refs[i]);
    if (const auto [it, inserted] = ps3_bone_index.emplace(name, i); !inserted)
      duplicate_ps3_bones.insert(name);
  }

  bool bone_order_conflict = false;
  std::size_t bone_order_anchors = 0;

  for (std::size_t i = 0; i < gc_bones.size(); ++i)
  {
    const std::string name = Lower(gc_bones[i]);

    if (duplicate_ps3_bones.contains(name))
      continue;

    const auto it = ps3_bone_index.find(name);
    if (it == ps3_bone_index.end())
      continue;

    if (it->second != i)
    {
      bone_order_conflict = true;
      break;
    }

    ++bone_order_anchors;
  }

  const std::size_t common_bones =
      std::min(gc_bones.size(), decoded.bone_refs.size());

  const std::size_t required_anchors =
      std::min<std::size_t>(8, common_bones);

  const bool bone_order_proven =
      common_bones != 0 &&
      !bone_order_conflict &&
      bone_order_anchors >= required_anchors;

  pair->gc_bone_order_proven = bone_order_proven;

  pair->ps3_group_to_gc.assign(decoded.skin_groups.size(), -1);
  pair->mapped_skin_groups = 0;

  std::unordered_map<std::string, std::size_t> next_occurrence;
  std::unordered_map<std::string, std::size_t> next_swapped_occurrence;
  std::unordered_map<std::string, std::size_t> next_index_occurrence;
  std::unordered_map<std::string, std::size_t> next_swapped_index_occurrence;

  std::size_t swapped_equivalent_groups = 0;
  std::size_t structural_index_groups = 0;

  // PS3 export can re-quantize the old 4.12 coefficient by a few integer
  // units while preserving the exact two bone refs. Only bridge a UNIQUE
  // nearest GC group and only inside this extremely small error window.
  constexpr int kBlendQRequantizationTolerance = 4;
  std::size_t requantized_groups = 0;
  int maximum_requantized_delta = 0;

  for (std::size_t i = 0; i < decoded.skin_groups.size(); ++i)
  {
    const auto& group = decoded.skin_groups[i];

    if (group.bone_a >= decoded.bone_refs.size() ||
        group.bone_b >= decoded.bone_refs.size())
      continue;

    // PS3 0x0502 stores the original legacy coefficient as float / 4096.
    const int blend_q = static_cast<int>(group.blend * 4096.0f);

    const std::string name_key =
        SkinGroupKey(decoded.bone_refs[group.bone_a],
                     decoded.bone_refs[group.bone_b],
                     blend_q);

    const std::string index_key =
        index_group_key(group.bone_a, group.bone_b, blend_q);

    const std::vector<u16>* matches = nullptr;
    std::size_t* occurrence = nullptr;

    bool swapped_equivalent = false;
    bool structural_index = false;

    // 1. Exact authored identity by bone strings and coefficient.
    if (const auto it = gc_by_key.find(name_key); it != gc_by_key.end())
    {
      matches = &it->second;
      occurrence = &next_occurrence[name_key];
    }

    // 2. Exact mathematical identity with reversed two-bone ordering.
    else if (blend_q >= 0 && blend_q <= 4096)
    {
      if (const auto it = gc_by_swapped_key.find(name_key);
          it != gc_by_swapped_key.end())
      {
        matches = &it->second;
        occurrence = &next_swapped_occurrence[name_key];
        swapped_equivalent = true;
      }
    }

    // 3. Some remaster variants rename a small number of refs while preserving
    // the complete numeric bone ordering. Only use numeric group identity once
    // that ordering has already been independently proven.
    if (!matches && bone_order_proven)
    {
      if (const auto it = gc_by_index_key.find(index_key);
          it != gc_by_index_key.end())
      {
        matches = &it->second;
        occurrence = &next_index_occurrence[index_key];
        structural_index = true;
      }
      else if (blend_q >= 0 && blend_q <= 4096)
      {
        if (const auto it = gc_by_swapped_index_key.find(index_key);
            it != gc_by_swapped_index_key.end())
        {
          matches = &it->second;
          occurrence = &next_swapped_index_occurrence[index_key];
          swapped_equivalent = true;
          structural_index = true;
        }
      }
    }

    // v12.3: generic character models may keep the same exact
    // two-bone semantic group but use a slightly different authored q value.
    //
    // Mapping != transform equality:
    // we only identify which GC group owns these vertices here.
    // The renderer later reconstructs local space from the exact GC q.
    if (!matches && bone_order_proven &&
        !IsSupportedPlayerWeaponDMF(
            CanonicalDMFName(pair->ps3->source_name)) &&
        blend_q >= 0 && blend_q <= 4096)
    {
      constexpr int maximum_delta = 96;
      constexpr int minimum_margin = 24;

      int best_delta = std::numeric_limits<int>::max();
      int second_delta = std::numeric_limits<int>::max();
      int best_q = -1;
      s16 best_gc_group = -1;

      for (std::size_t gc_index = 0;
           gc_index < pair->gc_skin_groups.size();
           ++gc_index)
      {
        const auto& gc = pair->gc_skin_groups[gc_index];

        int comparable_q = -1;

        if (gc.bone_a == group.bone_a &&
            gc.bone_b == group.bone_b)
        {
          comparable_q = gc.blend_q;
        }
        else if (gc.bone_a == group.bone_b &&
                 gc.bone_b == group.bone_a &&
                 gc.blend_q <= 4096)
        {
          comparable_q = 4096 - gc.blend_q;
        }
        else
        {
          continue;
        }

        const int delta =
            std::abs(comparable_q - blend_q);

        if (delta < best_delta)
        {
          second_delta = best_delta;
          best_delta = delta;
          best_q = comparable_q;
          best_gc_group =
              static_cast<s16>(gc_index);
        }
        else if (delta < second_delta)
        {
          second_delta = delta;
        }
      }

      if (best_gc_group >= 0 &&
          best_delta <= maximum_delta &&
          (second_delta ==
               std::numeric_limits<int>::max() ||
           second_delta - best_delta >=
               minimum_margin))
      {
        pair->ps3_group_to_gc[i] =
            best_gc_group;

        ++pair->mapped_skin_groups;

        static unsigned logs = 0;

        if (logs++ < 64)
        {
          std::fprintf(
              stderr,
              "[moh-ps3-dmf] GC-BIND GROUP MAP: "
              "ps3=%s group=%zu bones=%u/%u "
              "ps3_q=%d -> gc_group=%d "
              "gc_q=%d delta=%d/4096 | "
              "exact GC bind selected\n",
              pair->ps3->source_name.c_str(),
              i,
              group.bone_a,
              group.bone_b,
              blend_q,
              best_gc_group,
              best_q,
              best_delta);
        }

        continue;
      }
    }

    // v12.2: exact-bones / near-identical-q bridge.
    //
    // This runs only after:
    //   * string identity failed;
    //   * swapped exact identity failed;
    //   * bone-index ordering was independently proven;
    //   * exact index/q identity failed.
    //
    // A PS3 q may differ by a few 1/4096 units after remaster export.
    // Require the nearest GC group to be UNIQUE and <= 4/4096 away.
    if (!matches && bone_order_proven &&
        !IsSupportedPlayerWeaponDMF(
            CanonicalDMFName(pair->ps3->source_name)) &&
        blend_q >= 0 && blend_q <= 4096)
    {
      int best_delta = 0x7fffffff;
      int second_delta = 0x7fffffff;
      s16 best_gc_group = -1;
      int best_gc_q = -1;

      for (u32 gc_index = 0; gc_index < gc_group_count; ++gc_index)
      {
        const u8* gc =
            gc_bytes.data() + gc_group_offset +
            static_cast<std::size_t>(gc_index) * 4;

        const u8 gc_a = gc[0];
        const u8 gc_b = gc[1];
        const int gc_q = BE16(gc + 2);

        int comparable_q = -1;

        if (gc_a == group.bone_a &&
            gc_b == group.bone_b)
        {
          comparable_q = gc_q;
        }
        else if (gc_a == group.bone_b &&
                 gc_b == group.bone_a &&
                 gc_q >= 0 && gc_q <= 4096)
        {
          comparable_q = 4096 - gc_q;
        }
        else
        {
          continue;
        }

        const int delta = std::abs(comparable_q - blend_q);

        if (delta < best_delta)
        {
          second_delta = best_delta;
          best_delta = delta;
          best_gc_group = static_cast<s16>(gc_index);
          best_gc_q = comparable_q;
        }
        else if (delta < second_delta)
        {
          second_delta = delta;
        }
      }

      if (best_gc_group >= 0 &&
          best_delta <= kBlendQRequantizationTolerance &&
          second_delta > best_delta)
      {
        pair->ps3_group_to_gc[i] = best_gc_group;
        ++pair->mapped_skin_groups;
        ++requantized_groups;
        maximum_requantized_delta =
            std::max(maximum_requantized_delta, best_delta);

        static unsigned requantized_logs = 0;
        if (PS3RuntimeDebugEnabled() && requantized_logs++ < 64)
        {
          std::fprintf(
              stderr,
              "[moh-ps3-dmf] REQUANTIZED GROUP: ps3=%s "
              "ps3_group=%zu bones=%u/%u q=%d -> "
              "gc_group=%d q=%d delta=%d/4096\n",
              pair->ps3->source_name.c_str(),
              i,
              group.bone_a,
              group.bone_b,
              blend_q,
              best_gc_group,
              best_gc_q,
              best_delta);
        }

        continue;
      }
      else if (best_gc_group >= 0)
      {
        static unsigned nearest_reject_logs = 0;
        if (PS3RuntimeDebugEnabled() && nearest_reject_logs++ < 64)
        {
          std::fprintf(
              stderr,
              "[moh-ps3-dmf] REQUANTIZED GROUP REJECT: ps3=%s "
              "group=%zu bones=%u/%u q=%d nearest_gc=%d "
              "nearest_q=%d delta=%d second=%d limit=%d\n",
              pair->ps3->source_name.c_str(),
              i,
              group.bone_a,
              group.bone_b,
              blend_q,
              best_gc_group,
              best_gc_q,
              best_delta,
              second_delta,
              kBlendQRequantizationTolerance);
        }
      }
    }

    if (!matches || matches->empty() || !occurrence)
      continue;

    // Multiple authored records with the same exact transform are equivalent.
    // Preserve occurrence order while possible; overflow copies may alias the
    // first identical GC group because all bind inputs are exactly the same.
    const u16 gc_group =
        *occurrence < matches->size() ?
            (*matches)[(*occurrence)++] :
            matches->front();

    pair->ps3_group_to_gc[i] = static_cast<s16>(gc_group);
    ++pair->mapped_skin_groups;

    if (swapped_equivalent)
      ++swapped_equivalent_groups;

    if (structural_index)
      ++structural_index_groups;
  }

  if (swapped_equivalent_groups != 0 ||
      structural_index_groups != 0 ||
      requantized_groups != 0)
  {
    static unsigned structural_map_logs = 0;
    if (PS3RuntimeDebugEnabled() && structural_map_logs++ < 64)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-dmf] EXTENDED GROUP MAP: ps3=%s mapped=%zu/%zu "
          "swapped=%zu structural_index=%zu requantized=%zu "
          "max_q_delta=%d/4096 bone_order=%s anchors=%zu | "
          "strict structural mapping\n",
          pair->ps3->source_name.c_str(),
          pair->mapped_skin_groups,
          decoded.skin_groups.size(),
          swapped_equivalent_groups,
          structural_index_groups,
          requantized_groups,
          maximum_requantized_delta,
          bone_order_proven ? "proven" : "unproven",
          bone_order_anchors);
    }
  }

  if (pair->mapped_skin_groups != decoded.skin_groups.size())
  {
    static unsigned group_gap_logs = 0;
    if (PS3RuntimeDebugEnabled() && group_gap_logs++ < 64)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-dmf] GROUP MAP GAP: ps3=%s mapped=%zu/%zu "
          "bone_order=%s anchors=%zu",
          pair->ps3->source_name.c_str(),
          pair->mapped_skin_groups,
          decoded.skin_groups.size(),
          bone_order_proven ? "proven" : "unproven",
          bone_order_anchors);

      std::size_t shown = 0;

      for (std::size_t i = 0;
           i < decoded.skin_groups.size() && shown < 8;
           ++i)
      {
        if (pair->ps3_group_to_gc[i] >= 0)
          continue;

        const auto& group = decoded.skin_groups[i];

        const int q =
            static_cast<int>(group.blend * 4096.0f);

        const char* a =
            group.bone_a < decoded.bone_refs.size() ?
                decoded.bone_refs[group.bone_a].c_str() :
                "<bad-a>";

        const char* b =
            group.bone_b < decoded.bone_refs.size() ?
                decoded.bone_refs[group.bone_b].c_str() :
                "<bad-b>";

        std::fprintf(
            stderr,
            " [%zu:%u/%u:%s/%s:q=%d]",
            i,
            group.bone_a,
            group.bone_b,
            a,
            b,
            q);

        ++shown;
      }

      std::fputc('\n', stderr);
    }
  }

  // Some remaster DMFs keep exactly the same authored group/bone ordering but
  // rename the bone strings between platforms.  A name-only join then produces
  // mapped_groups=0 even though the binary skin topology is identical (the
  // uhm*_head family is one example observed at runtime).  Only accept the
  // fallback when the complete arrays have the same sizes and EVERY group has
  // the same bone indices and exact legacy 4.12 coefficient at the same index.
  // This is structural identity, not fuzzy name matching.
  if (pair->mapped_skin_groups == 0 && decoded.skin_groups.size() == gc_group_count &&
      decoded.bone_refs.size() == gc_bones.size())
  {
    bool identical = true;
    for (u32 i = 0; i < gc_group_count; ++i)
    {
      const u8* q = gc_bytes.data() + gc_group_offset + static_cast<std::size_t>(i) * 4;
      const auto& group = decoded.skin_groups[i];
      const int ps3_blend_q = static_cast<int>(group.blend * 4096.0f);
      if (group.bone_a != q[0] || group.bone_b != q[1] || ps3_blend_q != BE16(q + 2))
      {
        identical = false;
        break;
      }
    }
    if (identical)
    {
      pair->ps3_group_to_gc.resize(gc_group_count);
      for (u32 i = 0; i < gc_group_count; ++i)
        pair->ps3_group_to_gc[i] = static_cast<s16>(i);
      pair->mapped_skin_groups = gc_group_count;
      std::fprintf(stderr,
                   "[moh-ps3-dmf] STRUCTURAL GROUP MAP: ps3=%s groups=%u "
                   "bone/group order is binary-identical despite renamed refs\n",
                   pair->ps3->source_name.c_str(), gc_group_count);
    }
  }
}

void BindDMFPairsToSkeletons()
{
  std::scoped_lock lock(g_dmf_cache_mutex, g_skl_cache_mutex);
  for (auto& [name, pair] : g_dmf_pairs)
  {
    if (!pair.ps3 || !pair.ps3->decoded || !pair.ps3->decoded->valid)
      continue;
    const auto& refs = pair.ps3->decoded->bone_refs;
    if (refs.empty())
      continue;

    std::unordered_set<std::string> ref_names;
    for (const std::string& ref : refs)
      ref_names.insert(Lower(ref));

    std::shared_ptr<SKLInfo> best;
    std::size_t best_matches = 0;
    const std::string exact_skl = name.substr(0, name.size() - 4) + ".skl";
    if (const auto exact = g_skl_cache.find(exact_skl); exact != g_skl_cache.end())
      best = exact->second;

    auto score = [&](const std::shared_ptr<SKLInfo>& skl) {
      std::size_t matches = 0;
      if (skl)
        for (const std::string& bone : skl->bone_names)
          matches += ref_names.contains(Lower(bone)) ? 1 : 0;
      return matches;
    };
    if (best)
      best_matches = score(best);

    for (const auto& [key, skl] : g_skl_cache)
    {
      if (key.find('/') != std::string::npos || !skl)
        continue;
      const std::size_t matches = score(skl);
      if (matches > best_matches)
      {
        best = skl;
        best_matches = matches;
      }
    }

    pair.skeleton_name = best ? best->source_name : std::string{};
    pair.skeleton_name_matches = best_matches;
    if (best && pair.ps3->bytes)
    {
      namespace Bind = MOHFrontline::PS3::SkinBind;
      auto binding = std::make_shared<Bind::Binding>(Bind::Validate(*pair.ps3->bytes, best->hierarchy));
      bool valid = binding->valid;
      double max_error = 0;
      std::size_t vertices = 0;
      const auto& decoded = *pair.ps3->decoded;
      for (const auto& cluster : decoded.clusters)
      {
        for (std::size_t i = 0; valid && i < cluster.positions.size(); ++i)
        {
          const auto slot = cluster.vertex_palette_slots[i];
          const auto group = decoded.skin_groups[cluster.palette_groups[slot]];
          if (group.bone_a >= binding->ref_to_bone.size() || binding->ref_to_bone[group.bone_a] < 0)
          {
            valid = false;
            binding->reason = "required vertex bone missing";
            break;
          }
          const auto& bone = binding->bones[binding->ref_to_bone[group.bone_a]];
          const auto& position = cluster.positions[i];
          const Bind::Vector vertex{position[0], position[1], position[2]};
          const auto result = Bind::Transform(bone.world_bind, Bind::Transform(bone.inverse_bind, vertex));
          double scale = 1;
          for (unsigned axis = 0; axis < 3; ++axis)
            scale = std::max(scale, std::abs(vertex[axis]));
          for (unsigned axis = 0; axis < 3; ++axis)
          {
            const double error = std::abs(result[axis]-vertex[axis]);
            max_error = std::max(max_error, error);
            if (!std::isfinite(error) || error > 0.002*scale)
            {
              valid = false;
              binding->reason = "vertex bind roundtrip";
            }
          }
          ++vertices;
        }
      }
      pair.bind_vertices_valid = valid && vertices != 0;
      std::fprintf(stderr, "[moh-ps3-skin] PS3 BIND %s: model=%s skeleton=%s vertices=%zu max_error=%.9g matrix_error=%.9g reason=%s | GC conversion still unvalidated\n",
          pair.bind_vertices_valid ? "VALID" : "REJECT", name.c_str(), best->source_name.c_str(),
          vertices, max_error, binding->max_matrix_error, binding->reason.c_str());
      pair.bind = std::move(binding);
    }
    static unsigned logs = 0;
    if (best && logs++ < 32)
    {
      std::fprintf(stderr,
                   "[moh-ps3-skin] SKELETON CANDIDATE: model=%s skl=%s name_matches=%zu/%u dmf_refs=%zu | candidate only; animated GX palette not bound yet\n",
                   name.c_str(), best->source_name.c_str(), best_matches, best->bone_count,
                   refs.size());
    }
  }
}

void IndexOriginalGCLevelDMFPairs(std::string_view level)
{
  std::scoped_lock index_lock(g_dmf_cache_mutex);
  g_dmf_address_cache.fill({});
  g_dmf_pairs.clear();
  g_dmf_display_list_candidates.clear();
  g_dmf_prefixes.clear();
  g_dmf_player_topology_candidates.clear();
  g_dmf_player_topology_sizes.clear();
  g_dmf_palette_flow_clusters.clear();
  g_dmf_palette_flow_analyses.clear();
  g_dmf_palette_flow_material_attempts.clear();
  if (level.empty())
    return;

  const std::string campaign(1, level.front());
  const std::array<std::filesystem::path, 3> candidates{
      std::filesystem::current_path() / "extracted" / "files" / "DATA" / campaign /
          std::string(level) / "level.viv",
      std::filesystem::current_path() / "extracted" / "files" / "data" / campaign /
          std::string(level) / "level.viv",
      std::filesystem::current_path() / "extracted" / "DATA" / campaign /
          std::string(level) / "level.viv"};
  std::filesystem::path archive_path;
  for (const auto& candidate : candidates)
    if (std::filesystem::is_regular_file(candidate))
    {
      archive_path = candidate;
      break;
    }
  if (archive_path.empty())
    return;

  std::ifstream file(archive_path, std::ios::binary | std::ios::ate);
  if (!file)
    return;
  const std::streamoff end = file.tellg();
  if (end < 6 || end > static_cast<std::streamoff>(128 * 1024 * 1024))
    return;
  std::vector<u8> archive(static_cast<std::size_t>(end));
  file.seekg(0, std::ios::beg);
  if (!file.read(reinterpret_cast<char*>(archive.data()), static_cast<std::streamsize>(archive.size())) ||
      archive[0] != 0xC0 || archive[1] != 0xFB)
    return;

  const u32 header_size = ((u32(archive[2]) << 8) | u32(archive[3])) + 4u;
  const u32 entry_count = (u32(archive[4]) << 8) | u32(archive[5]);
  if (header_size < 6 || header_size > archive.size())
    return;

  std::size_t pos = 6, gc_dmf = 0, paired = 0, missing = 0, rejected = 0;
  unsigned pair_logs = 0;
  for (u32 entry = 0; entry < entry_count; ++entry)
  {
    if (pos + 6 > header_size)
      break;
    const u32 offset = ReadBE24Local(archive.data() + pos);
    const u32 packed_size = ReadBE24Local(archive.data() + pos + 3);
    pos += 6;
    const std::size_t name_start = pos;
    while (pos < header_size && archive[pos] != 0)
      ++pos;
    if (pos >= header_size)
      break;
    const std::string raw_name(reinterpret_cast<const char*>(archive.data() + name_start), pos - name_start);
    ++pos;

    const std::string filename = CanonicalDMFName(raw_name);
    if (!filename.ends_with(".dmf"))
      continue;
    ++gc_dmf;
    if (offset > archive.size() || packed_size > archive.size() - offset || packed_size < 8)
    {
      ++rejected;
      continue;
    }
    const u8* bytes = archive.data() + offset;
    if (bytes[0] != 'D' || bytes[1] != 'M' || bytes[2] != 'F' || bytes[3] != 0)
    {
      ++rejected;
      continue;
    }

    std::shared_ptr<DMFResource> ps3;
    {
      if (const auto it = g_dmf_cache.find(filename); it != g_dmf_cache.end())
        ps3 = it->second;
    }
    if (!ps3)
    {
      ++missing;
      continue;
    }

    ExactDMFPair pair;
    pair.ps3 = ps3;
    pair.gc_size = packed_size;
    pair.gc_version_word = BE32(bytes + 4);
    BuildExactSkinGroupMap(std::span<const u8>(bytes, packed_size), &pair);

    // CPartBin::Link in the retail GC ELF calls GXCallDisplayList with
    // record+0 as the DL pointer and (record+4 << 5) as its exact byte size.
    // record+6 is the palette count and record+8 the GC skin-group IDs.
    const u32 gc_groups = BE32(bytes + 0x20);
    const u32 gc_group_off = BE32(bytes + 0x24);
    const u32 gc_materials = BE32(bytes + 0x28);
    const u32 gc_material_off = BE32(bytes + 0x2c);
    const u32 gc_texture_table = BE32(bytes + 0x34);
    if (gc_materials <= 1024 && gc_material_off <= packed_size &&
        static_cast<std::size_t>(gc_materials) * 56 <= packed_size - gc_material_off)
    {
      for (u32 material = 0; material < gc_materials; ++material)
      {
        const u8* mat = bytes + gc_material_off + static_cast<std::size_t>(material) * 56;
        const u32 gc_texture_index = BE32(mat + 40);
        std::string gc_material_name;
        if (gc_texture_table <= packed_size && gc_texture_index <= 65535 &&
            static_cast<std::size_t>(gc_texture_index + 1) * 16 <= packed_size - gc_texture_table)
        {
          gc_material_name = Lower(FixedString(
              bytes + gc_texture_table + static_cast<std::size_t>(gc_texture_index) * 16, 16));
        }
        // v12.10:
        // Character geometry is GameCube-only. Skip before:
        //
        //   CountGCDMFTriangles()
        //   DL hashing
        //   prefix hashing
        //   candidate allocation
        //   PreparedDMFDraw creation
        //
        // This both closes the geometry leak and removes useless work.
        if (IsHardDisabledPS3DMFResource(filename) ||
            IsHardDisabledPS3DMFMaterial(gc_material_name))
          continue;

        const u32 draw_count = BE32(mat + 44);
        const u32 draw_table = BE32(mat + 48);
        if (draw_count > 8192 || draw_table > packed_size ||
            static_cast<std::size_t>(draw_count) * 18 > packed_size - draw_table)
          continue;
        for (u32 cluster = 0; cluster < draw_count; ++cluster)
        {
          const u8* record = bytes + draw_table + static_cast<std::size_t>(cluster) * 18;
          const u32 dl_offset = BE32(record);
          const u32 dl_size = static_cast<u32>(BE16(record + 4)) << 5;
          const u16 palette_count = BE16(record + 6);
          if (!dl_size || palette_count > 10 || dl_offset > packed_size || dl_size > packed_size - dl_offset)
            continue;
          OriginalGCDMFDrawCandidate candidate;
          candidate.ps3 = ps3;
          candidate.gc_name = filename;
          candidate.file_size = packed_size;
          std::copy_n(bytes + 12, 8, candidate.model_tag.begin());
          candidate.group_count = gc_groups;
          candidate.group_offset = gc_group_off;
          candidate.material_count = gc_materials;
          candidate.material_offset = gc_material_off;
          candidate.dl_offset = dl_offset;
          candidate.material_index = material;
          candidate.cluster_index = cluster;
          candidate.gc_material_name = gc_material_name;
          candidate.gc_palette_groups.assign(record + 8, record + 8 + palette_count);

          const std::span<const u8> authored_dl(bytes + dl_offset, dl_size);
          CountGCDMFTriangles(authored_dl, &candidate.gc_triangle_count);

          const DisplayListSignatureKey dl_key{
              dl_size, Common::GetHash64(bytes + dl_offset, dl_size, 0)};
          u64 prefix;
          std::memcpy(&prefix, bytes + dl_offset, sizeof(prefix));
          g_dmf_prefixes.insert({dl_size, prefix});

          if (IsSupportedPlayerWeaponDMF(filename) &&
              IsPlayerWeaponAtlasMaterial(gc_material_name))
          {
            u64 topology_hash = 0;
            if (BuildPlayerWeaponDMFTopologySignature(authored_dl, &topology_hash))
            {
              const DMFTopologySignatureKey topology_key{dl_size, topology_hash};
              g_dmf_player_topology_sizes.insert(dl_size);
              g_dmf_player_topology_candidates[topology_key].push_back(
                  {dl_key, filename, material, cluster});
            }
          }

          g_dmf_display_list_candidates[dl_key].push_back(std::move(candidate));
        }
      }
    }

    g_dmf_pairs[filename] = pair;
    ++paired;
    if (PS3RuntimeDebugEnabled() && pair_logs++ < 32)
    {
      std::fprintf(stderr,
                   "[moh-ps3-dmf] GC/PS3 EXACT PAIR: gc=%s gc_size=%u gc_ver=%08X ps3=%s ps3_ver=0x%X skin_groups=%zu mapped_groups=%zu clusters=%zu bones=%u\n",
                   filename.c_str(), packed_size, pair.gc_version_word, ps3->source_name.c_str(),
                   ps3->info.version, ps3->decoded ? ps3->decoded->skin_groups.size() : 0,
                   pair.mapped_skin_groups, ps3->decoded ? ps3->decoded->clusters.size() : 0,
                   ps3->info.bone_ref_count);
    }
  }

  std::fprintf(stderr,
               "[moh-ps3-dmf] GC/PS3 exact pair index ready: level=%.*s gc_dmf=%zu paired=%zu missing_ps3=%zu rejected=%zu dl_signature_keys=%zu player_topology_keys=%zu aliases=.dmf/.dmt | exact draw identity + skin groups ready; GC draw retained until animated palette skinning succeeds\n",
               static_cast<int>(level.size()), level.data(), gc_dmf, paired, missing, rejected,
               g_dmf_display_list_candidates.size(), g_dmf_player_topology_candidates.size());
}
}  // namespace

void ClearMSHCache()
{
  std::scoped_lock lock(g_msh_cache_mutex);
  g_msh_cache.clear();
  ++g_msh_cache_revision;
  g_display_lists.clear();
  g_display_list_candidates.clear();
  g_world_direct_matches.clear();
  g_world_direct_rejected.clear();
  g_world_direct_claims.clear();
  g_world_dynamic_descriptor_claims.clear();
  g_world_direct_descriptor_keys.clear();
  g_world_cpt_sequences.clear();
  g_world_full_level_mesh.reset();
  g_world_full_level_normals.reset();
  g_current_draw = {};
  g_current_draw_transient_world = false;
  g_current_world_direct_key = 0;
  MOHFrontline::WorldLevelRuntime::Clear();
}

void ClearDMFCache()
{
  std::scoped_lock lock(g_dmf_cache_mutex);
  g_dmf_address_cache.fill({});
  g_dmf_cache.clear();
  g_dmf_pairs.clear();
  g_dmf_display_list_candidates.clear();
  g_dmf_prefixes.clear();
  g_dmf_player_topology_candidates.clear();
  g_dmf_player_topology_sizes.clear();
  g_dmf_palette_flow_clusters.clear();
  g_dmf_palette_flow_analyses.clear();
  g_dmf_palette_flow_material_attempts.clear();
  g_current_dmf_draw = {};
}

void ClearSKLCache()
{
  std::scoped_lock lock(g_skl_cache_mutex);
  g_skl_cache.clear();
}

void ClearEMTCache()
{
  std::scoped_lock lock(g_emt_cache_mutex);
  g_emt_cache.clear();
}

void PreloadCurrentLevelMSH(std::string_view level)
{
  if (!PS3AssetPort::IsMSHEnabled() || !PS3RemasterAssets::IsReady() || level.empty())
    return;

  MOHFrontline::WorldLevelRuntime::Rebuild(level);

  std::unordered_map<std::string, std::shared_ptr<StaticMesh>> next;
  std::size_t candidates = 0;
  std::size_t decoded = 0;
  std::size_t recognized_no_draw = 0;
  std::size_t rejected = 0;

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string raw_filename = Lower(asset.filename);
    const std::string filename = CanonicalModelName(raw_filename);
    if ((!raw_filename.ends_with(".msh") && !raw_filename.ends_with(".msf")) ||
        !BelongsToLevel(asset, level))
      continue;

    ++candidates;
    const std::vector<u8> bytes = PS3RemasterAssets::ReadBinary(asset);
    if (bytes.empty())
    {
      ++rejected;
      continue;
    }

    auto mesh = std::make_shared<StaticMesh>();
    mesh->source_name = asset.relative_path;

    const bool parsed = ParseMSHv8(bytes, mesh.get());
    const bool renderable = parsed && IsHostRenderable(*mesh);

    const std::string no_draw_name =
        Lower(asset.relative_path);

    const bool intentional_no_draw =
        !parsed &&
        bytes.size() <= 256 &&
        (no_draw_name.find(
             "invisblocker") !=
             std::string::npos ||
         no_draw_name.find(
             "invisibleblocker") !=
             std::string::npos);

    if (intentional_no_draw)
    {
      ++decoded;
      ++recognized_no_draw;

      std::fprintf(
          stderr,
          "[moh-ps3-msh] READY NO-DRAW: "
          "%s size=%zu | "
          "invisible/collision-only resource\n",
          asset.relative_path.c_str(),
          bytes.size());

      continue;
    }

    if (!renderable)
    {
      ++rejected;

      static unsigned msh_decode_reject_logs = 0;
      if (PS3RuntimeDebugEnabled() && msh_decode_reject_logs++ < 64)
      {
        std::fprintf(
            stderr,
            "[moh-ps3-msh] DECODE REJECT DETAIL: %s size=%zu "
            "parsed=%d host_renderable=%d submeshes=%zu\n",
            asset.relative_path.c_str(),
            bytes.size(),
            parsed ? 1 : 0,
            renderable ? 1 : 0,
            mesh->submeshes.size());
      }

      continue;
    }

    ++decoded;
    if (decoded <= 32)
    {
      std::size_t vertices = 0;
      std::size_t indices = 0;
      for (const auto& sub : mesh->submeshes)
      {
        vertices += sub.vertex_count;
        indices += sub.index_count;
      }
      std::fprintf(stderr,
                   "[moh-ps3-msh] READY: %s submeshes=%zu vertices=%zu indices=%zu\n",
                   asset.relative_path.c_str(), mesh->submeshes.size(), vertices, indices);
    }

    next[filename] = mesh;
    next[raw_filename] = mesh;
    next[Lower(asset.relative_path)] = mesh;
  }

  // v9: the PS3 level world is authored in *_ART_cN.cpt, not as ordinary MSH files.
  // Decode strict CPT/RSX geometry candidates into one-submesh StaticMesh objects and feed
  // them to the already-proven MSH renderer/bootstrap.  Unrecognized chunks remain 100% GC.
  PS3WorldGeometry::DecodeStats world_geo_stats{};
  std::size_t world_cpt_chunks = 0;
  std::size_t world_meshes = 0;
  std::size_t world_packs = 0;
  std::vector<WorldCPTSequence> next_world_sequences;
  std::shared_ptr<StaticMesh> next_world_full_level_mesh;
  std::shared_ptr<std::vector<std::array<float, 3>>> next_world_full_level_normals;
  if (PS3WorldGeometry::Enabled())
  {
    const bool build_packs = EnvSwitchLocal("MOH_PS3_CPT_GEOMETRY_PACKS", true);
    constexpr std::array<std::size_t, 4> pack_widths{2, 4, 8, 16};
    for (const auto& asset : PS3RemasterAssets::GetAssets())
    {
      if (!PS3WorldGeometry::IsWorldChunk(asset) || !BelongsToLevel(asset, level))
        continue;
      ++world_cpt_chunks;
      auto converted = PS3WorldGeometry::Decode(asset, &world_geo_stats);
      MOHFrontline::WorldLevelRuntime::AnchorCPTChunk(asset, &converted);
      std::vector<std::shared_ptr<StaticMesh>> chunk_meshes;
      chunk_meshes.reserve(converted.size());
      for (auto& mesh : converted)
      {
        if (!mesh || mesh->submeshes.size() != 1 || mesh->submeshes[0].indices.empty() ||
            mesh->submeshes[0].position_uv.empty())
          continue;
        chunk_meshes.push_back(mesh);
        next["@world-cpt/" + std::to_string(world_meshes)] = mesh;
        next[Lower(mesh->source_name)] = mesh;
        ++world_meshes;
      }

      if (!chunk_meshes.empty())
        next_world_sequences.push_back(WorldCPTSequence{asset.relative_path, chunk_meshes});

      if (build_packs && chunk_meshes.size() >= 2)
      {
        std::vector<std::size_t> triangle_prefix(chunk_meshes.size() + 1);
        for (std::size_t i = 0; i < chunk_meshes.size(); ++i)
          triangle_prefix[i + 1] = triangle_prefix[i] +
              chunk_meshes[i]->submeshes[0].indices.size() / 3;
        for (std::size_t width : pack_widths)
        {
          if (width > chunk_meshes.size())
            continue;
          // Half-overlap gives enough phase coverage without materialising all
          // O(N^2) descriptor combinations.  Small pairs use step=1.
          const std::size_t step = std::max<std::size_t>(1, width / 2);
          for (std::size_t first = 0; first + width <= chunk_meshes.size(); first += step)
          {
            const auto candidate_triangles = triangle_prefix[first + width] - triangle_prefix[first];
            if (candidate_triangles < 48 || candidate_triangles > 2048)
              continue;
            auto pack = BuildWorldCPTPack(chunk_meshes, first, width);
            if (!pack || pack->submeshes.empty())
              continue;
            const std::size_t triangles = pack->submeshes[0].indices.size() / 3;
            // Existing single-descriptor matching already handles tiny draws.
            // Packs target the large architectural batches that remained GC in
            // v9.3, while keeping memory/runtime candidate counts bounded.
            if (triangles < 48 || triangles > 2048)
              continue;
            next["@world-cpt-pack/" + std::to_string(world_packs)] = pack;
            next[Lower(pack->source_name)] = pack;
            ++world_packs;
          }
        }
      }
    }
    if (IsFullCPTLevelRenderEnabled())
    {
      next_world_full_level_mesh =
          BuildWorldCPTFullLevelMesh(next_world_sequences, &next_world_full_level_normals);
      if (next_world_full_level_mesh && !next_world_full_level_mesh->submeshes.empty())
      {
        const auto& full = next_world_full_level_mesh->submeshes[0];
        std::fprintf(stderr,
                     "[moh-ps3-world-full] READY: level=%.*s members=%zu vertices=%u triangles=%zu indices=%zu | NODE70 world aggregate ready; material split remains diagnostic\n",
                     static_cast<int>(level.size()), level.data(), world_meshes,
                     full.vertex_count, full.indices.size() / 3, full.indices.size());
      }
      else
      {
        std::fprintf(stderr,
                     "[moh-ps3-world-full] BUILD FAILED: level=%.*s | normal CPT renderer retained\n",
                     static_cast<int>(level.size()), level.data());
      }
    }

    std::fprintf(stderr,
                 "[moh-ps3-world-geo] CACHE READY: level=%.*s chunks=%zu descriptors=%zu inline=%zu rsx=%zu meshes=%zu packs=%zu duplicate=%zu rejected=%zu | renderer=PS3MeshPort strict-bootstrap\n",
                 static_cast<int>(level.size()), level.data(), world_cpt_chunks,
                 world_geo_stats.descriptor_candidates, world_geo_stats.inline_candidates,
                 world_geo_stats.rsx_candidates, world_meshes, world_packs,
                 world_geo_stats.duplicate, world_geo_stats.rejected);
  }

  {
    std::scoped_lock lock(g_msh_cache_mutex);
    g_display_lists.clear();
    g_world_direct_matches.clear();
    g_world_direct_rejected.clear();
    g_world_direct_claims.clear();
    g_world_dynamic_descriptor_claims.clear();
    g_world_direct_descriptor_keys.clear();
    g_world_cpt_sequences = std::move(next_world_sequences);
    g_world_full_level_mesh = std::move(next_world_full_level_mesh);
    g_world_full_level_normals = std::move(next_world_full_level_normals);
    g_msh_cache = std::move(next);
    ++g_msh_cache_revision;
  }

  std::fprintf(stderr,
               "[moh-ps3-msh] ALL-MSH cache ready: level=%.*s "
               "candidates=%zu decoded=%zu nodraw=%zu rejected=%zu keys=%zu\n",
               static_cast<int>(level.size()), level.data(),
               candidates, decoded, recognized_no_draw, rejected,
               CachedMSHCount());

  IndexOriginalGCLevelMSHSignatures(level);
}

void PreloadCurrentLevelDMF(std::string_view level)
{
  if (!PS3AssetPort::IsDMFEnabled() || !PS3RemasterAssets::IsReady() || level.empty())
    return;

  std::unordered_map<std::string, std::shared_ptr<DMFResource>> next;
  std::size_t candidates = 0;
  std::size_t decoded = 0;
  std::size_t rejected = 0;
  std::size_t total_meshes = 0;
  std::size_t total_materials = 0;

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string raw_filename = Lower(asset.filename);
    const std::string filename = CanonicalDMFName(raw_filename);
    if ((!raw_filename.ends_with(".dmf") && !raw_filename.ends_with(".dmt")) ||
        !BelongsToLevel(asset, level))
      continue;

    ++candidates;
    const std::vector<u8> bytes = PS3RemasterAssets::ReadBinary(asset);
    if (bytes.empty())
    {
      ++rejected;
      continue;
    }

    const DMFInfo info = InspectDMF(bytes);
    if (!info.valid)
    {
      ++rejected;
      continue;
    }

    ++decoded;
    total_meshes += info.mesh_count;
    total_materials += info.material_count;

    auto decoded_mesh = std::make_shared<DMFDecoded>();
    if (info.version == 0x0502 && !DecodeDMF0502(bytes, decoded_mesh.get()))
    {
      ++rejected;
      --decoded;
      total_meshes -= info.mesh_count;
      total_materials -= info.material_count;
      static unsigned decode_reject_logs = 0;
      if (decode_reject_logs++ < 16 && bytes.size() >= 0x5c)
      {
        const u32 groups = BE32(bytes.data() + 0x20);
        const u32 group_off = BE32(bytes.data() + 0x24);
        const u32 materials = BE32(bytes.data() + 0x28);
        const u32 material_off = BE32(bytes.data() + 0x2c);
        const u32 texture_off = BE32(bytes.data() + 0x34);
        const u32 bones = BE32(bytes.data() + 0x48);
        const u32 bone_off = BE32(bytes.data() + 0x4c);
        std::fprintf(stderr,
                     "[moh-ps3-dmf] DECODE REJECT: %s size=%zu groups=%u@%08X materials=%u@%08X textures@%08X bones=%u@%08X | resource remains identity-only GC fallback\n",
                     asset.relative_path.c_str(), bytes.size(), groups, group_off, materials,
                     material_off, texture_off, bones, bone_off);


        const auto probe_fail =
            [&](u32 material, u32 cluster, const char* stage,
                u32 a = 0, u32 b = 0, u32 c = 0, u32 d = 0)
        {
          std::fprintf(
              stderr,
              "[moh-ps3-dmf] 0502 FIRST FAIL: %s "
              "material=%u cluster=%u stage=%s "
              "a=%08X b=%08X c=%08X d=%08X\n",
              asset.relative_path.c_str(),
              material,
              cluster,
              stage,
              a, b, c, d);
        };

        bool probe_reported = false;

        // Bone-ref table.
        if (!probe_reported)
        {
          if (!bones || bones > 4096 ||
              bone_off > bytes.size() ||
              static_cast<std::size_t>(bones) * 16 >
                  bytes.size() - bone_off)
          {
            probe_fail(0, 0, "bone-table",
                       bones, bone_off,
                       static_cast<u32>(bytes.size()), 0);
            probe_reported = true;
          }
          else
          {
            for (u32 bone = 0; bone < bones; ++bone)
            {
              const std::string name =
                  FixedString(bytes.data() + bone_off +
                                  static_cast<std::size_t>(bone) * 16,
                              16);

              if (name.empty())
              {
                probe_fail(0, 0, "empty-bone-ref",
                           bone, bone_off +
                                     static_cast<u32>(bone * 16),
                           0, 0);
                probe_reported = true;
                break;
              }
            }
          }
        }

        // Skin group table.
        if (!probe_reported)
        {
          if (!groups || groups > 4096 ||
              group_off > bytes.size() ||
              static_cast<std::size_t>(groups) * 28 >
                  bytes.size() - group_off)
          {
            probe_fail(0, 0, "skin-group-table",
                       groups, group_off,
                       static_cast<u32>(bytes.size()), 0);
            probe_reported = true;
          }
          else
          {
            for (u32 group = 0; group < groups; ++group)
            {
              const u8* g =
                  bytes.data() + group_off +
                  static_cast<std::size_t>(group) * 28;

              const u8 bone_a = g[0];
              const u8 bone_b = g[4];
              const float blend = BEFloat(g + 8);
              const float blend_q = blend * 4096.0f;

              if (bone_a >= bones || bone_b >= bones ||
                  !std::isfinite(blend) ||
                  !std::isfinite(blend_q) ||
                  blend_q < -0.5f ||
                  blend_q > 65535.5f)
              {
                probe_fail(0, group, "skin-group-record",
                           bone_a, bone_b,
                           static_cast<u32>(
                               std::max(0.0f, blend_q)),
                           0);
                probe_reported = true;
                break;
              }

              for (unsigned aux = 0; aux < 4; ++aux)
              {
                const float value =
                    BEFloat(g + 12 + aux * 4);

                if (!std::isfinite(value))
                {
                  probe_fail(0, group,
                             "skin-group-aux-nonfinite",
                             aux, 0, 0, 0);
                  probe_reported = true;
                  break;
                }
              }

              if (probe_reported)
                break;
            }
          }
        }

        // Material/cluster stream.
        if (!probe_reported)
        {
          if (materials > 1024 ||
              material_off > bytes.size() ||
              static_cast<std::size_t>(materials) * 60 >
                  bytes.size() - material_off)
          {
            probe_fail(0, 0, "material-table",
                       materials, material_off,
                       static_cast<u32>(bytes.size()), 0);
            probe_reported = true;
          }
        }

        if (!probe_reported)
        {
          for (u32 material = 0;
               material < materials && !probe_reported;
               ++material)
          {
            const u8* m =
                bytes.data() + material_off +
                static_cast<std::size_t>(material) * 60;

            const u32 texture_index = BE32(m + 44);
            const u32 cluster_count = BE32(m + 48);
            u32 palette = BE32(m + 52);
            u32 geometry = BE32(m + 56);

            if (cluster_count > 8192 ||
                palette > bytes.size() ||
                geometry > bytes.size())
            {
              probe_fail(material, 0,
                         "material-head",
                         texture_index,
                         cluster_count,
                         palette,
                         geometry);
              probe_reported = true;
              break;
            }

            for (u32 cluster = 0;
                 cluster < cluster_count;
                 ++cluster)
            {
              if (palette > bytes.size() ||
                  bytes.size() - palette < 4)
              {
                probe_fail(material, cluster,
                           "palette-header-oob",
                           palette, geometry,
                           cluster_count,
                           static_cast<u32>(bytes.size()));
                probe_reported = true;
                break;
              }

              if (geometry > bytes.size() ||
                  bytes.size() - geometry < 12)
              {
                probe_fail(material, cluster,
                           "geometry-header-oob",
                           palette, geometry,
                           cluster_count,
                           static_cast<u32>(bytes.size()));
                probe_reported = true;
                break;
              }

              const u8 palette_count =
                  bytes[palette + 2];

              const std::size_t palette_bytes =
                  4 +
                  static_cast<std::size_t>(
                      palette_count) * 2;

              if (!palette_count)
              {
                probe_fail(material, cluster,
                           "zero-palette",
                           palette, geometry,
                           0, 0);
                probe_reported = true;
                break;
              }

              if (palette_bytes >
                  bytes.size() - palette)
              {
                probe_fail(material, cluster,
                           "palette-size-oob",
                           palette_count,
                           static_cast<u32>(
                               palette_bytes),
                           palette,
                           geometry);
                probe_reported = true;
                break;
              }

              for (u8 slot = 0;
                   slot < palette_count;
                   ++slot)
              {
                const u16 group =
                    BE16(bytes.data() +
                         palette + 4 +
                         static_cast<std::size_t>(
                             slot) * 2);

                if (group >= groups)
                {
                  probe_fail(material, cluster,
                             "palette-group-oob",
                             slot, group,
                             groups, palette);
                  probe_reported = true;
                  break;
                }
              }

              if (probe_reported)
                break;

              palette +=
                  static_cast<u32>(
                      palette_bytes);

              const u32 index_count =
                  BE32(bytes.data() + geometry);

              const u32 vertex_count =
                  BE32(bytes.data() +
                       geometry + 4);

              const u8 stride =
                  bytes[geometry + 8];

              const u8 attribute_count =
                  bytes[geometry + 9];

              if (!index_count ||
                  !vertex_count ||
                  index_count >
                      16 * 1024 * 1024 ||
                  vertex_count >
                      4 * 1024 * 1024 ||
                  (index_count % 3) != 0)
              {
                probe_fail(material, cluster,
                           "geometry-counts",
                           index_count,
                           vertex_count,
                           stride,
                           attribute_count);
                probe_reported = true;
                break;
              }

              if (stride < 30 ||
                  stride > 192)
              {
                probe_fail(material, cluster,
                           "vertex-stride",
                           stride,
                           index_count,
                           vertex_count,
                           geometry);
                probe_reported = true;
                break;
              }

              if (attribute_count > 32)
              {
                probe_fail(material, cluster,
                           "attribute-count",
                           attribute_count,
                           stride,
                           geometry, 0);
                probe_reported = true;
                break;
              }

              const std::size_t attribute_start =
                  static_cast<std::size_t>(
                      geometry) + 12;

              const std::size_t attribute_bytes =
                  static_cast<std::size_t>(
                      attribute_count) * 4;

              if (attribute_start >
                      bytes.size() ||
                  attribute_bytes >
                      bytes.size() -
                          attribute_start)
              {
                probe_fail(material, cluster,
                           "attributes-oob",
                           static_cast<u32>(
                               attribute_start),
                           static_cast<u32>(
                               attribute_bytes),
                           geometry, stride);
                probe_reported = true;
                break;
              }

              bool duplicate_position = false;
              bool duplicate_normal = false;
              bool duplicate_uv = false;
              bool have_position = false;
              bool have_normal = false;
              bool have_uv = false;

              for (u8 attribute = 0;
                   attribute < attribute_count;
                   ++attribute)
              {
                const u8* d =
                    bytes.data() +
                    attribute_start +
                    static_cast<std::size_t>(
                        attribute) * 4;

                if (d[3] >= stride)
                {
                  probe_fail(material, cluster,
                             "attribute-offset",
                             attribute,
                             d[0], d[3],
                             stride);
                  probe_reported = true;
                  break;
                }

                if (d[0] == 0)
                {
                  duplicate_position =
                      have_position;
                  have_position = true;
                }
                else if (d[0] == 2)
                {
                  duplicate_normal =
                      have_normal;
                  have_normal = true;
                }
                else if (d[0] == 8)
                {
                  duplicate_uv =
                      have_uv;
                  have_uv = true;
                }
              }

              if (probe_reported)
                break;

              if (duplicate_position ||
                  duplicate_normal ||
                  duplicate_uv)
              {
                probe_fail(material, cluster,
                           "duplicate-semantic",
                           duplicate_position,
                           duplicate_normal,
                           duplicate_uv, 0);
                probe_reported = true;
                break;
              }

              const std::size_t vertex_start =
                  attribute_start +
                  attribute_bytes;

              const std::size_t vertex_bytes =
                  static_cast<std::size_t>(
                      vertex_count) *
                  stride;

              if (vertex_start >
                      bytes.size() ||
                  vertex_bytes >
                      bytes.size() -
                          vertex_start)
              {
                probe_fail(material, cluster,
                           "vertices-oob",
                           static_cast<u32>(
                               vertex_start),
                           static_cast<u32>(
                               vertex_bytes),
                           vertex_count,
                           stride);
                probe_reported = true;
                break;
              }

              // Current ordinary-0502 layout uses BE16
              // palette slot at vertex+28.
              for (u32 vertex = 0;
                   vertex < vertex_count;
                   ++vertex)
              {
                const u8* v =
                    bytes.data() +
                    vertex_start +
                    static_cast<std::size_t>(
                        vertex) * stride;

                const u16 slot =
                    BE16(v + 28);

                if (slot >= palette_count)
                {
                  probe_fail(material, cluster,
                             "vertex-palette-slot",
                             vertex,
                             slot,
                             palette_count,
                             stride);
                  probe_reported = true;
                  break;
                }
              }

              if (probe_reported)
                break;

              const std::size_t index_start =
                  vertex_start +
                  vertex_bytes;

              const std::size_t index_bytes =
                  static_cast<std::size_t>(
                      index_count) * 2;

              if (index_start >
                      bytes.size() ||
                  index_bytes >
                      bytes.size() -
                          index_start)
              {
                probe_fail(material, cluster,
                           "indices-oob",
                           static_cast<u32>(
                               index_start),
                           static_cast<u32>(
                               index_bytes),
                           index_count, 0);
                probe_reported = true;
                break;
              }

              for (u32 index = 0;
                   index < index_count;
                   ++index)
              {
                const u16 value =
                    BE16(bytes.data() +
                         index_start +
                         static_cast<std::size_t>(
                             index) * 2);

                if (value >= vertex_count)
                {
                  probe_fail(material, cluster,
                             "index-out-of-range",
                             index,
                             value,
                             vertex_count, 0);
                  probe_reported = true;
                  break;
                }
              }

              if (probe_reported)
                break;

              const std::size_t next =
                  (index_start +
                   index_bytes + 15) &
                  ~std::size_t(15);

              if (next > bytes.size())
              {
                probe_fail(material, cluster,
                           "next-geometry-oob",
                           static_cast<u32>(next),
                           geometry,
                           index_count,
                           vertex_count);
                probe_reported = true;
                break;
              }

              geometry =
                  static_cast<u32>(next);
            }
          }
        }

        if (!probe_reported)
        {
          std::fprintf(
              stderr,
              "[moh-ps3-dmf] 0502 FIRST FAIL: %s "
              "probe reached end of ordinary layout; "
              "failure is inside RSX component decoding/bind validation\n",
              asset.relative_path.c_str());
        }
      }
      // Keep the exact resource/name available even when a platform-layout
      // variant is not yet geometry-decodable. It must remain GC-rendered.
      auto resource = std::make_shared<DMFResource>();
      resource->info = info;
      resource->source_name = asset.relative_path;
      resource->bytes = std::make_shared<const std::vector<u8>>(bytes);
      next[filename] = resource;
      next[raw_filename] = resource;
      next[Lower(asset.relative_path)] = resource;
      continue;
    }

    auto resource = std::make_shared<DMFResource>();
    resource->info = info;
    resource->source_name = asset.relative_path;
    resource->bytes = std::make_shared<const std::vector<u8>>(bytes);
    resource->decoded = decoded_mesh->valid ? decoded_mesh : nullptr;

    if (decoded <= 32)
    {
      std::fprintf(stderr,
                   "[moh-ps3-dmf] READY: %s model=%s version=0x%X skin_groups=%zu clusters=%zu vertices=%zu indices=%zu materials=%u bones=%u\n",
                   asset.relative_path.c_str(), info.model_name.c_str(), info.version,
                   resource->decoded ? resource->decoded->skin_groups.size() : 0,
                   resource->decoded ? resource->decoded->clusters.size() : 0,
                   resource->decoded ? resource->decoded->total_vertices : 0,
                   resource->decoded ? resource->decoded->total_indices : 0,
                   info.material_count, info.bone_ref_count);
    }

    next[filename] = resource;
    next[raw_filename] = resource;
    next[Lower(asset.relative_path)] = resource;
  }

  {
    std::scoped_lock lock(g_dmf_cache_mutex);
    g_dmf_cache = std::move(next);
  }

  std::fprintf(stderr,
               "[moh-ps3-dmf] cache ready: level=%.*s candidates=%zu decoded=%zu rejected=%zu "
               "meshes=%zu materials=%zu keys=%zu\n",
               static_cast<int>(level.size()), level.data(), candidates, decoded, rejected,
               total_meshes, total_materials, CachedDMFCount());


  IndexOriginalGCLevelDMFPairs(level);
  PrepareDMFDraws();
}

void PreloadCurrentLevelSKL(std::string_view level)
{
  if (!PS3AssetPort::IsSKLEnabled() || !PS3RemasterAssets::IsReady() || level.empty())
    return;

  std::unordered_map<std::string, std::shared_ptr<SKLInfo>> next;
  std::size_t candidates = 0;
  std::size_t decoded = 0;
  std::size_t rejected = 0;

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string filename = Lower(asset.filename);
    if (!filename.ends_with(".skl") || !BelongsToLevel(asset, level))
      continue;

    ++candidates;
    const std::vector<u8> bytes = PS3RemasterAssets::ReadBinary(asset);
    if (bytes.empty())
    {
      ++rejected;
      continue;
    }

    auto info = std::make_shared<SKLInfo>(InspectSKL(bytes));
    if (!info->valid)
    {
      ++rejected;
      continue;
    }

    info->source_name = asset.relative_path;
    ++decoded;

    if (decoded <= 16)
    {
      std::fprintf(stderr,
                   "[moh-ps3-skl] READY: %s bones=%u endian=%s names=%zu\n",
                   asset.relative_path.c_str(), info->bone_count,
                   info->big_endian ? "BE" : "LE", info->bone_names.size());
    }

    next[filename] = info;
    next[Lower(asset.relative_path)] = info;
  }

  {
    std::scoped_lock lock(g_skl_cache_mutex);
    g_skl_cache = std::move(next);
  }

  std::fprintf(stderr,
               "[moh-ps3-skl] cache ready: level=%.*s candidates=%zu decoded=%zu rejected=%zu keys=%zu\n",
               static_cast<int>(level.size()), level.data(), candidates, decoded, rejected,
               CachedSKLCount());

  BindDMFPairsToSkeletons();
  PrepareDMFDraws();
}


void PreloadCurrentLevelEMT(std::string_view level)
{
  if (!PS3RemasterAssets::IsReady() || level.empty() || !EnvSwitchLocal("MOH_PS3_EMT", true))
    return;

  std::unordered_map<std::string, std::shared_ptr<EMTResource>> next;
  std::size_t candidates = 0, decoded = 0, rejected = 0;
  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string filename = Lower(asset.filename);
    if (!filename.ends_with(".emt"))
      continue;
    if (!BelongsToLevel(asset, level) && filename.find(Lower(std::string(level))) == std::string::npos)
      continue;
    ++candidates;
    const std::vector<u8> bytes = PS3RemasterAssets::ReadBinary(asset);
    if (bytes.empty())
    {
      ++rejected;
      continue;
    }
    const EMTInfo info = InspectEMT(bytes);
    if (!info.valid)
    {
      ++rejected;
      static unsigned emt_reject_logs = 0;
      if (PS3RuntimeDebugEnabled() && emt_reject_logs++ < 8)
      {
        char head[3 * 32 + 1]{};
        std::size_t w = 0;
        for (std::size_t i = 0; i < std::min<std::size_t>(32, bytes.size()); ++i)
          w += std::snprintf(head + w, sizeof(head) - w, "%02X%s", bytes[i], i + 1 < 32 ? " " : "");
        std::fprintf(stderr,
                     "[moh-ps3-emt] DECODE REJECT: %s size=%zu head=%s\n",
                     asset.relative_path.c_str(), bytes.size(), head);
      }
      continue;
    }
    auto resource = std::make_shared<EMTResource>();
    resource->info = info;
    resource->source_name = asset.relative_path;
    resource->bytes = std::make_shared<const std::vector<u8>>(bytes);
    next[filename] = resource;
    next[Lower(asset.relative_path)] = resource;
    ++decoded;
    if (decoded <= 8)
    {
      std::fprintf(stderr,
                   "[moh-ps3-emt] READY: %s endian=%s version=%u entities=%u A=%08X B=%08X C=%08X LEKS=%zu\n",
                   asset.relative_path.c_str(), info.big_endian ? "BE" : "LE", info.version,
                   info.entity_count, info.section_a, info.section_b, info.section_c,
                   info.leks_blocks);
    }
  }
  {
    std::scoped_lock lock(g_emt_cache_mutex);
    g_emt_cache = std::move(next);
  }
  std::fprintf(stderr,
               "[moh-ps3-emt] cache ready: level=%.*s candidates=%zu decoded=%zu rejected=%zu keys=%zu | pose source remains GC until runtime entity/palette identity is exact\n",
               static_cast<int>(level.size()), level.data(), candidates, decoded, rejected,
               CachedEMTCount());
}

const StaticMesh* FindCachedMSH(std::string_view name_or_path)
{
  const std::string raw = Lower(std::string(name_or_path));
  const std::string base = CanonicalModelName(name_or_path);
  std::scoped_lock lock(g_msh_cache_mutex);

  if (const auto it = g_msh_cache.find(raw); it != g_msh_cache.end())
    return it->second.get();
  if (const auto it = g_msh_cache.find(base); it != g_msh_cache.end())
    return it->second.get();
  return nullptr;
}

const DMFResource* FindCachedDMF(std::string_view name_or_path)
{
  const std::string raw = Lower(std::string(name_or_path));
  const std::string base = CanonicalDMFName(name_or_path);
  std::scoped_lock lock(g_dmf_cache_mutex);
  if (const auto it = g_dmf_cache.find(raw); it != g_dmf_cache.end()) return it->second.get();
  if (const auto it = g_dmf_cache.find(base); it != g_dmf_cache.end()) return it->second.get();
  return nullptr;
}

const SKLInfo* FindCachedSKL(std::string_view name_or_path)
{
  const std::string raw = Lower(std::string(name_or_path));
  const std::string base = BaseName(name_or_path);
  std::scoped_lock lock(g_skl_cache_mutex);
  if (const auto it = g_skl_cache.find(raw); it != g_skl_cache.end()) return it->second.get();
  if (const auto it = g_skl_cache.find(base); it != g_skl_cache.end()) return it->second.get();
  return nullptr;
}

const EMTResource* FindCachedEMT(std::string_view name_or_path)
{
  const std::string raw = Lower(std::string(name_or_path));
  const std::string base = BaseName(name_or_path);
  std::scoped_lock lock(g_emt_cache_mutex);
  if (const auto it = g_emt_cache.find(raw); it != g_emt_cache.end()) return it->second.get();
  if (const auto it = g_emt_cache.find(base); it != g_emt_cache.end()) return it->second.get();
  return nullptr;
}

std::size_t CachedMSHCount()
{
  std::scoped_lock lock(g_msh_cache_mutex);
  return g_msh_cache.size();
}

std::size_t CachedDMFCount()
{
  std::scoped_lock lock(g_dmf_cache_mutex);
  return g_dmf_cache.size();
}

std::size_t CachedSKLCount()
{
  std::scoped_lock lock(g_skl_cache_mutex);
  return g_skl_cache.size();
}

std::size_t CachedEMTCount()
{
  std::scoped_lock lock(g_emt_cache_mutex);
  return g_emt_cache.size();
}

bool IsFullCPTLevelRenderEnabled()
{
  // Launch-time diagnostic setting: this function is queried from the draw
  // path, so never call getenv() for every primitive batch.
  static const bool full_level_enabled =
      EnvSwitchLocal("MOH_PS3_CPT_FULL_LEVEL", false);

  if (!PS3WorldGeometry::Enabled() || !full_level_enabled)
  {
    return false;
  }

  // A raw full-level aggregate does not preserve per-material draws or the
  // original visibility selection. Keep it behind an explicit diagnostic flag.
  // NODE70 holds visibility bounds; CPT positions are already world-space.
  static const bool unsafe_raw_overlay =
      EnvSwitchLocal("MOH_PS3_CPT_FULL_LEVEL_UNSAFE", false);
  if (!unsafe_raw_overlay)
  {
    static bool logged_safe_hold = false;
    if (!logged_safe_hold)
    {
      logged_safe_hold = true;
      std::fprintf(stderr,
                   "[moh-ps3-world-full] SAFE HOLD: raw full-level overlay disabled; "
                   "NODE70 descriptors still require per-group GC-world anchors. "
                   "Normal CPT direct/range replacement remains active. "
                   "Set MOH_PS3_CPT_FULL_LEVEL_UNSAFE=1 only for geometry diagnostics.\n");
    }
    return false;
  }

  return true;
}

bool IsStaticDrawReplacementEnabled()
{
  static const bool enabled = PS3AssetPort::IsMSHEnabled() &&
      EnvSwitchLocal("MOH_PS3_MSH_DRAW", true) && EnvSwitchLocal("MOH_PS3_MSH_REPLACE", true);
  return enabled;
}

void RegisterGuestStaticMesh(std::string_view name, u32 address, std::span<const u8> bytes)
{
  if (!IsStaticDrawReplacementEnabled() || bytes.size() < 0x30 || BE32(bytes.data()) != 9)
    return;
  const auto level = MOHFrontline::NativeAssets::GetCurrentLevel();
  if (level.empty())
    return;
  const std::string scope = "data/" + level.substr(0, 1) + "/" + level + "/";
  std::string filename = CanonicalModelName(name);
  if (!filename.ends_with(".msh"))
    filename += ".msh";

  std::shared_ptr<StaticMesh> mesh;
  {
    std::scoped_lock lock(g_msh_cache_mutex);
    if (auto it = g_msh_cache.find(filename); it != g_msh_cache.end())
      mesh = it->second;
  }

  if (!mesh)
  {
    const auto* asset =
        PS3RemasterAssets::FindByRelativePath(scope + "level.viv::" + filename);
    if (!asset)
      asset = PS3RemasterAssets::FindByRelativePath(scope + filename);
    if (!asset && filename.ends_with(".msh"))
    {
      std::string alias = filename;
      alias.replace(alias.size() - 4, 4, ".msf");
      asset = PS3RemasterAssets::FindByRelativePath(scope + "level.viv::" + alias);
      if (!asset)
        asset = PS3RemasterAssets::FindByRelativePath(scope + alias);
    }
    if (!asset)
      return;

    mesh = std::make_shared<StaticMesh>();
    mesh->source_name = asset->relative_path;
    if (!ParseMSHv8(PS3RemasterAssets::ReadBinary(*asset), mesh.get()) || !IsHostRenderable(*mesh))
      return;
    std::scoped_lock lock(g_msh_cache_mutex);
    g_msh_cache[filename] = mesh;
    g_msh_cache[Lower(asset->filename)] = mesh;
    g_msh_cache[Lower(asset->relative_path)] = mesh;
    ++g_msh_cache_revision;
  }

  std::vector<OriginalGCNode> nodes;
  if (!ParseOriginalGCMSHNodes(bytes, &nodes))
    return;
  const std::vector<int> mapping = MapGCNodesToPS3Submeshes(nodes, *mesh);

  std::size_t registered = 0;
  for (std::size_t i = 0; i < nodes.size(); ++i)
  {
    if (mapping[i] < 0)
      continue;
    StaticDrawMatch match = BuildStaticDrawMatch(mesh, static_cast<std::size_t>(mapping[i]));
    if (!match)
      continue;

    DisplayListIdentity identity;
    identity.match = std::move(match);
    identity.match.guest_resource = address;
    identity.match.display_list = (address + nodes[i].dl_offset) & 0x1fffffff;
    identity.size = nodes[i].signature.size;
    identity.command_hash = nodes[i].signature.command_hash;
    {
      std::scoped_lock lock(g_msh_cache_mutex);
      g_display_lists[identity.match.display_list] = identity;
    }
    ++g_matches;
    ++registered;
  }

  if (registered)
  {
    static unsigned register_logs = 0;
    if (PS3RuntimeDebugEnabled() && register_logs++ < 32)
      std::fprintf(stderr,
                   "[moh-ps3-msh] LIVE EXACT MESH RESOURCE: level=%s gc=%s ps3=%s nodes=%zu submeshes=%zu registered=%zu base=%08x\n",
                   level.c_str(), filename.c_str(), mesh->source_name.c_str(), nodes.size(),
                   mesh->submeshes.size(), registered, address);
  }
}

StaticDrawMatch FindDisplayList(u32 address, std::span<const u8> commands)
{
  // A pending hash belongs only to this FindDisplayList -> SetDisplayListContext
  // sequence. The intermediate SetDisplayListContext(0,{}) intentionally does
  // not consume it.
  g_pending_display_list_hash.valid = false;

  ResolveDMFDisplayList(address, commands);
  if (!IsStaticDrawReplacementEnabled() || commands.size() <= 52)
    return {};

  const u32 runtime_address = address & 0x1fffffff;
  const u32 size = static_cast<u32>(commands.size());
  const u64 command_hash =
      Common::GetHash64(commands.data() + 52, commands.size() - 52, 0);

  g_pending_display_list_hash =
      {runtime_address, size, commands.data(), command_hash, true};

  {
    std::scoped_lock lock(g_msh_cache_mutex);
    if (auto it = g_display_lists.find(runtime_address); it != g_display_lists.end())
    {
      if (it->second.size == size && it->second.command_hash == command_hash)
        return it->second.match;
      g_display_lists.erase(it);
    }
  }

  const DisplayListSignatureKey key{size, command_hash};
  std::vector<OriginalGCDrawCandidate> candidates;
  {
    std::scoped_lock lock(g_msh_cache_mutex);
    const auto it = g_display_list_candidates.find(key);
    if (it == g_display_list_candidates.end())
      return {};
    candidates = it->second;
  }

  const OriginalGCDrawCandidate* resolved = nullptr;
  if (candidates.size() == 1)
  {
    resolved = &candidates.front();
  }
  else
  {
    for (const auto& candidate : candidates)
    {
      if (!ValidateOriginalGCCandidate(candidate, runtime_address))
        continue;
      if (resolved)
      {
        static unsigned ambiguous_logs = 0;
        if (PS3RuntimeDebugEnabled() && ambiguous_logs++ < 32)
          std::fprintf(stderr,
                       "[moh-ps3-msh] GC SIGNATURE COLLISION unresolved: DL=%08x size=%u hash=%016llX candidates=%zu -> keep GC\n",
                       runtime_address, size, static_cast<unsigned long long>(command_hash),
                       candidates.size());
        return {};
      }
      resolved = &candidate;
    }
    if (!resolved)
      return {};
  }

  DisplayListIdentity identity;
  identity.match = resolved->match;
  identity.match.display_list = runtime_address;
  identity.size = size;
  identity.command_hash = command_hash;
  {
    std::scoped_lock lock(g_msh_cache_mutex);
    g_display_lists[runtime_address] = identity;
  }
  ++g_matches;

  static unsigned signature_logs = 0;
  if (PS3RuntimeDebugEnabled() && signature_logs++ < 128)
  {
    std::fprintf(stderr,
                 "[moh-ps3-msh] LIVE EXACT GC MSH: gc=%s node=%u/%u ps3=%s submesh=%zu/%zu DL=%08x size=%u hash=%016llX candidates=%zu\n",
                 resolved->gc_name.c_str(), resolved->node_index + 1, resolved->node_count,
                 identity.match.mesh ? identity.match.mesh->source_name.c_str() : "<unknown>",
                 identity.match.submesh_index + 1,
                 identity.match.mesh ? identity.match.mesh->submeshes.size() : 0,
                 runtime_address, size, static_cast<unsigned long long>(command_hash),
                 candidates.size());
  }
  return identity.match;
}
void SetDisplayListContext(u32 address, std::span<const u8> commands)
{
  // Every GX display-list invocation gets an isolated context. This also
  // guarantees that a bootstrap match cannot leak into the next display list.
  if (g_current_draw_transient_world)
    RejectStaticDrawCandidate();
  g_current_draw = {};
  g_current_draw_transient_world = false;
  g_current_world_direct_key = 0;
  g_current_dmf_draw = {};
  g_active_display_list = {};
  if (!address || commands.size() <= 52)
    return;

  // Always remember that we are inside GXCallDisplayList, even when geometry
  // bootstrap is disabled. Direct CPT world matching must run only for
  // primitive batches that are NOT part of a display list.
  const u32 runtime_address = address & 0x1fffffff;
  const u32 command_size = static_cast<u32>(commands.size());

  u64 command_hash = 0;
  if (g_pending_display_list_hash.valid &&
      g_pending_display_list_hash.address == runtime_address &&
      g_pending_display_list_hash.size == command_size &&
      g_pending_display_list_hash.data == commands.data())
  {
    command_hash = g_pending_display_list_hash.hash;
  }
  else
  {
    command_hash =
        Common::GetHash64(commands.data() + 52, commands.size() - 52, 0);
  }

  g_pending_display_list_hash.valid = false;
  g_active_display_list.address = runtime_address;
  g_active_display_list.size = command_size;
  g_active_display_list.command_hash = command_hash;
}

void SetDisplayListMatch(StaticDrawMatch match)
{
  if (g_current_draw_transient_world)
    RejectStaticDrawCandidate();
  g_current_draw_transient_world = false;
  g_current_world_direct_key = 0;
  g_current_draw = std::move(match);
}
const StaticDrawMatch& CurrentStaticDraw() { return g_current_draw; }

void RejectStaticDrawCandidate()
{
  if (!g_current_draw_transient_world)
    return;

  const u64 direct_key = g_current_world_direct_key;
  const StaticMesh* mesh = g_current_draw.mesh;
  if (direct_key != 0)
  {
    std::scoped_lock lock(g_msh_cache_mutex);
    g_world_direct_matches.erase(direct_key);
    g_world_direct_rejected.insert(direct_key);
    if (mesh)
    {
      if (const auto claim = g_world_direct_claims.find(mesh);
          claim != g_world_direct_claims.end() && claim->second == direct_key)
        g_world_direct_claims.erase(claim);
    }
    if (const auto keys_it = g_world_direct_descriptor_keys.find(direct_key);
        keys_it != g_world_direct_descriptor_keys.end())
    {
      for (const auto& descriptor_key : keys_it->second)
      {
        if (const auto claim = g_world_dynamic_descriptor_claims.find(descriptor_key);
            claim != g_world_dynamic_descriptor_claims.end() && claim->second == direct_key)
          g_world_dynamic_descriptor_claims.erase(claim);
      }
      g_world_direct_descriptor_keys.erase(keys_it);
    }
  }

  g_current_draw = {};
  g_current_draw_transient_world = false;
  g_current_world_direct_key = 0;
}

SkinnedDrawMatch CurrentSkinnedDraw() { return g_current_dmf_draw; }
void SetSkinnedDrawMatch(SkinnedDrawMatch match) { g_current_dmf_draw = std::move(match); }
bool IsStaticBootstrapEnabled()
{
  // Exact archive signatures are the normal path. Geometry guessing is a
  // diagnostic fallback and must not scan every resource on unknown draws.
  static const bool enabled = EnvSwitchLocal("MOH_PS3_MSH_BOOTSTRAP", false);
  return enabled && IsStaticDrawReplacementEnabled();
}

namespace
{
SkinnedPaletteAnalysis AnalyzeSkinnedPaletteAtLoad(const SkinnedDrawMatch& draw)
{
  SkinnedPaletteAnalysis analysis;
  if (!draw || !draw.owner || !draw.owner->decoded || draw.gc_material_name.empty() ||
      draw.ps3_group_to_gc.empty() || draw.gc_palette_groups.empty())
    return analysis;

  // A GameCube DMF material may be split into many authored GX display lists,
  // while the PS3 0x0502 file uses a different cluster partition.  Reconstruct
  // the correspondence from the one thing both sides preserve exactly: the
  // material name and the set of skin groups required by each triangle.
  //
  // Deduplicate equal GC palettes first.  Two display lists with the same
  // palette are equivalent for matrix selection and must not create false
  // ambiguity merely because the authored file split them for index limits.
  std::vector<std::vector<u8>> gc_palettes;
  {
    for (const auto& [key, candidates] : g_dmf_display_list_candidates)
    {
      (void)key;
      for (const auto& candidate : candidates)
      {
        if (candidate.gc_name != draw.gc_name ||
            candidate.material_index != draw.material_index ||
            candidate.gc_material_name != draw.gc_material_name ||
            candidate.gc_palette_groups.empty())
          continue;
        if (std::find(gc_palettes.begin(), gc_palettes.end(), candidate.gc_palette_groups) ==
            gc_palettes.end())
          gc_palettes.push_back(candidate.gc_palette_groups);
      }
    }
  }
  if (gc_palettes.empty())
    return analysis;

  auto palette_contains = [](const auto& palette,
                             const std::array<s16, 3>& groups) {
    for (const s16 group : groups)
    {
      if (group < 0 || group > 255 ||
          std::find(palette.begin(), palette.end(), static_cast<u8>(group)) == palette.end())
        return false;
    }
    return true;
  };

  std::array<bool, 256> expected_groups{};
  std::size_t expected_group_count = 0;
  for (const u8 group : draw.gc_palette_groups)
  {
    if (expected_groups[group])
      return analysis;
    expected_groups[group] = true;
    ++expected_group_count;
  }

  const auto& decoded = *draw.owner->decoded;

  // Generic DMFs are commonly split into many authored clusters per material
  // (soldier mohf_body can have 50+).  Treating the whole material as one draw
  // makes every valid body fail the old `ps3_material_clusters == 1` rule.
  //
  // Both platform files preserve a per-material cluster ordinal.  Use that
  // structural identity first, but only for non-player DMFs so the already
  // proven M1/Thompson material-palette resolver remains untouched.  The
  // candidate must also prove the exact GC palette used by this display list.
  if (!IsSupportedPlayerWeaponDMF(draw.gc_name))
  {
    struct ClusterProbe
    {
      SkinnedPaletteAnalysis analysis;
      std::array<bool, 256> used_groups{};
      std::size_t used_group_count = 0;
      bool exact_palette_identity = false;
    };

    std::vector<ClusterProbe> cluster_probes;
    for (std::size_t cluster_ordinal = 0; cluster_ordinal < decoded.clusters.size();
         ++cluster_ordinal)
    {
      const DMFCluster& cluster = decoded.clusters[cluster_ordinal];
      if (cluster.material_name != draw.gc_material_name ||
          cluster.material_cluster_index != draw.cluster_index)
        continue;

      ClusterProbe probe;
      probe.analysis.ps3_material_index = cluster.material_index;
      probe.analysis.ps3_cluster_ordinal = static_cast<u32>(cluster_ordinal);
      probe.analysis.ps3_material_clusters = 1;
      probe.analysis.matrix_slots = draw.gc_palette_groups.size();

      std::unordered_set<u16> selected_vertices;
      for (std::size_t tri = 0; tri + 2 < cluster.indices.size(); tri += 3)
      {
        ++probe.analysis.total_triangles;
        std::array<s16, 3> mapped_groups{-1, -1, -1};
        std::array<u16, 3> vertex_indices{};
        bool mapped = true;
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
          const u16 vertex = cluster.indices[tri + corner];
          vertex_indices[corner] = vertex;
          if (vertex >= cluster.vertex_palette_slots.size())
          {
            mapped = false;
            break;
          }
          const u16 local_palette_slot = cluster.vertex_palette_slots[vertex];
          if (local_palette_slot >= cluster.palette_groups.size())
          {
            mapped = false;
            break;
          }
          const u16 ps3_group = cluster.palette_groups[local_palette_slot];
          if (ps3_group >= draw.ps3_group_to_gc.size())
          {
            mapped = false;
            break;
          }
          const s16 gc_group = draw.ps3_group_to_gc[ps3_group];
          if (gc_group < 0 || gc_group > 255)
          {
            mapped = false;
            break;
          }
          mapped_groups[corner] = gc_group;
        }

        if (!mapped || !palette_contains(draw.gc_palette_groups, mapped_groups))
        {
          ++probe.analysis.unmapped_triangles;
          continue;
        }

        ++probe.analysis.selected_triangles;
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
          const u16 vertex = vertex_indices[corner];
          selected_vertices.insert(vertex);
          const u16 local_palette_slot = cluster.vertex_palette_slots[vertex];
          const u16 ps3_group = cluster.palette_groups[local_palette_slot];
          const s16 gc_group = draw.ps3_group_to_gc[ps3_group];
          if (!probe.used_groups[static_cast<u8>(gc_group)])
          {
            probe.used_groups[static_cast<u8>(gc_group)] = true;
            ++probe.used_group_count;
          }
        }
      }

      probe.analysis.selected_vertices = selected_vertices.size();
      probe.analysis.valid =
          probe.analysis.total_triangles != 0 &&
          probe.analysis.selected_triangles == probe.analysis.total_triangles &&
          probe.analysis.unmapped_triangles == 0;
      probe.exact_palette_identity =
          probe.used_group_count == expected_group_count &&
          std::equal(probe.used_groups.begin(), probe.used_groups.end(), expected_groups.begin());
      cluster_probes.push_back(std::move(probe));
    }

    ClusterProbe* cluster_winner = nullptr;
    std::size_t cluster_compatible = 0;
    for (ClusterProbe& probe : cluster_probes)
    {
      if (!probe.analysis.valid || !probe.exact_palette_identity)
        continue;
      ++cluster_compatible;
      cluster_winner = &probe;
    }

    if (cluster_compatible == 1 && cluster_winner)
    {
      analysis = cluster_winner->analysis;
      analysis.exact_cluster_identity = true;
      analysis.ps3_material_candidates = cluster_probes.size();
      analysis.compatible_ps3_materials = 1;
      return analysis;
    }

    // v12: a different GC/PS3 cluster count is normal for large character
    // materials. Solve the complete material as a one-to-one triangle flow
    // over the live GC palettes instead of comparing cluster ordinals.
    EnsureDMFPaletteFlowPartition(draw);
    const std::string flow_draw_key =
        DMFDrawPartitionKey(draw.gc_name, draw.material_index,
                            draw.cluster_index, draw.gc_material_name);
    if (const auto flow = g_dmf_palette_flow_analyses.find(flow_draw_key);
        flow != g_dmf_palette_flow_analyses.end())
    {
      return flow->second;
    }

    // Generic models may be merged/split differently by the PS3 build (morph
    // heads are a known example).  Never fall through to the weapon-era
    // material-wide resolver: doing so could reuse one PS3 cluster for several
    // distinct GC draws and duplicate geometry.  Keep the strongest exact
    // cluster probe only for diagnostics; rendering stays GC unless exactly
    // one cluster+palette identity was proven above.
    ClusterProbe* best_cluster_probe = nullptr;
    for (ClusterProbe& probe : cluster_probes)
    {
      if (!best_cluster_probe ||
          probe.analysis.selected_triangles >
              best_cluster_probe->analysis.selected_triangles ||
          (probe.analysis.selected_triangles ==
               best_cluster_probe->analysis.selected_triangles &&
           probe.analysis.unmapped_triangles <
               best_cluster_probe->analysis.unmapped_triangles))
        best_cluster_probe = &probe;
    }
    if (best_cluster_probe)
      analysis = best_cluster_probe->analysis;
    analysis.valid = false;
    analysis.ps3_material_candidates = cluster_probes.size();
    analysis.compatible_ps3_materials = cluster_compatible;
    if (cluster_compatible > 1)
      ++analysis.ambiguous_triangles;
    return analysis;
  }

  // The older material-wide resolver is intentionally retained only for the
  // already-proven first-person M1/Thompson bridge.
  std::vector<u32> ps3_material_indices;
  for (const DMFCluster& cluster : decoded.clusters)
  {
    if (cluster.material_name != draw.gc_material_name)
      continue;
    if (std::find(ps3_material_indices.begin(), ps3_material_indices.end(),
                  cluster.material_index) == ps3_material_indices.end())
      ps3_material_indices.push_back(cluster.material_index);
  }
  std::sort(ps3_material_indices.begin(), ps3_material_indices.end());
  if (ps3_material_indices.empty())
    return analysis;

  struct MaterialProbe
  {
    SkinnedPaletteAnalysis analysis;
    std::array<bool, 256> used_groups{};
    std::size_t used_group_count = 0;
    bool exact_palette_identity = false;
  };

  const auto probe_material = [&](u32 ps3_material_index) {
    MaterialProbe probe;
    probe.analysis.ps3_material_index = ps3_material_index;
    probe.analysis.ps3_material_candidates = ps3_material_indices.size();

    std::unordered_set<u64> selected_vertices;
    for (std::size_t cluster_ordinal = 0; cluster_ordinal < decoded.clusters.size();
         ++cluster_ordinal)
    {
      const DMFCluster& cluster = decoded.clusters[cluster_ordinal];
      if (cluster.material_index != ps3_material_index ||
          cluster.material_name != draw.gc_material_name)
        continue;
      if (probe.analysis.ps3_material_clusters == 0)
        probe.analysis.ps3_cluster_ordinal = static_cast<u32>(cluster_ordinal);
      else
        probe.analysis.ps3_cluster_ordinal = 0xffffffffu;
      ++probe.analysis.ps3_material_clusters;

      for (std::size_t tri = 0; tri + 2 < cluster.indices.size(); tri += 3)
      {
        ++probe.analysis.total_triangles;
        std::array<s16, 3> mapped_groups{-1, -1, -1};
        std::array<u16, 3> vertex_indices{};
        bool mapped = true;
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
          const u16 vertex = cluster.indices[tri + corner];
          vertex_indices[corner] = vertex;
          if (vertex >= cluster.vertex_palette_slots.size())
          {
            mapped = false;
            break;
          }
          const u16 local_palette_slot = cluster.vertex_palette_slots[vertex];
          if (local_palette_slot >= cluster.palette_groups.size())
          {
            mapped = false;
            break;
          }
          const u16 ps3_group = cluster.palette_groups[local_palette_slot];
          if (ps3_group >= draw.ps3_group_to_gc.size())
          {
            mapped = false;
            break;
          }
          const s16 gc_group = draw.ps3_group_to_gc[ps3_group];
          if (gc_group < 0 || gc_group > 255)
          {
            mapped = false;
            break;
          }
          mapped_groups[corner] = gc_group;
        }
        if (!mapped)
        {
          ++probe.analysis.unmapped_triangles;
          continue;
        }

        std::size_t matching_palettes = 0;
        const std::vector<u8>* unique_palette = nullptr;
        for (const auto& palette : gc_palettes)
        {
          if (!palette_contains(palette, mapped_groups))
            continue;
          ++matching_palettes;
          unique_palette = &palette;
        }

        if (matching_palettes == 0)
        {
          ++probe.analysis.unmapped_triangles;
          continue;
        }
        if (matching_palettes != 1)
        {
          ++probe.analysis.ambiguous_triangles;
          continue;
        }
        if (!std::ranges::equal(*unique_palette, draw.gc_palette_groups))
          continue;

        ++probe.analysis.selected_triangles;
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
          const u16 vertex = vertex_indices[corner];
          selected_vertices.insert((u64(cluster_ordinal) << 32) | u64(vertex));
          const u16 local_palette_slot = cluster.vertex_palette_slots[vertex];
          const u16 ps3_group = cluster.palette_groups[local_palette_slot];
          const s16 gc_group = draw.ps3_group_to_gc[ps3_group];
          if (!probe.used_groups[static_cast<u8>(gc_group)])
          {
            probe.used_groups[static_cast<u8>(gc_group)] = true;
            ++probe.used_group_count;
          }
        }
      }
    }

    probe.analysis.selected_vertices = selected_vertices.size();
    probe.analysis.matrix_slots = draw.gc_palette_groups.size();
    probe.analysis.valid =
        probe.analysis.ps3_material_clusters != 0 && probe.analysis.selected_triangles != 0;

    probe.exact_palette_identity =
        probe.used_group_count == expected_group_count &&
        std::equal(probe.used_groups.begin(), probe.used_groups.end(), expected_groups.begin());
    return probe;
  };

  std::vector<MaterialProbe> probes;
  probes.reserve(ps3_material_indices.size());
  for (const u32 ps3_material_index : ps3_material_indices)
    probes.push_back(probe_material(ps3_material_index));

  const MaterialProbe* winner = nullptr;
  std::size_t compatible = 0;
  for (const MaterialProbe& probe : probes)
  {
    const auto& a = probe.analysis;
    const bool fully_compatible =
        probe.exact_palette_identity && a.valid && a.ps3_material_clusters == 1 &&
        a.total_triangles != 0 && a.selected_triangles == a.total_triangles &&
        a.ambiguous_triangles == 0 && a.unmapped_triangles == 0;
    if (!fully_compatible)
      continue;
    ++compatible;
    winner = &probe;
  }

  if (compatible == 1 && winner)
  {
    analysis = winner->analysis;
    analysis.compatible_ps3_materials = 1;
    return analysis;
  }

  // Keep the strongest probe for diagnostics, but never make an uncertain
  // material-slot mapping renderable.  The strict path below requires exactly
  // one compatible PS3 material.
  const MaterialProbe* best = nullptr;
  for (const MaterialProbe& probe : probes)
  {
    if (!best ||
        probe.analysis.selected_triangles > best->analysis.selected_triangles ||
        (probe.analysis.selected_triangles == best->analysis.selected_triangles &&
         probe.analysis.unmapped_triangles + probe.analysis.ambiguous_triangles <
             best->analysis.unmapped_triangles + best->analysis.ambiguous_triangles))
      best = &probe;
  }
  if (best)
    analysis = best->analysis;
  analysis.valid = false;
  analysis.ps3_material_candidates = ps3_material_indices.size();
  analysis.compatible_ps3_materials = compatible;
  if (compatible > 1)
    ++analysis.ambiguous_triangles;
  return analysis;
}
void PrepareDMFDraws()
{
  std::scoped_lock lock(g_dmf_cache_mutex);
  // CPU level-load phase only. Prepared objects own every view used by the GPU.
  std::unordered_map<std::string, std::size_t> material_draw_counts;
  for (const auto& [signature, candidates] : g_dmf_display_list_candidates)
  {
    (void)signature;
    for (const auto& candidate : candidates)
    {
      const std::string key = candidate.gc_name + "\n" +
                              std::to_string(candidate.material_index) + "\n" +
                              candidate.gc_material_name;
      ++material_draw_counts[key];
    }
  }

  std::size_t draws = 0;
  for (auto& [signature, candidates] : g_dmf_display_list_candidates)
  {
    (void)signature;
    for (auto& candidate : candidates)
    {
      const auto pair = g_dmf_pairs.find(candidate.gc_name);
      if (pair == g_dmf_pairs.end())
        continue;
      auto prepared = std::make_shared<PreparedDMFDraw>();
      prepared->gc_name = candidate.gc_name;
      prepared->material_name = candidate.gc_material_name;
      prepared->skeleton_name = pair->second.skeleton_name;
      prepared->palette = candidate.gc_palette_groups;
      prepared->group_map = pair->second.ps3_group_to_gc;
      SkinnedDrawMatch draw;
      draw.owner = candidate.ps3;
      draw.gc_name = prepared->gc_name;
      draw.material_index = candidate.material_index;
      draw.cluster_index = candidate.cluster_index;
      draw.gc_material_name = prepared->material_name;
      draw.gc_palette_groups = prepared->palette;
      draw.ps3_group_to_gc = prepared->group_map;
      prepared->analysis = candidate.prepared ? candidate.prepared->analysis :
                                               AnalyzeSkinnedPaletteAtLoad(draw);
      prepared->readiness.geometry_valid = candidate.ps3 && candidate.ps3->decoded;
      prepared->readiness.palettes_valid = prepared->analysis.valid &&
          !prepared->analysis.ambiguous_triangles && !prepared->analysis.unmapped_triangles;
      const bool direct_bind = candidate.ps3 && candidate.ps3->decoded &&
          candidate.ps3->decoded->bind_tables_valid &&
          candidate.ps3->decoded->inverse_bind_by_ref.size() ==
              candidate.ps3->decoded->bone_refs.size();
      // Runtime animation remains the GC/XF palette.  A PS3 SKL is useful for
      // validation/diagnostics but is not required to undo PS3 model-bind space
      // because the authoritative inverse-bind table is embedded in the DMF.
      prepared->readiness.skeleton_valid =
          direct_bind || (pair->second.bind && pair->second.bind->valid);
      prepared->readiness.bind_valid = direct_bind || pair->second.bind_vertices_valid;

      const std::string material_key = candidate.gc_name + "\n" +
                                       std::to_string(candidate.material_index) + "\n" +
                                       candidate.gc_material_name;
      const auto count_it = material_draw_counts.find(material_key);
      const std::size_t gc_draws =
          count_it == material_draw_counts.end() ? 0 : count_it->second;
      prepared->gc_material_draws = gc_draws;
      // A material can contain dozens of authored clusters.  Once the exact
      // per-material cluster ordinal AND its complete GC palette are proven,
      // sibling draw count is no longer an ambiguity.  Keep the old one-draw
      // rule as the fallback for M1/Thompson and files whose partition differs.
      prepared->readiness.materials_valid =
          prepared->analysis.exact_cluster_identity ||
          prepared->analysis.exact_triangle_partition ||
          (gc_draws == 1 && prepared->analysis.compatible_ps3_materials == 1 &&
           prepared->analysis.ps3_material_clusters == 1);
      prepared->readiness.all_required_parts_mapped =
          prepared->analysis.valid && prepared->analysis.total_triangles != 0 &&
          prepared->analysis.selected_triangles == prepared->analysis.total_triangles &&
          prepared->analysis.ambiguous_triangles == 0 &&
          prepared->analysis.unmapped_triangles == 0;
      candidate.prepared = std::move(prepared);
      ++draws;
    }
  }
  if (PS3RuntimeDebugEnabled())
    std::fprintf(stderr, "[moh-ps3-dmf] PRECOMPUTED: draws=%zu; bind validation pending, GC preserved\n", draws);
}
}  // namespace

SkinnedPaletteAnalysis AnalyzeCurrentSkinnedPalette()
{
  return g_current_dmf_draw.prepared ? g_current_dmf_draw.prepared->analysis :
                                      SkinnedPaletteAnalysis{};
}

SkinnedDrawReplacement BuildCurrentSkinnedReplacement()
{
  // First-person weapon bridge.  The existing VertexManager path still does
  // the final declaration/XF/finite-matrix checks before replacing the GC
  // draw, and every material still needs the strict one-cluster / one-GC-DL /
  // full-palette proof below.  M1 is deliberately enabled here now that its
  // 0x0502 skin-group coefficients no longer make the decoder reject the file.
  static const bool replace = EnvSwitchLocal("MOH_PS3_DMF_REPLACE", true);
  static const bool generic_exact = EnvSwitchLocal("MOH_PS3_DMF_GENERIC_EXACT", true);
  static const bool blended_groups = EnvSwitchLocal("MOH_PS3_DMF_BLEND_GROUPS", true);
  const auto& draw = g_current_dmf_draw;
  if (!replace || !draw.prepared || !draw.owner || !draw.owner->decoded ||
      IsHardDisabledPS3DMFResource(draw.gc_name) ||
      IsHardDisabledPS3DMFMaterial(draw.gc_material_name) ||
      (!generic_exact && !IsSupportedPlayerWeaponDMF(draw.gc_name)))
    return {};


  const auto& ready = draw.prepared->readiness;
  const auto& analysis = draw.prepared->analysis;

  const std::size_t gc_material_draws = draw.prepared->gc_material_draws;

  const DMFCluster* selected_cluster = nullptr;
  std::size_t ps3_material_cluster_count = 0;

  if (analysis.exact_triangle_partition)
  {
    const std::string partition_key =
        DMFDrawPartitionKey(draw.gc_name, draw.material_index,
                            draw.cluster_index, draw.gc_material_name);

    if (const auto partition = g_dmf_palette_flow_clusters.find(partition_key);
        partition != g_dmf_palette_flow_clusters.end() && partition->second)
    {
      selected_cluster = partition->second.get();
      ps3_material_cluster_count = 1;
    }
  }
  else if (analysis.exact_cluster_identity &&
      analysis.ps3_cluster_ordinal < draw.owner->decoded->clusters.size())
  {
    const DMFCluster& cluster =
        draw.owner->decoded->clusters[analysis.ps3_cluster_ordinal];
    if (cluster.material_index == analysis.ps3_material_index &&
        cluster.material_cluster_index == draw.cluster_index &&
        cluster.material_name == draw.gc_material_name)
    {
      selected_cluster = &cluster;
      ps3_material_cluster_count = 1;
    }
  }
  else if (analysis.ps3_material_index != 0xffffffffu)
  {
    for (const auto& cluster : draw.owner->decoded->clusters)
    {
      if (cluster.material_index != analysis.ps3_material_index ||
          cluster.material_name != draw.gc_material_name)
        continue;
      ++ps3_material_cluster_count;
      if (!selected_cluster)
        selected_cluster = &cluster;
    }
  }

  // Diagnostic only: this block does not relax a single strict replacement
  // condition.  It reports the complete proof state once per weapon material
  // so M1/Thompson atlas failures can be fixed from one runtime capture.
  static std::unordered_set<std::string>
      strict_diag_logged;

  const bool replacement_debug =
      PS3RuntimeDebugEnabled();

  const std::string strict_diag_key =
      replacement_debug ?
          std::string(draw.gc_name) + "|" +
              std::to_string(draw.material_index) + "|" +
              std::string(draw.gc_material_name) :
          std::string{};

  if (replacement_debug &&
      strict_diag_logged.insert(
          strict_diag_key).second)
  {
    const std::size_t cluster_positions = selected_cluster ? selected_cluster->positions.size() : 0;
    const std::size_t cluster_normals = selected_cluster ? selected_cluster->normals.size() : 0;
    const std::size_t cluster_uv0 = selected_cluster ? selected_cluster->uv0.size() : 0;
    const std::size_t cluster_slots =
        selected_cluster ? selected_cluster->vertex_palette_slots.size() : 0;
    const std::size_t cluster_indices = selected_cluster ? selected_cluster->indices.size() : 0;
    const std::size_t cluster_palette = selected_cluster ? selected_cluster->palette_groups.size() : 0;
    std::fprintf(
        stderr,
        "[moh-ps3-dmf] STRICT DIAG: gc=%.*s gc_material_index=%u gc_cluster=%u "
        "ps3_material_index=%u ps3_cluster=%u exact_cluster=%d "
        "material=%.*s ready[g=%d bind=%d skl=%d mat=%d parts=%d] "
        "analysis[valid=%d candidates=%zu compatible=%zu clusters=%zu tris=%zu selected=%zu "
        "ambiguous=%zu unmapped=%zu vertices=%zu matrix_slots=%zu] "
        "gc_draws=%zu ps3_clusters=%zu "
        "attrs[pos=%d nrm=%d uv0=%d] sizes[pos=%zu nrm=%zu uv0=%zu slots=%zu idx=%zu pal=%zu]\n",
        static_cast<int>(draw.gc_name.size()), draw.gc_name.data(),
        draw.material_index, draw.cluster_index, analysis.ps3_material_index,
        analysis.ps3_cluster_ordinal, analysis.exact_cluster_identity ? 1 : 0,
        static_cast<int>(draw.gc_material_name.size()), draw.gc_material_name.data(),
        ready.geometry_valid ? 1 : 0, ready.bind_valid ? 1 : 0, ready.skeleton_valid ? 1 : 0,
        ready.materials_valid ? 1 : 0, ready.all_required_parts_mapped ? 1 : 0,
        analysis.valid ? 1 : 0, analysis.ps3_material_candidates,
        analysis.compatible_ps3_materials, analysis.ps3_material_clusters,
        analysis.total_triangles, analysis.selected_triangles, analysis.ambiguous_triangles,
        analysis.unmapped_triangles, analysis.selected_vertices, analysis.matrix_slots,
        gc_material_draws,
        ps3_material_cluster_count, selected_cluster && selected_cluster->has_position ? 1 : 0,
        selected_cluster && selected_cluster->has_normal ? 1 : 0,
        selected_cluster && selected_cluster->has_uv0 ? 1 : 0, cluster_positions,
        cluster_normals, cluster_uv0, cluster_slots, cluster_indices, cluster_palette);
  }

  if (!ready.geometry_valid || !ready.bind_valid || !ready.skeleton_valid ||
      !ready.materials_valid || !ready.all_required_parts_mapped)
    return {};

  if (!analysis.valid || analysis.ps3_material_index == 0xffffffffu ||
      analysis.compatible_ps3_materials != 1 || analysis.total_triangles == 0 ||
      analysis.selected_triangles != analysis.total_triangles ||
      analysis.ambiguous_triangles != 0 || analysis.unmapped_triangles != 0)
    return {};

  if (!analysis.exact_cluster_identity &&
      !analysis.exact_triangle_partition &&
      analysis.ps3_material_clusters != 1)
    return {};

  // Multi-draw materials are valid when either v11 proved an exact individual
  // cluster identity OR v12 proved a complete material-wide palette flow.
  if (!analysis.exact_cluster_identity &&
      !analysis.exact_triangle_partition &&
      gc_material_draws != 1)
    return {};

  if (ps3_material_cluster_count != 1)
    return {};

  if (!selected_cluster || !selected_cluster->has_position ||
      !selected_cluster->has_normal || !selected_cluster->has_uv0 ||
      selected_cluster->positions.empty() ||
      selected_cluster->positions.size() != selected_cluster->normals.size() ||
      selected_cluster->positions.size() != selected_cluster->uv0.size() ||
      selected_cluster->positions.size() != selected_cluster->vertex_palette_slots.size())
    return {};

  static std::unordered_set<std::string>
      matrix_reject_logged;

  const auto matrix_reject =
      [&](std::string_view reason,
          std::size_t vertex,
          u32 local_slot,
          u32 ps3_group,
          s32 gc_group)
          -> SkinnedDrawReplacement
      {
        if (replacement_debug)
        {
          const std::string key =
              strict_diag_key + "|" +
              std::string(reason);

          if (matrix_reject_logged.insert(key).second)
          {
            std::fprintf(
                stderr,
                "[moh-ps3-dmf] MATRIX REJECT: gc=%.*s gc_material_index=%u "
                "ps3_material_index=%u material=%.*s reason=%.*s "
                "vertex=%zu local_slot=%u ps3_group=%u gc_group=%d "
                "gc_palette=%zu ps3_palette=%zu group_map=%zu\n",
                static_cast<int>(draw.gc_name.size()),
                draw.gc_name.data(),
                draw.material_index,
                analysis.ps3_material_index,
                static_cast<int>(draw.gc_material_name.size()),
                draw.gc_material_name.data(),
                static_cast<int>(reason.size()),
                reason.data(),
                vertex,
                local_slot,
                ps3_group,
                gc_group,
                draw.gc_palette_groups.size(),
                selected_cluster->palette_groups.size(),
                draw.ps3_group_to_gc.size());
          }
        }

        return {};
      };

  // PS3 positions are model/bind-space. Use the inverse bind authored in THIS
  // DMF by ref index; do not require a sibling SKL and never estimate offsets.
  const auto& decoded = *draw.owner->decoded;

  // v12.9.1:
  // mohf_body remains permanently disabled, but generic validated DMFs
  // still need the lightweight paired-GC skin-group metadata in order
  // to recover the authored GC bone pair/q associated with the live XF.
  //
  // This does NOT restore the experimental GC inverse-bind body path.
  const ExactDMFPair* exact_pair = nullptr;

  if (const auto pair_it =
          g_dmf_pairs.find(std::string(draw.gc_name));
      pair_it != g_dmf_pairs.end())
  {
    exact_pair = &pair_it->second;
  }

  if (!decoded.bind_tables_valid ||
      decoded.inverse_bind_by_ref.size() != decoded.bone_refs.size())
    return matrix_reject("dmf-bind-table-unavailable", 0, 0xffffu, 0xffffu, -1);
  const auto& inverse_bind_by_ref = decoded.inverse_bind_by_ref;

  const auto invert_affine = [](const std::array<float, 12>& input,
                                std::array<float, 12>* output) {
    if (!output)
      return false;

    const double a00 = input[0], a01 = input[1], a02 = input[2];
    const double a10 = input[4], a11 = input[5], a12 = input[6];
    const double a20 = input[8], a21 = input[9], a22 = input[10];
    const double determinant =
        a00 * (a11 * a22 - a12 * a21) -
        a01 * (a10 * a22 - a12 * a20) +
        a02 * (a10 * a21 - a11 * a20);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-10)
      return false;

    const double inv_det = 1.0 / determinant;
    std::array<double, 9> inverse_linear{
        (a11 * a22 - a12 * a21) * inv_det,
        (a02 * a21 - a01 * a22) * inv_det,
        (a01 * a12 - a02 * a11) * inv_det,
        (a12 * a20 - a10 * a22) * inv_det,
        (a00 * a22 - a02 * a20) * inv_det,
        (a02 * a10 - a00 * a12) * inv_det,
        (a10 * a21 - a11 * a20) * inv_det,
        (a01 * a20 - a00 * a21) * inv_det,
        (a00 * a11 - a01 * a10) * inv_det};

    const std::array<double, 3> translation{input[3], input[7], input[11]};
    for (std::size_t row = 0; row < 3; ++row)
    {
      for (std::size_t column = 0; column < 3; ++column)
      {
        const double value = inverse_linear[row * 3 + column];
        if (!std::isfinite(value) || std::abs(value) > 1000000.0)
          return false;
        (*output)[row * 4 + column] = static_cast<float>(value);
      }
      const double translated =
          -(inverse_linear[row * 3 + 0] * translation[0] +
            inverse_linear[row * 3 + 1] * translation[1] +
            inverse_linear[row * 3 + 2] * translation[2]);
      if (!std::isfinite(translated) || std::abs(translated) > 1000000.0)
        return false;
      (*output)[row * 4 + 3] = static_cast<float>(translated);
    }
    return true;
  };

  const auto load_inverse_bind = [&](u32 ref, std::array<float, 12>* output) {
    if (!output || ref >= inverse_bind_by_ref.size())
      return false;
    const auto& source = inverse_bind_by_ref[ref];
    for (std::size_t row = 0; row < 3; ++row)
      for (std::size_t column = 0; column < 4; ++column)
      {
        const float value = source[row * 4 + column];
        if (!std::isfinite(value) || std::abs(value) > 1000000.0f)
          return false;
        (*output)[row * 4 + column] = value;
      }
    return true;
  };

  const auto build_group_transform =
      [&](u16 ps3_group, s16 gc_group,
          std::array<float, 12>* position_transform,
          std::array<float, 9>* normal_transform,
          bool* blended,
          std::string_view* reason)
      {
        if (!position_transform ||
            !normal_transform ||
            !blended ||
            !reason)
          return false;

        *blended = false;
        *reason = "unknown-bind-transform";

        if (ps3_group >=
            decoded.skin_groups.size())
        {
          *reason =
              "ps3-skin-group-out-of-range";
          return false;
        }

        const auto& ps3 =
            decoded.skin_groups[ps3_group];

        u32 bone_a = ps3.bone_a;
        u32 bone_b = ps3.bone_b;

        float blend_q =
            ps3.blend * 4096.0f;

        bool gc_bind = false;

        if (!IsSupportedPlayerWeaponDMF(
                draw.gc_name) &&
            exact_pair &&
            exact_pair->gc_bone_order_proven &&
            gc_group >= 0 &&
            static_cast<std::size_t>(gc_group) <
                exact_pair->gc_skin_groups.size())
        {
          const auto& gc =
              exact_pair->gc_skin_groups[
                  static_cast<std::size_t>(
                      gc_group)];

          bone_a = gc.bone_a;
          bone_b = gc.bone_b;
          blend_q =
              static_cast<float>(
                  gc.blend_q);

          gc_bind = true;
        }

        if (!std::isfinite(blend_q))
        {
          *reason =
              "nonfinite-bind-weight";
          return false;
        }

        int rigid_ref = -1;

        if (std::abs(
                blend_q - 4096.0f) <=
            0.5f)
          rigid_ref =
              static_cast<int>(bone_a);

        else if (std::abs(blend_q) <=
                 0.5f)
          rigid_ref =
              static_cast<int>(bone_b);

        else if (bone_a == bone_b)
          rigid_ref =
              static_cast<int>(bone_a);

        if (rigid_ref >= 0)
        {
          if (!load_inverse_bind(
                  static_cast<u32>(
                      rigid_ref),
                  position_transform))
          {
            *reason =
                "invalid-rigid-inverse-bind";
            return false;
          }

          for (std::size_t row = 0;
               row < 3; ++row)
            for (std::size_t column = 0;
                 column < 3;
                 ++column)
              (*normal_transform)
                  [row * 3 + column] =
                  (*position_transform)
                      [row * 4 + column];

          return true;
        }

        if (!blended_groups)
        {
          *reason =
              "blended-bind-disabled";
          return false;
        }

        if (blend_q < -0.5f ||
            blend_q > 4096.5f ||
            bone_a >=
                inverse_bind_by_ref.size() ||
            bone_b >=
                inverse_bind_by_ref.size())
        {
          *reason =
              "unsupported-blended-bind-weight";
          return false;
        }

        std::array<float, 12>
            inverse_a{},
            inverse_b{},
            world_a{},
            world_b{};

        if (!load_inverse_bind(
                bone_a, &inverse_a) ||
            !load_inverse_bind(
                bone_b, &inverse_b) ||
            !invert_affine(
                inverse_a, &world_a) ||
            !invert_affine(
                inverse_b, &world_b))
        {
          *reason =
              "blend-bone-bind-noninvertible";
          return false;
        }

        const float weight_a =
            std::clamp(
                blend_q / 4096.0f,
                0.0f, 1.0f);

        const float weight_b =
            1.0f - weight_a;

        std::array<float, 12>
            group_world{};

        for (std::size_t i = 0;
             i < group_world.size();
             ++i)
        {
          group_world[i] =
              world_a[i] * weight_a +
              world_b[i] * weight_b;

          if (!std::isfinite(
                  group_world[i]))
          {
            *reason =
                "nonfinite-blended-bind";
            return false;
          }
        }

        if (!invert_affine(
                group_world,
                position_transform))
        {
          *reason =
              gc_bind ?
              "gc-group-bind-noninvertible" :
              "blended-group-bind-noninvertible";

          return false;
        }

        for (std::size_t row = 0;
             row < 3; ++row)
          for (std::size_t column = 0;
               column < 3;
               ++column)
          {
            const float value =
                group_world[
                    column * 4 + row];

            if (!std::isfinite(value))
            {
              *reason =
                  "nonfinite-blended-normal-bind";
              return false;
            }

            (*normal_transform)
                [row * 3 + column] =
                value;
          }

        *blended = true;
        return true;
      };

  // A palette slot identifies the same authored group throughout this draw.
  // Validate each distinct slot once, including conflicts with other slots.
  std::vector<int> validated_matrix_ids(selected_cluster->palette_groups.size(), -1);
  std::vector<u8> matrix_indices(selected_cluster->positions.size());
  std::vector<std::array<float, 12>> model_to_gc_local(draw.gc_palette_groups.size());
  std::vector<std::array<float, 9>> model_to_gc_local_normal(draw.gc_palette_groups.size());
  std::vector<bool> local_transform_ready(draw.gc_palette_groups.size(), false);
  bool blended_group_used = false;

  for (std::size_t vertex = 0; vertex < selected_cluster->positions.size(); ++vertex)
  {
    const u16 local_slot = selected_cluster->vertex_palette_slots[vertex];
    if (local_slot >= selected_cluster->palette_groups.size())
      return matrix_reject("local-slot-out-of-range", vertex, local_slot, 0xffffu, -1);
    if (validated_matrix_ids[local_slot] >= 0)
    {
      matrix_indices[vertex] = static_cast<u8>(validated_matrix_ids[local_slot]);
      continue;
    }
    const u16 ps3_group = selected_cluster->palette_groups[local_slot];
    if (ps3_group >= draw.ps3_group_to_gc.size())
      return matrix_reject("ps3-group-out-of-range", vertex, local_slot, ps3_group, -1);
    const s16 gc_group = draw.ps3_group_to_gc[ps3_group];
    if (gc_group < 0 || gc_group > 255)
      return matrix_reject("ps3-group-unmapped", vertex, local_slot, ps3_group, gc_group);

    const auto first = std::find(draw.gc_palette_groups.begin(), draw.gc_palette_groups.end(),
                                 static_cast<u8>(gc_group));
    if (first == draw.gc_palette_groups.end())
      return matrix_reject("gc-group-not-in-draw-palette", vertex, local_slot, ps3_group,
                           gc_group);
    if (std::find(first + 1, draw.gc_palette_groups.end(), static_cast<u8>(gc_group)) !=
        draw.gc_palette_groups.end())
      return matrix_reject("duplicate-gc-group", vertex, local_slot, ps3_group, gc_group);

    const std::size_t matrix_slot =
        static_cast<std::size_t>(std::distance(draw.gc_palette_groups.begin(), first));
    const std::size_t matrix_id = matrix_slot * 3u;
    if (matrix_id > 255u)
      return matrix_reject("matrix-id-overflow", vertex, local_slot, ps3_group, gc_group);
    matrix_indices[vertex] = static_cast<u8>(matrix_id);

    std::array<float, 12> position_transform{};
    std::array<float, 9> normal_transform{};
    bool blended = false;
    std::string_view transform_reason;
    if (!build_group_transform(
            ps3_group, gc_group,
            &position_transform,
            &normal_transform,
            &blended,
            &transform_reason))
      return matrix_reject(transform_reason, vertex, local_slot, ps3_group, gc_group);

    blended_group_used = blended_group_used || blended;
    if (!local_transform_ready[matrix_slot])
    {
      model_to_gc_local[matrix_slot] = position_transform;
      model_to_gc_local_normal[matrix_slot] = normal_transform;
      local_transform_ready[matrix_slot] = true;
    }
    else
    {
      // One live GX matrix slot represents one authored skin group. Different
      // PS3 refs may collapse to it only when their complete bind transforms
      // are numerically identical.
      for (std::size_t element = 0; element < position_transform.size(); ++element)
      {
        if (std::abs(model_to_gc_local[matrix_slot][element] -
                     position_transform[element]) > 1.0e-5f)
          return matrix_reject("bind-space-conflict", vertex, local_slot, ps3_group, gc_group);
      }
      for (std::size_t element = 0; element < normal_transform.size(); ++element)
      {
        if (std::abs(model_to_gc_local_normal[matrix_slot][element] -
                     normal_transform[element]) > 1.0e-5f)
          return matrix_reject("normal-bind-space-conflict", vertex, local_slot, ps3_group,
                               gc_group);
      }
    }
    validated_matrix_ids[local_slot] = static_cast<int>(matrix_id);
  }

  if (replacement_debug)
  {
  static std::unordered_set<std::string> logged_ready_draws;
  const std::string log_key =
      std::string(draw.gc_name) + "|" + std::to_string(draw.material_index) + "|" +
      std::to_string(draw.cluster_index) + "|" + std::string(draw.gc_material_name);
  if (logged_ready_draws.insert(log_key).second)
  {
    std::fprintf(stderr,
                 "[moh-ps3-dmf] STRICT replacement READY: "
                 "gc=%.*s gc_material_index=%u gc_cluster=%u "
                 "ps3_material_index=%u ps3_cluster=%u material=%s "
                 "verts=%zu tris=%zu palette=%zu groups=%zu/%zu "
                 "identity=%s\n",
                 static_cast<int>(draw.gc_name.size()), draw.gc_name.data(),
                 draw.material_index, draw.cluster_index, analysis.ps3_material_index,
                 analysis.ps3_cluster_ordinal, draw.gc_material_name.data(),
                 selected_cluster->positions.size(),
                 selected_cluster->indices.size() / 3, draw.gc_palette_groups.size(),
                 std::count_if(draw.ps3_group_to_gc.begin(), draw.ps3_group_to_gc.end(),
                               [](s16 value) { return value >= 0; }),
                 draw.ps3_group_to_gc.size(),
                 analysis.exact_triangle_partition ?
                     "material+palette-flow+triangle-quota" :
                     (analysis.exact_cluster_identity ?
                          "material+cluster+palette" :
                          "material+palette"));
  }

  static std::unordered_set<std::string> bind_local_logged;
  if (bind_local_logged.insert(log_key).second)
  {
    const std::size_t ready_slots = static_cast<std::size_t>(
        std::count(local_transform_ready.begin(), local_transform_ready.end(), true));
    std::fprintf(stderr,
                 "[moh-ps3-skin] BIND-LOCAL READY: gc=%.*s gc_material_index=%u "
                 "gc_cluster=%u ps3_material_index=%u ps3_cluster=%u material=%s "
                 "slots=%zu/%zu blended=%d | PS3 model-bind -> GC skin-group-local\n",
                 static_cast<int>(draw.gc_name.size()), draw.gc_name.data(), draw.material_index,
                 draw.cluster_index, analysis.ps3_material_index, analysis.ps3_cluster_ordinal,
                 draw.gc_material_name.data(), ready_slots, model_to_gc_local.size(),
                 blended_group_used ? 1 : 0);
    if (blended_group_used)
      std::fprintf(stderr,
                   "[moh-ps3-skin] BLEND-GROUP LOCAL READY: gc=%.*s material=%s "
                   "cluster=%u | inverse(blended bind) positions + transpose(bind) normals\n",
                   static_cast<int>(draw.gc_name.size()), draw.gc_name.data(),
                   draw.gc_material_name.data(), draw.cluster_index);
  }

  }

  SkinnedDrawReplacement replacement;
  replacement.owner = draw.owner;
  replacement.cluster = selected_cluster;
  replacement.position_matrix_indices = std::move(matrix_indices);
  replacement.model_to_gc_local = std::move(model_to_gc_local);
  replacement.model_to_gc_local_normal = std::move(model_to_gc_local_normal);
  replacement.gc_material_draws = gc_material_draws;
  return replacement;
}


StaticDrawMatch AcquireFullCPTLevelDraw(u32 gc_triangle_count)
{
  if (!IsFullCPTLevelRenderEnabled() || g_current_dmf_draw || g_active_display_list)
    return {};

  static const u32 minimum_triangles = static_cast<u32>(
      EnvFloatLocal("MOH_PS3_CPT_FULL_LEVEL_TRIGGER_TRIS", 96.0f, 1.0f, 100000.0f));
  if (gc_triangle_count < minimum_triangles)
    return {};

  if (g_current_draw_transient_world)
    RejectStaticDrawCandidate();
  if (g_current_draw)
    return {};

  std::shared_ptr<StaticMesh> owner;
  std::shared_ptr<std::vector<std::array<float, 3>>> normals;
  {
    std::scoped_lock lock(g_msh_cache_mutex);
    owner = g_world_full_level_mesh;
    normals = g_world_full_level_normals;
  }
  if (!owner || !normals || owner->submeshes.size() != 1)
    return {};

  const auto& sub = owner->submeshes[0];
  if (!sub.vertex_count || sub.position_uv.size() != sub.vertex_count ||
      sub.indices.empty() || normals->size() != sub.vertex_count)
    return {};

  const Bounds3 bounds = BoundsFromPS3(sub);
  StaticDrawMatch match;
  match.owner = owner;
  match.normals = normals;
  match.mesh = owner.get();
  match.submesh = &sub;
  match.submesh_index = 0;
  match.score = 0.0f;
  match.bounds_valid = bounds.valid;
  match.bounds_min = bounds.minimum;
  match.bounds_max = bounds.maximum;
  match.world_translation_valid = false;
  match.guest_resource = 0;
  match.display_list = 0;

  g_current_draw = match;
  g_current_draw_transient_world = true;
  g_current_world_direct_key = 0;
  ++g_matches;

  static unsigned logs = 0;
  if (logs++ < 8)
  {
    std::fprintf(stderr,
                 "[moh-ps3-world-full] ACQUIRE: trigger-gc-tris=%u vertices=%u triangles=%zu indices=%zu | replacing one direct GC world batch with complete PS3 CPT level\n",
                 gc_triangle_count, sub.vertex_count, sub.indices.size() / 3,
                 sub.indices.size());
  }
  return match;
}

StaticDrawMatch MatchStaticDraw(std::span<const u8> gc_vertices, u32 count, u32 stride, u32 offset,
                                u32 gc_triangle_count)
{
  if (g_current_dmf_draw || !IsStaticDrawReplacementEnabled() || count < 3)
    return {};

  // A direct CPT match has no OpcodeDecoding display-list boundary. If the
  // previous candidate failed a later stream validation, Notify... was never
  // called, so expire it here before processing the next batch.
  if (g_current_draw_transient_world)
    RejectStaticDrawCandidate();

  if (g_current_draw)
  {
    // Thompson is authored as 15 parts, but only a subset of those display
    // lists is observed by the current runtime replacement path.  Replacing
    // the observed parts produced a PS3/GC hybrid with mismatched material
    // order.  Keep the complete static Thompson on the GameCube path until
    // all sibling DLs can be proven at runtime.
    const std::string_view source =
        g_current_draw.owner ? std::string_view(g_current_draw.owner->source_name) :
                               std::string_view{};
    if (source.find("thompson") != std::string_view::npos ||
        source.find("Thompson") != std::string_view::npos)
    {
      static bool thompson_static_logged = false;
      if (!thompson_static_logged)
      {
        thompson_static_logged = true;
        std::fprintf(stderr,
                     "[moh-ps3-msh] Thompson static replacement SAFE-FALLBACK: "
                     "incomplete runtime DL coverage; keeping complete GC MSH\n");
      }
      return {};
    }

    // Exact runtime address + display-list size + command-tail hash identifies
    // the authored GameCube MSH command stream. A per-batch bounds comparison
    // is invalid here because a GX batch is not the whole PS3 submesh.
    return g_current_draw;
  }

  // MOH_PS3_CPT_SMALL_DIRECT_GUARD_V1
  //
  // Immediate-mode effects (particle smoke, muzzle flashes, sparks, etc.) are
  // commonly submitted as tiny direct GX batches with no GXCallDisplayList.
  // They are not authored CPT world geometry, but without this guard every
  // quad enters the expensive strict world-geometry matcher below and scans
  // the PS3 static-mesh cache.
  //
  // Existing exact/display-list matches have already returned g_current_draw
  // above, so this only affects otherwise-unidentified direct draws. Keep the
  // cutoff configurable for diagnostics; the environment is read once.
  static const u32 direct_world_min_vertices = [] {
    constexpr u32 fallback = 8;
    const char* value = std::getenv("MOH_PS3_CPT_DIRECT_MIN_VERTS");
    if (!value || !*value)
      return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value)
      return fallback;
    return static_cast<u32>(std::clamp<unsigned long>(parsed, 3ul, 64ul));
  }();

  if (!g_active_display_list && PS3WorldGeometry::Enabled() &&
      count < direct_world_min_vertices)
  {
    return {};
  }

  const auto gc = BoundsFromGC(gc_vertices, count, stride, offset);
  if (!gc.valid)
    return {};

  // Direct CPT world matching is deliberately stricter than v9.1.  Bounds alone
  // are not an identity: 1_1 proved that many unrelated GX batches can share the
  // same AABB.  Require compatible triangle topology and one GC identity per CPT
  // sector before a candidate is allowed to reach the renderer.
  if (!g_active_display_list && PS3WorldGeometry::Enabled() && gc_triangle_count != 0)
  {
    const u64 direct_key =
        WorldDirectKey(gc_vertices, count, stride, offset, gc_triangle_count, gc);

    {
      std::scoped_lock lock(g_msh_cache_mutex);
      if (const auto cached = g_world_direct_matches.find(direct_key);
          cached != g_world_direct_matches.end())
      {
        g_current_draw = cached->second;
        g_current_draw_transient_world = true;
        g_current_world_direct_key = direct_key;
        PS3WorldCPTRuntime::LearnExactDrawMaterial(g_current_draw);
        return g_current_draw;
      }
      if (g_world_direct_rejected.contains(direct_key))
        return {};
    }

    static const float maximum_triangle_ratio =
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_TRI_RATIO", 1.35f, 1.0f, 8.0f);
    static const u32 triangle_slop = static_cast<u32>(std::lround(
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_TRI_SLOP", 4.0f, 0.0f, 256.0f)));

    float best_score = std::numeric_limits<float>::infinity();
    float second_score = std::numeric_limits<float>::infinity();
    float best_extent_score = std::numeric_limits<float>::infinity();
    float second_extent_score = std::numeric_limits<float>::infinity();
    std::shared_ptr<StaticMesh> best_owner;
    std::shared_ptr<StaticMesh> best_extent_owner;
    Bounds3 best_extent_bounds;
    std::size_t world_candidates = 0;
    std::size_t topology_compatible = 0;
    std::size_t claimed_elsewhere = 0;
    std::size_t considered = 0;

    // MOH_PS3_MESH_MATCH_FAST_CANDIDATES_V1
    struct FastWorldCandidate
    {
      std::shared_ptr<StaticMesh> owner;
      Bounds3 bounds;
      u32 triangles = 0;
    };

    const std::string_view fast_world_level_now =
        MOHFrontline::NativeAssets::GetCurrentLevel();
    const auto fast_world_generation_now =
        PS3RemasterAssets::GetIndexGeneration();

    static thread_local u64 fast_world_revision = 0;
    static thread_local bool fast_world_ready = false;
    static thread_local std::string fast_world_level;
    static thread_local auto fast_world_generation = fast_world_generation_now;
    static thread_local std::vector<FastWorldCandidate> fast_world_candidates;
    static thread_local std::unordered_map<const StaticMesh*, Bounds3> fast_world_bounds;

    // Cache publication and lazy loads can change meshes without changing
    // the level or the asset index generation.
    {
      std::scoped_lock lock(g_msh_cache_mutex);
      if (!fast_world_ready || fast_world_revision != g_msh_cache_revision ||
          std::string_view(fast_world_level) != fast_world_level_now ||
          fast_world_generation != fast_world_generation_now)
      {
        fast_world_ready = true;
        fast_world_revision = g_msh_cache_revision;
        fast_world_level.assign(fast_world_level_now);
        fast_world_generation = fast_world_generation_now;
        fast_world_candidates.clear();
        fast_world_bounds.clear();

        std::unordered_set<const StaticMesh*> seen;
        fast_world_candidates.reserve(g_msh_cache.size());
        fast_world_bounds.reserve(g_msh_cache.size());

        for (const auto& [key, holder] : g_msh_cache)
        {
          (void)key;
          if (!holder || !seen.insert(holder.get()).second || !IsWorldCPTMesh(*holder) ||
              holder->submeshes.size() != 1)
            continue;

          const auto& submesh = holder->submeshes[0];
          if (!submesh.vertex_count || submesh.position_uv.size() != submesh.vertex_count ||
              submesh.indices.empty() || (submesh.indices.size() % 3) != 0)
            continue;

          const Bounds3 bounds = BoundsFromPS3(submesh);
          if (!bounds.valid)
            continue;

          const u32 triangles = static_cast<u32>(submesh.indices.size() / 3);
          fast_world_candidates.push_back({holder, bounds, triangles});
          fast_world_bounds.emplace(holder.get(), bounds);
        }

        std::sort(fast_world_candidates.begin(), fast_world_candidates.end(),
                  [](const FastWorldCandidate& a, const FastWorldCandidate& b) {
                    return a.triangles < b.triangles;
                  });
      }
    }

    world_candidates = fast_world_candidates.size();

    const u32 ratio_lower =
        std::max(1u, static_cast<u32>(std::floor(
                         static_cast<double>(gc_triangle_count) /
                         static_cast<double>(maximum_triangle_ratio))));
    const u64 ratio_upper64 = static_cast<u64>(std::ceil(
        static_cast<double>(gc_triangle_count) *
        static_cast<double>(maximum_triangle_ratio)));
    const u32 slop_lower =
        gc_triangle_count > triangle_slop ? gc_triangle_count - triangle_slop : 1u;
    const u64 slop_upper64 =
        static_cast<u64>(gc_triangle_count) + static_cast<u64>(triangle_slop);

    const u32 compatible_lower = std::min(ratio_lower, slop_lower);
    const u32 compatible_upper = static_cast<u32>(std::min<u64>(
        std::numeric_limits<u32>::max(), std::max(ratio_upper64, slop_upper64)));

    const auto candidate_first = std::lower_bound(
        fast_world_candidates.begin(), fast_world_candidates.end(), compatible_lower,
        [](const FastWorldCandidate& candidate, u32 triangles) {
          return candidate.triangles < triangles;
        });
    const auto candidate_last = std::upper_bound(
        candidate_first, fast_world_candidates.end(), compatible_upper,
        [](u32 triangles, const FastWorldCandidate& candidate) {
          return triangles < candidate.triangles;
        });

    topology_compatible =
        static_cast<std::size_t>(std::distance(candidate_first, candidate_last));

    {
      std::scoped_lock lock(g_msh_cache_mutex);
      for (auto candidate = candidate_first; candidate != candidate_last; ++candidate)
      {
        const auto& holder = candidate->owner;
        if (!holder)
          continue;

        if (const auto claim = g_world_direct_claims.find(holder.get());
            claim != g_world_direct_claims.end() && claim->second != direct_key)
        {
          ++claimed_elsewhere;
          continue;
        }

        ++considered;
        const Bounds3& ps3_bounds = candidate->bounds;
        const float score = BoundsScore(gc, ps3_bounds);
        const float extent_score = BoundsExtentScore(gc, ps3_bounds);
        if (!std::isfinite(score))
          continue;

        if (score < best_score)
        {
          second_score = best_score;
          best_score = score;
          best_owner = holder;
        }
        else if (score < second_score)
        {
          second_score = score;
        }

        if (std::isfinite(extent_score))
        {
          if (extent_score < best_extent_score)
          {
            second_extent_score = best_extent_score;
            best_extent_score = extent_score;
            best_extent_owner = holder;
            best_extent_bounds = ps3_bounds;
          }
          else if (extent_score < second_extent_score)
          {
            second_extent_score = extent_score;
          }
        }
      }
    }

    static const float maximum_direct_score =
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_DIRECT_SCORE", 0.025f, 0.00001f, 0.25f);
    static const float minimum_direct_margin =
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_DIRECT_MARGIN", 0.0025f, 0.0f, 0.25f);
    const float margin = std::isfinite(second_score) ? second_score - best_score :
                                                       std::numeric_limits<float>::infinity();
    const bool accepted_absolute =
        best_owner && best_score <= maximum_direct_score &&
        (!std::isfinite(second_score) || margin >= minimum_direct_margin);

    // v9.3: PS3 CPT chunks can preserve the exact sector shape while changing
    // only its local origin. The old BoundsScore penalises that center shift.
    // Use an origin-independent fallback only after the proven absolute matcher
    // fails, and require a clearly unique centered-bounds candidate.
    static const float maximum_local_extent =
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_LOCAL_EXTENT", 0.025f, 0.00001f, 0.20f);
    static const float minimum_local_margin =
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_LOCAL_MARGIN", 0.010f, 0.0f, 0.25f);
    const float extent_margin = std::isfinite(second_extent_score) ?
        second_extent_score - best_extent_score : std::numeric_limits<float>::infinity();
    const bool accepted_local =
        !accepted_absolute && best_extent_owner && best_extent_score <= maximum_local_extent &&
        (!std::isfinite(second_extent_score) || extent_margin >= minimum_local_margin);

    std::shared_ptr<StaticMesh> selected_owner =
        accepted_absolute ? best_owner : (accepted_local ? best_extent_owner : nullptr);
    float selected_score = accepted_absolute ? best_score : best_extent_score;
    bool selected_local = accepted_local;
    bool selected_dynamic_range = false;
    Bounds3 selected_local_bounds = best_extent_bounds;

    // v9.5: search every contiguous descriptor range only when the normal
    // single/fixed-pack match failed. This targets large architectural GC
    // batches without pre-building O(N^2) packs. Rejected GC identities are
    // cached, so the expensive search runs only once for each exact batch.
    float best_range_score = std::numeric_limits<float>::infinity();
    float second_range_score = std::numeric_limits<float>::infinity();
    float best_range_extent = std::numeric_limits<float>::infinity();
    Bounds3 best_range_bounds;
    std::string best_range_source;
    std::size_t best_range_sequence = std::numeric_limits<std::size_t>::max();
    std::size_t best_range_first = 0;
    std::size_t best_range_count = 0;
    u32 best_range_triangles = 0;
    std::size_t dynamic_ranges_tested = 0;

    static const u32 dynamic_min_triangles = static_cast<u32>(std::lround(
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_RANGE_MIN_TRIS", 96.0f, 12.0f, 4096.0f)));
    static const u32 dynamic_max_members = static_cast<u32>(std::lround(
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_RANGE_MAX_MEMBERS", 64.0f, 2.0f, 64.0f)));
    static const float dynamic_triangle_ratio =
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_RANGE_TRI_RATIO", 1.20f, 1.0f, 2.0f);
    static const u32 dynamic_triangle_slop = static_cast<u32>(std::lround(
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_RANGE_TRI_SLOP", 12.0f, 0.0f, 256.0f)));
    static const float dynamic_max_extent =
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_RANGE_EXTENT", 0.040f, 0.00001f, 0.25f);
    static const float dynamic_min_margin =
        EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_RANGE_MARGIN", 0.006f, 0.0f, 0.25f);

    if (!selected_owner && gc_triangle_count >= dynamic_min_triangles)
    {
      std::scoped_lock lock(g_msh_cache_mutex);
      struct DynamicRange
      {
        Bounds3 bounds;
        std::size_t sequence, first, count;
        u32 triangles, delta, larger;
      };
      // Topology and authored bounds do not depend on the current GC positions.
      // Keep a bounded working set by triangle count; claims and geometric
      // scores are always evaluated afresh, in the original enumeration order.
      static thread_local u64 ranges_revision = 0;
      static thread_local std::unordered_map<u32, std::vector<DynamicRange>> range_cache;
      static thread_local std::size_t cached_range_count = 0;
      if (ranges_revision != g_msh_cache_revision || range_cache.size() >= 32 ||
          cached_range_count > 131072)
      {
        range_cache.clear();
        cached_range_count = 0;
        ranges_revision = g_msh_cache_revision;
      }
      auto [range_it, inserted] = range_cache.try_emplace(gc_triangle_count);
      auto& ranges = range_it->second;
      if (inserted)
      {
        for (std::size_t sequence_index = 0; sequence_index < g_world_cpt_sequences.size();
             ++sequence_index)
        {
          const auto& sequence = g_world_cpt_sequences[sequence_index];
          const auto& members = sequence.meshes;
          if (members.size() < 2)
            continue;

          for (std::size_t first = 0; first + 1 < members.size(); ++first)
          {
            Bounds3 aggregate;
            std::size_t vertex_total = 0;
            u32 triangle_total = 0;
            const std::size_t last =
                std::min(members.size(), first + static_cast<std::size_t>(dynamic_max_members));

            for (std::size_t end = first; end < last; ++end)
            {
              const auto& member = members[end];
              if (!member || member->submeshes.size() != 1)
                break;
              const auto& sub = member->submeshes[0];
              if (!sub.vertex_count || sub.position_uv.size() != sub.vertex_count ||
                  sub.indices.empty() || (sub.indices.size() % 3) != 0)
                break;

              const std::size_t member_triangles = sub.indices.size() / 3;
              if (member_triangles > std::numeric_limits<u32>::max() - triangle_total)
                break;
              triangle_total += static_cast<u32>(member_triangles);
              vertex_total += sub.vertex_count;
              if (vertex_total > 65535u)
                break;
              if (const auto cached_bounds = fast_world_bounds.find(member.get());
                  cached_bounds != fast_world_bounds.end())
                UnionBounds(&aggregate, cached_bounds->second);
              else
              {
                // Sequence members need not be entries in g_msh_cache. Cache
                // those bounds too, instead of rescanning their vertices for
                // every overlapping range and every unmatched direct batch.
                const auto bounds = BoundsFromPS3(sub);
                fast_world_bounds.emplace(member.get(), bounds);
                UnionBounds(&aggregate, bounds);
              }

              const std::size_t range_count = end - first + 1;
              if (range_count < 2 || !aggregate.valid)
                continue;

              // Triangle count is monotonic while extending a range. Once it is
              // already too large, every longer range is also too large.
              const u32 smaller = std::max(1u, std::min(gc_triangle_count, triangle_total));
              const u32 larger = std::max(gc_triangle_count, triangle_total);
              const u32 triangle_delta = larger - smaller;
              const float triangle_ratio =
                  static_cast<float>(larger) / static_cast<float>(smaller);
              if (triangle_delta > dynamic_triangle_slop &&
                  triangle_ratio > dynamic_triangle_ratio)
              {
                if (triangle_total > gc_triangle_count)
                  break;
                continue;
              }

              ranges.push_back({aggregate, sequence_index, first, range_count,
                                triangle_total, triangle_delta, larger});
            }
          }
        }
        cached_range_count += ranges.size();
      }

      std::size_t prefix_sequence = std::numeric_limits<std::size_t>::max();
      std::vector<std::size_t> foreign_claim_prefix;
      for (const auto& range : ranges)
      {
        const auto& sequence = g_world_cpt_sequences[range.sequence];
        if (prefix_sequence != range.sequence)
        {
          prefix_sequence = range.sequence;
          foreign_claim_prefix.assign(sequence.meshes.size() + 1, 0);
          // No strings or hashes are needed before the first descriptor claim.
          if (!g_world_dynamic_descriptor_claims.empty())
          {
            for (std::size_t member = 0; member < sequence.meshes.size(); ++member)
            {
              const auto claim = g_world_dynamic_descriptor_claims.find(
                  WorldDescriptorClaimKey(sequence.source_name, member));
              foreign_claim_prefix[member + 1] = foreign_claim_prefix[member] +
                  (claim != g_world_dynamic_descriptor_claims.end() && claim->second != direct_key);
            }
          }
        }
        if (foreign_claim_prefix[range.first + range.count] != foreign_claim_prefix[range.first])
          continue;
        ++dynamic_ranges_tested;
        const float extent = BoundsExtentScore(gc, range.bounds);
        if (!std::isfinite(extent))
          continue;
        const float topology_error =
            static_cast<float>(range.delta) / static_cast<float>(std::max(1u, range.larger));
        const float range_score = extent + topology_error * 0.10f;
        if (range_score < best_range_score)
        {
          second_range_score = best_range_score;
          best_range_score = range_score;
          best_range_extent = extent;
          best_range_bounds = range.bounds;
          best_range_source = sequence.source_name;
          best_range_sequence = range.sequence;
          best_range_first = range.first;
          best_range_count = range.count;
          best_range_triangles = range.triangles;
        }
        else if (range_score < second_range_score)
        {
          second_range_score = range_score;
        }
      }

      const float range_margin = std::isfinite(second_range_score) ?
          second_range_score - best_range_score : std::numeric_limits<float>::infinity();
      if (best_range_sequence != std::numeric_limits<std::size_t>::max() &&
          best_range_extent <= dynamic_max_extent &&
          (!std::isfinite(second_range_score) || range_margin >= dynamic_min_margin))
      {
        const auto& sequence = g_world_cpt_sequences[best_range_sequence];
        auto dynamic_pack = BuildWorldCPTPack(sequence.meshes, best_range_first, best_range_count);
        if (dynamic_pack)
        {
          selected_owner = std::move(dynamic_pack);
          selected_score = best_range_score;
          selected_local = true;
          selected_dynamic_range = true;
          selected_local_bounds = best_range_bounds;
        }
      }
    }

    std::vector<std::string> selected_descriptor_claim_keys;
    if (selected_dynamic_range)
      selected_descriptor_claim_keys =
          WorldRangeDescriptorKeys(best_range_source, best_range_first, best_range_count);

    if (!selected_owner)
    {
      {
        std::scoped_lock lock(g_msh_cache_mutex);
        g_world_direct_rejected.insert(direct_key);
      }
      static thread_local unsigned reject_logs = 0;
      if (PS3RuntimeDebugEnabled() && reject_logs++ < 128)
      {
        const u32 best_triangles = best_owner && !best_owner->submeshes.empty() ?
            static_cast<u32>(best_owner->submeshes[0].indices.size() / 3) : 0;
        const u32 extent_triangles = best_extent_owner && !best_extent_owner->submeshes.empty() ?
            static_cast<u32>(best_extent_owner->submeshes[0].indices.size() / 3) : 0;
        const float range_margin = std::isfinite(second_range_score) ?
            second_range_score - best_range_score : std::numeric_limits<float>::infinity();
        std::fprintf(stderr,
                     "[moh-ps3-world-geo] DIRECT MATCH REJECT: GCverts=%u GCtris=%u world=%zu topology=%zu claimed=%zu considered=%zu best=%s PS3tris=%u score=%.6f second=%.6f margin=%.6f | centered-best=%s PS3tris=%u extent=%.6f second=%.6f margin=%.6f local-max=%.6f local-margin=%.6f | range-best=%s first=%zu count=%zu PS3tris=%u extent=%.6f score=%.6f second=%.6f margin=%.6f tested=%zu range-max=%.6f range-margin=%.6f | max=%.6f required-margin=%.6f tri-ratio<=%.3f slop=%u -> GC\n",
                     count, gc_triangle_count, world_candidates, topology_compatible,
                     claimed_elsewhere, considered,
                     best_owner ? best_owner->source_name.c_str() : "<none>", best_triangles,
                     best_score, second_score, margin,
                     best_extent_owner ? best_extent_owner->source_name.c_str() : "<none>",
                     extent_triangles, best_extent_score, second_extent_score, extent_margin,
                     maximum_local_extent, minimum_local_margin,
                     best_range_source.empty() ? "<none>" : best_range_source.c_str(),
                     best_range_first, best_range_count, best_range_triangles,
                     best_range_extent, best_range_score, second_range_score, range_margin,
                     dynamic_ranges_tested, dynamic_max_extent, dynamic_min_margin,
                     maximum_direct_score, minimum_direct_margin, maximum_triangle_ratio,
                     triangle_slop);
      }
      return {};
    }

    StaticDrawMatch learned = BuildStaticDrawMatch(selected_owner, 0);
    if (!learned)
      return {};
    learned.score = selected_score;
    learned.display_list = 0;

    if (selected_local)
    {
      const auto translation = BoundsCenterDelta(gc, selected_local_bounds);
      learned.world_translation_valid = true;
      learned.world_translation = translation;
      if (learned.bounds_valid)
      {
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
          learned.bounds_min[axis] += translation[axis];
          learned.bounds_max[axis] += translation[axis];
        }
      }
    }

    {
      std::scoped_lock lock(g_msh_cache_mutex);
      g_world_direct_matches[direct_key] = learned;
      g_world_direct_claims[selected_owner.get()] = direct_key;
      if (selected_dynamic_range && !selected_descriptor_claim_keys.empty())
      {
        for (const auto& descriptor_key : selected_descriptor_claim_keys)
          g_world_dynamic_descriptor_claims[descriptor_key] = direct_key;
        g_world_direct_descriptor_keys[direct_key] = selected_descriptor_claim_keys;
      }
    }
    g_current_draw = learned;
    g_current_draw_transient_world = true;
    g_current_world_direct_key = direct_key;
    PS3WorldCPTRuntime::LearnExactDrawMaterial(g_current_draw);
    ++g_matches;

    static thread_local unsigned match_logs = 0;
    if (PS3RuntimeDebugEnabled() && match_logs++ < 192)
    {
      const auto& sub = selected_owner->submeshes[0];
      if (selected_dynamic_range)
      {
        const float range_margin = std::isfinite(second_range_score) ?
            second_range_score - best_range_score : std::numeric_limits<float>::infinity();
        std::fprintf(stderr,
                     "[moh-ps3-world-geo] DYNAMIC RANGE MATCH: GCverts=%u GCtris=%u -> PS3=%s verts=%u tris=%zu indices=%zu first=%zu count=%zu extent=%.6f score=%.6f second=%.6f margin=%.6f tested=%zu translation=(%.6f %.6f %.6f) | exact contiguous CPT range\n",
                     count, gc_triangle_count, selected_owner->source_name.c_str(),
                     sub.vertex_count, sub.indices.size() / 3, sub.indices.size(),
                     best_range_first, best_range_count, best_range_extent, best_range_score,
                     second_range_score, range_margin, dynamic_ranges_tested,
                     learned.world_translation[0], learned.world_translation[1],
                     learned.world_translation[2]);
      }
      else if (accepted_local)
      {
        std::fprintf(stderr,
                     "[moh-ps3-world-geo] DIRECT LOCAL MATCH: GCverts=%u GCtris=%u -> PS3=%s verts=%u tris=%zu indices=%zu extent=%.6f second=%.6f margin=%.6f translation=(%.6f %.6f %.6f) world=%zu topology=%zu considered=%zu | translated transient world draw\n",
                     count, gc_triangle_count, selected_owner->source_name.c_str(),
                     sub.vertex_count, sub.indices.size() / 3, sub.indices.size(),
                     best_extent_score, second_extent_score, extent_margin,
                     learned.world_translation[0], learned.world_translation[1],
                     learned.world_translation[2], world_candidates, topology_compatible,
                     considered);
      }
      else
      {
        std::fprintf(stderr,
                     "[moh-ps3-world-geo] DIRECT MATCH: GCverts=%u GCtris=%u -> PS3=%s verts=%u tris=%zu indices=%zu score=%.6f second=%.6f margin=%.6f world=%zu topology=%zu considered=%zu | transient world draw\n",
                     count, gc_triangle_count, selected_owner->source_name.c_str(),
                     sub.vertex_count, sub.indices.size() / 3, sub.indices.size(), best_score,
                     second_score, margin, world_candidates, topology_compatible, considered);
      }
    }
    return learned;
  }
  // Unknown draws that are inside GXCallDisplayList keep the old diagnostic
  // MSH bootstrap path. Direct CPT world matching never runs inside a DL.
  if (!g_active_display_list || !IsStaticBootstrapEnabled())
    return {};

  const float maximum_score =
      EnvFloatLocal("MOH_PS3_MSH_BOOTSTRAP_SCORE", 0.075f, 0.001f, 1.0f);
  const float minimum_margin =
      EnvFloatLocal("MOH_PS3_MSH_BOOTSTRAP_MARGIN", 0.015f, 0.0f, 1.0f);

  float best_score = std::numeric_limits<float>::infinity();
  float second_score = std::numeric_limits<float>::infinity();
  std::shared_ptr<StaticMesh> best_owner;

  {
    // Mesh vertices are immutable while published in g_msh_cache. Rebuild in
    // map iteration order on every cache mutation to preserve tie handling,
    // while avoiding a full vertex scan and alias deduplication per GX batch.
    struct BootstrapCandidate
    {
      std::shared_ptr<StaticMesh> owner;
      Bounds3 bounds;
    };
    static thread_local bool candidates_ready = false;
    static thread_local u64 candidates_revision = 0;
    static thread_local std::vector<BootstrapCandidate> candidates;
    std::scoped_lock lock(g_msh_cache_mutex);
    if (!candidates_ready || candidates_revision != g_msh_cache_revision)
    {
      candidates.clear();
      std::unordered_set<const StaticMesh*> seen;
      for (const auto& [key, holder] : g_msh_cache)
      {
        (void)key;
        if (!holder || !seen.insert(holder.get()).second || holder->submeshes.size() != 1)
          continue;
        const auto& submesh = holder->submeshes[0];
        if (submesh.position_uv.size() != submesh.vertex_count ||
            submesh.indices.empty() || submesh.indices.size() % 3)
          continue;
        candidates.push_back({holder, BoundsFromPS3(submesh)});
      }
      candidates_revision = g_msh_cache_revision;
      candidates_ready = true;
    }
    for (const auto& candidate : candidates)
    {
      const auto& holder = candidate.owner;
      const float score = BoundsScore(gc, candidate.bounds);
      if (!std::isfinite(score))
        continue;

      if (score < best_score)
      {
        second_score = best_score;
        best_score = score;
        best_owner = holder;
      }
      else if (score < second_score)
      {
        second_score = score;
      }
    }
  }

  // CPT world geometry has many neighbouring sectors with similar bounds, so use a separate
  // but still strict threshold.  This does not affect normal MSH bootstrap behaviour.
  const bool best_is_world_cpt =
      best_owner && best_owner->source_name.find(".cpt#") != std::string::npos;
  const float effective_maximum_score = best_is_world_cpt ?
      EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_BOOTSTRAP_SCORE", 0.035f, 0.0001f, 0.25f) :
      maximum_score;
  const float effective_minimum_margin = best_is_world_cpt ?
      EnvFloatLocal("MOH_PS3_CPT_GEOMETRY_BOOTSTRAP_MARGIN", 0.004f, 0.0f, 0.25f) :
      minimum_margin;

  if (!best_owner || best_score > effective_maximum_score ||
      (std::isfinite(second_score) && second_score - best_score < effective_minimum_margin))
  {
    static thread_local unsigned bootstrap_miss_logs = 0;
    if (PS3RuntimeDebugEnabled() && bootstrap_miss_logs++ < 24)
    {
      const u32 best_ps3_vertices =
          best_owner && !best_owner->submeshes.empty() ? best_owner->submeshes[0].vertex_count : 0;
      const std::size_t best_ps3_indices =
          best_owner && !best_owner->submeshes.empty() ? best_owner->submeshes[0].indices.size() : 0;
      std::fprintf(stderr,
                   "[moh-ps3-msh] BOOTSTRAP MISS: gx_stream_verts=%u best=%s ps3_vertices=%u ps3_indices=%zu score=%.5f second=%.5f max=%.5f margin=%.5f DL=%08x\n",
                   count, best_owner ? best_owner->source_name.c_str() : "none",
                   best_ps3_vertices, best_ps3_indices, best_score, second_score, effective_maximum_score,
                   effective_minimum_margin, g_active_display_list.address);
    }
    return {};
  }

  const auto& sub = best_owner->submeshes[0];
  auto normals = std::make_shared<std::vector<std::array<float, 3>>>(sub.vertex_count);
  for (std::size_t i = 0; i < sub.indices.size(); i += 3)
  {
    const auto ia = sub.indices[i], ib = sub.indices[i + 1], ic = sub.indices[i + 2];
    if (ia >= sub.vertex_count || ib >= sub.vertex_count || ic >= sub.vertex_count)
      return {};
    const auto& a = sub.position_uv[ia].position;
    const auto& b = sub.position_uv[ib].position;
    const auto& c = sub.position_uv[ic].position;
    const std::array<float, 3> ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const std::array<float, 3> ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const std::array<float, 3> n{
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0]};
    for (u16 index : {ia, ib, ic})
      for (unsigned j = 0; j < 3; ++j)
        (*normals)[index][j] += n[j];
  }
  for (auto& n : *normals)
  {
    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (length > 0.000001f)
      for (auto& value : n)
        value /= length;
  }
  if (sub.has_normal)
    for (std::size_t i = 0; i < sub.position_uv.size(); ++i)
      (*normals)[i] = sub.position_uv[i].normal;

  StaticDrawMatch learned;
  learned.owner = best_owner;
  learned.normals = std::move(normals);
  learned.mesh = best_owner.get();
  learned.submesh = &best_owner->submeshes[0];
  learned.submesh_index = 0;
  learned.score = best_score;
  learned.display_list = g_active_display_list.address;

  DisplayListIdentity identity;
  identity.match = learned;
  identity.size = g_active_display_list.size;
  identity.command_hash = g_active_display_list.command_hash;
  {
    std::scoped_lock lock(g_msh_cache_mutex);
    g_display_lists[g_active_display_list.address] = identity;
  }

  g_current_draw = learned;
  ++g_matches;
  std::fprintf(stderr,
               "[moh-ps3-msh] BOOTSTRAP EXACT DL: ps3=%s vertices=%u indices=%zu score=%.5f DL=%08x | future draws use address+hash only\n",
               best_owner->source_name.c_str(), sub.vertex_count, sub.indices.size(), best_score,
               g_active_display_list.address);
  return learned;
}
void NotifyStaticDrawSubmitted()
{
  if (!g_current_draw)
    return;

  if (PS3RuntimeDebugEnabled())
  {
    ++g_draws;
    g_vertices +=
        g_current_draw.submesh->vertex_count;
    g_indices +=
        g_current_draw.submesh->indices.size();

    if (g_draw_logged
            .insert(
                g_current_draw.mesh->source_name)
            .second)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-msh] FIRST REAL PS3 DRAW: "
          "gc_resource=%08x ps3=%s DL=%08x | replacement active; "
          "current GX model/view/projection and CSM preserved\n",
          g_current_draw.guest_resource,
          g_current_draw.mesh->source_name.c_str(),
          g_current_draw.display_list);
    }
  }

  // Direct world matches exist only for the current primitive batch. Do not
  // let one CPT sector leak into the next unrelated GameCube world draw.
  if (g_current_draw_transient_world)
  {
    g_current_draw = {};
    g_current_draw_transient_world = false;
    g_current_world_direct_key = 0;
  }
}

void NotifySkinnedDrawSubmitted(const SkinnedDrawReplacement& replacement)
{
  if (!PS3RuntimeDebugEnabled())
    return;

  const SkinnedDrawMatch draw =
      g_current_dmf_draw;

  if (!draw ||
      !replacement ||
      !replacement.cluster)
    return;

  ++g_dmf_draws;

  g_dmf_vertices +=
      replacement.cluster->positions.size();

  g_dmf_indices +=
      replacement.cluster->indices.size();

  const std::string key =
      std::string(draw.gc_name) + "|" +
      std::string(draw.gc_material_name);

  if (g_dmf_draw_logged.insert(key).second)
  {
    std::fprintf(
        stderr,
        "[moh-ps3-dmf] FIRST REAL PS3 SKINNED DRAW: "
        "gc=%s ps3=%s material=%s vertices=%zu indices=%zu "
        "palette=%zu DL=%08x | GX/XF animation palette + "
        "current TPK/GX state preserved\n",
        draw.gc_name.data(),
        replacement.owner->source_name.c_str(),
        draw.gc_material_name.data(),
        replacement.cluster->positions.size(),
        replacement.cluster->indices.size(),
        draw.gc_palette_groups.size(),
        draw.display_list);
  }
}

void PrintDrawStatistics()
{
  if (!PS3RuntimeDebugEnabled())
    return;

  std::fprintf(stderr, "[moh-ps3-perf] dmf_lookup=%llu dmf_cache_hit=%llu dmf_cache_miss=%llu dmf_runtime_triangle_scans=0 dmf_runtime_allocations=0 fifo_backpressure_events=%llu fifo_peak_distance=%u\n",
      static_cast<unsigned long long>(g_dmf_lookups.load()),
      static_cast<unsigned long long>(g_dmf_hits.load()),
      static_cast<unsigned long long>(g_dmf_misses.load()),
      static_cast<unsigned long long>(Core::System::GetInstance().GetFifo().GetBackpressureEvents()),
      Core::System::GetInstance().GetFifo().GetPeakFifoDistance());
  std::fprintf(stderr, "[moh-ps3-msh] stats: matches=%llu replacements=%llu draws=%llu vertices=%llu indices=%llu fallbacks=%llu\n",
               (unsigned long long)g_matches.load(), (unsigned long long)g_draws.load(),
               (unsigned long long)g_draws.load(), (unsigned long long)g_vertices.load(),
               (unsigned long long)g_indices.load(), (unsigned long long)g_fallbacks.load());
  std::fprintf(stderr,
               "[moh-ps3-dmf] stats: skinned_draws=%llu vertices=%llu indices=%llu\n",
               (unsigned long long)g_dmf_draws.load(),
               (unsigned long long)g_dmf_vertices.load(),
               (unsigned long long)g_dmf_indices.load());
}

bool ParseMSHv8(std::span<const u8> bytes, StaticMesh* out)
{
  if (!PS3AssetPort::IsMSHEnabled())
    return false;

  const bool ok = MOHFrontline::PS3::MSH::Decode(bytes, out);
  static unsigned logs = 0;
  if (PS3RuntimeDebugEnabled() &&
      ok && out && logs++ < 64)
  {
    std::fprintf(stderr, "[moh-ps3-msh] decoded static MSH: submeshes=%zu\n",
                 out->submeshes.size());
  }
  return ok;
}


bool DecodeDMF0502(std::span<const u8> bytes, DMFDecoded* out)
{
  if (!out)
    return false;
  *out = {};
  if (bytes.size() < 0x5c || bytes[0] != 'D' || bytes[1] != 'M' || bytes[2] != 'F' ||
      bytes[3] != 0 || bytes[4] != 0x05 || bytes[5] != 0x02)
    return false;

  const u32 group_count = BE32(bytes.data() + 0x20);
  const u32 group_offset = BE32(bytes.data() + 0x24);
  const u32 material_count = BE32(bytes.data() + 0x28);
  const u32 material_offset = BE32(bytes.data() + 0x2c);
  const u32 texture_table = BE32(bytes.data() + 0x34);
  const u32 bone_count = BE32(bytes.data() + 0x48);
  const u32 bone_offset = BE32(bytes.data() + 0x4c);
  if (!group_count || group_count > 4096 || material_count > 1024 || !bone_count ||
      bone_count > 4096 || group_offset > bytes.size() || material_offset > bytes.size() ||
      bone_offset > bytes.size() || static_cast<std::size_t>(group_count) * 28 > bytes.size() - group_offset ||
      static_cast<std::size_t>(material_count) * 60 > bytes.size() - material_offset ||
      static_cast<std::size_t>(bone_count) * 16 > bytes.size() - bone_offset)
    return false;

  out->bone_refs.reserve(bone_count);
  for (u32 i = 0; i < bone_count; ++i)
  {
    const std::string name = FixedString(bytes.data() + bone_offset + static_cast<std::size_t>(i) * 16, 16);
    if (name.empty())
      return false;
    out->bone_refs.push_back(name);
  }

  // The inverse-world bind matrices are authored inside the DMF immediately
  // after the 6-byte-per-ref Euler table (16-byte aligned).  Decode them by
  // DMF ref index so runtime replacement does not depend on finding a separate
  // SKL just to undo model-bind space.  The file stores matrices transposed;
  // convert them to row-major 4x4 like SkinBind::Binding does.
  const u32 bind_angle_offset = BE32(bytes.data() + 0x50);
  const std::size_t bind_angles_size = static_cast<std::size_t>(bone_count) * 6;
  const std::size_t inverse_bind_offset =
      (static_cast<std::size_t>(bind_angle_offset) + bind_angles_size + 15u) & ~std::size_t(15u);
  if (bind_angle_offset <= bytes.size() && bind_angles_size <= bytes.size() - bind_angle_offset &&
      inverse_bind_offset <= bytes.size() &&
      static_cast<std::size_t>(bone_count) * 64 <= bytes.size() - inverse_bind_offset)
  {
    bool bind_ok = true;
    out->inverse_bind_by_ref.resize(bone_count);
    for (u32 ref = 0; ref < bone_count && bind_ok; ++ref)
      for (std::size_t row = 0; row < 4 && bind_ok; ++row)
        for (std::size_t column = 0; column < 4; ++column)
        {
          const float value = BEFloat(bytes.data() + inverse_bind_offset +
                                      static_cast<std::size_t>(ref) * 64 +
                                      (column * 4 + row) * 4);
          if (!std::isfinite(value))
          {
            bind_ok = false;
            break;
          }
          out->inverse_bind_by_ref[ref][row * 4 + column] = value;
        }
    out->bind_tables_valid = bind_ok;
    if (!bind_ok)
      out->inverse_bind_by_ref.clear();
  }

  const auto validate_group =
      [&](const DMFSkinGroup& group)
      {
        const float q =
            group.blend * 4096.0f;

        if (group.bone_a >= bone_count ||
            group.bone_b >= bone_count ||
            !std::isfinite(group.blend) ||
            !std::isfinite(q) ||
            q < -0.5f ||
            q > 65535.5f)
          return false;

        for (float value :
             group.auxiliary)
          if (!std::isfinite(value))
            return false;

        return true;
      };

  std::vector<DMFSkinGroup>
      parsed_groups;

  parsed_groups.reserve(group_count);

  bool ordinary = true;

  if (static_cast<std::size_t>(
          group_count) * 28 >
      bytes.size() - group_offset)
  {
    ordinary = false;
  }
  else
  {
    for (u32 i = 0;
         i < group_count;
         ++i)
    {
      const u8* q =
          bytes.data() +
          group_offset +
          static_cast<std::size_t>(i) *
              28;

      DMFSkinGroup group;

      group.bone_a = q[0];
      group.bone_b = q[4];
      group.blend =
          BEFloat(q + 8);

      for (std::size_t j = 0;
           j <
               group.auxiliary.size();
           ++j)
      {
        group.auxiliary[j] =
            BEFloat(
                q + 12 + j * 4);
      }

      if (!validate_group(group))
      {
        ordinary = false;
        parsed_groups.clear();
        break;
      }

      parsed_groups.push_back(
          group);
    }
  }

  if (!ordinary)
  {
    parsed_groups.clear();

    if (static_cast<std::size_t>(
            group_count) * 4 >
        bytes.size() - group_offset)
      return false;

    for (u32 i = 0;
         i < group_count;
         ++i)
    {
      const u8* q =
          bytes.data() +
          group_offset +
          static_cast<std::size_t>(i) *
              4;

      DMFSkinGroup group;

      group.bone_a = q[0];
      group.bone_b = q[1];

      group.blend =
          static_cast<float>(
              BE16(q + 2)) /
          4096.0f;

      group.auxiliary.fill(0.0f);

      if (!validate_group(group))
        return false;

      parsed_groups.push_back(
          group);
    }

    std::fprintf(
        stderr,
        "[moh-ps3-dmf] "
        "0502 COMPACT SKIN GROUPS: "
        "groups=%u stride=4 "
        "layout=boneA/boneB/q16\n",
        group_count);
  }

  out->skin_groups =
      std::move(parsed_groups);

  for (u32 material = 0; material < material_count; ++material)
  {
    const u8* q = bytes.data() + material_offset + static_cast<std::size_t>(material) * 60;
    const u32 texture_index = BE32(q + 44);
    const u32 cluster_count = BE32(q + 48);
    u32 palette = BE32(q + 52);
    u32 geometry = BE32(q + 56);
    if (cluster_count > 8192 || palette > bytes.size() || geometry > bytes.size())
      return false;

    for (u32 cluster_index = 0; cluster_index < cluster_count; ++cluster_index)
    {
      if (palette > bytes.size() || bytes.size() - palette < 4 || geometry > bytes.size() ||
          bytes.size() - geometry < 12)
        return false;

      const u8 palette_count = bytes[palette + 2];
      const std::size_t palette_bytes = 4 + static_cast<std::size_t>(palette_count) * 2;
      if (!palette_count || palette_bytes > bytes.size() - palette)
        return false;

      DMFCluster cluster;
      cluster.material_index = material;
      cluster.material_cluster_index = cluster_index;
      cluster.texture_index = texture_index;
      if (texture_table <= bytes.size() && texture_index <= 65535 &&
          static_cast<std::size_t>(texture_index + 1) * 16 <= bytes.size() - texture_table)
      {
        cluster.material_name =
            Lower(FixedString(bytes.data() + texture_table + static_cast<std::size_t>(texture_index) * 16, 16));
      }
      cluster.palette_groups.reserve(palette_count);
      for (u8 k = 0; k < palette_count; ++k)
      {
        const u16 group = BE16(bytes.data() + palette + 4 + static_cast<std::size_t>(k) * 2);
        if (group >= group_count)
          return false;
        cluster.palette_groups.push_back(group);
      }
      palette += static_cast<u32>(palette_bytes);

      const u32 index_count = BE32(bytes.data() + geometry);
      const u32 vertex_count = BE32(bytes.data() + geometry + 4);
      const u8 field_a =
          bytes[geometry + 8];

      const u8 field_b =
          bytes[geometry + 9];

      bool swapped_geometry_header =
          false;

      if (field_a >= 30 &&
          field_a <= 192 &&
          field_b <= 32)
      {
        cluster.vertex_stride =
            field_a;

        cluster.attribute_word_count =
            field_b;
      }
      else if (field_b >= 30 &&
               field_b <= 192 &&
               field_a > 0 &&
               field_a <= 32)
      {
        cluster.attribute_word_count =
            field_a;

        cluster.vertex_stride =
            field_b;

        swapped_geometry_header =
            true;

        std::fprintf(
            stderr,
            "[moh-ps3-dmf] "
            "0502 SWAPPED GEOMETRY HEADER: "
            "material=%u cluster=%u "
            "attributes=%u stride=%u\n",
            material,
            cluster_index,
            cluster.attribute_word_count,
            cluster.vertex_stride);
      }
      else
      {
        return false;
      }

      if (!index_count ||
          !vertex_count ||
          index_count >
              16 * 1024 * 1024 ||
          vertex_count >
              4 * 1024 * 1024 ||
          (index_count % 3) != 0)
        return false;

      const std::size_t attribute_start = static_cast<std::size_t>(geometry) + 12;
      const std::size_t attribute_bytes =
          static_cast<std::size_t>(cluster.attribute_word_count) * 4;
      if (attribute_start > bytes.size() || attribute_bytes > bytes.size() - attribute_start)
        return false;

      const Attribute* position = nullptr;
      const Attribute* normal = nullptr;
      const Attribute* uv0 = nullptr;
      cluster.attributes.reserve(cluster.attribute_word_count);
      for (u8 attribute_index = 0; attribute_index < cluster.attribute_word_count; ++attribute_index)
      {
        const u8* descriptor =
            bytes.data() + attribute_start + static_cast<std::size_t>(attribute_index) * 4;
        cluster.attributes.push_back(
            {descriptor[0], descriptor[1], descriptor[2], descriptor[3]});
      }
      for (const Attribute& attribute : cluster.attributes)
      {
        if (attribute.offset >= cluster.vertex_stride)
          return false;
        if (attribute.semantic == 0)
        {
          if (position)
            return false;
          position = &attribute;
        }
        else if (attribute.semantic == 2)
        {
          if (normal)
            return false;
          normal = &attribute;
        }
        else if (attribute.semantic == 8)
        {
          if (uv0)
            return false;
          uv0 = &attribute;
        }
      }

      const std::size_t vertex_start = attribute_start + attribute_bytes;
      const std::size_t vertex_bytes = static_cast<std::size_t>(vertex_count) * cluster.vertex_stride;
      if (vertex_start > bytes.size() || vertex_bytes > bytes.size() - vertex_start)
        return false;
      const std::size_t index_start = vertex_start + vertex_bytes;
      const std::size_t index_bytes = static_cast<std::size_t>(index_count) * 2;
      if (index_bytes > bytes.size() - index_start)
        return false;

      cluster.vertices.assign(
          bytes.begin() + vertex_start,
          bytes.begin() + index_start);

      cluster.vertex_palette_slots.reserve(
          vertex_count);

      const auto attribute_width =
          [](const Attribute& a) -> u32
          {
            if (a.type == 2)
              return
                  static_cast<u32>(
                      a.components) * 4;

            if (a.type == 3)
              return
                  static_cast<u32>(
                      a.components) * 2;

            if (a.type == 6)
              return 4;

            return 0;
          };

      const auto overlaps =
          [&](u32 offset)
          {
            for (const auto& a :
                 cluster.attributes)
            {
              const u32 width =
                  attribute_width(a);

              if (!width)
                continue;

              if (offset <
                      a.offset + width &&
                  offset + 2 >
                      a.offset)
                return true;
            }

            return false;
          };

      const auto valid_palette_offset =
          [&](u32 offset)
          {
            if (offset + 2 >
                cluster.vertex_stride)
              return false;

            for (u32 v = 0;
                 v < vertex_count;
                 ++v)
            {
              const u8* vertex =
                  bytes.data() +
                  vertex_start +
                  static_cast<std::size_t>(
                      v) *
                      cluster.vertex_stride;

              if (BE16(vertex + offset) >=
                  cluster.palette_groups.size())
                return false;
            }

            return true;
          };

      u32 palette_slot_offset = 28;

      if (!valid_palette_offset(
              palette_slot_offset))
      {
        std::vector<u32>
            offsets;

        for (u32 offset = 0;
             offset + 2 <=
                 cluster.vertex_stride;
             offset += 2)
        {
          if (overlaps(offset))
            continue;

          if (valid_palette_offset(offset))
            offsets.push_back(offset);
        }

        std::sort(
            offsets.begin(),
            offsets.end());

        offsets.erase(
            std::unique(
                offsets.begin(),
                offsets.end()),
            offsets.end());

        if (offsets.size() != 1)
        {
          std::fprintf(
              stderr,
              "[moh-ps3-dmf] "
              "0502 PALETTE SLOT REJECT: "
              "material=%u cluster=%u "
              "stride=%u candidates=%zu "
              "swapped=%d\n",
              material,
              cluster_index,
              cluster.vertex_stride,
              offsets.size(),
              swapped_geometry_header ? 1 : 0);

          return false;
        }

        palette_slot_offset =
            offsets.front();

        std::fprintf(
            stderr,
            "[moh-ps3-dmf] "
            "0502 PALETTE SLOT VARIANT: "
            "material=%u cluster=%u "
            "offset=%u stride=%u\n",
            material,
            cluster_index,
            palette_slot_offset,
            cluster.vertex_stride);
      }
      if (position)
        cluster.positions.resize(vertex_count);
      if (normal)
        cluster.normals.resize(vertex_count);
      if (uv0)
        cluster.uv0.resize(vertex_count);

      bool positions_ok = position != nullptr;
      bool normals_ok = normal != nullptr;
      bool uv0_ok = uv0 != nullptr;
      for (u32 v = 0; v < vertex_count; ++v)
      {
        const u8* vertex = bytes.data() + vertex_start + static_cast<std::size_t>(v) * cluster.vertex_stride;
        const u16 slot =
            BE16(
                vertex +
                palette_slot_offset);
        if (slot >= cluster.palette_groups.size())
          return false;
        cluster.vertex_palette_slots.push_back(slot);

        if (positions_ok &&
            !DecodeRSXComponents(vertex, *position, 3, cluster.positions[v].data(),
                                 cluster.vertex_stride))
          positions_ok = false;
        if (normals_ok && !DecodeRSXNormal(vertex, *normal, &cluster.normals[v],
                                           cluster.vertex_stride))
          normals_ok = false;
        if (uv0_ok && !DecodeRSXComponents(vertex, *uv0, 2, cluster.uv0[v].data(),
                                            cluster.vertex_stride))
          uv0_ok = false;
      }
      cluster.has_position = positions_ok;
      cluster.has_normal = normals_ok;
      cluster.has_uv0 = uv0_ok;
      if (!positions_ok)
        cluster.positions.clear();
      if (!normals_ok)
        cluster.normals.clear();
      if (!uv0_ok)
        cluster.uv0.clear();

      cluster.indices.reserve(index_count);
      for (u32 i = 0; i < index_count; ++i)
      {
        const u16 index = BE16(bytes.data() + index_start + static_cast<std::size_t>(i) * 2);
        if (index >= vertex_count)
          return false;
        cluster.indices.push_back(index);
      }

      out->total_vertices += vertex_count;
      out->total_indices += index_count;
      out->clusters.push_back(std::move(cluster));
      const std::size_t next = (index_start + index_bytes + 15) & ~std::size_t(15);
      if (next > bytes.size())
        return false;
      geometry = static_cast<u32>(next);
    }
  }

  out->valid = !out->skin_groups.empty() && !out->clusters.empty();
  return out->valid;
}

EMTInfo InspectEMT(std::span<const u8> bytes)
{
  EMTInfo out;
  if (bytes.size() < 0x20)
    return out;
  const bool be = bytes[0] == 'E' && bytes[1] == 'M' && bytes[2] == 'T' && bytes[3] == 0;
  const bool le = bytes[0] == 0 && bytes[1] == 'T' && bytes[2] == 'M' && bytes[3] == 'E';
  if (!be && !le)
    return out;
  const auto U32 = [be](const u8* q) { return be ? BE32(q) : LE32(q); };
  out.big_endian = be;
  out.version = U32(bytes.data() + 4);
  const u32 raw_entity_count = U32(bytes.data() + 0x0c);
  out.entity_count = raw_entity_count;
  // PS3 remaster EMT stores the 16-bit entity count in the high half of the
  // big-endian word (2_1: 6A 64 00 00 -> 27236 entities). Retail PS2/Xbox
  // references use a full 32-bit field, so accept both layouts explicitly.
  if (be && raw_entity_count > 100000 && (raw_entity_count & 0xffffu) == 0)
    out.entity_count = raw_entity_count >> 16;
  out.section_a = U32(bytes.data() + 0x10);
  out.section_b = U32(bytes.data() + 0x14);
  out.section_c = U32(bytes.data() + 0x18);
  if (out.version != 1 || out.entity_count > 100000 || out.section_a < 0x20 ||
      out.section_b < 0x20 || out.section_c < 0x20 || out.section_a >= bytes.size() ||
      out.section_b >= bytes.size() || out.section_c >= bytes.size())
    return {};

  for (std::size_t i = 0; i + 4 <= bytes.size(); ++i)
    if (bytes[i] == 'L' && bytes[i + 1] == 'E' && bytes[i + 2] == 'K' && bytes[i + 3] == 'S')
      ++out.leks_blocks;
  out.valid = true;
  return out;
}

DMFInfo InspectDMF(std::span<const u8> bytes)
{
  DMFInfo out;

  if (!PS3AssetPort::IsDMFEnabled())
    return out;

  // Binary magic is D M F NUL.  "DMF\\0" is five source characters and
  // comparing its first four bytes checks D M F '\\', rejecting every valid
  // Frontline DMF before the PS3 0x0502 parser can run.
  static constexpr std::array<u8, 4> dmf_magic = {'D', 'M', 'F', 0};
  if (bytes.size() < 0x5c ||
      !std::equal(dmf_magic.begin(), dmf_magic.end(), bytes.begin()))
  {
    return out;
  }

  // Frontline PS3 remaster DMF revision seen in the RPCS3 dump:
  // bytes +04 = 05 02 00 00 -> revision 0x0502 + flags 0x0000.
  const u32 ps3_revision =
      (u32(bytes[4]) << 8) |
      u32(bytes[5]);

  const u32 ps3_flags =
      (u32(bytes[6]) << 8) |
      u32(bytes[7]);

  if (ps3_revision >= 0x0100 &&
      ps3_revision <= 0x0FFF &&
      ps3_flags <= 0x00FF)
  {
    out.version =
        ps3_revision;

    out.mesh_count =
        BE32(bytes.data() + 0x20);

    out.material_count =
        BE32(bytes.data() + 0x28);

    // +0x48 = count.
    // +0x4C is the pointer/table field and MUST NOT be read as a count.
    out.bone_ref_count =
        BE32(bytes.data() + 0x48);
  }
  else
  {
    // Preserve conservative support for the older documented DMF family.
    const u32 version_le =
        LE32(bytes.data() + 4);

    const u32 version_be =
        BE32(bytes.data() + 4);

    if (version_le > 0 &&
        version_le < 64)
    {
      out.version =
          version_le;

      out.mesh_count =
          LE32(bytes.data() + 0x20);

      out.material_count =
          LE32(bytes.data() + 0x28);

      out.bone_ref_count =
          LE32(bytes.data() + 0x4c);
    }
    else if (version_be > 0 &&
             version_be < 64)
    {
      out.version =
          version_be;

      out.mesh_count =
          BE32(bytes.data() + 0x20);

      out.material_count =
          BE32(bytes.data() + 0x28);

      out.bone_ref_count =
          BE32(bytes.data() + 0x4c);
    }
    else
    {
      return out;
    }
  }

  char name[9]{};

  std::memcpy(
      name,
      bytes.data() + 0x0c,
      8);

  out.model_name =
      name;

  if (out.mesh_count > 65535 ||
      out.material_count > 65535 ||
      out.bone_ref_count > 4096)
  {
    return {};
  }

  out.valid = true;

  return out;
}

SKLInfo InspectSKL(std::span<const u8> bytes)
{
  SKLInfo out;
  if (!PS3AssetPort::IsSKLEnabled() || bytes.size() < 0x20)
    return out;

  // Frontline skeletons occur in both byte orders across platforms.
  // PS3 samples use the readable "SKL1" form, while older little-endian
  // assets may appear as "1LKS".  Parse both without raw replacement.
  const bool be =
      bytes[0] == 'S' && bytes[1] == 'K' && bytes[2] == 'L' && bytes[3] == '1';
  const bool le =
      bytes[0] == '1' && bytes[1] == 'L' && bytes[2] == 'K' && bytes[3] == 'S';
  if (!be && !le)
    return out;

  const auto U32 = [be](const u8* p) { return be ? BE32(p) : LE32(p); };
  const u32 count_word = U32(bytes.data() + 4);
  u32 bone_count = count_word >> 16;
  if (!bone_count)
    bone_count = count_word & 0xffff;

  const u32 bone_data = U32(bytes.data() + 8);
  const u32 names = U32(bytes.data() + 12);
  const u32 names_end = U32(bytes.data() + 20);

  if (!bone_count || bone_count > 512 || names < 0x20 || names >= bytes.size() ||
      bone_data > bytes.size() || names_end > bytes.size())
  {
    return out;
  }

  // Each known Frontline SKL name record is 20 bytes:
  // pointer/offset + char name[16].  The actual bone node stream is kept in
  // the original PS3 resource; only identity/hierarchy binding is decoded here.
  const std::size_t names_bytes = static_cast<std::size_t>(bone_count) * 20;
  if (names_bytes > bytes.size() - names)
    return out;

  out.big_endian = be;
  out.bone_count = bone_count;
  out.bone_data_offset = bone_data;
  out.names_offset = names;
  out.names_end = names_end;
  out.bone_names.reserve(bone_count);

  for (u32 i = 0; i < bone_count; ++i)
  {
    const std::size_t record = names + static_cast<std::size_t>(i) * 20;
    const std::string name = FixedString(bytes.data() + record + 4, 16);
    if (name.empty())
      return {};
    out.bone_names.push_back(name);
  }

  out.hierarchy = MOHFrontline::PS3::SkinBind::DecodeHierarchy(bytes);
  out.valid = true;
  return out;
}

Replacement ResolveReplacement(std::string_view gc_resource_name)
{
  std::string name = CanonicalModelName(gc_resource_name);

  if (name.ends_with(".msh"))
  {
    if (const StaticMesh* mesh = FindCachedMSH(name))
      return {ReplacementKind::StaticMSH, mesh, nullptr, nullptr};
  }
  else if (name.ends_with(".dmf") || name.ends_with(".dmt"))
  {
    const std::string dmf_name = CanonicalDMFName(name);
    if (const DMFResource* mesh = FindCachedDMF(dmf_name))
      return {ReplacementKind::SkinnedDMF, nullptr, mesh, nullptr};
  }
  else if (name.ends_with(".skl"))
  {
    if (const SKLInfo* skeleton = FindCachedSKL(name))
      return {ReplacementKind::SkeletonSKL, nullptr, nullptr, skeleton};
  }

  return {};
}

bool IsHostRenderable(const StaticMesh& mesh)
{
  if (mesh.submeshes.empty())
    return false;

  for (const auto& sub : mesh.submeshes)
  {
    if (sub.position_uv.size() != sub.vertex_count || sub.indices.empty() || !sub.vertex_stride)
      return false;

    // Require a position declaration. Semantic 0 is the position field in
    // the PS3 MSH v8 files observed in Frontline.
    const bool has_position =
        std::any_of(sub.attributes.begin(), sub.attributes.end(),
                    [](const Attribute& a) { return a.semantic == 0 && a.components >= 3; });
    if (!has_position)
      return false;
  }

  return true;
}
}  // namespace PS3MeshPort
