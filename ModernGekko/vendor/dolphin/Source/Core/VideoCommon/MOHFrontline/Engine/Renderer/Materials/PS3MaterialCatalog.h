#pragma once
#include <memory>
#include <optional>
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/TPK.h"
#include <array>
#include "VideoCommon/PS3RemasterAssets.h"
#include <string_view>
#include "VideoCommon/PS3TextureDecoder.h"
namespace MOHFrontline::Materials
{
struct Statistics { std::size_t catalogs = 0, records = 0, decoded = 0, failures = 0; };
// Catalog lookup is not activation. Callers must validate matching geometry/UVs.
std::shared_ptr<const std::vector<PS3TextureDecoder::Level>> LoadTexture(
    std::string_view level, std::string_view exact_name);
struct TextureResource
{
  PS3::TPK::Texture texture;
  std::string metadata_source, rsx_source;
};
std::optional<TextureResource> FindTextureResource(std::string_view level, std::string_view name);
std::shared_ptr<const std::vector<PS3TextureDecoder::CompressedLevel>> LoadCompressedTexture(
    std::string_view level, std::string_view exact_name);
std::vector<std::uint8_t> ReadTexturePayload(std::string_view level, std::string_view name);
bool HasTexture(std::string_view level, std::string_view exact_name);
std::shared_ptr<const std::vector<PS3TextureDecoder::Level>> DecodeRSXTexture(
    const PS3RemasterAssets::AssetInfo& asset, std::uint64_t offset, std::uint32_t size,
    const std::array<std::uint8_t, 24>& descriptor);
Statistics GetStatistics();
}
