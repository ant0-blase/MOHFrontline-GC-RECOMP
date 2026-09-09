#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace MOHFrontline::GCViv
{
struct Entry
{
  std::string name;
  std::uint64_t offset;
  std::uint64_t size;
};

inline std::uint32_t ReadBE(const unsigned char* p, unsigned width)
{
  std::uint32_t value = 0;
  for (unsigned i = 0; i < width; ++i)
    value = (value << 8) | p[i];
  return value;
}

// The span may contain just the directory probe, while file_size is the size
// of the complete archive. Frontline uses C0FB/BE24 for most archives and
// BIGF/BE32 for 2_2/comp.viv (which exceeds the 24-bit offset range).
inline std::optional<std::vector<Entry>> Parse(std::span<const unsigned char> bytes,
                                               std::uint64_t file_size)
{
  if (bytes.size() < 6 || bytes.size() > file_size)
    return std::nullopt;
  unsigned width;
  std::size_t pos;
  std::size_t limit = bytes.size();
  std::uint32_t count;
  const bool bigf = bytes.size() >= 16 && bytes[0] == 'B' && bytes[1] == 'I' &&
                    bytes[2] == 'G' && bytes[3] == 'F';
  if (bigf)
  {
    width = 4;
    pos = 16;
    count = ReadBE(bytes.data() + 8, 4);
    limit = ReadBE(bytes.data() + 12, 4);
    if (limit < pos || limit > bytes.size())
      return std::nullopt;
    // The retail BIGF size field excludes some padding; use the actual file
    // size for payload bounds, not header[4].
  }
  else if (bytes[0] == 0xc0 && bytes[1] == 0xfb)
  {
    width = 3;
    pos = 6;
    count = ReadBE(bytes.data() + 4, 2);
  }
  else
  {
    return std::nullopt;
  }
  if (!count || count > (limit - pos) / (2 * width + 2))
    return std::nullopt;

  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i)
  {
    if (2 * width > limit - pos)
      return std::nullopt;
    const auto offset = ReadBE(bytes.data() + pos, width);
    const auto size = ReadBE(bytes.data() + pos + width, width);
    pos += 2 * width;
    const auto begin = bytes.begin() + pos;
    const auto end = bytes.begin() + std::min(limit, pos + 256);
    const auto nul = std::find(begin, end, 0);
    if (nul == end || nul == begin || offset > file_size || size > file_size - offset)
      return std::nullopt;
    entries.push_back({std::string(reinterpret_cast<const char*>(bytes.data() + pos),
                                  static_cast<std::size_t>(nul - begin)), offset, size});
    pos += static_cast<std::size_t>(nul - begin) + 1;
  }
  if (bigf && pos != limit)
    return std::nullopt;
  for (const auto& entry : entries)
  {
    if (entry.size && entry.offset < pos)
      return std::nullopt;
  }
  return entries;
}
}  // namespace MOHFrontline::GCViv
