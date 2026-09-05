// Optional Medal of Honor: Frontline PS3 remaster asset provider.
//
// Raw PS3 resources stay outside extracted/ and are resolved from HD/PS3_FILES.
// This layer intentionally indexes raw assets before attempting to reinterpret
// RSX-specific data as GameCube data.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/PS3TextureBridge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

  if (ext == ".lit")
    return Kind::LightingData;

  if (ext == ".tpk")
    return Kind::TexturePack;

  if (ext == ".msh" || ext == ".dmf")
    return Kind::Mesh;

  if (ext == ".viv" || ext == ".lfc")
    return Kind::Container;

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

  case Kind::Container:
    ++s.stats.containers;
    break;

  case Kind::Unknown:
    break;
  }
}

void Reset()
{
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

    s.assets.emplace_back(std::move(asset));
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

    // Keep the first basename encountered.
    // Exact relative-path lookup remains available for duplicates.
    s.by_filename.emplace(
        Lower(asset.filename), i);

    AddStats(asset.kind);
  }

  s.ready = true;
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

  // Build host-side visual matching index for PS3 diffuse/UI textures.
  PS3TextureBridge::Initialize(s.root);

  const Stats& st = s.stats;

  std::fprintf(
      stderr,
      "[moh-ps3] asset layer: %s\n"
      "[moh-ps3] indexed %zu files | textures=%zu normals=%zu "
      "light-textures=%zu water=%zu lit=%zu tpk=%zu "
      "mesh=%zu rsx-mesh=%zu containers=%zu\n",
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
      st.containers);

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
  PS3TextureBridge::Shutdown();
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

std::vector<std::uint8_t> ReadBinary(
    const AssetInfo& asset)
{
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
       << " | rsx " << s.stats.rsx_meshes;
  }

  return ss.str();
}
}
