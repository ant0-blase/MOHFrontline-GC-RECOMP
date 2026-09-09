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

// Called by the native DVD/FST bridge when the guest starts reading an audio
// file. This only schedules host playback; the guest still receives its normal
// GC bytes, so unsupported assets transparently keep the original DSP path.
void NotifyGuestRead(std::string_view guest_name, std::uint64_t file_offset);

// Pump a bounded amount of decoded host PCM. Call once per rendered frame.
void Pump();
void Stop();

// Convenience one-shot path retained for small WAV/SFX tests.
bool TryPlay(std::string_view guest_name);
}  // namespace MOHFrontline::NativeAudio
