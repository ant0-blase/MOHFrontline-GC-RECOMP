#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace PS3NamedSky
{
// Identity comes from the LFC/BIG filename, never from pixels or dimensions.
inline std::string RelativePath(std::string_view name)
{
  const auto slash = name.find_last_of("/\\:");
  std::string file(name.substr(slash == std::string_view::npos ? 0 : slash + 1));
  std::transform(file.begin(), file.end(), file.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (!file.ends_with(".gsh") && !file.ends_with(".ssh"))
    return {};
  const auto split = file.rfind('_');
  if (split == std::string::npos)
    return {};
  const auto face = file.substr(split + 1, file.size() - split - 5);
  constexpr std::array<std::string_view, 6> faces = {"fr", "lf", "bk", "rt", "up", "dn"};
  if (std::find(faces.begin(), faces.end(), face) == faces.end())
    return {};
  const auto stem = file.substr(0, split);
  const auto level = stem == "level1test" ? std::string("1_1") : stem;
  constexpr std::array<std::string_view, 19> levels = {
      "1_1", "1_2", "1_3", "1_4", "2_1", "2_2", "2_3", "3_1", "3_2", "3_3",
      "4_1", "4_2", "4_3", "5_1", "5_2", "5_3", "5_4", "6_1", "6_2"};
  if (std::find(levels.begin(), levels.end(), level) == levels.end())
    return {};
  return "data/" + level.substr(0, 1) + "/" + level + "/" + stem + "_" + face + ".ssh";
}
}  // namespace PS3NamedSky
