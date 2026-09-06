// Standalone: c++ -std=c++20 -I ModernGekko/vendor/dolphin/Source/Core \
// tools/test_ps3_compass.cpp
// ModernGekko/vendor/dolphin/Source/Core/VideoCommon/PS3TextureDecoder.cpp -o
// /tmp/test-ps3-compass
#include "VideoCommon/PS3TextureDecoder.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char **argv) {
  assert(argc == 2);
  const char *names[] = {"compass.ssh", "compassdial.ssh", "compassedge.ssh"};
  const unsigned dimensions[] = {64, 512, 512}, mips[] = {7, 1, 10};
  for (unsigned i = 0; i < 3; ++i) {
    std::ifstream in(std::filesystem::path(argv[1]) / names[i],
                     std::ios::binary);
    assert(in);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    std::vector<PS3TextureDecoder::Level> levels;
    assert(PS3TextureDecoder::Decode(bytes, &levels));
    assert(levels.size() == mips[i]);
    assert(levels[0].width == dimensions[i] &&
           levels[0].height == dimensions[i]);
    unsigned transparent = 0, visible = 0;
    for (std::size_t p = 3; p < levels[0].rgba.size(); p += 4)
      levels[0].rgba[p] ? ++visible : ++transparent;
    assert(transparent && visible);
    std::cout << names[i] << ": " << dimensions[i] << "x" << dimensions[i]
              << ", " << levels.size() << " mips, " << visible
              << " visible pixels\n";
    std::ofstream out(std::filesystem::temp_directory_path() /
                          (std::string(names[i]) + ".rgba"),
                      std::ios::binary);
    out.write(reinterpret_cast<const char *>(levels[0].rgba.data()),
              levels[0].rgba.size());
    for (auto length : {std::size_t(0), std::size_t(47), bytes.size() - 1}) {
      assert(
          !PS3TextureDecoder::Decode(std::span(bytes).first(length), &levels));
      assert(levels.empty());
    }
    for (unsigned offset : {8u, 16u, 24u, 25u, 26u, 27u, 28u, 32u, 36u}) {
      auto bad = bytes;
      bad[offset] ^= 0x7f;
      assert(!PS3TextureDecoder::Decode(bad, &levels));
      assert(levels.empty());
    }
  }
}
