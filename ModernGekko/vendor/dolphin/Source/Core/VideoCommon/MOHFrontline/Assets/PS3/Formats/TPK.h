#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace MOHFrontline::PS3::TPK
{
struct Texture
{
  std::string name;
  std::uint32_t offset = 0, size = 0;
  std::array<std::uint8_t, 24> descriptor{};
};
bool Parse(std::span<const std::uint8_t> bytes, std::vector<Texture>* textures);
}
