#include "moderngekko/nod_disc_source.hpp"

#ifdef MODERNGEKKO_HAVE_NOD
#include <nod.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace moderngekko
{
#ifdef MODERNGEKKO_HAVE_NOD
namespace
{
struct NodHandleOwner
{
  NodHandle* value = nullptr;
  NodHandleOwner() = default;
  explicit NodHandleOwner(NodHandle* handle) : value(handle) {}
  NodHandleOwner(const NodHandleOwner&) = delete;
  NodHandleOwner& operator=(const NodHandleOwner&) = delete;
  NodHandleOwner(NodHandleOwner&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
  NodHandleOwner& operator=(NodHandleOwner&& other) noexcept
  {
    if (this != &other)
    {
      nod_free(value);
      value = std::exchange(other.value, nullptr);
    }
    return *this;
  }
  ~NodHandleOwner() { nod_free(value); }
};

std::string NormalizePath(std::string_view input)
{
  std::string value;
  value.reserve(input.size());
  for (char ch : input)
  {
    if (ch == '\\')
      ch = '/';
    if (value.empty() && ch == '/')
      continue;
    value.push_back(ch);
  }
  while (value.starts_with("./"))
    value.erase(0, 2);
  while (!value.empty() && value.back() == '/')
    value.pop_back();
  return value;
}

struct FileEntry
{
  std::uint32_t index = 0;
  std::uint32_t size = 0;
};

struct DirFrame
{
  std::uint32_t end = 0;
  std::string path;
};

class NodDiscSource final : public DiscVfsSource
{
public:
  static std::unique_ptr<NodDiscSource> Open(const std::filesystem::path& image,
                                             std::string* error)
  {
    const auto u8 = std::filesystem::absolute(image).u8string();
    const std::string utf8(u8.begin(), u8.end());

    NodDiscOptions options{};
    options.preloader_threads = 2u;
    NodHandle* raw_disc = nullptr;
    if (nod_disc_open(utf8.c_str(), &options, &raw_disc) != NOD_RESULT_OK || !raw_disc)
    {
      if (error)
      {
        *error = "encounter/nod could not open disc image";
        if (const char* detail = nod_error_message())
          *error += ": " + std::string(detail);
      }
      return {};
    }

    NodHandleOwner disc(raw_disc);
    NodDiscHeader header{};
    constexpr std::uint8_t gc_magic[4]{0xc2u, 0x33u, 0x9fu, 0x3du};
    if (nod_disc_header(disc.value, &header) != NOD_RESULT_OK ||
        !std::equal(std::begin(gc_magic), std::end(gc_magic), header.gcn_magic))
    {
      if (error)
        *error = "disc image is not a GameCube image";
      return {};
    }

    NodPartitionOptions partition_options{};
    NodHandle* raw_partition = nullptr;
    if (nod_disc_open_partition_kind(disc.value, NOD_PARTITION_KIND_DATA,
                                     &partition_options, &raw_partition) != NOD_RESULT_OK ||
        !raw_partition)
    {
      if (error)
        *error = "encounter/nod could not open GameCube data partition";
      return {};
    }

    auto source = std::unique_ptr<NodDiscSource>(
        new NodDiscSource(std::move(disc), NodHandleOwner(raw_partition)));
    if (!source->BuildIndex(error))
      return {};
    return source;
  }

  std::uint64_t GetSize() const override
  {
    return nod_disc_size(m_disc.value);
  }

  bool Read(std::uint64_t offset, std::span<std::uint8_t> output) override
  {
    std::scoped_lock lock(m_mutex);
    if (output.empty())
      return true;
    const std::uint64_t size = GetSize();
    if (offset > size || output.size() > size - offset)
      return false;
    if (nod_seek(m_disc.value, static_cast<std::int64_t>(offset), SEEK_SET) < 0)
      return false;

    std::size_t done = 0;
    while (done < output.size())
    {
      const std::int64_t got =
          nod_read(m_disc.value, output.data() + done, output.size() - done);
      if (got <= 0)
        return false;
      done += static_cast<std::size_t>(got);
    }
    return true;
  }

  bool HasFile(std::string_view path) const override
  {
    return m_files.contains(NormalizePath(path));
  }

  std::uint64_t GetFileSize(std::string_view path) const override
  {
    const auto it = m_files.find(NormalizePath(path));
    return it == m_files.end() ? 0u : it->second.size;
  }

  bool ReadFile(std::string_view path, std::uint64_t offset,
                std::span<std::uint8_t> output) override
  {
    const auto it = m_files.find(NormalizePath(path));
    if (it == m_files.end())
      return false;
    const FileEntry entry = it->second;
    if (offset > entry.size || output.size() > entry.size - offset)
      return false;

    std::scoped_lock lock(m_mutex);
    NodHandle* raw_file = nullptr;
    if (nod_partition_open_file(m_partition.value, entry.index, &raw_file) != NOD_RESULT_OK ||
        !raw_file)
      return false;
    NodHandleOwner file(raw_file);
    if (offset && nod_seek(file.value, static_cast<std::int64_t>(offset), SEEK_SET) < 0)
      return false;

    std::size_t done = 0;
    while (done < output.size())
    {
      const std::int64_t got =
          nod_read(file.value, output.data() + done, output.size() - done);
      if (got <= 0)
        return false;
      done += static_cast<std::size_t>(got);
    }
    return true;
  }

private:
  NodDiscSource(NodHandleOwner disc, NodHandleOwner partition)
      : m_disc(std::move(disc)), m_partition(std::move(partition))
  {
  }

  struct WalkContext
  {
    NodDiscSource* self = nullptr;
    std::vector<DirFrame> dirs;
    bool ok = true;
  };

  static std::uint32_t Walk(std::uint32_t index, NodNodeKind kind, const char* raw_name,
                            std::uint32_t size, void* user)
  {
    auto& ctx = *static_cast<WalkContext*>(user);
    while (!ctx.dirs.empty() && index >= ctx.dirs.back().end)
      ctx.dirs.pop_back();

    const std::string name = raw_name ? std::string(raw_name) : std::string{};
    if (index == 0u && kind == NOD_NODE_KIND_DIRECTORY)
    {
      ctx.dirs.push_back({size, {}});
      return 1u;
    }
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
    {
      ctx.ok = false;
      return NOD_FST_STOP;
    }

    std::string path = ctx.dirs.empty() ? name : ctx.dirs.back().path + "/" + name;
    if (kind == NOD_NODE_KIND_DIRECTORY)
    {
      if (size <= index)
      {
        ctx.ok = false;
        return NOD_FST_STOP;
      }
      ctx.dirs.push_back({size, std::move(path)});
      return index + 1u;
    }

    ctx.self->m_files.emplace(std::move(path), FileEntry{index, size});
    return index + 1u;
  }

  bool BuildIndex(std::string* error)
  {
    WalkContext context{this};
    nod_partition_iterate_fst(m_partition.value, &Walk, &context);
    if (!context.ok)
    {
      if (error)
        *error = "invalid GameCube FST while building native disc VFS";
      return false;
    }
    return true;
  }

  NodHandleOwner m_disc;
  NodHandleOwner m_partition;
  std::unordered_map<std::string, FileEntry> m_files;
  mutable std::mutex m_mutex;
};
}
#endif

std::unique_ptr<DiscVfsSource>
CreateNodDiscSource(const std::filesystem::path& image, std::string* error)
{
#ifdef MODERNGEKKO_HAVE_NOD
  return NodDiscSource::Open(image, error);
#else
  if (error)
    *error = "ModernGekko was built without encounter/nod support";
  (void)image;
  return {};
#endif
}
}
