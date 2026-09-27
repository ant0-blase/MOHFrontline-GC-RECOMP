#pragma once

#include "moderngekko/disc_interface.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace moderngekko
{
class DiscVfsSource : public DiscSource
{
public:
  virtual bool HasFile(std::string_view path) const = 0;
  virtual std::uint64_t GetFileSize(std::string_view path) const = 0;
  virtual bool ReadFile(std::string_view path, std::uint64_t offset,
                        std::span<std::uint8_t> output) = 0;
};

std::unique_ptr<DiscVfsSource>
CreateNodDiscSource(const std::filesystem::path& image, std::string* error);
}
