// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace PS3Pkg
{
struct Entry
{
  std::string path;
  std::string filename;
  std::uint64_t data_offset = 0;
  std::uint64_t size = 0;
  std::uint32_t flags = 0;
  bool directory = false;
};

class Reader
{
public:
  static std::unique_ptr<Reader> Open(const std::filesystem::path& path,
                                      std::string* error = nullptr);

  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  const std::filesystem::path& GetPath() const;
  const std::string& GetContentId() const;
  const std::vector<Entry>& GetEntries() const;

  bool Read(const Entry& entry, std::uint64_t offset, std::span<u8> output) const;
  bool Read(std::size_t entry_index, std::uint64_t offset, std::span<u8> output) const;

private:
  class Impl;
  explicit Reader(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> m_impl;
};
}  // namespace PS3Pkg
