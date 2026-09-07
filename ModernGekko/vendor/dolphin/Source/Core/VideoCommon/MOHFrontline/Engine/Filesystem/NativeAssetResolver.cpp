#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace MOHFrontline::NativeAssets
{
namespace
{
std::string Normalize(std::string_view input)
{
  std::string out(input);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
  });
  while (out.starts_with('/')) out.erase(0, 1);
  return out;
}
std::string Filename(std::string_view p)
{
  return std::string(p.substr(p.find_last_of("/:") == p.npos ? 0 : p.find_last_of("/:") + 1));
}
std::string Extension(std::string_view p)
{
  p = p.substr(p.find_last_of("/:") == p.npos ? 0 : p.find_last_of("/:") + 1);
  return p.find_last_of('.') == p.npos ? "" : std::string(p.substr(p.find_last_of('.')));
}
std::string LevelOf(std::string_view p)
{
  // Only authored data/<mission>/<mission>_<stage>/ directory components count.
  if (!p.starts_with("data/")) return {};
  p.remove_prefix(5);
  const auto slash = p.find('/');
  if (slash == p.npos) return {};
  const auto mission = p.substr(0, slash);
  if (mission.empty() || !std::all_of(mission.begin(), mission.end(), ::isdigit)) return {};
  p.remove_prefix(slash + 1);
  const auto end = p.find('/');
  if (end == p.npos) return {};
  const auto level = p.substr(0, end);
  const std::string prefix = std::string(mission) + '_';
  if (!level.starts_with(prefix) || level.size() == prefix.size()) return {};
  const auto stage = level.substr(prefix.size());
  if (!std::all_of(stage.begin(), stage.end(), ::isdigit)) return {};
  return std::string(level);
}
std::string Locale()
{
  const char* env = std::getenv("MOH_PS3_LOCALE");
  std::string v = Normalize(env && *env ? env : "usa");
  if (v == "en" || v == "us" || v == "eng" || v == "english") return "usa";
  if (v == "fr" || v == "fra") return "french";
  if (v == "de" || v == "ger") return "german";
  if (v == "it" || v == "ita") return "italian";
  if (v == "es" || v == "spa") return "spanish";
  return v;
}
int LocaleRank(std::string_view path, std::string_view wanted)
{
  constexpr std::array locales = {"usa", "english", "french", "german", "spanish", "italian",
      "japanese", "korean", "russian", "polish", "dutch", "portuguese"};
  for (std::string_view locale : locales)
  {
    const auto token = "_" + std::string(locale);
    const auto p = path.find(token);
    if (p == path.npos || (p && path[p-1] != '/')) continue;
    const auto end = p + token.size();
    if (end != path.size() && path[end] != '/') continue;
    return locale == wanted || (locale == "english" && wanted == "usa") ? 20 : -1;
  }
  return 0;
}
struct Candidate
{
  const PS3RemasterAssets::AssetInfo* asset;
  std::string path, level;
  Domain domain;
};
std::mutex mutex;
std::unordered_map<std::string, std::vector<Candidate>> index;
std::set<std::string> levels;
std::string current_level, locale;
std::uint64_t generation = ~std::uint64_t(0);
Statistics stats;
void Refresh()
{
  const auto next = PS3RemasterAssets::GetIndexGeneration();
  if (generation == next) return;
  generation = next;
  index.clear(); levels.clear(); current_level.clear(); stats = {};
  locale = Locale();
  if (!PS3RemasterAssets::IsReady()) return;
  for (const auto& a : PS3RemasterAssets::GetAssets())
  {
    const auto path = Normalize(a.relative_path);
    const auto level = LevelOf(path);
    if (!level.empty()) levels.insert(level);
    index[Normalize(a.filename)].push_back({&a, path, level, Classify(a.filename)});
    ++stats.indexed;
  }
  stats.levels = levels.size();
  std::fprintf(stderr, "[moh-native][filesystem] indexed=%zu levels=%zu locale=%s exact identities only\n",
               stats.indexed, stats.levels, locale.c_str());
}
std::vector<std::string> Names(const std::string& path, Domain domain)
{
  const auto ext = Extension(path);
  const auto stem = path.substr(0, path.size() - ext.size());
  static const std::map<std::string, std::string> mappings = {{".gsh", ".ssh"}, {".gfn", ".sfn"},
      {".xfn", ".sfn"}, {".abk", ".abkx"}, {".ast", ".astx"}, {".asf", ".asfx"},
      {".mus", ".musx"}, {".mpf", ".mpfx"}, {".bpd", ".xpd"}, {".mpk", ".msh"}};
  if (auto it = mappings.find(ext); it != mappings.end()) return {stem + it->second, path};
  if (ext.empty())
  {
    if (domain == Domain::Texture) return {path + ".ssh"};
    if (domain == Domain::Font) return {path + ".sfn"};
    if (domain == Domain::StaticMesh) return {path + ".msh"};
  }
  return {path};
}
}

Domain Classify(std::string_view path)
{
  const auto e = Extension(Normalize(path));
  if (e == ".ssh" || e == ".gsh" || e == ".gtf" || e == ".dds" || e == ".png" || e == ".tga") return Domain::Texture;
  if (e == ".sfn" || e == ".gfn" || e == ".xfn") return Domain::Font;
  if (e == ".abk" || e == ".abkx" || e == ".ast" || e == ".astx" || e == ".asf" || e == ".asfx" ||
      e == ".mus" || e == ".musx" || e == ".mpf" || e == ".mpfx" || e == ".bnk") return Domain::Audio;
  if (e == ".msh" || e == ".mpk") return Domain::StaticMesh;
  if (e == ".dmf") return Domain::SkinnedMesh;
  if (e == ".skl") return Domain::Skeleton;
  if (e == ".mvd" || e == ".emt" || e == ".aem" || e == ".aemx") return Domain::Animation;
  if (e == ".xpd" || e == ".bpd" || e == ".cpt" || e == ".cdb" || e == ".lit" || e == ".som") return Domain::World;
  if (e == ".lfc" || e == ".scr" || e == ".sin" || e == ".cls" || e == ".dat") return Domain::Script;
  if (e == ".viv" || e == ".big" || e == ".tpk") return Domain::Container;
  return Domain::Unknown;
}
void Initialize() { std::scoped_lock lock(mutex); Refresh(); }
void Shutdown()
{
  std::scoped_lock lock(mutex);
  index.clear(); levels.clear(); current_level.clear(); stats = {}; generation = ~std::uint64_t(0);
}
void SetCurrentLevel(std::string_view value)
{
  std::scoped_lock lock(mutex); Refresh();
  auto level = Normalize(value);
  if (auto path_level = LevelOf(level); !path_level.empty()) level = path_level;
  if (level.empty() || levels.contains(level)) current_level = level;
}
std::string GetCurrentLevel() { std::scoped_lock lock(mutex); Refresh(); return current_level; }
std::vector<std::string> GetLevels() { std::scoped_lock lock(mutex); Refresh(); return {levels.begin(), levels.end()}; }
Statistics GetStatistics() { std::scoped_lock lock(mutex); Refresh(); return stats; }
Match Resolve(std::string_view guest, Domain wanted)
{
  std::scoped_lock lock(mutex); Refresh(); ++stats.requests;
  const auto path = Normalize(guest);
  const auto filename = Filename(path);
  std::string scope = LevelOf(path);
  if (scope.empty())
    for (const auto& level : levels)
      if (filename == "load" + level + ".gsh" || filename == "load" + level + ".ssh" ||
          filename == "tpk" + level + ".tpk") { scope = level; break; }
  if (!scope.empty() && levels.contains(scope)) current_level = scope;
  if (filename == "start.gsh" || filename == "return.gsh") current_level.clear();
  if (scope.empty()) scope = current_level;
  Match best;
  bool ambiguous = false;
  const auto names = Names(path, wanted);
  for (std::size_t rank = 0; rank < names.size(); ++rank)
  {
    auto it = index.find(Filename(names[rank]));
    if (it == index.end()) continue;
    for (const auto& c : it->second)
    {
      if (wanted != Domain::Unknown && c.domain != wanted) continue;
      const int language = LocaleRank(c.path, locale);
      if (language < 0 || (!c.level.empty() && c.level != scope)) continue;
      const bool exact_path = c.path == names[rank];
      const int score = (exact_path ? 10000 : 1000) +
          (!c.level.empty() ? 200 : 0) + language - int(rank) * 30 + (!c.asset->embedded ? 1 : 0);
      if (!best || score > best.score)
      {
        best = {c.asset, c.domain, c.path, score}; ambiguous = false;
      }
      else if (score == best.score && c.path != best.normalized_path) ambiguous = true;
    }
  }
  if (ambiguous) { ++stats.ambiguous; return {}; }
  if (best) ++stats.resolved; else ++stats.missing;
  return best;
}
std::vector<std::uint8_t> Read(const Match& m) { return m ? PS3RemasterAssets::ReadBinary(*m.asset) : std::vector<std::uint8_t>{}; }
bool CanRawReplace(const Match&) { return false; }
}
