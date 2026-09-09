#include "VideoCommon/MOHFrontline/Engine/Audio/NativeAudio.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string>

#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/Swap.h"
#include "Core/System.h"
#include "VideoCommon/MOHFrontline/Engine/NativePCStatus.h"

#if defined(MOH_NATIVE_AUDIO_FFMPEG)
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}
#endif

namespace MOHFrontline::NativeAudio
{
namespace
{
std::mutex s_stream_mutex;
std::string s_pending_guest;
std::string s_current_guest;
PCMBuffer s_stream_pcm;
std::size_t s_stream_frame = 0;

std::string NormalizeAudioName(std::string_view input)
{
  std::string value(input);
  std::replace(value.begin(), value.end(), '\\', '/');
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsStreamCandidate(std::string_view guest_name)
{
  const std::string name = NormalizeAudioName(guest_name);
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos)
    return false;
  const std::string_view ext(name.data() + dot, name.size() - dot);
  return ext == ".mpc" || ext == ".asf" || ext == ".asfx" || ext == ".mus" || ext == ".musx" ||
         ext == ".ast" || ext == ".astx" || ext == ".wav";
}

u16 LE16(const u8* p)
{
  return u16(p[0]) | (u16(p[1]) << 8);
}

u32 LE32(const u8* p)
{
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

bool ExtractMPCNativeAudio(std::span<const u8> bytes, std::vector<u8>* out,
                           std::size_t* packets)
{
  if (!out || bytes.size() < 8)
    return false;
  out->clear();
  std::size_t count = 0;
  std::size_t p = 0;
  while (p + 8 <= bytes.size())
  {
    const u8* chunk = bytes.data() + p;
    const u32 size = LE32(chunk + 4);
    if (size < 8 || size > bytes.size() - p)
      return false;

    const bool audio = std::memcmp(chunk, "SCHl", 4) == 0 ||
                       std::memcmp(chunk, "SCCl", 4) == 0 ||
                       std::memcmp(chunk, "SCDl", 4) == 0 ||
                       std::memcmp(chunk, "SCEl", 4) == 0;
    if (audio)
    {
      out->insert(out->end(), chunk, chunk + size);
      if (std::memcmp(chunk, "SCDl", 4) == 0)
        ++count;
    }
    p += size;
  }
  if (packets)
    *packets = count;
  return count != 0 && !out->empty();
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

std::string FFmpegErrorText(int error)
{
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  if (av_strerror(error, text, sizeof(text)) < 0)
    return "unknown FFmpeg error";
  return text;
}

struct EAMemoryReader
{
  std::span<const u8> bytes;
  std::size_t position = 0;
};

int ReadEAMemory(void* opaque, u8* buffer, int buffer_size)
{
  auto* reader = static_cast<EAMemoryReader*>(opaque);
  if (!reader || !buffer || buffer_size <= 0)
    return AVERROR(EINVAL);
  if (reader->position >= reader->bytes.size())
    return AVERROR_EOF;

  const std::size_t available = reader->bytes.size() - reader->position;
  const std::size_t amount =
      std::min<std::size_t>(available, static_cast<std::size_t>(buffer_size));
  std::memcpy(buffer, reader->bytes.data() + reader->position, amount);
  reader->position += amount;
  return static_cast<int>(amount);
}

std::int64_t SeekEAMemory(void* opaque, std::int64_t offset, int whence)
{
  auto* reader = static_cast<EAMemoryReader*>(opaque);
  if (!reader)
    return AVERROR(EINVAL);
  if (whence == AVSEEK_SIZE)
    return static_cast<std::int64_t>(reader->bytes.size());

  const int origin = whence & ~AVSEEK_FORCE;
  std::int64_t base = 0;
  if (origin == SEEK_SET)
    base = 0;
  else if (origin == SEEK_CUR)
    base = static_cast<std::int64_t>(reader->position);
  else if (origin == SEEK_END)
    base = static_cast<std::int64_t>(reader->bytes.size());
  else
    return AVERROR(EINVAL);

  if ((offset < 0 && base < -offset) ||
      (offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset))
    return AVERROR(EINVAL);

  const std::int64_t target = base + offset;
  if (target < 0 || static_cast<std::uint64_t>(target) > reader->bytes.size())
    return AVERROR(EINVAL);

  reader->position = static_cast<std::size_t>(target);
  return target;
}

bool DecodeEAStreamDemuxFFmpeg(std::span<const u8> bytes, PCMBuffer* out)
{
  if (!out || bytes.size() < 8)
    return false;

  EAMemoryReader reader{bytes, 0};
  constexpr int kIOBufferSize = 64 * 1024;
  u8* io_buffer = static_cast<u8*>(av_malloc(kIOBufferSize));
  if (!io_buffer)
    return false;

  AVIOContext* io =
      avio_alloc_context(io_buffer, kIOBufferSize, 0, &reader, &ReadEAMemory, nullptr,
                         &SeekEAMemory);
  if (!io)
  {
    av_free(io_buffer);
    return false;
  }

  AVFormatContext* format = avformat_alloc_context();
  if (!format)
  {
    avio_context_free(&io);
    return false;
  }

  format->pb = io;
  format->flags |= AVFMT_FLAG_CUSTOM_IO;

  int result = avformat_open_input(&format, nullptr, nullptr, nullptr);
  if (result < 0)
  {
    static unsigned logs = 0;
    if (logs++ < 8)
      std::fprintf(stderr, "[moh-native-audio] EA demux probe failed: %s (%d)\n",
                   FFmpegErrorText(result).c_str(), result);
    avformat_free_context(format);
    avio_context_free(&io);
    return false;
  }

  result = avformat_find_stream_info(format, nullptr);
  if (result < 0)
  {
    static unsigned logs = 0;
    if (logs++ < 8)
      std::fprintf(stderr, "[moh-native-audio] EA stream-info failed: %s (%d)\n",
                   FFmpegErrorText(result).c_str(), result);
    avformat_close_input(&format);
    avio_context_free(&io);
    return false;
  }

  const int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (stream_index < 0)
  {
    static unsigned logs = 0;
    if (logs++ < 8)
      std::fprintf(stderr, "[moh-native-audio] EA demux found no audio stream\n");
    avformat_close_input(&format);
    avio_context_free(&io);
    return false;
  }

  AVStream* stream = format->streams[stream_index];
  if (!stream || !stream->codecpar)
  {
    avformat_close_input(&format);
    avio_context_free(&io);
    return false;
  }

  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec)
  {
    static unsigned logs = 0;
    if (logs++ < 8)
      std::fprintf(stderr, "[moh-native-audio] no decoder for EA codec id=%d\n",
                   stream->codecpar->codec_id);
    avformat_close_input(&format);
    avio_context_free(&io);
    return false;
  }

  AVCodecContext* context = avcodec_alloc_context3(codec);
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  if (!context || !packet || !frame)
  {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    avformat_close_input(&format);
    avio_context_free(&io);
    return false;
  }

  result = avcodec_parameters_to_context(context, stream->codecpar);
  if (result >= 0 && context->sample_rate <= 0)
    context->sample_rate = 48000;
  if (result >= 0 && context->ch_layout.nb_channels <= 0)
    av_channel_layout_default(&context->ch_layout, 2);
  if (result >= 0)
    result = avcodec_open2(context, codec, nullptr);

  PCMBuffer pcm;
  pcm.sample_rate = context->sample_rate > 0 ? static_cast<u32>(context->sample_rate) : 48000u;
  pcm.channels = 2;
  std::size_t audio_packets = 0;

  auto receive_frames = [&]() -> bool
  {
    while (true)
    {
      const int receive = avcodec_receive_frame(context, frame);
      if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF)
        return true;
      if (receive < 0)
        return false;
      const bool ok = AppendFFmpegFrame(*frame, &pcm);
      av_frame_unref(frame);
      if (!ok)
        return false;
    }
  };

  if (result >= 0)
  {
    while ((result = av_read_frame(format, packet)) >= 0)
    {
      if (packet->stream_index == stream_index && packet->size > 0)
      {
        int send = avcodec_send_packet(context, packet);
        if (send == AVERROR(EAGAIN))
        {
          if (!receive_frames())
          {
            av_packet_unref(packet);
            result = AVERROR_INVALIDDATA;
            break;
          }
          send = avcodec_send_packet(context, packet);
        }
        if (send < 0 || !receive_frames())
        {
          av_packet_unref(packet);
          result = send < 0 ? send : AVERROR_INVALIDDATA;
          break;
        }
        ++audio_packets;
      }
      av_packet_unref(packet);
    }

    if (result == AVERROR_EOF || result >= 0)
    {
      (void)avcodec_send_packet(context, nullptr);
      (void)receive_frames();
    }
  }

  const AVCodecID codec_id = context->codec_id;
  const int codec_rate = context->sample_rate;
  const int codec_channels = context->ch_layout.nb_channels;

  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  avformat_close_input(&format);
  avio_context_free(&io);

  if (pcm.interleaved_stereo.empty() || audio_packets == 0)
    return false;

  static unsigned logs = 0;
  if (logs++ < 16)
  {
    std::fprintf(stderr,
                 "[moh-native-audio] EA DEMUX OK: codec=%d packets=%zu frames=%zu rate=%u "
                 "src_rate=%d src_channels=%d\n",
                 static_cast<int>(codec_id), audio_packets, pcm.Frames(), pcm.sample_rate,
                 codec_rate, codec_channels);
  }

  *out = std::move(pcm);
  return true;
}

bool DecodeEAStreamPacketsFFmpeg(std::span<const u8> bytes, PCMBuffer* out)
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

  int open_result = avcodec_open2(context, codec, nullptr);
  if (open_result < 0)
  {
    static unsigned logs = 0;
    if (logs++ < 8)
      std::fprintf(stderr, "[moh-native-audio] ADPCM_EA_R1 open failed: %s (%d)\n",
                   FFmpegErrorText(open_result).c_str(), open_result);
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
      {
        static unsigned logs = 0;
        if (logs++ < 8)
          std::fprintf(stderr, "[moh-native-audio] ADPCM_EA_R1 receive failed: %s (%d)\n",
                       FFmpegErrorText(result).c_str(), result);
        return false;
      }

      const bool ok = AppendFFmpegFrame(*frame, &pcm);
      av_frame_unref(frame);
      if (!ok)
        return false;
    }
  };

  bool decoded_packet = false;
  std::size_t packet_count = 0;
  std::size_t offset = 0;
  while (offset + 8 <= bytes.size())
  {
    const u32 tag = LE32(bytes.data() + offset);
    const u32 block_size = LE32(bytes.data() + offset + 4);
    if (block_size < 8 || block_size > 0x40000u ||
        block_size > bytes.size() - offset)
    {
      static unsigned logs = 0;
      if (logs++ < 8)
      {
        std::fprintf(stderr,
                     "[moh-native-audio] SCx bad block at 0x%zx tag=%c%c%c%c size=0x%x "
                     "remaining=0x%zx\n",
                     offset, bytes[offset + 0], bytes[offset + 1], bytes[offset + 2],
                     bytes[offset + 3], block_size, bytes.size() - offset);
      }
      break;
    }

    if (tag == kTagScdl)
    {
      const std::size_t payload_size = block_size - 8u;
      if (payload_size != 0 && payload_size <= 0x10000u)
      {
        av_packet_unref(packet);
        if (av_new_packet(packet, static_cast<int>(payload_size)) < 0)
          break;

        std::memcpy(packet->data, bytes.data() + offset + 8, payload_size);
        int send = avcodec_send_packet(context, packet);
        if (send == AVERROR(EAGAIN))
        {
          if (!receive_frames())
          {
            av_packet_unref(packet);
            break;
          }
          send = avcodec_send_packet(context, packet);
        }
        av_packet_unref(packet);

        if (send < 0)
        {
          static unsigned logs = 0;
          if (logs++ < 8)
            std::fprintf(stderr,
                         "[moh-native-audio] SCDl packet decode failed: %s (%d) "
                         "payload=0x%zx offset=0x%zx\n",
                         FFmpegErrorText(send).c_str(), send, payload_size, offset);
          break;
        }
        if (!receive_frames())
          break;
        decoded_packet = true;
        ++packet_count;
      }
    }
    else if (tag == kTagScel)
    {
      break;
    }
    else if (tag != kTagSchl && tag != kTagSccl)
    {
      static unsigned logs = 0;
      if (logs++ < 8)
        std::fprintf(stderr, "[moh-native-audio] SCx unknown tag at 0x%zx: %c%c%c%c\n",
                     offset, bytes[offset + 0], bytes[offset + 1], bytes[offset + 2],
                     bytes[offset + 3]);
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

  static unsigned logs = 0;
  if (logs++ < 16)
  {
    std::fprintf(stderr,
                 "[moh-native-audio] EA PACKET WALK OK: packets=%zu frames=%zu rate=%u\n",
                 packet_count, pcm.Frames(), pcm.sample_rate);
  }

  *out = std::move(pcm);
  return true;
}

bool DecodeEAStreamFFmpeg(std::span<const u8> bytes, PCMBuffer* out)
{
  if (DecodeEAStreamDemuxFFmpeg(bytes, out))
    return true;

  static unsigned logs = 0;
  if (logs++ < 16)
    std::fprintf(stderr, "[moh-native-audio] EA demux fallback -> direct SCDl packet walk\n");

  return DecodeEAStreamPacketsFFmpeg(bytes, out);
}

#endif

// Dependency-free EA ADPCM R1 decoder.
//
// MOH's SCHl/SCCl/SCDl streams store one complete EA R1 packet in each SCDl
// payload.  This mirrors the packet layout used by FFmpeg's ADPCM_EA_R1
// decoder, but keeps the MOH native path available even when Dolphin was built
// without FFmpeg/avformat.
constexpr std::array<int, 20> kEAAdpcmCoefficients = {
    0, 240, 460, 392,
    0,   0, -208, -220,
    0,   1,    3,    4,
    7,   8,   10,   11,
    0,  -1,   -3,   -4,
};

int SignExtendNibble(u8 nibble)
{
  const int value = nibble & 0x0f;
  return (value & 8) ? value - 16 : value;
}

u16 EAR1BE16(const u8* p)
{
  return (u16(p[0]) << 8) | u16(p[1]);
}

u32 EAR1BE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u32 ReadEAR1U32(const u8* p, bool big_endian)
{
  return big_endian ? EAR1BE32(p) : LE32(p);
}

s16 ReadEAR1S16(const u8* p, bool big_endian)
{
  return static_cast<s16>(big_endian ? EAR1BE16(p) : LE16(p));
}

s16 ClipS16(std::int64_t value)
{
  return static_cast<s16>(std::clamp<std::int64_t>(value, -32768, 32767));
}

bool DecodeEAR1PacketForChannels(std::span<const u8> packet, unsigned channels,
                                 bool big_endian,
                                 std::vector<std::vector<s16>>* decoded,
                                 u32* coded_samples_out)
{
  if (!decoded || !coded_samples_out || channels == 0 || channels > 6)
    return false;

  const std::size_t table_bytes = (static_cast<std::size_t>(channels) + 1u) * 4u;
  if (packet.size() < table_bytes)
    return false;

  u32 coded_samples = ReadEAR1U32(packet.data(), big_endian);
  coded_samples -= coded_samples % 28u;
  if (coded_samples == 0 || coded_samples > 48000u * 30u)
    return false;

  const std::size_t block_count = coded_samples / 28u;
  if (block_count == 0 || block_count > (std::numeric_limits<std::size_t>::max() - 4u) / 15u)
    return false;
  const std::size_t channel_bytes = 4u + block_count * 15u;

  std::array<std::size_t, 6> offsets{};
  for (unsigned channel = 0; channel < channels; ++channel)
  {
    const u32 relative =
        ReadEAR1U32(packet.data() + 4u + static_cast<std::size_t>(channel) * 4u, big_endian);
    const std::uint64_t absolute = static_cast<std::uint64_t>(relative) + table_bytes;
    if (absolute > packet.size() || channel_bytes > packet.size() - static_cast<std::size_t>(absolute))
      return false;
    offsets[channel] = static_cast<std::size_t>(absolute);
  }

  // Reject obviously wrong channel guesses. Real R1 channel payloads don't
  // overlap and their offsets are laid out monotonically in MOH streams.
  for (unsigned channel = 1; channel < channels; ++channel)
  {
    if (offsets[channel] < offsets[channel - 1] + channel_bytes)
      return false;
  }

  decoded->assign(channels, std::vector<s16>(coded_samples));

  for (unsigned channel = 0; channel < channels; ++channel)
  {
    std::size_t pos = offsets[channel];
    int current_sample = ReadEAR1S16(packet.data() + pos + 0u, big_endian);
    int previous_sample = ReadEAR1S16(packet.data() + pos + 2u, big_endian);
    pos += 4u;

    std::vector<s16>& output = (*decoded)[channel];
    std::size_t sample_index = 0;

    for (std::size_t block = 0; block < block_count; ++block)
    {
      if (pos >= packet.size())
        return false;

      int byte = packet[pos++];
      const unsigned filter = static_cast<unsigned>(byte >> 4);
      if (filter >= 16)
        return false;

      const int coeff1 = kEAAdpcmCoefficients[filter];
      const int coeff2 = kEAAdpcmCoefficients[filter + 4u];
      const int shift = 20 - (byte & 0x0f);
      if (shift < 5 || shift > 20)
        return false;

      for (unsigned n = 0; n < 28; ++n)
      {
        int nibble = 0;
        if (n & 1u)
        {
          nibble = SignExtendNibble(static_cast<u8>(byte));
        }
        else
        {
          if (pos >= packet.size())
            return false;
          byte = packet[pos++];
          nibble = SignExtendNibble(static_cast<u8>(byte >> 4));
        }

        std::int64_t next_sample =
            static_cast<std::int64_t>(nibble) * (std::int64_t{1} << shift);
        next_sample += static_cast<std::int64_t>(current_sample) * coeff1;
        next_sample += static_cast<std::int64_t>(previous_sample) * coeff2;
        next_sample >>= 8;

        const s16 clipped = ClipS16(next_sample);
        previous_sample = current_sample;
        current_sample = clipped;
        output[sample_index++] = clipped;
      }
    }
  }

  *coded_samples_out = coded_samples;
  return true;
}

bool DecodeEAR1Packet(std::span<const u8> packet, PCMBuffer* pcm, unsigned* channels_out,
                       bool* big_endian_out)
{
  if (!pcm || packet.size() < 16)
    return false;

  std::vector<std::vector<s16>> channels_pcm;
  u32 coded_samples = 0;
  unsigned channels = 0;

  // Frontline music/stream banks are normally stereo. The remaining guesses
  // make the decoder useful for mono or multichannel EA R1 without risking a
  // false stereo parse: every candidate is structurally validated above.
  constexpr std::array<unsigned, 4> kChannelCandidates = {2, 1, 4, 6};

  // Original EA R1 streams use little-endian packet metadata. The PS3
  // Frontline remaster stores the same R1 packet structure with big-endian
  // u32 offsets/sample counts and s16 predictor history. Structural validation
  // below makes endian auto-detection deterministic instead of relying on the
  // filename/platform.
  bool big_endian = false;
  bool decoded_packet = false;
  constexpr std::array<bool, 2> kEndianCandidates = {false, true};
  for (const bool candidate_big_endian : kEndianCandidates)
  {
    for (const unsigned candidate : kChannelCandidates)
    {
      if (DecodeEAR1PacketForChannels(packet, candidate, candidate_big_endian,
                                      &channels_pcm, &coded_samples))
      {
        channels = candidate;
        big_endian = candidate_big_endian;
        decoded_packet = true;
        break;
      }
    }
    if (decoded_packet)
      break;
  }

  if (channels == 0 || channels_pcm.empty() || coded_samples == 0)
    return false;

  const unsigned left_channel = 0;
  const unsigned right_channel =
      channels == 1 ? 0 : (channels >= 4 ? 2 : 1);
  if (right_channel >= channels_pcm.size())
    return false;

  const std::size_t old_frames = pcm->Frames();
  if (coded_samples > (std::numeric_limits<std::size_t>::max() / 2u) - old_frames)
    return false;

  pcm->interleaved_stereo.resize((old_frames + coded_samples) * 2u);
  s16* dst = pcm->interleaved_stereo.data() + old_frames * 2u;
  for (u32 i = 0; i < coded_samples; ++i)
  {
    dst[static_cast<std::size_t>(i) * 2u + 0u] = channels_pcm[left_channel][i];
    dst[static_cast<std::size_t>(i) * 2u + 1u] = channels_pcm[right_channel][i];
  }

  if (channels_out)
    *channels_out = channels;
  if (big_endian_out)
    *big_endian_out = big_endian;
  return true;
}

bool DecodeEAStreamNativeR1(std::span<const u8> bytes, PCMBuffer* out)
{
  if (!out || bytes.size() < 8)
    return false;

  constexpr u32 kTagSchl = 0x6c484353u;  // SCHl
  constexpr u32 kTagSccl = 0x6c434353u;  // SCCl
  constexpr u32 kTagScdl = 0x6c444353u;  // SCDl
  constexpr u32 kTagScel = 0x6c454353u;  // SCEl

  PCMBuffer pcm;
  pcm.sample_rate = 48000;
  pcm.channels = 2;

  std::size_t offset = 0;
  std::size_t packet_count = 0;
  unsigned detected_channels = 0;
  bool detected_big_endian = false;
  bool detected_endian_known = false;

  while (offset + 8u <= bytes.size())
  {
    const u32 tag = LE32(bytes.data() + offset);
    const u32 block_size = LE32(bytes.data() + offset + 4u);
    if (block_size < 8u || block_size > 0x40000u || block_size > bytes.size() - offset)
    {
      static unsigned logs = 0;
      if (logs++ < 12)
        std::fprintf(stderr,
                     "[moh-native-audio] EA R1 native bad SCx block off=0x%zx tag=%c%c%c%c "
                     "size=0x%x remaining=0x%zx\n",
                     offset, bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3],
                     block_size, bytes.size() - offset);
      return false;
    }

    if (tag == kTagScdl)
    {
      const std::span<const u8> packet(bytes.data() + offset + 8u, block_size - 8u);
      unsigned packet_channels = 0;
      bool packet_big_endian = false;
      if (!DecodeEAR1Packet(packet, &pcm, &packet_channels, &packet_big_endian))
      {
        static unsigned logs = 0;
        if (logs++ < 12)
        {
          const u32 samples_le = packet.size() >= 4 ? LE32(packet.data()) : 0;
          const u32 samples_be = packet.size() >= 4 ? EAR1BE32(packet.data()) : 0;
          const u32 off0_le = packet.size() >= 8 ? LE32(packet.data() + 4u) : 0;
          const u32 off0_be = packet.size() >= 8 ? EAR1BE32(packet.data() + 4u) : 0;
          const u32 off1_le = packet.size() >= 12 ? LE32(packet.data() + 8u) : 0;
          const u32 off1_be = packet.size() >= 12 ? EAR1BE32(packet.data() + 8u) : 0;
          std::fprintf(stderr,
                       "[moh-native-audio] EA R1 native packet rejected off=0x%zx payload=0x%zx "
                       "LE[samples=%u off0=0x%x off1=0x%x] "
                       "BE[samples=%u off0=0x%x off1=0x%x]\n",
                       offset, packet.size(), samples_le, off0_le, off1_le,
                       samples_be, off0_be, off1_be);
        }
        return false;
      }

      if (detected_channels == 0)
        detected_channels = packet_channels;
      if (!detected_endian_known)
      {
        detected_big_endian = packet_big_endian;
        detected_endian_known = true;
      }
      else if (detected_big_endian != packet_big_endian)
      {
        static unsigned endian_logs = 0;
        if (endian_logs++ < 4)
          std::fprintf(stderr,
                       "[moh-native-audio] EA R1 packet endian changed at off=0x%zx (%s -> %s)\n",
                       offset, detected_big_endian ? "BE" : "LE",
                       packet_big_endian ? "BE" : "LE");
      }
      ++packet_count;
    }
    else if (tag == kTagScel)
    {
      break;
    }
    else if (tag != kTagSchl && tag != kTagSccl)
    {
      static unsigned logs = 0;
      if (logs++ < 12)
        std::fprintf(stderr,
                     "[moh-native-audio] EA R1 native unknown SCx tag off=0x%zx: %c%c%c%c\n",
                     offset, bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]);
      return false;
    }

    offset += block_size;
  }

  if (packet_count == 0 || pcm.interleaved_stereo.empty())
    return false;

  static unsigned logs = 0;
  if (logs++ < 32)
  {
    std::fprintf(stderr,
                 "[moh-native-audio] EA R1 NATIVE OK: packets=%zu frames=%zu rate=%u "
                 "source_channels=%u endian=%s (no FFmpeg)\n",
                 packet_count, pcm.Frames(), pcm.sample_rate, detected_channels,
                 detected_big_endian ? "BE" : "LE");
  }

  *out = std::move(pcm);
  return true;
}

bool SubmitFrameRange(const PCMBuffer& pcm, std::size_t first_frame, std::size_t frame_count)
{
  if (!pcm || frame_count == 0 || first_frame >= pcm.Frames())
    return false;

  frame_count = std::min(frame_count, pcm.Frames() - first_frame);

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

  std::vector<s16> dolphin_order(frame_count * 2);
  for (std::size_t i = 0; i < frame_count; ++i)
  {
    const std::size_t source = (first_frame + i) * 2;
    const u16 left = static_cast<u16>(pcm.interleaved_stereo[source + 0]);
    const u16 right = static_cast<u16>(pcm.interleaved_stereo[source + 1]);
    dolphin_order[i * 2 + 0] = static_cast<s16>(Common::swap16(right));
    dolphin_order[i * 2 + 1] = static_cast<s16>(Common::swap16(left));
  }

  mixer->PushStreamingSamples(dolphin_order.data(), frame_count);
  return true;
}
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

  if (bytes.size() >= 8 && std::memcmp(bytes.data(), "MPCh", 4) == 0)
    return Format::EAMovie;

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
  case Format::EAMovie: return "EA MPC (MPCh+SCx)";
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
    // Frontline's GC/PS3 SCHl/SCDl R1 streams are understood directly by the
    // host decoder. Avoid probing FFmpeg first and falling through predictable
    // "invalid number of samples" errors on every bank/music load.
    if (DecodeEAStreamNativeR1(asset.bytes, out))
      return true;
#if defined(MOH_NATIVE_AUDIO_FFMPEG)
    return DecodeEAStreamFFmpeg(asset.bytes, out);
#else
    return false;
#endif

  case Format::EAMovie:
  {
    std::vector<u8> audio;
    std::size_t packets = 0;
    if (!ExtractMPCNativeAudio(asset.bytes, &audio, &packets))
      return false;
    if (DecodeEAStreamNativeR1(audio, out))
    {
      static unsigned logs = 0;
      if (logs++ < 16)
        std::fprintf(stderr,
                     "[NATIVE-PC] AUDIO     MPC demux: SCDl packets=%zu bytes=%zu -> native EA R1\n",
                     packets, audio.size());
      return true;
    }
#if defined(MOH_NATIVE_AUDIO_FFMPEG)
    return DecodeEAStreamFFmpeg(audio, out);
#else
    return false;
#endif
  }

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
  return pcm && IsEnabled() && SubmitFrameRange(pcm, 0, pcm.Frames());
}

void NotifyGuestRead(std::string_view guest_name, std::uint64_t file_offset)
{
  if (!IsEnabled() || file_offset > 0x100 || !IsStreamCandidate(guest_name))
    return;

  if (!EnvSwitch("MOH_NATIVE_AUDIO_AUTOPLAY", true))
    return;

  const std::string normalized = NormalizeAudioName(guest_name);
  std::scoped_lock lock(s_stream_mutex);
  if (normalized == s_pending_guest || normalized == s_current_guest)
    return;

  s_pending_guest = normalized;
  std::fprintf(stderr, "[moh-native-audio] guest stream detected: %s (native decode scheduled)\n",
               s_pending_guest.c_str());
}

void Pump()
{
  if (!IsEnabled())
    return;

  std::string pending;
  {
    std::scoped_lock lock(s_stream_mutex);
    if (!s_pending_guest.empty())
    {
      pending = std::move(s_pending_guest);
      s_pending_guest.clear();
    }
  }

  if (!pending.empty())
  {
    Asset asset = Load(pending);
    PCMBuffer decoded;
    bool ok = asset && Decode(asset, &decoded);

    // A PS3 remaster match may exist but use a container revision we have not
    // decoded yet. In that case try the extracted GC host file before giving
    // up; the guest itself still has the ISO/DSP fallback regardless.
    if (!ok && asset.file.IsPS3())
    {
      const NativeVFS::File gc =
          NativeVFS::ResolveGameCube(pending, PS3AssetPort::Class::Audio);
      if (gc)
      {
        Asset gc_asset;
        gc_asset.file = gc;
        gc_asset.bytes = NativeVFS::Read(gc);
        gc_asset.format = Detect(gc_asset.bytes);
        PCMBuffer gc_decoded;
        if (gc_asset && Decode(gc_asset, &gc_decoded))
        {
          asset = std::move(gc_asset);
          decoded = std::move(gc_decoded);
          ok = true;
        }
      }
    }

    std::scoped_lock lock(s_stream_mutex);
    s_current_guest = pending;
    s_stream_frame = 0;
    s_stream_pcm = ok ? std::move(decoded) : PCMBuffer{};

    if (ok)
    {
      std::fprintf(stderr,
                   "[moh-native-audio] HOST STREAM START: %s frames=%zu rate=%u source=%s\n",
                   s_current_guest.c_str(), s_stream_pcm.Frames(), s_stream_pcm.sample_rate,
                   asset.file.IsPS3() ? "PS3" : "GC-host");
      NativePCStatus::Native(
          NativePCStatus::Domain::Audio, s_current_guest,
          asset.file.IsPS3() ? "host PCM stream source=PS3" :
                               "host PCM stream source=GC");
    }
    else
    {
      std::fprintf(stderr,
                   "[moh-native-audio] native stream unavailable: %s -> guest GC audio continues\n",
                   s_current_guest.c_str());
      NativePCStatus::Fallback(NativePCStatus::Domain::Audio, s_current_guest,
                               "guest/DSP audio path continues");
    }
  }

  std::scoped_lock lock(s_stream_mutex);
  if (!s_stream_pcm || s_stream_frame >= s_stream_pcm.Frames())
    return;

  // Feed roughly 1/30 s per rendered frame. This intentionally keeps a small
  // lead over 60 Hz without flooding Dolphin's bounded streaming FIFO.
  const std::size_t chunk =
      std::max<std::size_t>(256, static_cast<std::size_t>(s_stream_pcm.sample_rate / 30u));
  const std::size_t count = std::min(chunk, s_stream_pcm.Frames() - s_stream_frame);
  if (!SubmitFrameRange(s_stream_pcm, s_stream_frame, count))
    return;

  s_stream_frame += count;
  if (s_stream_frame >= s_stream_pcm.Frames())
  {
    std::fprintf(stderr, "[moh-native-audio] HOST STREAM END: %s\n",
                 s_current_guest.c_str());
    s_stream_pcm = {};
    s_stream_frame = 0;
    s_current_guest.clear();
  }
}

void Stop()
{
  std::scoped_lock lock(s_stream_mutex);
  s_pending_guest.clear();
  s_current_guest.clear();
  s_stream_pcm = {};
  s_stream_frame = 0;
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
