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

#if defined(MOH_NATIVE_VFS_NOD)
#include <nod.h>
#endif

#include "Core/HW/DVD/MOHNativeVFSBridge.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeGCAssets.h"
#include "VideoCommon/MOHFrontline/Engine/Audio/NativeAudio.h"
#include "VideoCommon/MOHFrontline/Engine/Video/NativeVideo.h"
#include "VideoCommon/MOHFrontline/Engine/NativePCStatus.h"
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

std::string Lower(std::string value);
bool SamePathString(const std::filesystem::path& a, const std::filesystem::path& b);

#if defined(MOH_NATIVE_VFS_NOD)
struct NodFileEntry
{
  u32 index = 0;
  u32 size = 0;
};

struct NodDirFrame
{
  u32 end = 0;
  std::string path;
};

NodHandle* s_nod_disc = nullptr;
NodHandle* s_nod_partition = nullptr;
std::unordered_map<std::string, NodFileEntry> s_nod_files;
std::mutex s_nod_mutex;
std::filesystem::path s_nod_image;

void CloseNodDisc()
{
  std::scoped_lock lock(s_nod_mutex);
  nod_free(s_nod_partition);
  nod_free(s_nod_disc);
  s_nod_partition = nullptr;
  s_nod_disc = nullptr;
  s_nod_files.clear();
  s_nod_image.clear();
}

std::string NormalizeNodPath(std::string_view value)
{
  std::string path(value);
  std::replace(path.begin(), path.end(), '\\', '/');
  while (path.rfind("./", 0) == 0)
    path.erase(0, 2);
  while (!path.empty() && path.front() == '/')
    path.erase(path.begin());
  return Lower(std::move(path));
}

struct NodWalkContext
{
  std::vector<NodDirFrame> dirs;
  bool ok = true;
};

u32 NodWalk(u32 index, NodNodeKind kind, const char* raw_name, u32 size, void* user)
{
  auto& context = *static_cast<NodWalkContext*>(user);
  while (!context.dirs.empty() && index >= context.dirs.back().end)
    context.dirs.pop_back();

  const std::string name = raw_name ? std::string(raw_name) : std::string{};
  if (index == 0 && kind == NOD_NODE_KIND_DIRECTORY)
  {
    context.dirs.push_back({size, {}});
    return 1;
  }

  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
  {
    context.ok = false;
    return NOD_FST_STOP;
  }

  const std::string path =
      context.dirs.empty() ? name : context.dirs.back().path + "/" + name;
  if (kind == NOD_NODE_KIND_DIRECTORY)
  {
    if (size <= index)
    {
      context.ok = false;
      return NOD_FST_STOP;
    }
    context.dirs.push_back({size, path});
    return index + 1;
  }

  s_nod_files.emplace(Lower(path), NodFileEntry{index, size});
  return index + 1;
}

bool OpenNodDiscFromEnvironment()
{
  const char* value = std::getenv("MOH_NATIVE_DISC_IMAGE");
  if (!value || !*value)
    return false;

  const std::filesystem::path requested(value);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(requested, ec) || ec)
    return false;

  {
    std::scoped_lock lock(s_nod_mutex);
    if (s_nod_disc && SamePathString(s_nod_image, requested))
      return true;
  }

  CloseNodDisc();

  NodDiscOptions options{};
  options.preloader_threads = 2u;
  NodHandle* disc = nullptr;
  const auto utf8_path = requested.u8string();
  const std::string utf8(utf8_path.begin(), utf8_path.end());
  if (nod_disc_open(utf8.c_str(), &options, &disc) != NOD_RESULT_OK || !disc)
    return false;

  NodPartitionOptions partition_options{};
  NodHandle* partition = nullptr;
  if (nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA,
                                   &partition_options, &partition) != NOD_RESULT_OK ||
      !partition)
  {
    nod_free(disc);
    return false;
  }

  {
    std::scoped_lock lock(s_nod_mutex);
    s_nod_disc = disc;
    s_nod_partition = partition;
    s_nod_image = requested;
    s_nod_files.clear();
    NodWalkContext context;
    nod_partition_iterate_fst(s_nod_partition, &NodWalk, &context);
    if (!context.ok)
    {
      nod_free(s_nod_partition);
      nod_free(s_nod_disc);
      s_nod_partition = nullptr;
      s_nod_disc = nullptr;
      s_nod_files.clear();
      s_nod_image.clear();
      return false;
    }
  }

  std::fprintf(stderr, "[moh-native-vfs] nod mount: %s files=%zu\n",
               requested.string().c_str(), s_nod_files.size());
  return true;
}

File ResolveNod(std::string_view guest_path, PS3AssetPort::Class wanted)
{
  if (!OpenNodDiscFromEnvironment())
    return {};

  const std::string normalized = NormalizeNodPath(guest_path);
  if (normalized.empty())
    return {};

  std::scoped_lock lock(s_nod_mutex);
  const auto it = s_nod_files.find(normalized);
  if (it == s_nod_files.end())
    return {};

  File out;
  out.source = Source::GameCubeDisc;
  out.asset_class =
      wanted == PS3AssetPort::Class::Unknown ? PS3AssetPort::Classify(guest_path) : wanted;
  out.disc_index = it->second.index;
  out.guest_path = std::string(guest_path);
  out.resolved_path = "nod:" + normalized;
  out.size = it->second.size;
  return out;
}

bool ReadNodRange(const File& file, std::uint64_t offset, std::span<u8> destination)
{
  if (!file.IsGCDisc() || offset > file.size ||
      destination.size() > static_cast<std::size_t>(file.size - offset))
    return false;
  if (destination.empty())
    return true;

  std::scoped_lock lock(s_nod_mutex);
  if (!s_nod_partition)
    return false;

  NodHandle* raw_file = nullptr;
  if (nod_partition_open_file(s_nod_partition, file.disc_index, &raw_file) != NOD_RESULT_OK ||
      !raw_file)
    return false;

  struct ScopedFile
  {
    NodHandle* handle;
    ~ScopedFile() { nod_free(handle); }
  } scoped{raw_file};

  if (offset && nod_seek(raw_file, static_cast<std::int64_t>(offset), SEEK_SET) < 0)
    return false;

  std::size_t done = 0;
  while (done < destination.size())
  {
    const std::int64_t got =
        nod_read(raw_file, destination.data() + done, destination.size() - done);
    if (got <= 0)
      return false;
    done += static_cast<std::size_t>(got);
  }
  return true;
}
#endif

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
#if defined(MOH_NATIVE_VFS_NOD)
  if (File direct = ResolveNod(guest_path, wanted); direct)
    return direct;
#endif

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
  // are consumed by semantic native loaders (textures/audio/meshes/video), never
  // injected raw into the original GC loader.
  NativeAudio::NotifyGuestRead(guest_path, file_offset);
  NativeVideo::NotifyGuestRead(guest_path, file_offset);

  if (const char* value = std::getenv("MOH_NATIVE_VFS_DISC"); value && *value)
  {
    const std::string enabled = Lower(value);
    if (enabled == "0" || enabled == "false" || enabled == "off" || enabled == "no")
      return false;
  }

  const File file = ResolveGC(guest_path, PS3AssetPort::Class::Unknown);
  if (!file || !file.IsGC())
  {
    NativePCStatus::Fallback(NativePCStatus::Domain::FileIO, guest_path,
                             "host miss -> ModernGekko DVD/disc path");
    return false;
  }

  if (!ReadRange(file, file_offset, destination))
  {
    NativePCStatus::Fallback(NativePCStatus::Domain::FileIO, guest_path,
                             "host read rejected -> ModernGekko DVD/disc path");
    return false;
  }

  NativePCStatus::Native(NativePCStatus::Domain::FileIO, guest_path, file.resolved_path);
  if (!file.IsGCDisc())
    NativeGCAssets::ObserveHostRead(file.host_path, guest_path, file_offset, destination.size());
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

  NativePCStatus::Initialize();
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
  NativeVideo::Stop();
  NativeGCAssets::Shutdown();
  NativePCStatus::Summary();

  std::scoped_lock lock(s_mutex);
  s_gc_roots.clear();
  s_gc_path_cache.clear();
  s_initialized = false;
#if defined(MOH_NATIVE_VFS_NOD)
  CloseNodDisc();
#endif
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
  case Source::GameCubeDisc: return "GC-disc/nod";
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

#if defined(MOH_NATIVE_VFS_NOD)
  if (file.IsGCDisc())
  {
    std::vector<u8> bytes(static_cast<std::size_t>(file.size));
    if (!ReadNodRange(file, 0, bytes))
      return {};
    NativePCStatus::Native(NativePCStatus::Domain::FileIO, file.guest_path,
                           file.resolved_path);
    return bytes;
  }
#endif

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

  if (file.IsGC())
  {
    NativePCStatus::Native(NativePCStatus::Domain::FileIO, file.guest_path, file.resolved_path);
    if (!file.IsGCDisc())
      NativeGCAssets::ObserveHostRead(file.host_path, file.guest_path, 0, bytes.size());
  }
  return bytes;
}

bool ReadRange(const File& file, std::uint64_t offset, std::span<u8> destination)
{
  if (!file || offset > file.size ||
      destination.size() > static_cast<std::size_t>(file.size - offset))
    return false;

  if (destination.empty())
    return true;

#if defined(MOH_NATIVE_VFS_NOD)
  if (file.IsGCDisc())
    return ReadNodRange(file, offset, destination);
#endif

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
