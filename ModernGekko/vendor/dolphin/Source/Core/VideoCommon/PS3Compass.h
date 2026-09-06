#pragma once
#include <memory>
#include <string_view>
#include <vector>
#include "Common/CommonTypes.h"
class TextureInfo;
namespace VideoCommon
{
class CustomTextureData;
}
namespace PS3Compass
{
int NameIndex(std::string_view name);
void Register(int index, u32 address, u32 width, u32 height, u32 format, std::vector<u8> original,
              u32 palette_format, std::vector<u8> palette);
std::shared_ptr<VideoCommon::CustomTextureData> Find(const TextureInfo& info);
void Shutdown();
}  // namespace PS3Compass
