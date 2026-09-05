#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PS3SshDecoder
{
struct Image
{
  std::string tag;

  std::uint32_t width = 0;
  std::uint32_t height = 0;

  std::uint8_t source_type = 0;

  // Always converted to host-side RGBA8.
  std::vector<std::uint8_t> rgba;
};

bool DecodeFile(const std::filesystem::path& path,
                std::vector<Image>* images,
                std::string* error = nullptr);
}
