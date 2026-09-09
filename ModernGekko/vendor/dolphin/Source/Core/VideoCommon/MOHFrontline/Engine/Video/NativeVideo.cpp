#include "VideoCommon/MOHFrontline/Engine/Video/NativeVideo.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/MohPcLayer.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeVFS.h"
#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/TextureConfig.h"

#if defined(MOH_NATIVE_VIDEO_FFMPEG)
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
#endif

namespace MOHFrontline::NativeVideo
{
namespace
{
using Clock = std::chrono::steady_clock;

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

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string Filename(std::string_view path)
{
  const std::size_t slash = path.find_last_of("/\\:");
  return Lower(std::string(path.substr(slash == std::string_view::npos ? 0 : slash + 1)));
}

std::string Stem(std::string_view filename)
{
  const std::size_t dot = filename.find_last_of('.');
  return std::string(filename.substr(0, dot == std::string_view::npos ? filename.size() : dot));
}

bool LooksLikeMovie(std::string_view guest_name)
{
  const std::string path = Lower(std::string(guest_name));
  const std::string file = Filename(path);
  if (path.find("movies/") != std::string::npos || path.find("/movies") != std::string::npos)
    return true;
  if (file.ends_with(".mpc") || file.ends_with(".mpcx") || file.ends_with(".bik") ||
      file.ends_with(".vp6") || file.ends_with(".thp") || file.ends_with(".m2v") ||
      file.ends_with(".mpg") || file.ends_with(".mpeg") || file.ends_with(".avi"))
    return true;

  // Frontline briefing movies have historically appeared at an ASF semantic
  // boundary on some builds, while the remaster counterpart is MPCX.
  return file.starts_with("brief") && file.ends_with(".asf");
}

// Do not infer PS3 names from the GameCube filename. Frontline's remaster renamed
// several media files. Unknown movies deliberately stay on the original GC path.
std::vector<std::string> PS3MovieNames(std::string_view guest_name)
{
  const std::string file = Filename(guest_name);

  if (file == "ealogo.mpc" || file == "ea_logo.mpc")
    return {"moh_ea_logo.bik"};
  if (file == "intro.mpc")
    return {"intro.mpcx"};
  if (file == "brief11.asf")
    return {"brief11.mpcx"};
  if (file == "brief21.asf")
    return {"brief21.mpcx"};
  if (file == "brief31.asf")
    return {"brief31.mpcx"};
  if (file == "brief41.asf")
    return {"brief41.mpcx"};
  if (file == "brief51.asf")
    return {"brief51.mpcx"};

  if (file.ends_with(".mpcx") || file.ends_with(".bik"))
    return {file};

  return {};
}

std::string PreferredPS3MovieLocale()
{
  if (const char* value = std::getenv("MOH_NATIVE_VIDEO_LOCALE"); value && *value)
    return Lower(value);

  if (const char* lang = std::getenv("LANG"); lang && *lang)
  {
    const std::string v = Lower(lang);
    if (v.starts_with("fr"))
      return "french";
    if (v.starts_with("de"))
      return "german";
    if (v.starts_with("it"))
      return "italian";
    if (v.starts_with("es"))
      return "spanish";
  }
  return "usa";
}

const PS3RemasterAssets::AssetInfo* FindPS3Movie(std::string_view guest_name)
{
  if (!PS3RemasterAssets::IsReady())
    return nullptr;

  const std::vector<std::string> names = PS3MovieNames(guest_name);
  if (names.empty())
    return nullptr;

  const std::string locale = PreferredPS3MovieLocale();
  const PS3RemasterAssets::AssetInfo* best = nullptr;
  int best_score = -1;

  for (std::size_t rank = 0; rank < names.size(); ++rank)
  {
    const std::string wanted = Lower(names[rank]);
    for (const auto& asset : PS3RemasterAssets::GetAssets())
    {
      if (Lower(asset.filename) != wanted)
        continue;

      const std::string path = Lower(asset.relative_path);
      int score = 1000 - static_cast<int>(rank) * 100;
      if (!locale.empty() && path.find("/_" + locale + "/") != std::string::npos)
        score += 500;
      if (path.find("movie") != std::string::npos)
        score += 100;
      if (path.find("/_usa/") != std::string::npos && locale != "usa")
        score += 10;
      if (!asset.embedded)
        score += 1;

      if (score > best_score)
      {
        best_score = score;
        best = &asset;
      }
    }
  }

  static unsigned mappings_logged = 0;
  if (best && mappings_logged++ < 16)
  {
    std::fprintf(stderr, "[moh-native-video] semantic map guest=%.*s -> PS3=%s locale=%s\n",
                 static_cast<int>(guest_name.size()), guest_name.data(),
                 best->relative_path.c_str(), locale.c_str());
  }
  return best;
}

enum class Preference
{
  Auto,
  PS3First,
  GCFirst,
  PS3Only,
  GCOnly,
};

Preference GetPreference()
{
  if (const char* value = std::getenv("MOH_NATIVE_VIDEO_SOURCE"); value && *value)
  {
    const std::string v = Lower(value);
    if (v == "ps3" || v == "ps3-first" || v == "ps3_first") return Preference::PS3First;
    if (v == "gc" || v == "gc-first" || v == "gc_first") return Preference::GCFirst;
    if (v == "ps3-only" || v == "ps3_only") return Preference::PS3Only;
    if (v == "gc-only" || v == "gc_only") return Preference::GCOnly;
  }

  switch (NativeVFS::GetPolicy())
  {
  case NativeVFS::Policy::PS3First: return Preference::PS3First;
  case NativeVFS::Policy::GCFirst: return Preference::GCFirst;
  case NativeVFS::Policy::PS3Only: return Preference::PS3Only;
  case NativeVFS::Policy::GCOnly: return Preference::GCOnly;
  default: return Preference::Auto;
  }
}

struct EncodedMovie
{
  std::vector<u8> bytes;
  std::string description;
  bool ps3 = false;
};

std::optional<EncodedMovie> ReadGC(std::string_view guest_name)
{
  const NativeVFS::File file =
      NativeVFS::ResolveGameCube(guest_name, PS3AssetPort::Class::Unknown);
  if (!file || file.size == 0 || file.size > (1024ull * 1024ull * 1024ull))
    return std::nullopt;

  auto bytes = NativeVFS::Read(file);
  if (bytes.empty())
    return std::nullopt;
  return EncodedMovie{std::move(bytes), NativeVFS::Describe(file), false};
}

std::optional<EncodedMovie> ReadPS3(std::string_view guest_name)
{
  const auto* asset = FindPS3Movie(guest_name);
  if (!asset || asset->size == 0 || asset->size > (1024ull * 1024ull * 1024ull))
    return std::nullopt;

  auto bytes = PS3RemasterAssets::ReadBinary(*asset);
  if (bytes.empty())
    return std::nullopt;
  return EncodedMovie{std::move(bytes), "PS3-remaster:" + asset->relative_path, true};
}

std::mutex s_request_mutex;
std::vector<std::string> s_pending_guests;
std::string s_last_detected_guest;

std::optional<double> RequestedDisplayAspect()
{
  const char* value = std::getenv("MOH_NATIVE_VIDEO_ASPECT");
  if (!value || !*value)
    return std::nullopt;

  const std::string v = Lower(value);
  if (v == "auto" || v == "source")
    return std::nullopt;
  if (v == "stretch" || v == "fill" || v == "window")
    return 0.0;
  if (v == "4:3" || v == "4/3")
    return 4.0 / 3.0;
  if (v == "16:9" || v == "16/9")
    return 16.0 / 9.0;

  if (const std::size_t colon = v.find(':'); colon != std::string::npos)
  {
    const std::string lhs = v.substr(0, colon);
    const std::string rhs = v.substr(colon + 1);
    char* end_num = nullptr;
    char* end_den = nullptr;
    const double num = std::strtod(lhs.c_str(), &end_num);
    const double den = std::strtod(rhs.c_str(), &end_den);
    if (end_num != lhs.c_str() && end_den != rhs.c_str() && end_num && end_den &&
        *end_num == '\0' && *end_den == '\0' && std::isfinite(num) &&
        std::isfinite(den) && num > 0.0 && den > 0.0)
    {
      return std::clamp(num / den, 0.5, 3.0);
    }
  }

  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end != value && end && *end == '\0' && std::isfinite(parsed) && parsed > 0.0)
    return std::clamp(parsed, 0.5, 3.0);

  return std::nullopt;
}

#if defined(MOH_NATIVE_VIDEO_FFMPEG)
struct MemoryReader
{
  const std::vector<u8>* bytes = nullptr;
  std::size_t offset = 0;
};

struct Decoder
{
  std::string guest_name;
  std::string source_description;
  bool source_ps3 = false;
  std::vector<u8> encoded;
  MemoryReader reader;
  AVIOContext* io = nullptr;
  AVFormatContext* format = nullptr;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  SwsContext* sws = nullptr;
  int video_stream = -1;
  AVRational time_base{1, 1};
  double fps = 30.0;
  double display_aspect = 4.0 / 3.0;
  double first_pts = 0.0;
  bool have_first_pts = false;
  double current_pts = 0.0;
  std::uint64_t decoded_frames = 0;
  bool eof = false;
  bool flush_sent = false;
  bool rgba_dirty = false;
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> rgba;
  std::unique_ptr<AbstractTexture> texture;
  Clock::time_point started{};

  ~Decoder()
  {
    texture.reset();
    if (sws)
      sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    if (format)
      avformat_close_input(&format);
    if (io)
      avio_context_free(&io);
  }
};

std::unique_ptr<Decoder> s_decoder;

int ReadPacket(void* opaque, u8* buffer, int buffer_size)
{
  auto* reader = static_cast<MemoryReader*>(opaque);
  if (!reader || !reader->bytes || buffer_size <= 0 || reader->offset >= reader->bytes->size())
    return AVERROR_EOF;

  const std::size_t count =
      std::min<std::size_t>(static_cast<std::size_t>(buffer_size),
                            reader->bytes->size() - reader->offset);
  std::memcpy(buffer, reader->bytes->data() + reader->offset, count);
  reader->offset += count;
  return static_cast<int>(count);
}

std::int64_t SeekPacket(void* opaque, std::int64_t offset, int whence)
{
  auto* reader = static_cast<MemoryReader*>(opaque);
  if (!reader || !reader->bytes)
    return -1;
  if ((whence & AVSEEK_SIZE) != 0)
    return static_cast<std::int64_t>(reader->bytes->size());

  const int origin = whence & 0xffff;
  std::int64_t base = 0;
  if (origin == SEEK_CUR)
    base = static_cast<std::int64_t>(reader->offset);
  else if (origin == SEEK_END)
    base = static_cast<std::int64_t>(reader->bytes->size());
  else if (origin != SEEK_SET)
    return -1;

  const std::int64_t target = base + offset;
  if (target < 0 || static_cast<std::uint64_t>(target) > reader->bytes->size())
    return -1;
  reader->offset = static_cast<std::size_t>(target);
  return target;
}

void CloseDecoder()
{
  s_decoder.reset();
}

double ResolveDisplayAspect(Decoder& decoder)
{
  if (const auto requested = RequestedDisplayAspect())
    return *requested;

  if (!decoder.frame || decoder.frame->width <= 0 || decoder.frame->height <= 0)
    return 4.0 / 3.0;

  AVStream* stream = nullptr;
  if (decoder.format && decoder.video_stream >= 0 &&
      decoder.video_stream < static_cast<int>(decoder.format->nb_streams))
  {
    stream = decoder.format->streams[decoder.video_stream];
  }

  AVRational sar{0, 1};
  if (stream)
    sar = av_guess_sample_aspect_ratio(decoder.format, stream, decoder.frame);
  if (sar.num <= 0 || sar.den <= 0)
    sar = decoder.frame->sample_aspect_ratio;
  if ((sar.num <= 0 || sar.den <= 0) && decoder.codec)
    sar = decoder.codec->sample_aspect_ratio;
  if (sar.num <= 0 || sar.den <= 0)
    sar = AVRational{1, 1};

  const double coded_aspect =
      static_cast<double>(decoder.frame->width) / static_cast<double>(decoder.frame->height);
  const double pixel_aspect = av_q2d(sar);
  double display_aspect = coded_aspect * pixel_aspect;

  // Frontline stores movie surfaces in console-oriented sizes.  In particular
  // the PS3 remaster can expose a 512x512 MPEG2 surface even though the movie
  // is authored for a 4:3 display.  The GC 640x448 surface is likewise not
  // intended to be shown with square pixels.
  const std::string file = Filename(decoder.guest_name);
  const bool frontline_movie =
      file.ends_with(".mpc") || file.ends_with(".mpcx") || file.ends_with(".asf");
  const bool legacy_surface =
      (decoder.frame->width == 640 && decoder.frame->height == 448) ||
      (decoder.frame->width == 512 && decoder.frame->height == 512);
  const bool square_sar = std::abs(pixel_aspect - 1.0) < 0.001;
  if (frontline_movie && legacy_surface && square_sar &&
      !EnvSwitch("MOH_NATIVE_VIDEO_SQUARE_PIXELS", false))
    display_aspect = 4.0 / 3.0;

  if (!std::isfinite(display_aspect) || display_aspect < 0.5 || display_aspect > 3.0)
    display_aspect = 4.0 / 3.0;
  return display_aspect;
}

bool ConvertFrame(Decoder& decoder)
{
  if (!decoder.frame || decoder.frame->width <= 0 || decoder.frame->height <= 0)
    return false;

  const u32 width = static_cast<u32>(decoder.frame->width);
  const u32 height = static_cast<u32>(decoder.frame->height);
  decoder.sws = sws_getCachedContext(
      decoder.sws, decoder.frame->width, decoder.frame->height,
      static_cast<AVPixelFormat>(decoder.frame->format), decoder.frame->width,
      decoder.frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!decoder.sws)
    return false;

  decoder.width = width;
  decoder.height = height;
  decoder.display_aspect = ResolveDisplayAspect(decoder);
  decoder.rgba.resize(static_cast<std::size_t>(width) * height * 4u);
  u8* dst[4] = {decoder.rgba.data(), nullptr, nullptr, nullptr};
  int dst_stride[4] = {static_cast<int>(width * 4u), 0, 0, 0};
  sws_scale(decoder.sws, decoder.frame->data, decoder.frame->linesize, 0,
            decoder.frame->height, dst, dst_stride);

  // The GC MPC and PS3 MPCX streams can expose discontinuous MPEG timestamps.
  // Frontline's movies are constant-rate, so use decoded-frame cadence as the
  // playback clock. Otherwise the first frame can remain on screen indefinitely.
  decoder.current_pts =
      static_cast<double>(decoder.decoded_frames) / std::max(decoder.fps, 1.0);
  ++decoder.decoded_frames;
  decoder.rgba_dirty = true;
  return true;
}

bool DecodeNextFrame(Decoder& decoder)
{
  for (;;)
  {
    int receive = avcodec_receive_frame(decoder.codec, decoder.frame);
    if (receive == 0)
      return ConvertFrame(decoder);
    if (receive != AVERROR(EAGAIN) && receive != AVERROR_EOF)
      return false;

    if (decoder.eof)
    {
      if (!decoder.flush_sent)
      {
        decoder.flush_sent = true;
        if (avcodec_send_packet(decoder.codec, nullptr) < 0)
          return false;
        continue;
      }
      return false;
    }

    int read = 0;
    do
    {
      av_packet_unref(decoder.packet);
      read = av_read_frame(decoder.format, decoder.packet);
      if (read < 0)
      {
        decoder.eof = true;
        break;
      }
      if (decoder.packet->stream_index != decoder.video_stream)
        av_packet_unref(decoder.packet);
    } while (read >= 0 && decoder.packet->stream_index != decoder.video_stream);

    if (decoder.eof)
      continue;

    const int send = avcodec_send_packet(decoder.codec, decoder.packet);
    av_packet_unref(decoder.packet);
    if (send < 0 && send != AVERROR(EAGAIN))
      return false;
  }
}

bool OpenEncoded(std::string_view guest_name, EncodedMovie movie)
{
  CloseDecoder();
  auto decoder = std::make_unique<Decoder>();
  decoder->guest_name = std::string(guest_name);
  decoder->source_description = std::move(movie.description);
  decoder->source_ps3 = movie.ps3;
  decoder->encoded = std::move(movie.bytes);
  decoder->reader = {&decoder->encoded, 0};

  constexpr int io_size = 64 * 1024;
  u8* io_buffer = static_cast<u8*>(av_malloc(io_size));
  if (!io_buffer)
    return false;
  decoder->io = avio_alloc_context(io_buffer, io_size, 0, &decoder->reader,
                                   &ReadPacket, nullptr, &SeekPacket);
  if (!decoder->io)
  {
    av_free(io_buffer);
    return false;
  }

  decoder->format = avformat_alloc_context();
  if (!decoder->format)
    return false;
  decoder->format->pb = decoder->io;
  decoder->format->flags |= AVFMT_FLAG_CUSTOM_IO;

  if (avformat_open_input(&decoder->format, nullptr, nullptr, nullptr) < 0 ||
      avformat_find_stream_info(decoder->format, nullptr) < 0)
  {
    return false;
  }

  const AVCodec* codec = nullptr;
  decoder->video_stream =
      av_find_best_stream(decoder->format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (decoder->video_stream < 0 || !codec)
    return false;

  decoder->codec = avcodec_alloc_context3(codec);
  if (!decoder->codec)
    return false;
  AVStream* stream = decoder->format->streams[decoder->video_stream];
  if (avcodec_parameters_to_context(decoder->codec, stream->codecpar) < 0 ||
      avcodec_open2(decoder->codec, codec, nullptr) < 0)
  {
    return false;
  }

  decoder->packet = av_packet_alloc();
  decoder->frame = av_frame_alloc();
  if (!decoder->packet || !decoder->frame)
    return false;

  decoder->time_base = stream->time_base;
  const AVRational guessed = av_guess_frame_rate(decoder->format, stream, nullptr);
  if (guessed.num > 0 && guessed.den > 0)
    decoder->fps = std::clamp(av_q2d(guessed), 1.0, 240.0);

  s_decoder = std::move(decoder);
  if (!DecodeNextFrame(*s_decoder))
  {
    CloseDecoder();
    return false;
  }
  // Probing/codec startup can be noticeable for large MPCX/Bink files.  Do not
  // count that time as playback time or the first Present will skip frames.
  s_decoder->started = Clock::now();

  std::fprintf(stderr,
               "[moh-native-video] START guest=%s source=%s codec=%s %ux%u fps=%.3f dar=%.4f bytes=%zu\n",
               s_decoder->guest_name.c_str(), s_decoder->source_description.c_str(), codec->name,
               s_decoder->width, s_decoder->height, s_decoder->fps, s_decoder->display_aspect,
               s_decoder->encoded.size());
  return true;
}

bool TryOpen(std::string_view guest_name)
{
  const Preference preference = GetPreference();
  const bool allow_ps3 = preference != Preference::GCOnly;
  const bool allow_gc = preference != Preference::PS3Only;
  const bool gc_first = preference == Preference::GCFirst || preference == Preference::GCOnly;

  auto try_ps3 = [&]() {
    if (!allow_ps3)
      return false;
    auto movie = ReadPS3(guest_name);
    if (!movie)
      return false;
    if (OpenEncoded(guest_name, std::move(*movie)))
      return true;
    std::fprintf(stderr,
                 "[moh-native-video] PS3 movie decode rejected for %.*s -> trying GC original\n",
                 static_cast<int>(guest_name.size()), guest_name.data());
    return false;
  };

  auto try_gc = [&]() {
    if (!allow_gc)
      return false;
    auto movie = ReadGC(guest_name);
    return movie && OpenEncoded(guest_name, std::move(*movie));
  };

  const bool opened = gc_first ? (try_gc() || try_ps3()) : (try_ps3() || try_gc());
  if (!opened)
  {
    static unsigned misses = 0;
    if (misses++ < 16)
    {
      std::fprintf(stderr,
                   "[moh-native-video] native decode unavailable guest=%.*s -> guest VP6/GC fallback\n",
                   static_cast<int>(guest_name.size()), guest_name.data());
    }
  }
  return opened;
}
#else
void CloseDecoder() {}
#endif
}  // namespace

void NotifyGuestRead(std::string_view guest_name, std::uint64_t file_offset)
{
  if (!EnvSwitch("MOH_NATIVE_VIDEO", true) || !LooksLikeMovie(guest_name) || file_offset > 0x10000)
    return;

  std::scoped_lock lock(s_request_mutex);

  const std::string normalized = Lower(std::string(guest_name));
  if (!s_last_detected_guest.empty() && Lower(s_last_detected_guest) == normalized)
    return;

#if defined(MOH_NATIVE_VIDEO_FFMPEG)
  if (s_decoder && Lower(s_decoder->guest_name) == normalized)
  {
    s_last_detected_guest = std::string(guest_name);
    return;
  }
#endif

  const bool already_queued =
      std::any_of(s_pending_guests.begin(), s_pending_guests.end(),
                  [&](const std::string& queued) { return Lower(queued) == normalized; });
  s_last_detected_guest = std::string(guest_name);
  if (already_queued)
    return;

  if (s_pending_guests.size() >= 16)
    s_pending_guests.erase(s_pending_guests.begin());

  s_pending_guests.emplace_back(guest_name);
  std::fprintf(stderr, "[moh-native-video] QUEUE guest=%s depth=%zu\n",
               s_pending_guests.back().c_str(), s_pending_guests.size());
}

void PrepareFrame()
{
  if (!EnvSwitch("MOH_NATIVE_VIDEO", true))
  {
    Stop();
    return;
  }

#if !defined(MOH_NATIVE_VIDEO_FFMPEG)
  static bool logged = false;
  if (MohPcLayer::IsMovieActive() && !logged)
  {
    logged = true;
    std::fprintf(stderr,
                 "[moh-native-video] FFmpeg not linked -> original guest movie path remains active\n");
  }
  return;
#else
  // The guest VP6 flag is a detection signal only. On the static recomp it can
  // toggle OFF immediately after the source file is opened. Once queued, the
  // host decoder owns the movie lifetime until EOF.
  if (!s_decoder)
  {
    std::string pending;
    {
      std::scoped_lock lock(s_request_mutex);
      if (!s_pending_guests.empty())
      {
        pending = std::move(s_pending_guests.front());
        s_pending_guests.erase(s_pending_guests.begin());
      }
    }

    if (!pending.empty())
      (void)TryOpen(pending);
  }

  if (!s_decoder)
    return;

  const double elapsed = std::chrono::duration<double>(Clock::now() - s_decoder->started).count();
  const double fps = std::max(s_decoder->fps, 1.0);
  const std::uint64_t wanted_frame =
      static_cast<std::uint64_t>(std::max(0.0, std::floor(elapsed * fps)));
  unsigned catches = 0;
  bool finished = false;
  while (s_decoder->decoded_frames <= wanted_frame && catches++ < 12)
  {
    if (!DecodeNextFrame(*s_decoder))
    {
      finished = true;
      break;
    }
  }

  if (finished)
  {
    std::fprintf(stderr, "[moh-native-video] END guest=%s frames=%llu source=%s\n",
                 s_decoder->guest_name.c_str(),
                 static_cast<unsigned long long>(s_decoder->decoded_frames),
                 s_decoder->source_ps3 ? "PS3" : "GC");
    CloseDecoder();
    return;
  }

  static std::string progress_guest;
  static std::uint64_t last_logged_frame = 0;
  if (progress_guest != s_decoder->guest_name)
  {
    progress_guest = s_decoder->guest_name;
    last_logged_frame = 0;
  }
  if (s_decoder->decoded_frames >= last_logged_frame + 120)
  {
    last_logged_frame = s_decoder->decoded_frames;
    std::fprintf(stderr,
                 "[moh-native-video] PLAY guest=%s frame=%llu t=%.2f source=%s\n",
                 s_decoder->guest_name.c_str(),
                 static_cast<unsigned long long>(s_decoder->decoded_frames), elapsed,
                 s_decoder->source_ps3 ? "PS3" : "GC");
  }

  if (!s_decoder->rgba_dirty || !g_gfx || s_decoder->width == 0 || s_decoder->height == 0)
    return;

  if (!s_decoder->texture || s_decoder->texture->GetWidth() != s_decoder->width ||
      s_decoder->texture->GetHeight() != s_decoder->height)
  {
    const TextureConfig config(s_decoder->width, s_decoder->height, 1, 1, 1,
                               AbstractTextureFormat::RGBA8, 0,
                               AbstractTextureType::Texture_2DArray);
    s_decoder->texture = g_gfx->CreateTexture(config, "MOH native movie frame");
    if (!s_decoder->texture)
      return;
  }

  s_decoder->texture->Load(0, s_decoder->width, s_decoder->height, s_decoder->width,
                           s_decoder->rgba.data(), s_decoder->rgba.size());
  s_decoder->texture->FinishedRendering();
  s_decoder->rgba_dirty = false;
#endif
}

PresentFrame GetPresentFrame()
{
#if defined(MOH_NATIVE_VIDEO_FFMPEG)
  if (s_decoder && s_decoder->texture)
    return {s_decoder->texture.get(), s_decoder->width, s_decoder->height,
            static_cast<float>(s_decoder->display_aspect)};
#endif
  return {};
}

void Stop()
{
#if defined(MOH_NATIVE_VIDEO_FFMPEG)
  CloseDecoder();
#endif
  std::scoped_lock lock(s_request_mutex);
  s_pending_guests.clear();
  s_last_detected_guest.clear();
}
}  // namespace MOHFrontline::NativeVideo
