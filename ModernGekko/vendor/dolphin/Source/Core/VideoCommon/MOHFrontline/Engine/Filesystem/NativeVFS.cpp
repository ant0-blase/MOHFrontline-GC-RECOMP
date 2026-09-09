#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeVFS.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "Core/HW/DVD/MOHNativeVFSBridge.h"
#include "VideoCommon/MOHFrontline/Engine/Audio/NativeAudio.h"
#include "VideoCommon/PS3RemasterAssets.h"

namespace MOHFrontline::NativeVFS
{
namespace
{
std::mutex s_mutex;
bool s_initialized = false;
std::filesystem::path s_explicit_gc_root;
std::vector<std::filesystem::path> s_gc_roots;
struct GCCacheEntry
{
  std::filesystem::path path;
  std::uint64_t size = 0;
};
std::unordered_map<std::string, GCCacheEntry> s_gc_path_cache;

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string NormalizeGuest(std::string_view input)
{
  std::string path(input);
  std::replace(path.begin(), path.end(), '\\', '/');

  std::string lower = Lower(path);
  for (std::string_view prefix : {"dvd:", "cd:"})
  {
    if (lower.rfind(prefix, 0) == 0)
    {
      path.erase(0, prefix.size());
      break;
    }
  }

  while (path.rfind("./", 0) == 0)
    path.erase(0, 2);
  while (!path.empty() && path.front() == '/')
    path.erase(path.begin());

  // Never let a guest path escape a configured host asset root.
  std::filesystem::path checked;
  for (const auto& component : std::filesystem::path(path))
  {
    if (component == "..")
      return {};
    if (component == "." || component.empty())
      continue;
    checked /= component;
  }
  return checked.generic_string();
}

bool SamePathString(const std::filesystem::path& a, const std::filesystem::path& b)
{
  return Lower(a.lexically_normal().generic_string()) ==
         Lower(b.lexically_normal().generic_string());
}

void AddRoot(std::vector<std::filesystem::path>& roots, std::filesystem::path root)
{
  if (root.empty())
    return;

  root = root.lexically_normal();
  if (std::none_of(roots.begin(), roots.end(),
                   [&](const auto& existing) { return SamePathString(existing, root); }))
    roots.push_back(std::move(root));
}

std::vector<std::filesystem::path> BuildRoots()
{
  std::vector<std::filesystem::path> roots;

  if (!s_explicit_gc_root.empty())
    AddRoot(roots, s_explicit_gc_root);

  for (const char* name : {"MOH_GC_FILES", "MOH_NATIVE_GC_ROOT"})
  {
    if (const char* value = std::getenv(name); value && *value)
      AddRoot(roots, value);
  }

  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec)
  {
    AddRoot(roots, cwd / "GC_FILES");
    AddRoot(roots, cwd / "HD" / "GC_FILES");
    AddRoot(roots, cwd / "extracted" / "files");
    AddRoot(roots, cwd / "extracted");
    AddRoot(roots, cwd / "files");
  }

  return roots;
}

bool ResolveCaseInsensitive(const std::filesystem::path& root,
                            const std::filesystem::path& relative,
                            std::filesystem::path* out)
{
  if (!out || root.empty() || relative.empty())
    return false;

  std::error_code ec;
  std::filesystem::path current = root;
  if (!std::filesystem::is_directory(current, ec) || ec)
    return false;

  for (const auto& component : relative)
  {
    if (component == "." || component.empty())
      continue;
    if (component == "..")
      return false;

    const auto exact = current / component;
    ec.clear();
    if (std::filesystem::exists(exact, ec) && !ec)
    {
      current = exact;
      continue;
    }

    const std::string wanted = Lower(component.string());
    bool found = false;
    ec.clear();
    for (std::filesystem::directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec))
    {
      if (Lower(it->path().filename().string()) == wanted)
      {
        current = it->path();
        found = true;
        break;
      }
    }
    if (!found || ec)
      return false;
  }

  ec.clear();
  if (!std::filesystem::is_regular_file(current, ec) || ec)
    return false;

  *out = std::move(current);
  return true;
}

Policy ParsePolicy()
{
  const char* value = std::getenv("MOH_NATIVE_VFS");
  if (!value || !*value)
    return Policy::Auto;

  const std::string v = Lower(value);
  if (v == "ps3" || v == "ps3-first" || v == "ps3_first")
    return Policy::PS3First;
  if (v == "gc" || v == "gc-first" || v == "gc_first")
    return Policy::GCFirst;
  if (v == "ps3-only" || v == "ps3_only")
    return Policy::PS3Only;
  if (v == "gc-only" || v == "gc_only")
    return Policy::GCOnly;
  return Policy::Auto;
}

File ResolvePS3(std::string_view guest_path, PS3AssetPort::Class wanted)
{
  const auto match = PS3AssetPort::Resolve(guest_path, wanted);
  if (!match || !match.asset)
    return {};

  File out;
  out.source = Source::PlayStation3Host;
  out.asset_class = match.asset_class;
  out.ps3 = match;
  out.guest_path = std::string(guest_path);
  out.resolved_path = match.normalized_path;
  out.size = static_cast<std::uint64_t>(match.asset->size);
  return out;
}

File ResolveGC(std::string_view guest_path, PS3AssetPort::Class wanted)
{
  const std::string normalized = NormalizeGuest(guest_path);
  if (normalized.empty())
    return {};

  const std::filesystem::path relative(normalized);

  std::vector<std::filesystem::path> roots;
  {
    std::scoped_lock lock(s_mutex);
    if (const auto cached = s_gc_path_cache.find(normalized);
        cached != s_gc_path_cache.end())
    {
      File out;
      out.source = Source::GameCubeHost;
      out.asset_class =
          wanted == PS3AssetPort::Class::Unknown ? PS3AssetPort::Classify(guest_path) : wanted;
      out.host_path = cached->second.path;
      out.guest_path = std::string(guest_path);
      out.resolved_path = cached->second.path.generic_string();
      out.size = cached->second.size;
      return out;
    }
    roots = s_gc_roots;
  }

  for (const auto& root : roots)
  {
    std::filesystem::path path;
    if (!ResolveCaseInsensitive(root, relative, &path))
      continue;

    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec)
      continue;

    File out;
    out.source = Source::GameCubeHost;
    out.asset_class =
        wanted == PS3AssetPort::Class::Unknown ? PS3AssetPort::Classify(guest_path) : wanted;
    out.host_path = path;
    out.guest_path = std::string(guest_path);
    out.resolved_path = path.generic_string();
    out.size = static_cast<std::uint64_t>(file_size);
    {
      std::scoped_lock lock(s_mutex);
      s_gc_path_cache[normalized] = {path, out.size};
    }
    return out;
  }

  return {};
}

bool DiscReadCallback(std::string_view guest_path, u64 file_offset, std::span<u8> destination)
{
  // The guest must keep receiving GameCube-compatible bytes. PS3 resources
  // are consumed by semantic native loaders (textures/audio/meshes), never
  // injected raw into the original GC loader.
  NativeAudio::NotifyGuestRead(guest_path, file_offset);

  if (const char* value = std::getenv("MOH_NATIVE_VFS_DISC"); value && *value)
  {
    const std::string enabled = Lower(value);
    if (enabled == "0" || enabled == "false" || enabled == "off" || enabled == "no")
      return false;
  }

  const File file = ResolveGC(guest_path, PS3AssetPort::Class::Unknown);
  if (!file || !file.IsGC())
    return false;

  if (!ReadRange(file, file_offset, destination))
    return false;

  static std::uint64_t hits = 0;
  const std::uint64_t hit = ++hits;
  if (hit <= 256 || (hit % 1024) == 0)
  {
    std::fprintf(stderr,
                 "[moh-native-vfs] DVD HOST hit=%llu guest=%.*s off=0x%llx bytes=%zu -> %s\n",
                 static_cast<unsigned long long>(hit),
                 static_cast<int>(guest_path.size()), guest_path.data(),
                 static_cast<unsigned long long>(file_offset), destination.size(),
                 file.resolved_path.c_str());
  }
  return true;
}
}  // namespace

void Initialize()
{
  std::scoped_lock lock(s_mutex);
  if (s_initialized)
    return;

  s_gc_roots = BuildRoots();
  s_gc_path_cache.clear();
  s_initialized = true;
  DVD::SetMOHNativeVFSReadCallback(&DiscReadCallback);

  std::fprintf(stderr, "[moh-native-vfs] policy=%s gc_roots=%zu ps3_ready=%d disc_hook=ON\n",
               PolicyName(ParsePolicy()), s_gc_roots.size(),
               PS3RemasterAssets::IsReady() ? 1 : 0);
  for (const auto& root : s_gc_roots)
    std::fprintf(stderr, "[moh-native-vfs] GC root: %s\n", root.string().c_str());
}

void Shutdown()
{
  DVD::SetMOHNativeVFSReadCallback(nullptr);
  NativeAudio::Stop();

  std::scoped_lock lock(s_mutex);
  s_gc_roots.clear();
  s_gc_path_cache.clear();
  s_initialized = false;
}

Policy GetPolicy()
{
  return ParsePolicy();
}

const char* PolicyName(Policy policy)
{
  switch (policy)
  {
  case Policy::PS3First: return "ps3-first";
  case Policy::GCFirst: return "gc-first";
  case Policy::PS3Only: return "ps3-only";
  case Policy::GCOnly: return "gc-only";
  default: return "auto";
  }
}

const char* SourceName(Source source)
{
  switch (source)
  {
  case Source::GameCubeHost: return "GC-host";
  case Source::PlayStation3Host: return "PS3-host";
  default: return "none";
  }
}

void SetGameCubeRoot(std::filesystem::path root)
{
  {
    std::scoped_lock lock(s_mutex);
    s_explicit_gc_root = std::move(root);
    s_gc_roots = BuildRoots();
    s_gc_path_cache.clear();
    s_initialized = true;
  }
  DVD::SetMOHNativeVFSReadCallback(&DiscReadCallback);
}

std::filesystem::path GetGameCubeRoot()
{
  std::scoped_lock lock(s_mutex);
  return s_gc_roots.empty() ? std::filesystem::path{} : s_gc_roots.front();
}

File ResolveGameCube(std::string_view guest_path, PS3AssetPort::Class wanted)
{
  {
    std::scoped_lock lock(s_mutex);
    if (!s_initialized)
    {
      s_gc_roots = BuildRoots();
      s_initialized = true;
    }
  }
  return ResolveGC(guest_path, wanted);
}

File ResolvePlayStation3(std::string_view guest_path, PS3AssetPort::Class wanted)
{
  return ResolvePS3(guest_path, wanted);
}

File Resolve(std::string_view guest_path, PS3AssetPort::Class wanted)
{
  {
    std::scoped_lock lock(s_mutex);
    if (!s_initialized)
    {
      s_gc_roots = BuildRoots();
      s_initialized = true;
    }
  }

  const Policy policy = ParsePolicy();
  File result;

  switch (policy)
  {
  case Policy::PS3Only:
    result = ResolvePS3(guest_path, wanted);
    break;
  case Policy::GCOnly:
    result = ResolveGC(guest_path, wanted);
    break;
  case Policy::GCFirst:
    result = ResolveGC(guest_path, wanted);
    if (!result)
      result = ResolvePS3(guest_path, wanted);
    break;
  case Policy::PS3First:
    result = ResolvePS3(guest_path, wanted);
    if (!result)
      result = ResolveGC(guest_path, wanted);
    break;
  case Policy::Auto:
  default:
    // Prefer remaster assets when an exact semantic replacement exists; the
    // extracted GC tree remains a deterministic fallback.
    result = ResolvePS3(guest_path, wanted);
    if (!result)
      result = ResolveGC(guest_path, wanted);
    break;
  }

  static std::uint64_t logs = 0;
  if (result && logs++ < 256)
  {
    std::fprintf(stderr, "[moh-native-vfs] %.*s -> %s:%s size=%llu\n",
                 static_cast<int>(guest_path.size()), guest_path.data(),
                 SourceName(result.source), result.resolved_path.c_str(),
                 static_cast<unsigned long long>(result.size));
  }

  return result;
}

std::vector<u8> Read(const File& file)
{
  if (!file)
    return {};

  if (file.IsPS3())
    return PS3AssetPort::Read(file.ps3);

  std::ifstream stream(file.host_path, std::ios::binary | std::ios::ate);
  if (!stream)
    return {};

  const auto end = stream.tellg();
  if (end < 0)
    return {};

  std::vector<u8> bytes(static_cast<std::size_t>(end));
  stream.seekg(0, std::ios::beg);
  if (!bytes.empty())
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

  if (!stream && !bytes.empty())
    return {};
  return bytes;
}

bool ReadRange(const File& file, std::uint64_t offset, std::span<u8> destination)
{
  if (!file || offset > file.size ||
      destination.size() > static_cast<std::size_t>(file.size - offset))
    return false;

  if (destination.empty())
    return true;

  if (file.IsPS3())
  {
    if (!file.ps3.asset)
      return false;
    const auto bytes =
        PS3RemasterAssets::ReadBinaryRange(*file.ps3.asset, offset, destination.size());
    if (bytes.size() != destination.size())
      return false;
    std::copy(bytes.begin(), bytes.end(), destination.begin());
    return true;
  }

  std::ifstream stream(file.host_path, std::ios::binary);
  if (!stream)
    return false;

  stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream)
    return false;

  stream.read(reinterpret_cast<char*>(destination.data()),
              static_cast<std::streamsize>(destination.size()));
  return static_cast<std::size_t>(stream.gcount()) == destination.size();
}

std::string Describe(const File& file)
{
  if (!file)
    return "<native-vfs miss>";
  return std::string(SourceName(file.source)) + ":" + file.resolved_path;
}
}  // namespace MOHFrontline::NativeVFS
