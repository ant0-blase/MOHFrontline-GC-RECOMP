#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace MOHFrontline::NativePCStatus
{
enum class Domain : std::size_t
{
  FileIO,
  VIV,
  AssetCPU,
  Audio,
  Video,
  Texture,
  Font,
  Render,
  Count,
};

inline constexpr std::array<const char*, static_cast<std::size_t>(Domain::Count)> kNames = {
    "FILE-IO", "VIV", "ASSET-CPU", "AUDIO", "VIDEO", "TEXTURE", "FONT", "RENDER"};

struct Counters
{
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Domain::Count)> native{};
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Domain::Count)> fallback{};
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Domain::Count)> native_logs{};
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Domain::Count)> fallback_logs{};

  Counters()
  {
    for (auto& value : native) value.store(0, std::memory_order_relaxed);
    for (auto& value : fallback) value.store(0, std::memory_order_relaxed);
    for (auto& value : native_logs) value.store(0, std::memory_order_relaxed);
    for (auto& value : fallback_logs) value.store(0, std::memory_order_relaxed);
  }
};

inline Counters& Get()
{
  static Counters counters;
  return counters;
}

inline const char* Name(Domain domain)
{
  const auto index = static_cast<std::size_t>(domain);
  return index < kNames.size() ? kNames[index] : "UNKNOWN";
}

inline void Initialize()
{
  static std::atomic<bool> once = false;
  if (!once.exchange(true, std::memory_order_relaxed))
  {
    std::fprintf(stderr,
                 "[NATIVE-PC] takeover tracker active | FILE/VIV=host | "
                 "MSH/CPT/SKL/DMF/GSH/GFN/etc semantic CPU=static-recomp | "
                 "audio/video=host | render=host when accepted | "
                 "ModernGekko fallbacks are tagged explicitly\n");
  }
}

inline void Native(Domain domain, std::string_view subject, std::string_view detail = {})
{
  Counters& counters = Get();
  const auto index = static_cast<std::size_t>(domain);
  counters.native[index].fetch_add(1, std::memory_order_relaxed);
  const auto log_index = counters.native_logs[index].fetch_add(1, std::memory_order_relaxed);
  if (log_index < 48)
  {
    std::fprintf(stderr, "[NATIVE-PC] %-9s %.*s",
                 Name(domain), static_cast<int>(subject.size()), subject.data());
    if (!detail.empty())
      std::fprintf(stderr, " | %.*s", static_cast<int>(detail.size()), detail.data());
    std::fputc('\n', stderr);
  }
}

inline void Fallback(Domain domain, std::string_view subject, std::string_view detail = {})
{
  Counters& counters = Get();
  const auto index = static_cast<std::size_t>(domain);
  counters.fallback[index].fetch_add(1, std::memory_order_relaxed);
  const auto log_index = counters.fallback_logs[index].fetch_add(1, std::memory_order_relaxed);
  if (log_index < 96)
  {
    std::fprintf(stderr, "[FALLBACK-MODERNGEKKO] %-9s %.*s",
                 Name(domain), static_cast<int>(subject.size()), subject.data());
    if (!detail.empty())
      std::fprintf(stderr, " | %.*s", static_cast<int>(detail.size()), detail.data());
    std::fputc('\n', stderr);
  }
}

inline void Summary()
{
  Counters& counters = Get();
  std::fprintf(stderr, "[NATIVE-PC] ================= TAKEOVER SUMMARY ================\n");
  std::uint64_t native_total = 0;
  std::uint64_t fallback_total = 0;
  for (std::size_t i = 0; i < static_cast<std::size_t>(Domain::Count); ++i)
  {
    const auto native = counters.native[i].load(std::memory_order_relaxed);
    const auto fallback = counters.fallback[i].load(std::memory_order_relaxed);
    native_total += native;
    fallback_total += fallback;
    std::fprintf(stderr,
                 "[NATIVE-PC] %-9s native=%llu fallback-ModernGekko=%llu\n",
                 kNames[i],
                 static_cast<unsigned long long>(native),
                 static_cast<unsigned long long>(fallback));
  }

  const double ratio =
      native_total + fallback_total == 0 ? 0.0 :
      (100.0 * static_cast<double>(native_total) /
       static_cast<double>(native_total + fallback_total));
  std::fprintf(stderr,
               "[NATIVE-PC] total native=%llu fallback=%llu takeover=%.2f%% "
               "(event-count metric, not code-removal percentage)\n",
               static_cast<unsigned long long>(native_total),
               static_cast<unsigned long long>(fallback_total), ratio);
  std::fprintf(stderr, "[NATIVE-PC] =====================================================\n");
}
}  // namespace MOHFrontline::NativePCStatus
