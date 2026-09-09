#pragma once

#include <memory>
#include <span>

#include "Common/CommonTypes.h"

namespace VideoCommon
{
class CustomTextureData;
}

namespace MOHFrontline::GCTexture
{
// Live replacement is intentionally opt-in while the decoder is being
// validated against every GSH/FNTG palette/mip variant.
bool ReplacementEnabled();

// Host-side decoder for the GameCube GX texture payloads uploaded by Frontline.
// Supports the formats observed in the supplied disc assets/Dolphin dumps plus
// the common GX palette/intensity formats used by the frontend and fonts.
std::shared_ptr<VideoCommon::CustomTextureData>
DecodeUpload(u32 width, u32 height, u32 format, std::span<const u8> texture,
             u32 palette_format, std::span<const u8> palette);
}  // namespace MOHFrontline::GCTexture
