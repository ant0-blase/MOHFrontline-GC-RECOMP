#pragma once

#include <span>
#include <string_view>

#include "Common/CommonTypes.h"

namespace MOHFrontline::GCFont
{
// Validates the original Frontline GameCube FNTG container in host memory.
// The game's statically recompiled CFont code remains the semantic layout
// authority; this parser verifies the exact glyph/atlas offsets and reports
// what is already running from native PC bytes.
bool Inspect(std::string_view name, std::span<const u8> bytes);
}  // namespace MOHFrontline::GCFont
