#pragma once
#include <memory>
#include <string_view>
#include "VideoCommon/PS3TextureDecoder.h"
namespace MOHFrontline::Materials
{
struct Statistics { std::size_t catalogs = 0, records = 0, decoded = 0, failures = 0; };
// Catalog lookup is not activation. Callers must validate matching geometry/UVs.
std::shared_ptr<const std::vector<PS3TextureDecoder::Level>> LoadTexture(
    std::string_view level, std::string_view exact_name);
bool HasTexture(std::string_view level, std::string_view exact_name);
Statistics GetStatistics();
}
