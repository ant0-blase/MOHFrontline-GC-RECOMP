// Read-only, extraction-less PlayStation 3 finalized PKG reader.
//
// This intentionally implements only the part needed by MOH Frontline's asset
// VFS: parse the encrypted item table and decrypt arbitrary file ranges on
// demand. It does not install a package and it does not write retail contents
// to disk.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PS3PkgReader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <mbedtls/aes.h>

namespace PS3Pkg
{
namespace
{
constexpr std::uint32_t PKG_MAGIC = 0x7f504b47u;
constexpr std::uint16_t PKG_REVISION_FINALIZED = 0x8000u;
constexpr std::uint16_t PKG_TYPE_PS3 = 0x0001u;
constexpr std::size_t PKG_HEADER_MIN = 0x80u;
constexpr std::size_t ITEM_RECORD_SIZE = 0x20u;
constexpr std::size_t MAX_ITEM_NAME = 16u * 1024u;
constexpr std::uint32_t META_ITEM_TABLE = 0x0du;

// NPDRM finalized PS3 PKG AES key. This is the platform package-format key,
// not a per-user license or title key.
constexpr std::array<u8, 16> PS3_PKG_KEY{
    0x2e, 0x7b, 0x71, 0xd7, 0xc9, 0xc9, 0xa1, 0x4e,
    0xa3, 0x22, 0x1f, 0x18, 0x88, 0x28, 0xb8, 0xf8};

std::uint16_t ReadBE16(const u8* p)
{
  return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
}

std::uint32_t ReadBE32(const u8* p)
{
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint64_t ReadBE64(const u8* p)
{
  return (std::uint64_t(ReadBE32(p)) << 32) | ReadBE32(p + 4);
}

std::string NormalizePath(std::string value)
{
  std::replace(value.begin(), value.end(), '\\', '/');
  while (value.rfind("./", 0) == 0)
    value.erase(0, 2);
  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());
  return value;
}

bool SafePath(std::string_view value)
{
  if (value.empty() || value.front() == '/')
    return false;

  std::size_t start = 0;
  while (start <= value.size())
  {
    const std::size_t slash = value.find('/', start);
    const std::string_view part =
        value.substr(start, slash == std::string_view::npos ? value.size() - start
                                                            : slash - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
  }
  return true;
}

void AddCounter(std::array<u8, 16>* counter, std::uint64_t blocks)
{
  std::uint64_t carry = blocks;
  for (int i = 15; i >= 0 && carry; --i)
  {
    const std::uint64_t sum = (*counter)[static_cast<std::size_t>(i)] + (carry & 0xffu);
    (*counter)[static_cast<std::size_t>(i)] = static_cast<u8>(sum & 0xffu);
    carry = (carry >> 8) + (sum >> 8);
  }
}

void IncrementCounter(std::array<u8, 16>* counter)
{
  for (int i = 15; i >= 0; --i)
  {
    u8& value = (*counter)[static_cast<std::size_t>(i)];
    ++value;
    if (value != 0)
      break;
  }
}

bool IsDirectoryFlags(std::uint32_t flags)
{
  const std::uint8_t type = static_cast<std::uint8_t>(flags & 0xffu);
  return type == 0x04u || type == 0x12u;
}
}  // namespace

class Reader::Impl
{
public:
  std::filesystem::path path;
  mutable std::ifstream file;
  mutable std::mutex mutex;
  std::uint64_t file_size = 0;
  std::uint64_t data_offset = 0;
  std::uint64_t data_size = 0;
  std::array<u8, 16> iv{};
  std::string content_id;
  std::vector<Entry> entries;

  bool ReadRaw(std::uint64_t offset, std::span<u8> output) const
  {
    if (output.empty())
      return true;
    if (offset > file_size || output.size() > file_size - offset)
      return false;

    std::scoped_lock lock(mutex);
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file)
      return false;
    return static_cast<bool>(
        file.read(reinterpret_cast<char*>(output.data()),
                  static_cast<std::streamsize>(output.size())));
  }

  bool ReadEncrypted(std::uint64_t relative_offset, std::span<u8> output) const
  {
    if (output.empty())
      return true;
    if (relative_offset > data_size || output.size() > data_size - relative_offset)
      return false;

    std::vector<u8> encrypted(output.size());
    if (!ReadRaw(data_offset + relative_offset, encrypted))
      return false;

    mbedtls_aes_context aes{};
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, PS3_PKG_KEY.data(), 128) != 0)
    {
      mbedtls_aes_free(&aes);
      return false;
    }

    const std::uint64_t block_index = relative_offset / 16u;
    const std::size_t initial_skip = static_cast<std::size_t>(relative_offset % 16u);
    std::array<u8, 16> counter = iv;
    AddCounter(&counter, block_index);

    std::array<u8, 16> stream{};
    std::size_t done = 0;
    std::size_t skip = initial_skip;

    while (done < output.size())
    {
      if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, counter.data(), stream.data()) != 0)
      {
        mbedtls_aes_free(&aes);
        return false;
      }

      const std::size_t take =
          std::min<std::size_t>(16u - skip, output.size() - done);
      for (std::size_t i = 0; i < take; ++i)
        output[done + i] = encrypted[done + i] ^ stream[skip + i];

      done += take;
      skip = 0;
      IncrementCounter(&counter);
    }

    mbedtls_aes_free(&aes);
    return true;
  }

  bool Parse(std::string* error)
  {
    file.open(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
      if (error)
        *error = "cannot open PS3 PKG";
      return false;
    }

    const std::streamoff end = file.tellg();
    if (end < static_cast<std::streamoff>(PKG_HEADER_MIN))
    {
      if (error)
        *error = "PS3 PKG is too small";
      return false;
    }
    file_size = static_cast<std::uint64_t>(end);

    std::array<u8, PKG_HEADER_MIN> header{};
    if (!ReadRaw(0, header))
    {
      if (error)
        *error = "cannot read PS3 PKG header";
      return false;
    }

    if (ReadBE32(header.data()) != PKG_MAGIC)
    {
      if (error)
        *error = "not an NPDRM PKG";
      return false;
    }

    const std::uint16_t revision = ReadBE16(header.data() + 0x04);
    const std::uint16_t type = ReadBE16(header.data() + 0x06);
    if (type != PKG_TYPE_PS3)
    {
      if (error)
        *error = "PKG is not a PS3 package";
      return false;
    }
    if (revision != PKG_REVISION_FINALIZED)
    {
      if (error)
        *error = "only finalized/retail PS3 PKG files are supported";
      return false;
    }

    const std::uint32_t metadata_offset = ReadBE32(header.data() + 0x08);
    const std::uint32_t metadata_count = ReadBE32(header.data() + 0x0c);
    const std::uint32_t item_count = ReadBE32(header.data() + 0x14);
    const std::uint64_t total_size = ReadBE64(header.data() + 0x18);
    data_offset = ReadBE64(header.data() + 0x20);
    data_size = ReadBE64(header.data() + 0x28);

    if (!item_count || item_count > 2'000'000u ||
        total_size > file_size || data_offset > file_size ||
        data_size > file_size - data_offset)
    {
      if (error)
        *error = "invalid PS3 PKG header bounds";
      return false;
    }

    std::memcpy(iv.data(), header.data() + 0x70, iv.size());
    content_id.assign(reinterpret_cast<const char*>(header.data() + 0x30), 0x30);
    if (const auto nul = content_id.find('\0'); nul != std::string::npos)
      content_id.resize(nul);

    std::uint32_t items_offset = 0;
    bool item_metadata_found = false;
    std::uint64_t meta = metadata_offset;
    for (std::uint32_t i = 0; i < metadata_count; ++i)
    {
      std::array<u8, 8> mh{};
      if (!ReadRaw(meta, mh))
      {
        if (error)
          *error = "truncated PS3 PKG metadata";
        return false;
      }
      const std::uint32_t id = ReadBE32(mh.data());
      const std::uint32_t size = ReadBE32(mh.data() + 4);
      if (meta + 8u > file_size || size > file_size - (meta + 8u))
      {
        if (error)
          *error = "invalid PS3 PKG metadata bounds";
        return false;
      }

      if (id == META_ITEM_TABLE && size >= 8u)
      {
        std::array<u8, 8> value{};
        if (!ReadRaw(meta + 8u, value))
          return false;
        items_offset = ReadBE32(value.data());
        const std::uint32_t items_size = ReadBE32(value.data() + 4);
        if (items_size < item_count * ITEM_RECORD_SIZE)
        {
          if (error)
            *error = "PS3 PKG item table is smaller than item count";
          return false;
        }
        item_metadata_found = true;
      }
      meta += 8u + size;
    }

    // Older PS3 packages may omit metadata id 0x0d. Their item table begins
    // at the start of the encrypted data area.
    if (!item_metadata_found)
      items_offset = 0;

    const std::uint64_t table_bytes =
        static_cast<std::uint64_t>(item_count) * ITEM_RECORD_SIZE;
    if (items_offset > data_size || table_bytes > data_size - items_offset)
    {
      if (error)
        *error = "PS3 PKG item table exceeds encrypted data";
      return false;
    }

    entries.clear();
    entries.reserve(item_count);
    for (std::uint32_t i = 0; i < item_count; ++i)
    {
      std::array<u8, ITEM_RECORD_SIZE> record{};
      const std::uint64_t record_offset =
          static_cast<std::uint64_t>(items_offset) +
          static_cast<std::uint64_t>(i) * ITEM_RECORD_SIZE;
      if (!ReadEncrypted(record_offset, record))
      {
        if (error)
          *error = "failed to decrypt PS3 PKG item table";
        return false;
      }

      const std::uint32_t name_offset = ReadBE32(record.data());
      const std::uint32_t name_size = ReadBE32(record.data() + 4);
      const std::uint64_t entry_data_offset = ReadBE64(record.data() + 8);
      const std::uint64_t entry_data_size = ReadBE64(record.data() + 16);
      const std::uint32_t flags = ReadBE32(record.data() + 24);

      if (!name_size || name_size > MAX_ITEM_NAME ||
          name_offset > data_size || name_size > data_size - name_offset ||
          entry_data_offset > data_size || entry_data_size > data_size - entry_data_offset)
      {
        if (error)
          *error = "invalid PS3 PKG item bounds";
        return false;
      }

      std::vector<u8> name_bytes(name_size);
      if (!ReadEncrypted(name_offset, name_bytes))
      {
        if (error)
          *error = "failed to decrypt PS3 PKG item name";
        return false;
      }

      std::string name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
      if (const auto nul = name.find('\0'); nul != std::string::npos)
        name.resize(nul);
      name = NormalizePath(std::move(name));
      if (!SafePath(name))
        continue;

      Entry entry;
      entry.path = std::move(name);
      entry.filename = std::filesystem::path(entry.path).filename().string();
      entry.data_offset = entry_data_offset;
      entry.size = entry_data_size;
      entry.flags = flags;
      entry.directory = IsDirectoryFlags(flags);
      entries.emplace_back(std::move(entry));
    }

    return true;
  }
};

Reader::Reader(std::unique_ptr<Impl> impl) : m_impl(std::move(impl))
{
}

Reader::~Reader() = default;

std::unique_ptr<Reader> Reader::Open(const std::filesystem::path& path, std::string* error)
{
  auto impl = std::make_unique<Impl>();
  impl->path = path;
  if (!impl->Parse(error))
    return {};
  return std::unique_ptr<Reader>(new Reader(std::move(impl)));
}

const std::filesystem::path& Reader::GetPath() const
{
  return m_impl->path;
}

const std::string& Reader::GetContentId() const
{
  return m_impl->content_id;
}

const std::vector<Entry>& Reader::GetEntries() const
{
  return m_impl->entries;
}

bool Reader::Read(const Entry& entry, std::uint64_t offset, std::span<u8> output) const
{
  if (entry.directory || offset > entry.size || output.size() > entry.size - offset)
    return false;
  if (entry.data_offset > m_impl->data_size ||
      entry.size > m_impl->data_size - entry.data_offset)
    return false;
  return m_impl->ReadEncrypted(entry.data_offset + offset, output);
}

bool Reader::Read(std::size_t entry_index, std::uint64_t offset, std::span<u8> output) const
{
  if (entry_index >= m_impl->entries.size())
    return false;
  return Read(m_impl->entries[entry_index], offset, output);
}
}  // namespace PS3Pkg
