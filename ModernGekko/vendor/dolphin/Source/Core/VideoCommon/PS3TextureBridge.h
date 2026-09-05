#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"

namespace VideoCommon
{
class CustomTextureData;
}

namespace PS3TextureBridge
{
void Initialize(const std::filesystem::path& root);
void Shutdown();

bool IsEnabled();

// rgba is the CPU-decoded original GameCube texture.
// row_length is in pixels and may be larger than width.
std::shared_ptr<VideoCommon::CustomTextureData>
FindReplacement(
    const u8* rgba,
    u32 width,
    u32 height,
    u32 row_length,
    u64 texture_hash,
    std::string* source_name = nullptr,
    float* match_score = nullptr);
}
