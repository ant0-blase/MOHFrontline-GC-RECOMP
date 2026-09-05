#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace PS3RemasterAssets
{
enum class Kind
{
  Texture,
  DetailNormal,
  LightTexture,
  WaterTexture,
  LightingData,
  TexturePack,
  Mesh,
  RsxMesh,
  Container,
  Unknown
};

struct AssetInfo
{
  std::filesystem::path absolute_path;
  std::string relative_path;
  std::string filename;
  Kind kind = Kind::Unknown;
  std::uintmax_t size = 0;
};

struct Stats
{
  std::size_t total = 0;
  std::size_t textures = 0;
  std::size_t detail_normals = 0;
  std::size_t light_textures = 0;
  std::size_t water_textures = 0;
  std::size_t lighting_files = 0;
  std::size_t texture_packs = 0;
  std::size_t meshes = 0;
  std::size_t rsx_meshes = 0;
  std::size_t containers = 0;
};

void Initialize();
void Shutdown();

bool IsEnabled();
bool IsReady();

const std::filesystem::path& GetRoot();
const std::vector<AssetInfo>& GetAssets();
const Stats& GetStats();

const AssetInfo* FindByRelativePath(std::string_view path);
const AssetInfo* FindByFilename(std::string_view filename);

// PS3 material helpers.
const AssetInfo* FindNormalMap(std::string_view material_name);
const AssetInfo* FindLevelLighting(std::string_view level_name,
                                   std::string_view filename);
const AssetInfo* FindWeaponsLighting();

// Loads file contents only when requested.
// The index itself does not keep the full PS3 pack in RAM.
std::vector<std::uint8_t> ReadBinary(const AssetInfo& asset);

std::string Describe();
}
