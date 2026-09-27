// SPDX-License-Identifier: GPL-3.0-or-later
#include <nod.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
struct Handle {
  NodHandle* value = nullptr;
  explicit Handle(NodHandle* h = nullptr) : value(h) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { nod_free(value); }
};

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

const char* FormatName(NodFormat format) {
  switch (format) {
  case NOD_FORMAT_ISO: return "ISO";
  case NOD_FORMAT_CISO: return "CISO";
  case NOD_FORMAT_GCZ: return "GCZ";
  case NOD_FORMAT_NFS: return "NFS";
  case NOD_FORMAT_RVZ: return "RVZ";
  case NOD_FORMAT_WBFS: return "WBFS";
  case NOD_FORMAT_WIA: return "WIA";
  case NOD_FORMAT_TGC: return "TGC";
  }
  return "unknown";
}

bool WriteBlob(const fs::path& path, const NodBlob& blob) {
  if (!blob.data || blob.size == 0) return false;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(blob.data),
            static_cast<std::streamsize>(blob.size));
  return static_cast<bool>(out);
}

bool SafeName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos;
}

bool CopyFile(NodHandle* partition, std::uint32_t index, std::uint32_t size,
              const fs::path& destination, std::string* error) {
  NodHandle* raw = nullptr;
  if (nod_partition_open_file(partition, index, &raw) != NOD_RESULT_OK || !raw) {
    *error = "nod_partition_open_file failed";
    return false;
  }
  Handle file(raw);

  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    *error = ec.message();
    return false;
  }

  std::ofstream out(destination, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot create " + destination.string();
    return false;
  }

  std::array<std::uint8_t, 256 * 1024> buffer{};
  std::uint64_t remaining = size;
  while (remaining) {
    const std::size_t request =
        static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
    const std::int64_t got = nod_read(file.value, buffer.data(), request);
    if (got <= 0) {
      *error = "short read from nod file stream";
      return false;
    }
    out.write(reinterpret_cast<const char*>(buffer.data()),
              static_cast<std::streamsize>(got));
    if (!out) {
      *error = "write failed";
      return false;
    }
    remaining -= static_cast<std::uint64_t>(got);
  }
  return true;
}

struct DirFrame {
  std::uint32_t end = 0;
  fs::path path;
};

struct WalkState {
  NodHandle* partition = nullptr;
  fs::path output;
  std::vector<DirFrame> dirs;
  std::size_t executable_count = 1;
  bool ok = true;
  std::string error;
};

std::uint32_t Walk(std::uint32_t index, NodNodeKind kind, const char* raw_name,
                   std::uint32_t size, void* user) {
  auto& state = *static_cast<WalkState*>(user);
  if (!state.ok) return NOD_FST_STOP;

  while (!state.dirs.empty() && index >= state.dirs.back().end)
    state.dirs.pop_back();

  const std::string_view name =
      raw_name ? std::string_view(raw_name) : std::string_view{};
  if (index == 0 && kind == NOD_NODE_KIND_DIRECTORY) {
    state.dirs.push_back({size, {}});
    return 1;
  }
  if (!SafeName(name)) {
    state.ok = false;
    state.error = "unsafe FST name";
    return NOD_FST_STOP;
  }

  const fs::path base = state.dirs.empty() ? fs::path{} : state.dirs.back().path;
  const fs::path relative = base / std::string(name);

  if (kind == NOD_NODE_KIND_DIRECTORY) {
    if (size <= index) {
      state.ok = false;
      state.error = "invalid FST directory";
      return NOD_FST_STOP;
    }
    state.dirs.push_back({size, relative});
    return index + 1;
  }

  const std::string ext = Lower(relative.extension().string());
  if (ext == ".dol" || ext == ".elf" || ext == ".rel" || ext == ".bin") {
    if (!CopyFile(state.partition, index, size,
                  state.output / "files" / relative, &state.error)) {
      state.ok = false;
      return NOD_FST_STOP;
    }
    ++state.executable_count;
  }

  return index + 1;
}

int Run(const fs::path& image_arg, const fs::path& output_arg) {
  const auto image_u8 = fs::absolute(image_arg).u8string();
  const std::string image(image_u8.begin(), image_u8.end());

  NodDiscOptions options{};
  options.preloader_threads = 0;

  NodHandle* raw_disc = nullptr;
  if (nod_disc_open(image.c_str(), &options, &raw_disc) != NOD_RESULT_OK ||
      !raw_disc) {
    std::cerr << "nod could not open image";
    if (const char* detail = nod_error_message())
      std::cerr << ": " << detail;
    std::cerr << '\n';
    return 1;
  }
  Handle disc(raw_disc);

  NodDiscHeader header{};
  constexpr std::array<std::uint8_t, 4> magic{0xc2, 0x33, 0x9f, 0x3d};
  if (nod_disc_header(disc.value, &header) != NOD_RESULT_OK ||
      !std::equal(magic.begin(), magic.end(), header.gcn_magic)) {
    std::cerr << "not a GameCube disc image\n";
    return 1;
  }

  const std::string game_id(header.game_id, header.game_id + 6);
  if (game_id != "GMFE69") {
    std::cerr << "unsupported disc ID: " << game_id
              << " (expected GMFE69)\n";
    return 2;
  }

  NodPartitionOptions popt{};
  NodHandle* raw_partition = nullptr;
  if (nod_disc_open_partition_kind(disc.value, NOD_PARTITION_KIND_DATA, &popt,
                                   &raw_partition) != NOD_RESULT_OK ||
      !raw_partition) {
    std::cerr << "cannot open GameCube data partition\n";
    return 1;
  }
  Handle partition(raw_partition);

  NodPartitionMeta meta{};
  if (nod_partition_meta(partition.value, &meta) != NOD_RESULT_OK ||
      meta.raw_boot.size < 0x440 || meta.raw_dol.size < 0x100 ||
      meta.raw_fst.size < 12) {
    std::cerr << "incomplete partition metadata\n";
    return 1;
  }

  std::error_code ec;
  fs::remove_all(output_arg, ec);
  ec.clear();
  fs::create_directories(output_arg / "sys", ec);
  fs::create_directories(output_arg / "files", ec);
  fs::create_directories(output_arg / "meta", ec);
  if (ec) {
    std::cerr << "cannot create cache: " << ec.message() << '\n';
    return 1;
  }

  if (!WriteBlob(output_arg / "sys/boot.bin", meta.raw_boot) ||
      !WriteBlob(output_arg / "sys/main.dol", meta.raw_dol) ||
      !WriteBlob(output_arg / "sys/fst.bin", meta.raw_fst)) {
    std::cerr << "cannot write required executable cache\n";
    return 1;
  }

  if (meta.raw_bi2.data && meta.raw_bi2.size)
    WriteBlob(output_arg / "sys/bi2.bin", meta.raw_bi2);
  if (meta.raw_apploader.data && meta.raw_apploader.size)
    WriteBlob(output_arg / "sys/apploader.img", meta.raw_apploader);

  WalkState state;
  state.partition = partition.value;
  state.output = output_arg;
  nod_partition_iterate_fst(partition.value, &Walk, &state);
  if (!state.ok) {
    std::cerr << "FST cache failed: " << state.error << '\n';
    return 1;
  }

  NodDiscMeta disc_meta{};
  std::string format = "unknown";
  if (nod_disc_meta(disc.value, &disc_meta) == NOD_RESULT_OK)
    format = FormatName(disc_meta.format);

  std::ofstream(output_arg / "meta/disc-id.txt") << game_id << '\n';
  std::ofstream(output_arg / "meta/disc-format.txt") << format << '\n';
  std::ofstream(output_arg / "meta/source-path.txt") << image << '\n';

  std::cout << "MOH_NOD_DISC_CACHE=1 format=" << format
            << " executables=" << state.executable_count
            << " source=\"" << image << "\"\n";
  return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) {
    std::cerr << "usage: MOHFrontline-DiscCache <disc-image> <cache-root>\n";
    return 2;
  }
  return Run(fs::path(argv[1]), fs::path(argv[2]));
}
#else
int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: MOHFrontline-DiscCache <disc-image> <cache-root>\n";
    return 2;
  }
  return Run(fs::path(argv[1]), fs::path(argv[2]));
}
#endif
