#include "VideoCommon/MOHFrontline/Engine/Audio/NativeAudio.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>

#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/Swap.h"
#include "Core/System.h"

#if defined(MOH_NATIVE_AUDIO_FFMPEG)
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}
#endif

namespace MOHFrontline::NativeAudio
{
namespace
{
u16 LE16(const u8* p)
{
  return u16(p[0]) | (u16(p[1]) << 8);
}

u32 LE32(const u8* p)
{
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

bool EnvSwitch(const char* name, bool fallback)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  std::string v(value);
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (v == "0" || v == "false" || v == "off" || v == "no")
    return false;
  if (v == "1" || v == "true" || v == "on" || v == "yes")
    return true;
  return fallback;
}

s16 ClampS16(long value)
{
  return static_cast<s16>(
      std::clamp<long>(value, std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max()));
}

bool DecodeWave(std::span<const u8> bytes, PCMBuffer* out)
{
  if (!out || bytes.size() < 12 ||
      std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    return false;

  u16 format_tag = 0;
  u16 channels = 0;
  u32 sample_rate = 0;
  u16 bits = 0;
  std::span<const u8> payload;

  std::size_t p = 12;
  while (p + 8 <= bytes.size())
  {
    const u8* header = bytes.data() + p;
    const u32 chunk_size = LE32(header + 4);
    const std::size_t data_pos = p + 8;
    if (data_pos > bytes.size() || chunk_size > bytes.size() - data_pos)
      return false;

    const std::span<const u8> chunk(bytes.data() + data_pos, chunk_size);
    if (std::memcmp(header, "fmt ", 4) == 0 && chunk.size() >= 16)
    {
      format_tag = LE16(chunk.data() + 0);
      channels = LE16(chunk.data() + 2);
      sample_rate = LE32(chunk.data() + 4);
      bits = LE16(chunk.data() + 14);
    }
    else if (std::memcmp(header, "data", 4) == 0)
    {
      payload = chunk;
    }

    p = data_pos + chunk_size + (chunk_size & 1u);
  }

  if (payload.empty() || sample_rate < 4000 || sample_rate > 192000 ||
      (channels != 1 && channels != 2))
    return false;

  if (format_tag != 1 && format_tag != 3)
    return false;

  if ((format_tag == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
      (format_tag == 3 && bits != 32))
    return false;

  const std::size_t bytes_per_sample = bits / 8;
  const std::size_t frame_bytes = bytes_per_sample * channels;
  if (!frame_bytes || payload.size() < frame_bytes)
    return false;

  const std::size_t frames = payload.size() / frame_bytes;
  PCMBuffer pcm;
  pcm.sample_rate = sample_rate;
  pcm.channels = 2;
  pcm.interleaved_stereo.resize(frames * 2);

  auto sample_at = [&](const u8* src) -> s16
  {
    if (format_tag == 3)
    {
      float value = 0.0f;
      std::memcpy(&value, src, sizeof(value));
      if (!std::isfinite(value))
        value = 0.0f;
      value = std::clamp(value, -1.0f, 1.0f);
      return ClampS16(std::lround(value * 32767.0f));
    }

    switch (bits)
    {
    case 8:
      return static_cast<s16>((static_cast<int>(src[0]) - 128) << 8);
    case 16:
      return static_cast<s16>(LE16(src));
    case 24:
    {
      s32 value = s32(src[0]) | (s32(src[1]) << 8) | (s32(src[2]) << 16);
      if (value & 0x00800000)
        value |= static_cast<s32>(0xff000000u);
      return ClampS16(value >> 8);
    }
    case 32:
    {
      const s32 value = static_cast<s32>(LE32(src));
      return ClampS16(value >> 16);
    }
    default:
      return 0;
    }
  };

  for (std::size_t frame = 0; frame < frames; ++frame)
  {
    const u8* src = payload.data() + frame * frame_bytes;
    const s16 left = sample_at(src);
    const s16 right = channels == 2 ? sample_at(src + bytes_per_sample) : left;
    pcm.interleaved_stereo[frame * 2 + 0] = left;
    pcm.interleaved_stereo[frame * 2 + 1] = right;
  }

  *out = std::move(pcm);
  return true;
}

#if defined(MOH_NATIVE_AUDIO_FFMPEG)
bool AppendFFmpegFrame(const AVFrame& frame, PCMBuffer* pcm)
{
  if (!pcm || frame.nb_samples <= 0 || !frame.extended_data)
    return false;

  const AVSampleFormat format = static_cast<AVSampleFormat>(frame.format);
  const int channels =
      frame.ch_layout.nb_channels > 0 ? frame.ch_layout.nb_channels : 2;
  const std::size_t old_size = pcm->interleaved_stereo.size();
  pcm->interleaved_stereo.resize(old_size + static_cast<std::size_t>(frame.nb_samples) * 2);
  s16* dst = pcm->interleaved_stereo.data() + old_size;

  if (format == AV_SAMPLE_FMT_S16P)
  {
    const s16* left = reinterpret_cast<const s16*>(frame.extended_data[0]);
    const s16* right =
        channels > 1 && frame.extended_data[1]
            ? reinterpret_cast<const s16*>(frame.extended_data[1])
            : left;
    if (!left || !right)
    {
      pcm->interleaved_stereo.resize(old_size);
      return false;
    }

    for (int i = 0; i < frame.nb_samples; ++i)
    {
      dst[static_cast<std::size_t>(i) * 2 + 0] = left[i];
      dst[static_cast<std::size_t>(i) * 2 + 1] = right[i];
    }
    return true;
  }

  if (format == AV_SAMPLE_FMT_S16)
  {
    const s16* source = reinterpret_cast<const s16*>(frame.extended_data[0]);
    if (!source)
    {
      pcm->interleaved_stereo.resize(old_size);
      return false;
    }

    for (int i = 0; i < frame.nb_samples; ++i)
    {
      const std::size_t base = static_cast<std::size_t>(i) * channels;
      dst[static_cast<std::size_t>(i) * 2 + 0] = source[base];
      dst[static_cast<std::size_t>(i) * 2 + 1] =
          source[base + (channels > 1 ? 1 : 0)];
    }
    return true;
  }

  pcm->interleaved_stereo.resize(old_size);
  return false;
}

bool DecodeEAStreamFFmpeg(std::span<const u8> bytes, PCMBuffer* out)
{
  if (!out || bytes.size() < 8)
    return false;

  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ADPCM_EA_R1);
  if (!codec)
    return false;

  AVCodecContext* context = avcodec_alloc_context3(codec);
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  if (!context || !packet || !frame)
  {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return false;
  }

  context->sample_rate = 48000;
  av_channel_layout_default(&context->ch_layout, 2);

  if (avcodec_open2(context, codec, nullptr) < 0)
  {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return false;
  }

  PCMBuffer pcm;
  pcm.sample_rate = 48000;
  pcm.channels = 2;

  constexpr u32 kTagSchl = 0x6c484353u;  // SCHl
  constexpr u32 kTagSccl = 0x6c434353u;  // SCCl
  constexpr u32 kTagScdl = 0x6c444353u;  // SCDl
  constexpr u32 kTagScel = 0x6c454353u;  // SCEl

  auto receive_frames = [&]() -> bool
  {
    while (true)
    {
      const int result = avcodec_receive_frame(context, frame);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        return true;
      if (result < 0)
        return false;

      const bool ok = AppendFFmpegFrame(*frame, &pcm);
      av_frame_unref(frame);
      if (!ok)
        return false;
    }
  };

  bool decoded_packet = false;
  std::size_t offset = 0;
  while (offset + 8 <= bytes.size())
  {
    const u32 tag = LE32(bytes.data() + offset);
    const u32 block_size = LE32(bytes.data() + offset + 4);
    if (block_size < 8 || block_size > 0x40000u ||
        block_size > bytes.size() - offset)
      break;

    if (tag == kTagScdl)
    {
      const std::size_t payload_size = block_size - 8u;
      if (payload_size != 0 && payload_size <= 0x10000u)
      {
        av_packet_unref(packet);
        if (av_new_packet(packet, static_cast<int>(payload_size)) < 0)
          break;

        std::memcpy(packet->data, bytes.data() + offset + 8, payload_size);

        int result = avcodec_send_packet(context, packet);
        if (result == AVERROR(EAGAIN))
        {
          if (!receive_frames())
          {
            av_packet_unref(packet);
            break;
          }
          result = avcodec_send_packet(context, packet);
        }
        av_packet_unref(packet);

        if (result < 0 || !receive_frames())
          break;
        decoded_packet = true;
      }
    }
    else if (tag == kTagScel)
    {
      break;
    }
    else if (tag != kTagSchl && tag != kTagSccl)
    {
      // This decoder is intentionally only for the verified EA SCx container.
      break;
    }

    offset += block_size;
  }

  if (decoded_packet)
  {
    (void)avcodec_send_packet(context, nullptr);
    (void)receive_frames();
  }

  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&context);

  if (!decoded_packet || pcm.interleaved_stereo.empty())
    return false;

  *out = std::move(pcm);
  return true;
}
#endif

}  // namespace

bool IsEnabled()
{
  return EnvSwitch("MOH_NATIVE_AUDIO", true);
}

Format Detect(std::span<const u8> bytes)
{
  if (bytes.size() >= 12 &&
      std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
      std::memcmp(bytes.data() + 8, "WAVE", 4) == 0)
    return Format::WavePCM;

  if (bytes.size() >= 4 &&
      ((std::memcmp(bytes.data(), "SCHl", 4) == 0) ||
       (std::memcmp(bytes.data(), "SCDl", 4) == 0)))
    return Format::EAStream;

  if (bytes.size() >= 2 && bytes[0] == 'A' && bytes[1] == 'B')
  {
    if (bytes.size() >= 0x20)
      return LE32(bytes.data() + 0x1c) != 0 ? Format::AEMSBank : Format::AEMSStream;
    return Format::AEMSBank;
  }

  return Format::Unknown;
}

const char* FormatName(Format format)
{
  switch (format)
  {
  case Format::WavePCM: return "RIFF/WAVE PCM";
  case Format::EAStream: return "EA SCHl/SCDl";
  case Format::AEMSBank: return "AEMS bank";
  case Format::AEMSStream: return "AEMS stream";
  default: return "unknown";
  }
}

Asset Load(std::string_view guest_name)
{
  Asset asset;
  if (!IsEnabled())
    return asset;

  asset.file = NativeVFS::Resolve(guest_name, PS3AssetPort::Class::Audio);
  if (!asset.file)
    return asset;

  asset.bytes = NativeVFS::Read(asset.file);
  asset.format = Detect(asset.bytes);

  static unsigned logs = 0;
  if (logs++ < 128)
  {
    std::fprintf(stderr, "[moh-native-audio] load %.*s -> %s format=%s bytes=%zu\n",
                 static_cast<int>(guest_name.size()), guest_name.data(),
                 NativeVFS::Describe(asset.file).c_str(), FormatName(asset.format),
                 asset.bytes.size());
  }
  return asset;
}

bool Decode(const Asset& asset, PCMBuffer* out)
{
  if (!asset || !out)
    return false;

  switch (asset.format)
  {
  case Format::WavePCM:
    return DecodeWave(asset.bytes, out);

  case Format::EAStream:
#if defined(MOH_NATIVE_AUDIO_FFMPEG)
    return DecodeEAStreamFFmpeg(asset.bytes, out);
#else
    return false;
#endif

  // AEMS/ABK banks still need their event/module lookup layer. Never decode
  // the bank container itself as if it were a single PCM stream.
  case Format::AEMSBank:
  case Format::AEMSStream:
  case Format::Unknown:
  default:
    return false;
  }
}

bool Submit(const PCMBuffer& pcm)
{
  if (!pcm || !IsEnabled())
    return false;

  auto& system = Core::System::GetInstance();
  SoundStream* stream = system.GetSoundStream();
  if (!stream)
    return false;

  Mixer* mixer = stream->GetMixer();
  if (!mixer || !mixer->IsOutputSampleRateValid())
    return false;

  const double divisor_f =
      static_cast<double>(Mixer::FIXED_SAMPLE_RATE_DIVIDEND) / pcm.sample_rate;
  const u32 divisor =
      std::max<u32>(1u, static_cast<u32>(std::llround(divisor_f)));
  mixer->SetStreamInputSampleRateDivisor(divisor);

  // PushStreamingSamples consumes big-endian R/L pairs. NativeAudio exposes
  // ordinary host-endian L/R, so adapt only at this boundary.
  std::vector<s16> dolphin_order(pcm.interleaved_stereo.size());
  for (std::size_t i = 0; i < pcm.Frames(); ++i)
  {
    const u16 left = static_cast<u16>(pcm.interleaved_stereo[i * 2 + 0]);
    const u16 right = static_cast<u16>(pcm.interleaved_stereo[i * 2 + 1]);
    dolphin_order[i * 2 + 0] = static_cast<s16>(Common::swap16(right));
    dolphin_order[i * 2 + 1] = static_cast<s16>(Common::swap16(left));
  }

  mixer->PushStreamingSamples(dolphin_order.data(), pcm.Frames());
  return true;
}

bool TryPlay(std::string_view guest_name)
{
  const Asset asset = Load(guest_name);
  if (!asset)
    return false;

  PCMBuffer pcm;
  if (!Decode(asset, &pcm))
  {
    static unsigned fallback_logs = 0;
    if (fallback_logs++ < 64)
    {
      std::fprintf(stderr,
                   "[moh-native-audio] fallback GC DSP: %.*s format=%s\n",
                   static_cast<int>(guest_name.size()), guest_name.data(),
                   FormatName(asset.format));
    }
    return false;
  }

  const bool submitted = Submit(pcm);
  if (submitted)
  {
    std::fprintf(stderr, "[moh-native-audio] HOST PLAY: %.*s frames=%zu rate=%u\n",
                 static_cast<int>(guest_name.size()), guest_name.data(),
                 pcm.Frames(), pcm.sample_rate);
  }
  return submitted;
}
}  // namespace MOHFrontline::NativeAudio
