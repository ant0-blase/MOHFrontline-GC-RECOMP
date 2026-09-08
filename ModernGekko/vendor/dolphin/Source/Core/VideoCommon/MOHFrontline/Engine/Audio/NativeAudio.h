#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeVFS.h"

namespace MOHFrontline::NativeAudio
{
enum class Format
{
  Unknown,
  WavePCM,
  EAStream,
  AEMSBank,
  AEMSStream,
};

struct Asset
{
  NativeVFS::File file;
  Format format = Format::Unknown;
  std::vector<u8> bytes;

  explicit operator bool() const { return bool(file) && !bytes.empty(); }
};

struct PCMBuffer
{
  u32 sample_rate = 0;
  u16 channels = 2;
  std::vector<s16> interleaved_stereo;

  std::size_t Frames() const { return interleaved_stereo.size() / 2; }
  explicit operator bool() const
  {
    return sample_rate != 0 && channels == 2 && !interleaved_stereo.empty();
  }
};

bool IsEnabled();
Asset Load(std::string_view guest_name);
Format Detect(std::span<const u8> bytes);
const char* FormatName(Format format);

// Real host-side decoding:
// - uncompressed RIFF/WAVE: built-in;
// - EA SCHl/SCDl ADPCM R1: FFmpeg when Dolphin found FFmpeg at configure time;
// - AEMS/ABK banks: fallback until their event/module lookup is natively mapped.
bool Decode(const Asset& asset, PCMBuffer* out);

// Sends host-endian PCM to Dolphin's host mixer without passing through the
// emulated GameCube DSP. The conversion to Dolphin's streaming FIFO byte order
// is handled internally.
bool Submit(const PCMBuffer& pcm);

// Convenience: NativeVFS -> decode -> host mixer. Returns false for any
// unsupported/missing asset so the caller can immediately fall back to GC.
bool TryPlay(std::string_view guest_name);
}  // namespace MOHFrontline::NativeAudio
