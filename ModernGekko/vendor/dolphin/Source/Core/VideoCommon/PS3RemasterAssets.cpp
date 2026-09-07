// Optional Medal of Honor: Frontline PS3 remaster asset provider.
//
// Raw PS3 resources stay outside extracted/ and are resolved from HD/PS3_FILES.
// This layer intentionally indexes raw assets before attempting to reinterpret
// RSX-specific data as GameCube data.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/PS3Compass.h"
#include "VideoCommon/PS3FontParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace PS3RemasterAssets
{
namespace
{
struct State
{
  bool enabled = false;
  bool ready = false;
  std::uint64_t generation = 0;

  std::filesystem::path root;

  std::vector<AssetInfo> assets;
  std::unordered_map<std::string, std::size_t> by_relative;
  std::unordered_map<std::string, std::size_t> by_filename;

  Stats stats;
};

State s;

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

std::string Normalize(std::string value)
{
  std::replace(value.begin(), value.end(), '\\', '/');

  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());

  return Lower(std::move(value));
}

bool Contains(std::string_view haystack, std::string_view needle)
{
  return haystack.find(needle) != std::string_view::npos;
}

bool EndsWith(std::string_view value, std::string_view suffix)
{
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

bool EnvironmentFalse(const char* value)
{
  if (!value || !*value)
    return false;

  const std::string v = Lower(value);

  return v == "0" ||
         v == "false" ||
         v == "off" ||
         v == "no";
}

Kind Classify(const std::filesystem::path& path)
{
  const std::string full =
      Normalize(path.generic_string());

  const std::string filename =
      Lower(path.filename().string());

  const std::string ext =
      Lower(path.extension().string());

  if (EndsWith(full, ".msh.rsx"))
    return Kind::RsxMesh;

  // rsx.viv is an opaque RSX GPU blob. TPAC/TPK/EMT/XPD
  // metadata supplies offsets into it; it is not BIGF/C0FB.
  if (filename == "rsx.viv")
    return Kind::RsxBlob;

  if (ext == ".lit")
    return Kind::LightingData;

  if (ext == ".tpk")
    return Kind::TexturePack;

  if (ext == ".msh" || ext == ".dmf")
    return Kind::Mesh;

  if (ext == ".viv" ||
      ext == ".big" ||
      ext == ".lfc" ||
      ext == ".qfs")
  {
    return Kind::Container;
  }

  if (ext == ".ssh" ||
      ext == ".dds" ||
      ext == ".png" ||
      ext == ".tga")
  {
    if (Contains(filename, "_normal") ||
        Contains(filename, "normalmap") ||
        Contains(full, "/detailmaps/"))
    {
      return Kind::DetailNormal;
    }

    if (Contains(filename, "light_") ||
        Contains(full, "/mohfl_exports/light"))
    {
      return Kind::LightTexture;
    }

    if (Contains(filename, "water") ||
        Contains(filename, "ocean") ||
        Contains(filename, "wave") ||
        Contains(filename, "refraction") ||
        Contains(filename, "_bump"))
    {
      return Kind::WaterTexture;
    }

    return Kind::Texture;
  }

  return Kind::Unknown;
}

void AddStats(Kind kind)
{
  ++s.stats.total;

  switch (kind)
  {
  case Kind::Texture:
    ++s.stats.textures;
    break;

  case Kind::DetailNormal:
    ++s.stats.detail_normals;
    break;

  case Kind::LightTexture:
    ++s.stats.light_textures;
    break;

  case Kind::WaterTexture:
    ++s.stats.water_textures;
    break;

  case Kind::LightingData:
    ++s.stats.lighting_files;
    break;

  case Kind::TexturePack:
    ++s.stats.texture_packs;
    break;

  case Kind::Mesh:
    ++s.stats.meshes;
    break;

  case Kind::RsxMesh:
    ++s.stats.rsx_meshes;
    break;

  case Kind::RsxBlob:
    ++s.stats.rsx_blobs;
    break;

  case Kind::Container:
    ++s.stats.containers;
    break;

  case Kind::Unknown:
    break;
  }
}


u32 ReadBE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u32 ReadBE24(const u8* p)
{
  return
      (u32(p[0]) << 16) |
      (u32(p[1]) << 8) |
      u32(p[2]);
}


u32 ReadLE32(const u8* p)
{
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

bool IsRefPack(std::span<const u8> data, std::size_t* header_offset = nullptr)
{
  auto match = [&](std::size_t off) {
    return data.size() >= off + 5 && data[off + 1] == 0xFB && (data[off] & 0x10u) != 0;
  };

  if (match(0))
  {
    if (header_offset)
      *header_offset = 0;
    return true;
  }

  if (data.size() >= 9 && match(4))
  {
    if (header_offset)
      *header_offset = 4;
    return true;
  }

  return false;
}

std::uint64_t RefPackExpandedSize(std::span<const u8> data)
{
  std::size_t pos = 0;
  if (!IsRefPack(data, &pos))
    return 0;

  const u8 flags = data[pos];
  pos += 2;

  if ((flags & 0x80u) != 0)
  {
    if (data.size() < pos + 4)
      return 0;
    return ReadBE32(data.data() + pos);
  }

  if (data.size() < pos + 3)
    return 0;

  return (std::uint64_t(data[pos]) << 16) |
         (std::uint64_t(data[pos + 1]) << 8) |
         std::uint64_t(data[pos + 2]);
}

bool DecompressRefPack(std::span<const u8> input, std::vector<u8>* output)
{
  if (!output)
    return false;

  output->clear();
  std::size_t src = 0;
  if (!IsRefPack(input, &src))
    return false;

  const u8 flags = input[src];
  src += 2;

  std::uint64_t wanted = 0;
  if ((flags & 0x80u) != 0)
  {
    if (input.size() < src + 4)
      return false;
    wanted = ReadBE32(input.data() + src);
    src += 4;
  }
  else
  {
    if (input.size() < src + 3)
      return false;
    wanted = (std::uint64_t(input[src]) << 16) |
             (std::uint64_t(input[src + 1]) << 8) |
             std::uint64_t(input[src + 2]);
    src += 3;
  }

  constexpr std::uint64_t MAX_EXPANDED = 512ull * 1024ull * 1024ull;
  if (!wanted || wanted > MAX_EXPANDED || wanted > std::numeric_limits<std::size_t>::max())
    return false;

  output->reserve(static_cast<std::size_t>(wanted));

  auto literals = [&](std::size_t count) {
    if (src + count > input.size() || output->size() + count > wanted)
      return false;
    output->insert(output->end(), input.begin() + static_cast<std::ptrdiff_t>(src),
                   input.begin() + static_cast<std::ptrdiff_t>(src + count));
    src += count;
    return true;
  };

  auto copy = [&](std::size_t distance, std::size_t count) {
    if (!distance || distance > output->size() || output->size() + count > wanted)
      return false;
    for (std::size_t i = 0; i < count; ++i)
      output->push_back((*output)[output->size() - distance]);
    return true;
  };

  while (src < input.size() && output->size() < wanted)
  {
    const u8 code = input[src];

    if (code <= 0x7F)
    {
      if (src + 1 >= input.size())
        return false;
      const u8 b1 = input[src + 1];
      const std::size_t lit = code & 0x03u;
      const std::size_t len = ((code & 0x1Cu) >> 2) + 3u;
      const std::size_t dist = ((code & 0x60u) << 3) + b1 + 1u;
      src += 2;
      if (!literals(lit) || !copy(dist, len))
        return false;
    }
    else if (code <= 0xBF)
    {
      if (src + 2 >= input.size())
        return false;
      const u8 b1 = input[src + 1];
      const u8 b2 = input[src + 2];
      const std::size_t lit = (b1 >> 6) & 0x03u;
      const std::size_t len = (code & 0x3Fu) + 4u;
      const std::size_t dist = ((b1 & 0x3Fu) << 8) + b2 + 1u;
      src += 3;
      if (!literals(lit) || !copy(dist, len))
        return false;
    }
    else if (code <= 0xDF)
    {
      if (src + 3 >= input.size())
        return false;
      const u8 b1 = input[src + 1];
      const u8 b2 = input[src + 2];
      const u8 b3 = input[src + 3];
      const std::size_t lit = code & 0x03u;
      const std::size_t len = ((code & 0x0Cu) << 6) + b3 + 5u;
      const std::size_t dist = ((code & 0x10u) << 12) + (std::size_t(b1) << 8) + b2 + 1u;
      src += 4;
      if (!literals(lit) || !copy(dist, len))
        return false;
    }
    else if (code <= 0xFB)
    {
      const std::size_t lit = ((code & 0x1Fu) << 2) + 4u;
      ++src;
      if (!literals(lit))
        return false;
    }
    else
    {
      const std::size_t lit = code & 0x03u;
      ++src;
      if (!literals(lit))
        return false;
      break;
    }
  }

  if (output->size() < wanted)
    return false;

  output->resize(static_cast<std::size_t>(wanted));
  return true;
}

bool ReadArchiveSlice(const std::filesystem::path& archive, std::uint64_t offset,
                      std::uint64_t size, std::vector<u8>* out)
{
  if (!out || !size || size > std::numeric_limits<std::size_t>::max())
    return false;

  std::ifstream file(archive, std::ios::binary);
  if (!file)
    return false;

  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file)
    return false;

  out->resize(static_cast<std::size_t>(size));
  return static_cast<bool>(file.read(reinterpret_cast<char*>(out->data()),
                                     static_cast<std::streamsize>(out->size())));
}

bool PlausibleArchiveEntry(std::uint64_t file_size, u32 offset, u32 size)
{
  return size != 0 && offset < file_size && std::uint64_t(offset) + size <= file_size;
}

bool IndexC0FBArchive(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& archive_relative)
{
  std::ifstream file(
      archive_path,
      std::ios::binary |
      std::ios::ate);

  if (!file)
    return false;

  const std::streamoff end =
      file.tellg();

  if (end < 6)
    return false;

  const std::uint64_t file_size =
      static_cast<std::uint64_t>(end);

  file.seekg(0, std::ios::beg);

  std::array<u8, 6> header{};

  if (!file.read(
          reinterpret_cast<char*>(header.data()),
          static_cast<std::streamsize>(header.size())))
  {
    return false;
  }

  if (header[0] != 0xC0 ||
      header[1] != 0xFB)
  {
    return false;
  }

  const u32 header_size =
      ((u32(header[2]) << 8) |
       u32(header[3])) +
      4u;

  const u32 entry_count =
      (u32(header[4]) << 8) |
      u32(header[5]);

  if (header_size < 6 ||
      header_size > file_size ||
      !entry_count ||
      entry_count > 65535u)
  {
    std::fprintf(
        stderr,
        "[moh-ps3-ea] invalid C0FB header: %s header=%u count=%u\n",
        archive_relative.generic_string().c_str(),
        header_size,
        entry_count);
    return false;
  }

  std::size_t added = 0;
  std::size_t refpacked = 0;

  for (u32 index = 0;
       index < entry_count;
       ++index)
  {
    const std::streamoff table_pos =
        file.tellg();

    if (table_pos < 0 ||
        static_cast<std::uint64_t>(table_pos) + 7u >
            header_size)
    {
      break;
    }

    std::array<u8, 6> entry{};

    if (!file.read(
            reinterpret_cast<char*>(entry.data()),
            static_cast<std::streamsize>(entry.size())))
    {
      break;
    }

    const u32 offset =
        ReadBE24(entry.data());

    const u32 packed_size =
        ReadBE24(entry.data() + 3);

    if (!PlausibleArchiveEntry(file_size, offset, packed_size))
    {
      std::fprintf(
          stderr,
          "[moh-ps3-ea] invalid C0FB entry: %s index=%u off=%u size=%u\n",
          archive_relative.generic_string().c_str(),
          index,
          offset,
          packed_size);
      break;
    }

    std::string name;
    name.reserve(96);

    for (std::size_t n = 0;
         n < 1024;
         ++n)
    {
      char ch = 0;

      if (!file.get(ch))
        break;

      if (ch == '\0')
        break;

      const unsigned char uch =
          static_cast<unsigned char>(ch);

      if (uch < 0x20 || uch > 0x7E)
      {
        name.clear();
        break;
      }

      name.push_back(ch);
    }

    if (name.empty())
      continue;

    const std::streampos saved =
        file.tellg();

    std::array<u8, 9> prefix{};
    std::size_t prefix_size = 0;

    file.seekg(
        static_cast<std::streamoff>(offset),
        std::ios::beg);

    if (file)
    {
      const std::size_t wanted =
          static_cast<std::size_t>(
              std::min<u32>(
                  packed_size,
                  static_cast<u32>(prefix.size())));

      file.read(
          reinterpret_cast<char*>(prefix.data()),
          static_cast<std::streamsize>(wanted));

      prefix_size =
          static_cast<std::size_t>(file.gcount());
    }

    file.clear();
    file.seekg(saved);

    const std::span<const u8> prefix_span(
        prefix.data(),
        prefix_size);

    const bool refpack =
        IsRefPack(prefix_span);

    std::uint64_t logical_size =
        packed_size;

    if (refpack)
    {
      const std::uint64_t expanded =
          RefPackExpandedSize(prefix_span);

      if (expanded)
        logical_size = expanded;

      ++refpacked;
    }

    AssetInfo asset;
    asset.absolute_path = archive_path;
    asset.relative_path =
        Normalize(archive_relative.generic_string()) +
        "::" +
        Normalize(name);
    asset.filename =
        std::filesystem::path(name).filename().string();
    asset.kind =
        Classify(std::filesystem::path(name));
    asset.size = logical_size;
    asset.embedded = true;
    asset.refpack = refpack;
    asset.archive_offset = offset;
    asset.packed_size = packed_size;

    s.assets.emplace_back(std::move(asset));
    ++added;
  }

  ++s.stats.ea_archives_opened;
  s.stats.refpack_entries += refpacked;

  std::fprintf(
      stderr,
      "[moh-ps3-ea] indexed C0FB %s: %zu entries (%zu RefPack)\n",
      archive_relative.generic_string().c_str(),
      added,
      refpacked);

  return added != 0;
}

bool IndexEAArchive(const std::filesystem::path& archive_path,
                    const std::filesystem::path& archive_relative)
{
  std::ifstream file(archive_path, std::ios::binary | std::ios::ate);
  if (!file)
    return false;

  const std::streamoff end = file.tellg();
  if (end < 16)
    return false;

  const std::uint64_t file_size = static_cast<std::uint64_t>(end);
  file.seekg(0, std::ios::beg);

  std::array<u8, 16> header{};

  if (!file.read(
          reinterpret_cast<char*>(
              header.data()),
          static_cast<std::streamsize>(
              header.size())))
  {
    return false;
  }

  if (header[0] == 0xC0 &&
      header[1] == 0xFB)
  {
    return IndexC0FBArchive(
        archive_path,
        archive_relative);
  }

  const std::string magic(reinterpret_cast<const char*>(header.data()), 4);
  if (magic != "BIGF" && magic != "BIG4" && magic != "BIGH")
  {
    static unsigned unsupported_logs = 0;
    if (unsupported_logs < 32)
    {
      ++unsupported_logs;
      std::fprintf(stderr,
                   "[moh-ps3-rsx] opaque/non-EA blob registered: %s magic=%02X%02X%02X%02X\n",
                   archive_relative.generic_string().c_str(), header[0], header[1], header[2], header[3]);
    }
    return false;
  }

  auto plausible_count = [](u32 v) { return v > 0 && v < 1000000u; };
  auto plausible_header = [&](u32 v) { return v >= 16 && v <= file_size; };

  u32 entry_count = ReadBE32(header.data() + 8);
  u32 table_end = ReadBE32(header.data() + 12);
  bool big_endian = true;

  if (!plausible_count(entry_count) || !plausible_header(table_end))
  {
    entry_count = ReadLE32(header.data() + 8);
    table_end = ReadLE32(header.data() + 12);
    big_endian = false;
    if (!plausible_count(entry_count) || !plausible_header(table_end))
      return false;
  }

  file.seekg(16, std::ios::beg);
  std::size_t added = 0;
  std::size_t compressed = 0;

  for (u32 i = 0; i < entry_count; ++i)
  {
    const std::streamoff table_pos = file.tellg();
    if (table_pos < 0 || static_cast<std::uint64_t>(table_pos) + 9 > table_end)
      break;

    std::array<u8, 8> eh{};
    if (!file.read(reinterpret_cast<char*>(eh.data()), static_cast<std::streamsize>(eh.size())))
      break;

    u32 offset = big_endian ? ReadBE32(eh.data()) : ReadLE32(eh.data());
    u32 packed = big_endian ? ReadBE32(eh.data() + 4) : ReadLE32(eh.data() + 4);

    if (!PlausibleArchiveEntry(file_size, offset, packed))
    {
      const u32 alt_offset = big_endian ? ReadLE32(eh.data()) : ReadBE32(eh.data());
      const u32 alt_packed = big_endian ? ReadLE32(eh.data() + 4) : ReadBE32(eh.data() + 4);
      if (!PlausibleArchiveEntry(file_size, alt_offset, alt_packed))
        break;
      offset = alt_offset;
      packed = alt_packed;
    }

    std::string name;
    for (std::size_t n = 0; n < 1024; ++n)
    {
      char ch = 0;
      if (!file.get(ch))
        break;
      if (ch == '\0')
        break;
      const unsigned char u = static_cast<unsigned char>(ch);
      if (u < 0x20 || u > 0x7E)
      {
        name.clear();
        break;
      }
      name.push_back(ch);
    }

    if (name.empty())
      continue;

    const std::streampos return_pos = file.tellg();
    std::array<u8, 9> prefix{};
    std::size_t prefix_size = 0;

    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (file)
    {
      const std::size_t want = std::min<std::size_t>(prefix.size(), packed);
      file.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(want));
      prefix_size = static_cast<std::size_t>(file.gcount());
    }
    file.clear();
    file.seekg(return_pos, std::ios::beg);

    const std::span<const u8> prefix_span(prefix.data(), prefix_size);
    const bool refpack = IsRefPack(prefix_span);
    std::uint64_t logical = packed;
    if (refpack)
    {
      const std::uint64_t expanded = RefPackExpandedSize(prefix_span);
      if (expanded)
        logical = expanded;
      ++compressed;
    }

    AssetInfo asset;
    asset.absolute_path = archive_path;
    asset.relative_path = Normalize(archive_relative.generic_string()) + "::" + Normalize(name);
    asset.filename = std::filesystem::path(name).filename().string();
    asset.kind = Classify(std::filesystem::path(name));
    asset.size = logical;
    asset.embedded = true;
    asset.refpack = refpack;
    asset.archive_offset = offset;
    asset.packed_size = packed;
    s.assets.emplace_back(std::move(asset));
    ++added;
  }

  ++s.stats.ea_archives_opened;
  s.stats.refpack_entries += compressed;

  std::fprintf(stderr,
               "[moh-ps3-ea] indexed %s: %zu entries (%zu RefPack)\n",
               archive_relative.generic_string().c_str(), added, compressed);
  return added != 0;
}

void Reset()
{
  ++s.generation;
  s.ready = false;

  s.assets.clear();
  s.by_relative.clear();
  s.by_filename.clear();

  s.stats = {};
}

void BuildIndex()
{
  Reset();

  if (!s.enabled || s.root.empty())
    return;

  std::error_code ec;

  if (!std::filesystem::exists(s.root, ec) ||
      !std::filesystem::is_directory(s.root, ec))
  {
    std::fprintf(stderr,
                 "[moh-ps3] asset directory does not exist: %s\n",
                 s.root.string().c_str());
    return;
  }

  const auto options =
      std::filesystem::directory_options::skip_permission_denied;

  std::filesystem::recursive_directory_iterator it(
      s.root, options, ec);

  std::filesystem::recursive_directory_iterator end;

  for (; !ec && it != end; it.increment(ec))
  {
    const auto& entry = *it;

    std::error_code file_ec;

    if (!entry.is_regular_file(file_ec))
      continue;

    const std::filesystem::path path = entry.path();

    AssetInfo asset;
    asset.absolute_path = path;

    std::filesystem::path relative =
        std::filesystem::relative(path, s.root, file_ec);

    if (file_ec)
      relative = path.filename();

    asset.relative_path =
        Normalize(relative.generic_string());

    asset.filename =
        path.filename().string();

    asset.kind =
        Classify(relative);

    asset.size =
        entry.file_size(file_ec);

    if (file_ec)
      asset.size = 0;

    const Kind kind = asset.kind;
    s.assets.emplace_back(std::move(asset));

    if (kind == Kind::Container)
      IndexEAArchive(path, relative);
  }

  std::sort(s.assets.begin(), s.assets.end(),
            [](const AssetInfo& a, const AssetInfo& b) {
              return a.relative_path < b.relative_path;
            });

  for (std::size_t i = 0; i < s.assets.size(); ++i)
  {
    const AssetInfo& asset = s.assets[i];

    s.by_relative.emplace(
        Normalize(asset.relative_path), i);

    const std::string filename_key = Lower(asset.filename);
    const auto existing = s.by_filename.find(filename_key);

    // Prefer directly extracted files; otherwise archive entries remain usable
    // through exact relative-path lookup and as filename fallbacks.
    if (existing == s.by_filename.end() ||
        (s.assets[existing->second].embedded && !asset.embedded))
    {
      s.by_filename[filename_key] = i;
    }

    AddStats(asset.kind);
    if (asset.embedded)
      ++s.stats.embedded_files;
  }

  s.ready = true;
  ++s.generation;
}

const AssetInfo* FindFilenameInternal(std::string_view filename)
{
  const auto it =
      s.by_filename.find(Lower(std::string(filename)));

  if (it == s.by_filename.end())
    return nullptr;

  return &s.assets[it->second];
}
}

void Initialize()
{
  const char* enabled =
      std::getenv("MOH_PS3_ASSETS");

  if (EnvironmentFalse(enabled))
  {
    s.enabled = false;
    Reset();
    return;
  }

  const char* path =
      std::getenv("MOH_PS3_FILES");

  if (path && *path)
  {
    s.root =
        std::filesystem::path(path);
  }
  else
  {
    // Allows Windows/manual launches without run.sh as long as the runtime
    // is started from the project root.
    s.root =
        std::filesystem::current_path() /
        "HD" /
        "PS3_FILES";
  }

  std::error_code ec;

  s.enabled =
      std::filesystem::exists(s.root, ec) &&
      std::filesystem::is_directory(s.root, ec);

  if (!s.enabled)
  {
    Reset();
    return;
  }

  BuildIndex();

  // Parse the actual PS3 SFNH fonts independently of the old experimental
  // fuzzy texture bridge.
  PS3FontParser::Initialize(s.root);

  const Stats& st = s.stats;

  std::fprintf(
      stderr,
      "[moh-ps3] asset layer: %s\n"
      "[moh-ps3] indexed %zu files | textures=%zu normals=%zu "
      "light-textures=%zu water=%zu lit=%zu tpk=%zu "
      "mesh=%zu rsx-mesh=%zu containers=%zu embedded=%zu "
      "ea-archives=%zu refpack=%zu\n",
      s.root.string().c_str(),
      st.total,
      st.textures,
      st.detail_normals,
      st.light_textures,
      st.water_textures,
      st.lighting_files,
      st.texture_packs,
      st.meshes,
      st.rsx_meshes,
      st.containers,
      st.embedded_files,
      st.ea_archives_opened,
      st.refpack_entries);

  if (const AssetInfo* weapons = FindWeaponsLighting())
  {
    std::fprintf(stderr,
                 "[moh-ps3] weapons lighting found: %s (%ju bytes)\n",
                 weapons->relative_path.c_str(),
                 static_cast<std::uintmax_t>(weapons->size));
  }

  if (FindNormalMap("Concrete1"))
  {
    std::fprintf(stderr,
                 "[moh-ps3] DetailMaps normal-map set detected\n");
  }
}

void Shutdown()
{
  PS3Compass::Shutdown();
  PS3FontParser::Shutdown();

  s.enabled = false;
  Reset();
  s.root.clear();
}

bool IsEnabled()
{
  return s.enabled;
}

bool IsReady()
{
  return s.enabled && s.ready;
}

const std::filesystem::path& GetRoot()
{
  return s.root;
}

const std::vector<AssetInfo>& GetAssets()
{
  return s.assets;
}

std::uint64_t GetIndexGeneration() { return s.generation; }

const Stats& GetStats()
{
  return s.stats;
}

const AssetInfo* FindByRelativePath(std::string_view path)
{
  if (!IsReady())
    return nullptr;

  const auto it =
      s.by_relative.find(
          Normalize(std::string(path)));

  if (it == s.by_relative.end())
    return nullptr;

  return &s.assets[it->second];
}

const AssetInfo* FindByFilename(std::string_view filename)
{
  if (!IsReady())
    return nullptr;

  return FindFilenameInternal(filename);
}

const AssetInfo* FindNormalMap(std::string_view material_name)
{
  if (!IsReady())
    return nullptr;

  std::string material =
      Normalize(std::string(material_name));

  const std::size_t slash =
      material.find_last_of('/');

  if (slash != std::string::npos)
    material.erase(0, slash + 1);

  const std::size_t dot =
      material.find_last_of('.');

  if (dot != std::string::npos)
    material.erase(dot);

  const std::string base =
      material + "_normal";

  const std::string candidates[] = {
      base + ".ssh",
      base + ".dds",
      base + ".png",
      base + ".tga",
  };

  for (const std::string& candidate : candidates)
  {
    if (const AssetInfo* asset =
            FindFilenameInternal(candidate))
    {
      return asset;
    }
  }

  return nullptr;
}

const AssetInfo* FindLevelLighting(
    std::string_view level_name,
    std::string_view filename)
{
  if (!IsReady())
    return nullptr;

  const std::string wanted_level =
      Normalize(std::string(level_name));

  const std::string wanted_file =
      Lower(std::string(filename));

  for (const AssetInfo& asset : s.assets)
  {
    if (asset.kind != Kind::LightingData)
      continue;

    if (Lower(asset.filename) != wanted_file)
      continue;

    if (wanted_level.empty() ||
        Contains(asset.relative_path, wanted_level))
    {
      return &asset;
    }
  }

  return nullptr;
}

const AssetInfo* FindWeaponsLighting()
{
  if (!IsReady())
    return nullptr;

  return FindFilenameInternal("weapons.lit");
}


std::vector<std::uint8_t> ReadBinaryRange(
    const AssetInfo& asset,
    std::uint64_t offset,
    std::size_t size)
{
  if (!size)
    return {};

  if (asset.size)
  {
    const std::uint64_t total =
        static_cast<std::uint64_t>(asset.size);

    if (offset > total ||
        static_cast<std::uint64_t>(size) > total - offset)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-rsx] out-of-range read: %s off=0x%llX size=%zu total=%ju\n",
          asset.relative_path.c_str(),
          static_cast<unsigned long long>(offset),
          size,
          static_cast<std::uintmax_t>(asset.size));

      return {};
    }
  }

  std::ifstream file(
      asset.absolute_path,
      std::ios::binary);

  if (!file)
    return {};

  file.seekg(
      static_cast<std::streamoff>(offset),
      std::ios::beg);

  if (!file)
    return {};

  std::vector<std::uint8_t> data(size);

  if (!file.read(
          reinterpret_cast<char*>(data.data()),
          static_cast<std::streamsize>(data.size())))
  {
    return {};
  }

  return data;
}

std::vector<std::uint8_t> ReadBinary(
    const AssetInfo& asset)
{
  if (asset.embedded)
  {
    constexpr std::uint64_t MAX_PACKED_ENTRY = 512ull * 1024ull * 1024ull;
    if (!asset.packed_size || asset.packed_size > MAX_PACKED_ENTRY)
      return {};

    std::vector<u8> packed;
    if (!ReadArchiveSlice(asset.absolute_path, asset.archive_offset, asset.packed_size, &packed))
      return {};

    if (asset.refpack || IsRefPack(std::span<const u8>(packed.data(), packed.size())))
    {
      std::vector<u8> expanded;
      if (!DecompressRefPack(std::span<const u8>(packed.data(), packed.size()), &expanded))
      {
        std::fprintf(stderr, "[moh-ps3-ea] RefPack decode failed: %s\n",
                     asset.relative_path.c_str());
        return {};
      }

      static unsigned logs = 0;
      if (logs < 48)
      {
        ++logs;
        std::fprintf(stderr, "[moh-ps3-ea] RefPack OK: %s packed=%zu expanded=%zu\n",
                     asset.relative_path.c_str(), packed.size(), expanded.size());
      }
      return expanded;
    }

    return packed;
  }

  // Protect against accidentally pulling entire PS3 VIV archives into RAM.
  constexpr std::uintmax_t MAX_SINGLE_ASSET =
      512ull * 1024ull * 1024ull;

  if (asset.size > MAX_SINGLE_ASSET)
  {
    std::fprintf(stderr,
                 "[moh-ps3] refusing oversized asset: %s\n",
                 asset.relative_path.c_str());
    return {};
  }

  std::ifstream file(
      asset.absolute_path,
      std::ios::binary |
      std::ios::ate);

  if (!file)
    return {};

  const std::streamoff end =
      file.tellg();

  if (end <= 0)
    return {};

  file.seekg(0, std::ios::beg);

  std::vector<std::uint8_t> data(
      static_cast<std::size_t>(end));

  if (!file.read(
          reinterpret_cast<char*>(data.data()),
          static_cast<std::streamsize>(data.size())))
  {
    return {};
  }

  return data;
}

std::vector<std::uint8_t> ReadRange(const AssetInfo& asset, std::uint64_t offset, std::uint64_t size)
{
  if (!size || size > 128ull * 1024 * 1024 || offset > asset.size || size > asset.size - offset)
    return {};
  if (asset.refpack)
  {
    const auto bytes = ReadBinary(asset);
    if (offset > bytes.size() || size > bytes.size() - offset) return {};
    return {bytes.begin() + offset, bytes.begin() + offset + size};
  }
  std::vector<u8> result;
  const auto base = asset.embedded ? asset.archive_offset : 0;
  if (base > std::numeric_limits<std::uint64_t>::max() - offset ||
      !ReadArchiveSlice(asset.absolute_path, base + offset, size, &result)) return {};
  return result;
}

std::string Describe()
{
  std::ostringstream ss;

  ss << "PS3 assets: "
     << (IsReady() ? "ready" : "disabled");

  if (IsReady())
  {
    ss << " | "
       << s.stats.total << " files"
       << " | normals " << s.stats.detail_normals
       << " | lit " << s.stats.lighting_files
       << " | tpk " << s.stats.texture_packs
       << " | rsx " << s.stats.rsx_meshes
       << " | embedded " << s.stats.embedded_files
       << " | refpack " << s.stats.refpack_entries;
  }

  return ss.str();
}
}
