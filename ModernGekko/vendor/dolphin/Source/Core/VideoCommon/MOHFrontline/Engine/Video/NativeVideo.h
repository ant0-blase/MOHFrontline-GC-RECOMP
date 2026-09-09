#pragma once

#include <cstdint>
#include <string_view>

class AbstractTexture;

namespace MOHFrontline::NativeVideo
{
struct PresentFrame
{
  const AbstractTexture* texture = nullptr;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Display aspect ratio (DAR), after codec/container sample-aspect correction.
  // 0 means "stretch to the current target rectangle".
  float display_aspect = 0.0f;

  explicit operator bool() const { return texture != nullptr && width != 0 && height != 0; }
};

// Called from the DVD/FST native VFS worker. This only records movie intent;
// all host decode/GPU upload work stays on the Presenter/video thread.
void NotifyGuestRead(std::string_view guest_name, std::uint64_t file_offset);

// Called once per host Present before utility drawing begins.
void PrepareFrame();
PresentFrame GetPresentFrame();

// Host movie lifetime/skip bridge. RequestSkip() is safe from the input thread;
// the actual decoder close happens on the presenter thread.
bool IsPlaying();
void RequestSkip();
void Stop();
}  // namespace MOHFrontline::NativeVideo
