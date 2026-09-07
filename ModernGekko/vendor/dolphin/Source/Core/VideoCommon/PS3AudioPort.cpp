#include "VideoCommon/PS3AudioPort.h"

#include <cstring>

namespace PS3AudioPort
{
namespace
{
Format Detect(std::span<const u8> bytes)
{
  if (bytes.size() >= 12 &&
      std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
      std::memcmp(bytes.data() + 8, "WAVE", 4) == 0)
    return Format::Wave;

  if (bytes.size() >= 4 && bytes[0] == 'A' && bytes[1] == 'B')
  {
    // Frontline AEMS bank/stream family. The archive documentation shows
    // versions 0x08/0x09 and a module offset at +0x1c for BNKl banks.
    if (bytes.size() >= 0x20)
    {
      const u32 module_offset =
          u32(bytes[0x1c]) | (u32(bytes[0x1d]) << 8) |
          (u32(bytes[0x1e]) << 16) | (u32(bytes[0x1f]) << 24);

      return module_offset ? Format::AemsBank : Format::AemsStream;
    }
    return Format::AemsBank;
  }

  if (bytes.size() >= 4 &&
      ((bytes[0] == 'S' && bytes[1] == 'C' && bytes[2] == 'H' && bytes[3] == 'l') ||
       (bytes[0] == 'S' && bytes[1] == 'C' && bytes[2] == 'D' && bytes[3] == 'l')))
    return Format::EAStream;

  return Format::Unknown;
}
}  // namespace

AudioAsset Load(std::string_view guest_name)
{
  AudioAsset out;
  out.match = PS3AssetPort::ResolveAudio(guest_name);
  if (!out.match)
    return out;

  out.bytes = PS3AssetPort::Read(out.match);
  out.format = Detect(out.bytes);
  return out;
}

bool IsDirectHostPlayable(const AudioAsset& asset)
{
  return asset.format == Format::Wave;
}

const char* FormatName(Format format)
{
  switch (format)
  {
  case Format::AemsBank: return "AEMS bank";
  case Format::AemsStream: return "AEMS stream";
  case Format::EAStream: return "EA SCHl/SCDl stream";
  case Format::Wave: return "WAVE";
  default: return "unknown";
  }
}
}  // namespace PS3AudioPort
