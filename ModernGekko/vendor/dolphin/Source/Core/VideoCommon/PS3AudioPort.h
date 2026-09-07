#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/PS3AssetPort.h"

namespace PS3AudioPort
{
enum class Format
{
  Unknown,
  AemsBank,
  AemsStream,
  EAStream,
  Wave
};

struct AudioAsset
{
  PS3AssetPort::Match match;
  Format format = Format::Unknown;
  std::vector<u8> bytes;

  explicit operator bool() const { return bool(match) && !bytes.empty(); }
};

AudioAsset Load(std::string_view guest_name);

// Returns true only for PCM/WAVE-style content that a host mixer can consume
// without an EA codec. ABK/AST/ASF still need a native EA decoder bridge.
bool IsDirectHostPlayable(const AudioAsset& asset);

const char* FormatName(Format format);
}  // namespace PS3AudioPort
