#include "VideoCommon/PS3MeshPort.h"
#include "Common/Hash.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include <atomic>
#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/MSH.h"
#include "VideoCommon/PS3RemasterAssets.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
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

float LEFloat(const u8* p)
{
  const u32 bits = LE32(p);
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
struct ActiveDisplayListContext
{
  u32 address = 0;
  u32 size = 0;
  u64 command_hash = 0;

  explicit operator bool() const { return address != 0 && size > 52; }
};
thread_local ActiveDisplayListContext g_active_display_list;
std::atomic<u64> g_matches{0}, g_draws{0}, g_vertices{0}, g_indices{0}, g_fallbacks{0};
std::unordered_set<std::string> g_draw_logged;
std::atomic<u64> g_dmf_draws{0}, g_dmf_vertices{0}, g_dmf_indices{0};
std::unordered_set<std::string> g_dmf_draw_logged;

std::mutex g_dmf_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<DMFResource>> g_dmf_cache;
struct ExactDMFPair
{
  std::shared_ptr<DMFResource> ps3;
  u32 gc_size = 0;
  u32 gc_version_word = 0;
  std::vector<s16> ps3_group_to_gc;
  std::size_t mapped_skin_groups = 0;
  std::string skeleton_name;
  std::size_t skeleton_name_matches = 0;
};
std::unordered_map<std::string, ExactDMFPair> g_dmf_pairs;
struct OriginalGCDMFDrawCandidate
{
  std::shared_ptr<DMFResource> ps3;
  std::string gc_name;
  u32 file_size = 0;
  u32 group_count = 0;
  u32 group_offset = 0;
  u32 material_count = 0;
  u32 material_offset = 0;
  u32 dl_offset = 0;
  u32 material_index = 0;
  u32 cluster_index = 0;
  std::string gc_material_name;
  std::vector<u8> gc_palette_groups;
};
std::unordered_map<DisplayListSignatureKey, std::vector<OriginalGCDMFDrawCandidate>,
                   DisplayListSignatureKeyHash>
    g_dmf_display_list_candidates;
thread_local SkinnedDrawMatch g_current_dmf_draw;
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

  // The platform layouts preserve authored node/submesh order for the normal
  // case. This covers the overwhelming majority of Frontline MSH files and is
  // exact (no fuzzy runtime matching).
  if (nodes.size() == mesh.submeshes.size())
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

  const float maximum_score = EnvFloatLocal("MOH_PS3_MSH_NODE_SCORE", 0.10f, 0.001f, 1.0f);
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
  return BE32(header + 0x20) == candidate.group_count &&
         BE32(header + 0x24) == candidate.group_offset &&
         BE32(header + 0x28) == candidate.material_count &&
         BE32(header + 0x2c) == candidate.material_offset;
}

void ResolveDMFDisplayList(u32 address, std::span<const u8> commands)
{
  g_current_dmf_draw = {};
  if (!EnvSwitchLocal("MOH_PS3_DMF_DRAW", true) || commands.empty())
    return;

  const u32 runtime_address = address & 0x1fffffff;
  const DisplayListSignatureKey key{static_cast<u32>(commands.size()),
                                    Common::GetHash64(commands.data(), commands.size(), 0)};
  std::vector<OriginalGCDMFDrawCandidate> candidates;
  {
    std::scoped_lock lock(g_dmf_cache_mutex);
    const auto it = g_dmf_display_list_candidates.find(key);
    if (it == g_dmf_display_list_candidates.end())
      return;
    candidates = it->second;
  }

  const OriginalGCDMFDrawCandidate* resolved = nullptr;
  for (const auto& candidate : candidates)
  {
    if (candidates.size() > 1 && !ValidateOriginalGCDMFCandidate(candidate, runtime_address))
      continue;
    if (resolved)
    {
      static unsigned collision_logs = 0;
      if (collision_logs++ < 32)
        std::fprintf(stderr,
                     "[moh-ps3-dmf] GC DMF DL COLLISION unresolved: DL=%08x size=%u candidates=%zu -> keep GC\n",
                     runtime_address, key.size, candidates.size());
      return;
    }
    resolved = &candidate;
  }
  if (!resolved)
    return;

  g_current_dmf_draw.owner = resolved->ps3;
  g_current_dmf_draw.gc_name = resolved->gc_name;
  g_current_dmf_draw.display_list = runtime_address;
  g_current_dmf_draw.material_index = resolved->material_index;
  g_current_dmf_draw.cluster_index = resolved->cluster_index;
  g_current_dmf_draw.gc_material_name = resolved->gc_material_name;
  g_current_dmf_draw.gc_palette_groups = resolved->gc_palette_groups;
  {
    std::scoped_lock lock(g_dmf_cache_mutex);
    if (const auto it = g_dmf_pairs.find(resolved->gc_name); it != g_dmf_pairs.end())
    {
      g_current_dmf_draw.ps3_group_to_gc = it->second.ps3_group_to_gc;
      g_current_dmf_draw.skeleton_name = it->second.skeleton_name;
    }
  }

  static unsigned logs = 0;
  if (logs++ < 128)
  {
    std::fprintf(stderr,
                 "[moh-ps3-dmf] LIVE EXACT GC DMF: gc=%s ps3=%s material=%u(%s) cluster=%u DL=%08x size=%u palette=%zu mapped_groups=%zu skeleton=%s\n",
                 resolved->gc_name.c_str(),
                 resolved->ps3 ? resolved->ps3->source_name.c_str() : "<missing>",
                 resolved->material_index,
                 resolved->gc_material_name.empty() ? "<unnamed>" : resolved->gc_material_name.c_str(),
                 resolved->cluster_index, runtime_address, key.size,
                 resolved->gc_palette_groups.size(),
                 std::count_if(g_current_dmf_draw.ps3_group_to_gc.begin(),
                               g_current_dmf_draw.ps3_group_to_gc.end(),
                               [](s16 value) { return value >= 0; }),
                 g_current_dmf_draw.skeleton_name.empty() ? "<unbound>" : g_current_dmf_draw.skeleton_name.c_str());
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
      if (ready_logs++ < 64)
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

  const u32 gc_group_count = BE32(gc_bytes.data() + 0x20);
  const u32 gc_group_offset = BE32(gc_bytes.data() + 0x24);
  if (!gc_group_count || gc_group_count > 4096 || gc_group_offset > gc_bytes.size() ||
      static_cast<std::size_t>(gc_group_count) * 4 > gc_bytes.size() - gc_group_offset)
    return;

  const std::vector<std::string> gc_bones = ReadDMFBoneRefs(gc_bytes, true);
  if (gc_bones.empty())
    return;

  std::unordered_map<std::string, std::vector<u16>> gc_by_key;
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
  }

  const auto& decoded = *pair->ps3->decoded;
  pair->ps3_group_to_gc.assign(decoded.skin_groups.size(), -1);
  pair->mapped_skin_groups = 0;
  for (std::size_t i = 0; i < decoded.skin_groups.size(); ++i)
  {
    const auto& group = decoded.skin_groups[i];
    if (group.bone_a >= decoded.bone_refs.size() || group.bone_b >= decoded.bone_refs.size())
      continue;
    // Codex recovery probe established that PS3 stores the old 12-bit blend
    // coefficient as float/4096. Preserve truncation; do not renormalize it.
    const int blend_q = static_cast<int>(group.blend * 4096.0f);
    const auto it = gc_by_key.find(
        SkinGroupKey(decoded.bone_refs[group.bone_a], decoded.bone_refs[group.bone_b], blend_q));
    if (it == gc_by_key.end() || it->second.size() != 1)
      continue;
    pair->ps3_group_to_gc[i] = static_cast<s16>(it->second.front());
    ++pair->mapped_skin_groups;
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
  g_dmf_pairs.clear();
  g_dmf_display_list_candidates.clear();
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
      std::scoped_lock lock(g_dmf_cache_mutex);
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
          candidate.group_count = gc_groups;
          candidate.group_offset = gc_group_off;
          candidate.material_count = gc_materials;
          candidate.material_offset = gc_material_off;
          candidate.dl_offset = dl_offset;
          candidate.material_index = material;
          candidate.cluster_index = cluster;
          candidate.gc_material_name = gc_material_name;
          candidate.gc_palette_groups.assign(record + 8, record + 8 + palette_count);
          const DisplayListSignatureKey dl_key{
              dl_size, Common::GetHash64(bytes + dl_offset, dl_size, 0)};
          g_dmf_display_list_candidates[dl_key].push_back(std::move(candidate));
        }
      }
    }

    g_dmf_pairs[filename] = pair;
    ++paired;
    if (pair_logs++ < 32)
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
               "[moh-ps3-dmf] GC/PS3 exact pair index ready: level=%.*s gc_dmf=%zu paired=%zu missing_ps3=%zu rejected=%zu dl_signature_keys=%zu aliases=.dmf/.dmt | exact draw identity + skin groups ready; GC draw retained until animated palette skinning succeeds\n",
               static_cast<int>(level.size()), level.data(), gc_dmf, paired, missing, rejected,
               g_dmf_display_list_candidates.size());
}
}  // namespace

void ClearMSHCache()
{
  std::scoped_lock lock(g_msh_cache_mutex);
  g_msh_cache.clear();
  g_display_lists.clear();
  g_display_list_candidates.clear();
}

void ClearDMFCache()
{
  std::scoped_lock lock(g_dmf_cache_mutex);
  g_dmf_cache.clear();
  g_dmf_pairs.clear();
  g_dmf_display_list_candidates.clear();
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

  std::unordered_map<std::string, std::shared_ptr<StaticMesh>> next;
  std::size_t candidates = 0;
  std::size_t decoded = 0;
  std::size_t rejected = 0;

  for (const auto& asset : PS3RemasterAssets::GetAssets())
  {
    const std::string filename = Lower(asset.filename);
    if (!filename.ends_with(".msh") || !BelongsToLevel(asset, level))
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
    if (!ParseMSHv8(bytes, mesh.get()) || !IsHostRenderable(*mesh))
    {
      ++rejected;
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
    next[Lower(asset.relative_path)] = mesh;
  }

  {
    std::scoped_lock lock(g_msh_cache_mutex);
    g_display_lists.clear();
    g_msh_cache = std::move(next);
  }

  std::fprintf(stderr,
               "[moh-ps3-msh] ALL-MSH cache ready: level=%.*s candidates=%zu decoded=%zu rejected=%zu keys=%zu\n",
               static_cast<int>(level.size()), level.data(), candidates, decoded, rejected,
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
    const std::string filename = Lower(asset.filename);
    if (!filename.ends_with(".dmf") || !BelongsToLevel(asset, level))
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
      }
      // Keep the exact resource/name available even when a platform-layout
      // variant is not yet geometry-decodable. It must remain GC-rendered.
      auto resource = std::make_shared<DMFResource>();
      resource->info = info;
      resource->source_name = asset.relative_path;
      resource->bytes = std::make_shared<const std::vector<u8>>(bytes);
      next[filename] = resource;
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
      if (emt_reject_logs++ < 8)
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

bool IsStaticDrawReplacementEnabled()
{
  return PS3AssetPort::IsMSHEnabled() && EnvSwitchLocal("MOH_PS3_MSH_DRAW", true) &&
         EnvSwitchLocal("MOH_PS3_MSH_REPLACE", true);
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

  const auto* asset = PS3RemasterAssets::FindByRelativePath(scope + "level.viv::" + filename);
  if (!asset)
    asset = PS3RemasterAssets::FindByRelativePath(scope + filename);
  if (!asset)
    return;

  std::shared_ptr<StaticMesh> mesh;
  {
    std::scoped_lock lock(g_msh_cache_mutex);
    if (auto it = g_msh_cache.find(Lower(asset->relative_path)); it != g_msh_cache.end())
      mesh = it->second;
    else if (auto it = g_msh_cache.find(filename); it != g_msh_cache.end())
      mesh = it->second;
  }
  if (!mesh)
  {
    mesh = std::make_shared<StaticMesh>();
    mesh->source_name = asset->relative_path;
    if (!ParseMSHv8(PS3RemasterAssets::ReadBinary(*asset), mesh.get()) || !IsHostRenderable(*mesh))
      return;
    std::scoped_lock lock(g_msh_cache_mutex);
    g_msh_cache[filename] = mesh;
    g_msh_cache[Lower(asset->relative_path)] = mesh;
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
    if (register_logs++ < 32)
      std::fprintf(stderr,
                   "[moh-ps3-msh] LIVE EXACT MESH RESOURCE: level=%s gc=%s ps3=%s nodes=%zu submeshes=%zu registered=%zu base=%08x\n",
                   level.c_str(), filename.c_str(), asset->relative_path.c_str(), nodes.size(),
                   mesh->submeshes.size(), registered, address);
  }
}

StaticDrawMatch FindDisplayList(u32 address, std::span<const u8> commands)
{
  ResolveDMFDisplayList(address, commands);
  if (!IsStaticDrawReplacementEnabled() || commands.size() <= 52)
    return {};

  const u32 runtime_address = address & 0x1fffffff;
  const u32 size = static_cast<u32>(commands.size());
  const u64 command_hash =
      Common::GetHash64(commands.data() + 52, commands.size() - 52, 0);

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
        if (ambiguous_logs++ < 32)
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
  if (signature_logs++ < 128)
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
  g_current_draw = {};
  g_current_dmf_draw = {};
  g_active_display_list = {};
  if (!IsStaticDrawReplacementEnabled() || !address || commands.size() <= 52)
    return;

  g_active_display_list.address = address & 0x1fffffff;
  g_active_display_list.size = static_cast<u32>(commands.size());
  g_active_display_list.command_hash =
      Common::GetHash64(commands.data() + 52, commands.size() - 52, 0);
}

void SetDisplayListMatch(StaticDrawMatch match) { g_current_draw = std::move(match); }

SkinnedDrawMatch CurrentSkinnedDraw() { return g_current_dmf_draw; }

SkinnedPaletteAnalysis AnalyzeCurrentSkinnedPalette()
{
  SkinnedPaletteAnalysis analysis;
  const SkinnedDrawMatch draw = g_current_dmf_draw;
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
    std::scoped_lock lock(g_dmf_cache_mutex);
    for (const auto& [key, candidates] : g_dmf_display_list_candidates)
    {
      (void)key;
      for (const auto& candidate : candidates)
      {
        if (candidate.gc_name != draw.gc_name ||
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

  auto palette_contains = [](const std::vector<u8>& palette,
                             const std::array<s16, 3>& groups) {
    for (const s16 group : groups)
    {
      if (group < 0 || group > 255 ||
          std::find(palette.begin(), palette.end(), static_cast<u8>(group)) == palette.end())
        return false;
    }
    return true;
  };

  std::unordered_set<u64> selected_vertices;
  const auto& decoded = *draw.owner->decoded;
  for (std::size_t cluster_ordinal = 0; cluster_ordinal < decoded.clusters.size();
       ++cluster_ordinal)
  {
    const DMFCluster& cluster = decoded.clusters[cluster_ordinal];
    if (cluster.material_name != draw.gc_material_name)
      continue;
    ++analysis.ps3_material_clusters;

    for (std::size_t tri = 0; tri + 2 < cluster.indices.size(); tri += 3)
    {
      ++analysis.total_triangles;
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
        ++analysis.unmapped_triangles;
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
        ++analysis.unmapped_triangles;
        continue;
      }
      if (matching_palettes != 1)
      {
        ++analysis.ambiguous_triangles;
        continue;
      }
      if (*unique_palette != draw.gc_palette_groups)
        continue;

      ++analysis.selected_triangles;
      for (const u16 vertex : vertex_indices)
      {
        selected_vertices.insert((u64(cluster_ordinal) << 32) | u64(vertex));
      }
    }
  }

  analysis.selected_vertices = selected_vertices.size();
  analysis.matrix_slots = draw.gc_palette_groups.size();
  analysis.valid = analysis.ps3_material_clusters != 0 && analysis.selected_triangles != 0;
  return analysis;
}

SkinnedDrawReplacement BuildCurrentSkinnedReplacement()
{
  SkinnedDrawReplacement replacement;
  if (!EnvSwitchLocal("MOH_PS3_DMF_REPLACE", true))
    return replacement;

  // v16.9 proved that exact draw identity + a finite GX/XF palette is not
  // sufficient to reuse the GameCube skin matrix directly on PS3 bind-pose
  // vertices.  The matrix loaded by the GC renderer already contains the GC
  // bind-pose correction.  A PS3 DMF may use a different bind pose, so feeding
  // its bind vertices to that matrix can produce floating/exploded pieces even
  // when material/palette matching is otherwise exact.
  //
  // Keep the whole DMF/SKL/EMT identification and palette validation pipeline
  // active, but make the old direct-GC-skin path an explicit unsafe opt-in
  // until we apply PS3 inverse-bind (or reconstruct current bone-world)
  // matrices.  This makes v16.9.1 safe by default while preserving the useful
  // instrumentation for the real bind-correction implementation.
  if (!EnvSwitchLocal("MOH_PS3_DMF_UNSAFE_DIRECT_GC_SKIN", false))
  {
    static std::unordered_set<std::string> s_bind_gate_logged;
    const SkinnedDrawMatch& blocked = g_current_dmf_draw;
    if (blocked && s_bind_gate_logged.insert(blocked.gc_name).second)
    {
      std::fprintf(stderr,
                   "[moh-ps3-dmf] BIND SAFETY GATE: gc=%s ps3=%s | direct GC skin matrix -> PS3 bind vertices disabled; GC draw preserved (set MOH_PS3_DMF_UNSAFE_DIRECT_GC_SKIN=1 only to reproduce v16.9 experimental rendering)\n",
                   blocked.gc_name.c_str(),
                   blocked.owner ? blocked.owner->source_name.c_str() : "<none>");
    }
    return replacement;
  }

  const SkinnedDrawMatch draw = g_current_dmf_draw;
  if (!draw || !draw.owner || !draw.owner->decoded || draw.gc_material_name.empty() ||
      draw.ps3_group_to_gc.empty() || draw.gc_palette_groups.empty())
    return replacement;

  // v16.9 proof-of-life is deliberately strict.  Only a material with one
  // authored GC display list and one PS3 cluster can replace the current draw.
  // This excludes shared materials such as gi_256/mohf_body whose triangles
  // need a later per-DL partition fingerprint.
  std::unordered_set<u32> gc_draw_offsets;
  {
    std::scoped_lock lock(g_dmf_cache_mutex);
    for (const auto& [key, candidates] : g_dmf_display_list_candidates)
    {
      (void)key;
      for (const auto& candidate : candidates)
      {
        if (candidate.gc_name == draw.gc_name &&
            candidate.gc_material_name == draw.gc_material_name)
          gc_draw_offsets.insert(candidate.dl_offset);
      }
    }
  }
  replacement.gc_material_draws = gc_draw_offsets.size();
  if (replacement.gc_material_draws != 1)
    return {};

  const DMFCluster* ps3_cluster = nullptr;
  std::size_t ps3_clusters = 0;
  for (const DMFCluster& cluster : draw.owner->decoded->clusters)
  {
    if (cluster.material_name != draw.gc_material_name)
      continue;
    ++ps3_clusters;
    ps3_cluster = &cluster;
  }
  if (ps3_clusters != 1 || !ps3_cluster || !ps3_cluster->has_position ||
      !ps3_cluster->has_normal || !ps3_cluster->has_uv0 ||
      ps3_cluster->positions.size() != ps3_cluster->vertex_palette_slots.size() ||
      ps3_cluster->normals.size() != ps3_cluster->positions.size() ||
      ps3_cluster->uv0.size() != ps3_cluster->positions.size() ||
      ps3_cluster->indices.empty() || (ps3_cluster->indices.size() % 3) != 0)
    return {};

  const SkinnedPaletteAnalysis analysis = AnalyzeCurrentSkinnedPalette();
  if (!analysis.valid || analysis.ps3_material_clusters != 1 ||
      analysis.selected_triangles != analysis.total_triangles ||
      analysis.ambiguous_triangles != 0 || analysis.unmapped_triangles != 0)
    return {};

  replacement.owner = draw.owner;
  replacement.cluster = ps3_cluster;
  replacement.position_matrix_indices.reserve(ps3_cluster->positions.size());
  for (std::size_t vertex = 0; vertex < ps3_cluster->positions.size(); ++vertex)
  {
    const u16 local_slot = ps3_cluster->vertex_palette_slots[vertex];
    if (local_slot >= ps3_cluster->palette_groups.size())
      return {};
    const u16 ps3_group = ps3_cluster->palette_groups[local_slot];
    if (ps3_group >= draw.ps3_group_to_gc.size())
      return {};
    const s16 gc_group = draw.ps3_group_to_gc[ps3_group];
    if (gc_group < 0 || gc_group > 255)
      return {};

    const auto it = std::find(draw.gc_palette_groups.begin(), draw.gc_palette_groups.end(),
                              static_cast<u8>(gc_group));
    if (it == draw.gc_palette_groups.end())
      return {};
    const std::size_t palette_slot =
        static_cast<std::size_t>(std::distance(draw.gc_palette_groups.begin(), it));
    if (palette_slot >= 10)
      return {};
    replacement.position_matrix_indices.push_back(static_cast<u8>(palette_slot * 3));
  }

  return replacement;
}

StaticDrawMatch MatchStaticDraw(std::span<const u8> gc_vertices, u32 count, u32 stride, u32 offset)
{
  if (!IsStaticDrawReplacementEnabled() || count < 3)
    return {};

  const auto gc = BoundsFromGC(gc_vertices, count, stride, offset);
  if (!gc.valid)
    return {};

  if (g_current_draw)
  {
    // Exact runtime address + display-list size + command-tail hash identifies
    // the authored GameCube MSH command stream. A per-batch bounds comparison
    // is invalid here because a GX batch is not the whole PS3 submesh.
    return g_current_draw;
  }

  // The loader hostcall used by the texture/font path does not see static-MSH
  // loads in Frontline, so there is no resource address to seed g_display_lists.
  // Bootstrap the identity once, inside the *actual* GX display list, using the
  // previous strict geometry test. Once learned, later frames use only
  // display-list address + command hash.
  if (!g_active_display_list || !EnvSwitchLocal("MOH_PS3_MSH_BOOTSTRAP", true))
    return {};

  const float maximum_score =
      EnvFloatLocal("MOH_PS3_MSH_BOOTSTRAP_SCORE", 0.075f, 0.001f, 1.0f);
  const float minimum_margin =
      EnvFloatLocal("MOH_PS3_MSH_BOOTSTRAP_MARGIN", 0.015f, 0.0f, 1.0f);

  float best_score = std::numeric_limits<float>::infinity();
  float second_score = std::numeric_limits<float>::infinity();
  std::shared_ptr<StaticMesh> best_owner;

  {
    std::unordered_set<const StaticMesh*> seen;
    std::scoped_lock lock(g_msh_cache_mutex);
    for (const auto& [key, holder] : g_msh_cache)
    {
      (void)key;
      if (!holder || !seen.insert(holder.get()).second)
        continue;

      // Safe bootstrap milestone: one PS3 submesh only. This avoids assigning
      // one learned display-list identity to several unrelated material batches.
      if (holder->submeshes.size() != 1)
        continue;

      const auto& submesh = holder->submeshes[0];

      // IMPORTANT: gc `count` is the expanded GX vertex stream for this draw,
      // not the number of unique vertices in the source MSH. Triangle strips
      // and indexed GameCube display lists can therefore produce a completely
      // different count from the PS3 mesh while still describing the exact
      // same object-space geometry. Use bounds for identity bootstrap instead.
      if (submesh.position_uv.size() != submesh.vertex_count ||
          submesh.indices.empty() || submesh.indices.size() % 3)
        continue;

      const float score = BoundsScore(gc, BoundsFromPS3(submesh));
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

  if (!best_owner || best_score > maximum_score ||
      (std::isfinite(second_score) && second_score - best_score < minimum_margin))
  {
    static thread_local unsigned bootstrap_miss_logs = 0;
    if (bootstrap_miss_logs++ < 24)
    {
      const u32 best_ps3_vertices =
          best_owner && !best_owner->submeshes.empty() ? best_owner->submeshes[0].vertex_count : 0;
      const std::size_t best_ps3_indices =
          best_owner && !best_owner->submeshes.empty() ? best_owner->submeshes[0].indices.size() : 0;
      std::fprintf(stderr,
                   "[moh-ps3-msh] BOOTSTRAP MISS: gx_stream_verts=%u best=%s ps3_vertices=%u ps3_indices=%zu score=%.5f second=%.5f max=%.5f margin=%.5f DL=%08x\n",
                   count, best_owner ? best_owner->source_name.c_str() : "none",
                   best_ps3_vertices, best_ps3_indices, best_score, second_score, maximum_score,
                   minimum_margin, g_active_display_list.address);
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
  if (!g_current_draw) return;
  ++g_draws;
  g_vertices += g_current_draw.submesh->vertex_count;
  g_indices += g_current_draw.submesh->indices.size();
  if (g_draw_logged.insert(g_current_draw.mesh->source_name).second)
    std::fprintf(stderr, "[moh-ps3-msh] FIRST REAL PS3 DRAW: gc_resource=%08x ps3=%s DL=%08x | replacement active; current GX model/view/projection and CSM preserved\n",
                 g_current_draw.guest_resource, g_current_draw.mesh->source_name.c_str(),
                 g_current_draw.display_list);
}

void NotifySkinnedDrawSubmitted(const SkinnedDrawReplacement& replacement)
{
  const SkinnedDrawMatch draw = g_current_dmf_draw;
  if (!draw || !replacement || !replacement.cluster)
    return;

  ++g_dmf_draws;
  g_dmf_vertices += replacement.cluster->positions.size();
  g_dmf_indices += replacement.cluster->indices.size();
  const std::string key = draw.gc_name + "|" + draw.gc_material_name;
  if (g_dmf_draw_logged.insert(key).second)
  {
    std::fprintf(stderr,
                 "[moh-ps3-dmf] FIRST REAL PS3 SKINNED DRAW: gc=%s ps3=%s material=%s vertices=%zu indices=%zu palette=%zu DL=%08x | GX/XF animation palette + current TPK/GX state preserved\n",
                 draw.gc_name.c_str(), replacement.owner->source_name.c_str(),
                 draw.gc_material_name.c_str(), replacement.cluster->positions.size(),
                 replacement.cluster->indices.size(), draw.gc_palette_groups.size(),
                 draw.display_list);
  }
}

void PrintDrawStatistics()
{
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
  if (ok && out && logs++ < 64)
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

  out->skin_groups.reserve(group_count);
  for (u32 i = 0; i < group_count; ++i)
  {
    const u8* q = bytes.data() + group_offset + static_cast<std::size_t>(i) * 28;
    DMFSkinGroup group;
    group.bone_a = q[0];
    group.bone_b = q[4];
    group.blend = BEFloat(q + 8);
    for (std::size_t j = 0; j < group.auxiliary.size(); ++j)
      group.auxiliary[j] = BEFloat(q + 12 + j * 4);
    if (group.bone_a >= bone_count || group.bone_b >= bone_count || !std::isfinite(group.blend) ||
        group.blend < -0.01f || group.blend > 1.01f)
      return false;
    for (float value : group.auxiliary)
      if (!std::isfinite(value))
        return false;
    out->skin_groups.push_back(group);
  }

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
      cluster.vertex_stride = bytes[geometry + 8];
      cluster.attribute_word_count = bytes[geometry + 9];
      if (!index_count || !vertex_count || index_count > 16 * 1024 * 1024 ||
          vertex_count > 4 * 1024 * 1024 || (index_count % 3) != 0 ||
          cluster.vertex_stride < 30 || cluster.vertex_stride > 192 ||
          cluster.attribute_word_count > 32)
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

      cluster.vertices.assign(bytes.begin() + vertex_start, bytes.begin() + index_start);
      cluster.vertex_palette_slots.reserve(vertex_count);
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
        const u16 slot = BE16(vertex + 28);
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
