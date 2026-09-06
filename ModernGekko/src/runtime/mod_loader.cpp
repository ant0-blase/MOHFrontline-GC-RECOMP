#include "moderngekko/mod_loader.hpp"
#if defined(MODERNGEKKO_MOH_PC_LAYER)
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "VideoCommon/MohPcLayer.h"
#include "VideoCommon/PS3Compass.h"
#include "VideoCommon/TextureDecoder.h"
#endif

#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
#include "Common/DynamicLibrary.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace moderngekko {
namespace {
constexpr std::uint32_t MAX_ITEMS = 1u << 20;
#if defined(MODERNGEKKO_MOH_PC_LAYER)
std::unordered_map<std::uint32_t, std::string>
    loaded_font_files;

std::unordered_map<std::uint32_t, std::string>
    cfont_files;

std::string NormalizePS3FontAssetName(
    std::string name)
{
  const auto slash =
      name.find_last_of(
          "/\\:");

  if (slash !=
      std::string::npos)
  {
    name.erase(
        0,
        slash + 1);
  }

  std::transform(
      name.begin(),
      name.end(),
      name.begin(),
      [](unsigned char c)
      {
        return static_cast<char>(
            std::tolower(c));
      });

  if (name.ends_with(
          ".gfn"))
  {
    name.replace(
        name.size() - 4,
        4,
        ".sfn");
  }

  return name;
}

std::string CanonicalKnownPS3FontForGCAsset(const std::string& gc_name)
{
  const std::string font = NormalizePS3FontAssetName(gc_name);

  if (font == "popupdisplay.sfn" ||
      font == "objfont.sfn" ||
      font == "subtitlefont.sfn" ||
      font == "pausescreenfont.sfn" ||
      font == "ddayintro.sfn" ||
      font == "comicfont.sfn")
  {
    return font;
  }

  if (font == "upcomicfont.sfn")
    return "comicfont.sfn";

  if (font.starts_with("mohgamefont_") && font.ends_with(".sfn"))
    return "mohgamefont_72.sfn";

  return {};
}


constexpr std::uint32_t MOH_HOSTCALL_VI_GAMEPLAY_ON = 0xFFFFF100u;
constexpr std::uint32_t MOH_HOSTCALL_VI_GAMEPLAY_OFF = 0xFFFFF101u;
constexpr std::uint32_t MOH_HOSTCALL_GAMEPLAY_ENTER = 0xFFFFF110u;
constexpr std::uint32_t MOH_HOSTCALL_GAMEPLAY_EXIT = 0xFFFFF111u;
constexpr std::uint32_t MOH_HOSTCALL_MOUSE_LOOK = 0xFFFFF120u;
constexpr std::uint32_t MOH_HOSTCALL_ADS_VIEWMODEL = 0xFFFFF130u;
constexpr std::uint32_t MOH_HOSTCALL_ADS_STATE = 0xFFFFF131u;
constexpr std::uint32_t MOH_HOSTCALL_ADS_CROSSHAIR = 0xFFFFF132u;
constexpr std::uint32_t MOH_HOSTCALL_FRAME_PRESENT = 0xFFFFF133u;
constexpr std::uint32_t MOH_HOSTCALL_VP6_MOVIE_ON = 0xFFFFF140u;
constexpr std::uint32_t MOH_HOSTCALL_VP6_MOVIE_OFF = 0xFFFFF141u;

constexpr std::uint32_t MOH_HOSTCALL_SCENE_POSTPROCESS = 0xFFFFF150u;

constexpr std::uint32_t MOH_HOSTCALL_PS3_FONT_DRAW = 0xFFFFF160u;
constexpr std::uint32_t MOH_HOSTCALL_PS3_FONT_CENTERED = 0xFFFFF161u;

std::uint32_t ReadGuestU32(CPUState* state, std::uint32_t address)
{
  if (!state || !state->external_read)
    return 0u;
  return static_cast<std::uint32_t>(state->external_read(state, address, 4));
}

bool IsMem1Address(std::uint32_t address)
{
  return address >= 0x80000000u && address < 0x81800000u;
}

float ReadGuestF32(CPUState* state, std::uint32_t address)
{
  if (!state || !state->external_read)
    return 0.0f;
  const std::uint32_t bits = static_cast<std::uint32_t>(state->external_read(state, address, 4));
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void WriteGuestF32(CPUState* state, std::uint32_t address, float value)
{
  if (!state || !state->external_write)
    return;
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  state->external_write(state, address, bits, 4);
}

std::uint32_t F32Bits(float value)
{
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
#endif

bool ReadGuestCString(
    CPUState* state,
    std::uint32_t address,
    std::string* out)
{
  if (!state ||
      !state->external_read ||
      !out ||
      !(address >= 0x80000000u &&
        address < 0x81800000u))
  {
    return false;
  }

  out->clear();

  constexpr std::size_t MAX_TEXT =
      160;

  bool has_alnum = false;

  for (std::size_t i = 0;
       i < MAX_TEXT;
       ++i)
  {
    const auto ch =
        static_cast<unsigned char>(
            state->external_read(
                state,
                address +
                    static_cast<std::uint32_t>(i),
                1));

    if (ch == 0)
    {
      return
          !out->empty() &&
          has_alnum;
    }

    if (ch == '\n' ||
        ch == '\r' ||
        ch == '\t')
    {
      out->push_back(
          static_cast<char>(ch));
      continue;
    }

    if (ch < 0x20 ||
        ch > 0x7e)
    {
      return false;
    }

    if (std::isalnum(ch))
      has_alnum = true;

    out->push_back(
        static_cast<char>(ch));
  }

  return false;
}

bool FindGuestFontText(
    CPUState* state,
    std::string* out,
    int* out_reg)
{
  if (!state ||
      !out)
  {
    return false;
  }

  // PPC EABI C++ CFont calls normally keep "this" in r3 while text pointers
  // land around r4-r8 depending on preceding float arguments. Also inspect
  // nonvolatile registers because this hook sits inside CFont, not necessarily
  // at function entry.
  static constexpr int priority[] = {
      6, 5, 4, 7, 8, 9, 10,
      31, 30, 29, 28, 27, 26,
      3, 11, 12, 13, 14, 15,
      16, 17, 18, 19, 20, 21,
      22, 23, 24, 25
  };

  int best_score =
      std::numeric_limits<int>::min();

  std::string best;
  int best_reg = -1;

  for (const int reg :
       priority)
  {
    const std::uint32_t address =
        state->gpr[reg];

    std::string text;

    if (!ReadGuestCString(
            state,
            address,
            &text))
    {
      continue;
    }

    // Do not mistake asset/debug paths for CFont text.
    if (text.find(".sfn") !=
            std::string::npos ||
        text.find(".ssh") !=
            std::string::npos ||
        text.find(".dol") !=
            std::string::npos ||
        text.find(".elf") !=
            std::string::npos)
    {
      continue;
    }

    int score =
        static_cast<int>(
            std::min<std::size_t>(
                text.size(),
                80));

    if (reg >= 4 &&
        reg <= 10)
    {
      score += 100;
    }

    if (text.size() <= 48)
      score += 20;

    if (text.find('/') !=
            std::string::npos ||
        text.find('\\') !=
            std::string::npos)
    {
      score -= 50;
    }

    if (score > best_score)
    {
      best_score = score;
      best = std::move(text);
      best_reg = reg;
    }
  }

  if (best_reg < 0)
    return false;

  *out =
      std::move(best);

  if (out_reg)
    *out_reg = best_reg;

  return true;
}

struct Version {
  std::array<std::uint32_t, 3> parts{};
};

bool ParseVersion(const char *text, Version *version) {
  if (!text || !*text)
    return false;
  const char *current = text;
  for (std::size_t i = 0; i < version->parts.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(*current)))
      return false;
    std::uint64_t value = 0;
    do {
      value = value * 10u + static_cast<unsigned int>(*current - '0');
      if (value > std::numeric_limits<std::uint32_t>::max())
        return false;
      ++current;
    } while (std::isdigit(static_cast<unsigned char>(*current)));
    version->parts[i] = static_cast<std::uint32_t>(value);
    if (i + 1u != version->parts.size()) {
      if (*current != '.')
        return false;
      ++current;
    }
  }
  return *current == '\0' || *current == '-' || *current == '+';
}

bool VersionAtLeast(const char *actual, const char *minimum) {
  if (!minimum || !*minimum)
    return true;
  Version actual_version;
  Version minimum_version;
  if (!ParseVersion(actual, &actual_version) ||
      !ParseVersion(minimum, &minimum_version))
    return false;
  return actual_version.parts >= minimum_version.parts;
}

bool ValidName(const char *text) {
  if (!text || !*text)
    return false;
  for (const unsigned char ch : std::string(text)) {
    if (!std::isalnum(ch) && ch != '_' && ch != '-' && ch != '.')
      return false;
  }
  return true;
}

bool ValidText(const char *text) { return text && *text; }

template <typename T> bool ValidArray(const T *values, std::uint32_t count) {
  return count <= MAX_ITEMS && (count == 0u || values != nullptr);
}

std::string EventKey(const std::string &provider, const std::string &event) {
  std::string key = provider;
  key.push_back('\0');
  key += event;
  return key;
}

std::string LibrarySuffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

bool EndsWith(const std::string &text, const std::string &suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsPackagedLibrary(const std::filesystem::path &path) {
  return EndsWith(path.filename().string(), ".mgm" + LibrarySuffix());
}

// Keep the very large CPUState snapshot out of ModManager::Dispatch's stack
// frame. Dispatch is entered extremely often by native recomp host-call
// probes, while state-preserving entry/return hooks are comparatively rare.
// If this helper is inlined, clang reserves the snapshot in Dispatch anyway.
#if defined(_MSC_VER)
#define MODERNGEKKO_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define MODERNGEKKO_NOINLINE __attribute__((noinline))
#else
#define MODERNGEKKO_NOINLINE
#endif

MODERNGEKKO_NOINLINE void InvokePreservingCpuState(
    CPUState *state, ModernGekkoModFunction function) {
  const CPUState saved = *state;
  function(state);
  *state = saved;
}

#undef MODERNGEKKO_NOINLINE
}

struct ModManager::Impl {
  struct Mod {
    std::string source;
#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
    std::unique_ptr<Common::DynamicLibrary> library;
#endif
    const ModernGekkoModDesc *descriptor = nullptr;
    bool loaded = false;
  };

  struct Hooks {
    std::vector<ModernGekkoModFunction> entry;
    std::vector<ModernGekkoModFunction> returning;
  };

  struct PendingReturn {
    std::uint32_t address = 0;
    std::uint32_t stack_pointer = 0;
    std::vector<ModernGekkoModFunction> functions;
  };

  struct DispatchCacheEntry {
    std::uint32_t address = 0;
    const Hooks *hooks = nullptr;
    ModernGekkoModFunction patch = nullptr;
    bool valid = false;
  };

  static constexpr std::size_t DISPATCH_CACHE_SIZE = 256;
  static_assert((DISPATCH_CACHE_SIZE & (DISPATCH_CACHE_SIZE - 1)) == 0);

  // Fast negative filter for the StaticRecomp host-call probe.
  //
  // dolrecomp_call() asks ctx->host_call() before entering every generated
  // native block.  The overwhelming majority of GMFE69 PCs have no mod hook
  // or patch at all, but sending those misses through ModManager::HostCall()
  // and Dispatch() still pays their comparatively large C++ prologues and
  // lookup machinery.  Keep one bit per 4 KiB MEM1 page containing at least
  // one static hook/patch so the chassis can reject empty pages cheaply.
  //
  // Addresses outside canonical MEM1 deliberately stay conservative and use
  // the exact maps.  Pending return hooks are handled separately because the
  // return PC itself need not live on a page containing a static hook.
  static constexpr std::uint32_t MEM1_DISPATCH_BASE = 0x80000000u;
  static constexpr std::uint32_t MEM1_DISPATCH_END = 0x81800000u;
  static constexpr std::uint32_t HOST_CALL_PAGE_SHIFT = 12u;
  static constexpr std::size_t HOST_CALL_PAGE_COUNT =
      (MEM1_DISPATCH_END - MEM1_DISPATCH_BASE) >> HOST_CALL_PAGE_SHIFT;
  static constexpr std::size_t HOST_CALL_PAGE_WORDS =
      (HOST_CALL_PAGE_COUNT + 63u) / 64u;

  void MarkHostCallPage(std::uint32_t address) {
    if (address < MEM1_DISPATCH_BASE || address >= MEM1_DISPATCH_END)
      return;
    const std::size_t page =
        (address - MEM1_DISPATCH_BASE) >> HOST_CALL_PAGE_SHIFT;
    host_call_pages[page >> 6u] |= std::uint64_t{1} << (page & 63u);
  }

  bool HostCallPageMayContain(std::uint32_t address) const {
    if (address < MEM1_DISPATCH_BASE || address >= MEM1_DISPATCH_END)
      return true;
    const std::size_t page =
        (address - MEM1_DISPATCH_BASE) >> HOST_CALL_PAGE_SHIFT;
    return (host_call_pages[page >> 6u] &
            (std::uint64_t{1} << (page & 63u))) != 0;
  }

  std::vector<std::unique_ptr<Mod>> mods;
  std::vector<LoadedModInfo> loaded;
  std::unordered_map<std::string, Mod *> mods_by_id;
  std::unordered_map<std::string, ModernGekkoModFunction> exports;
  std::unordered_map<std::uint32_t, ModernGekkoModFunction> patches;
  std::unordered_map<std::uint32_t, Hooks> hooks;
  std::unordered_map<std::string, std::vector<ModernGekkoModFunction>>
      callbacks;
  std::vector<ModernGekkoModFunction *> import_slots;
  std::vector<PendingReturn> pending_returns;
  std::array<DispatchCacheEntry, DISPATCH_CACHE_SIZE> dispatch_cache{};
  std::array<std::uint64_t, HOST_CALL_PAGE_WORDS> host_call_pages{};
  bool runtime_started = false;
  ModernGekkoModHostApi host_api{};

  bool DispatchSlow(ModManager *owner, CPUState *state,
                    std::uint32_t address);

  static std::string ExportKey(const std::string &provider,
                               const std::string &name) {
    return EventKey(provider, name);
  }

  static int TriggerEventThunk(void *user_data, const char *provider_id,
                               const char *event_name, CPUState *state) {
    if (!user_data || !provider_id || !event_name || !state)
      return 0;
    return static_cast<ModManager *>(user_data)->TriggerEvent(provider_id,
                                                              event_name, state)
               ? 1
               : 0;
  }

  static ModernGekkoModFunction FindExportThunk(void *user_data,
                                                const char *provider_id,
                                                const char *export_name) {
    if (!user_data || !provider_id || !export_name)
      return nullptr;
    return static_cast<ModManager *>(user_data)->FindExport(provider_id,
                                                            export_name);
  }
};

ModSource ModSource::DynamicPath(std::filesystem::path path) {
  ModSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModSource ModSource::AttachedDescriptor(const ModernGekkoModDesc *descriptor,
                                        std::string label) {
  ModSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  source.label = std::move(label);
  return source;
}

ModManager::ModManager() : m_impl(std::make_unique<Impl>()) {
  m_impl->host_api.abi_version = MODERNGEKKO_MOD_HOST_ABI_VERSION;
  m_impl->host_api.user_data = this;
  m_impl->host_api.trigger_event = Impl::TriggerEventThunk;
  m_impl->host_api.find_export = Impl::FindExportThunk;
}

ModManager::~ModManager() { Unload(); }

ModLoadReport ModManager::Load(const std::vector<ModSource> &sources,
                               const std::string &game_id) {
  Unload();
  ModLoadReport report;
  const auto issue = [&](const std::string &source, std::string message) {
    report.issues.push_back({source, std::move(message)});
  };

  for (std::size_t index = 0; index < sources.size(); ++index) {
    const ModSource &source = sources[index];
    auto mod = std::make_unique<Impl::Mod>();
    mod->source = source.kind == ModSource::Kind::DynamicPath
                      ? source.path.string()
                      : source.label;
    if (mod->source.empty())
      mod->source = "attached:" + std::to_string(index);
    if (source.kind == ModSource::Kind::AttachedDescriptor) {
      mod->descriptor = source.descriptor;
    } else {
#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
      mod->library = std::make_unique<Common::DynamicLibrary>();
      if (!mod->library->Open(source.path.string().c_str())) {
        issue(mod->source, "could not open the mod library");
        continue;
      }
      ModernGekkoGetModFn get_mod = nullptr;
      if (!mod->library->GetSymbol(MODERNGEKKO_GET_MOD_SYMBOL, &get_mod)) {
        issue(mod->source, "missing moderngekko_get_mod");
        continue;
      }
      mod->descriptor = get_mod();
#else
      issue(mod->source, "dynamic mod loading is unavailable");
      continue;
#endif
    }

    const ModernGekkoModDesc *desc = mod->descriptor;
    if (!desc)
      issue(mod->source, "null mod descriptor");
    else if (desc->abi_version != MODERNGEKKO_MOD_ABI_VERSION)
      issue(mod->source, "unsupported mod ABI");
    else if (desc->cpu_abi_version != MODERNGEKKO_CPU_ABI_VERSION ||
             desc->cpu_state_size != sizeof(CPUState))
      issue(mod->source, "CPU ABI mismatch");
    else if (!std::memchr(desc->game_id, '\0', sizeof(desc->game_id)) ||
             game_id != desc->game_id)
      issue(mod->source, "target game ID mismatch");
    else if (!ValidName(desc->id))
      issue(mod->source, "invalid mod ID");
    else if (!ValidText(desc->display_name))
      issue(mod->source, "missing display name");
    else {
      Version version;
      if (!ParseVersion(desc->version, &version))
        issue(mod->source, "invalid mod version");
      else if (!ValidArray(desc->dependencies, desc->num_dependencies) ||
               !ValidArray(desc->patches, desc->num_patches) ||
               !ValidArray(desc->hooks, desc->num_hooks) ||
               !ValidArray(desc->exports, desc->num_exports) ||
               !ValidArray(desc->imports, desc->num_imports) ||
               !ValidArray(desc->events, desc->num_events) ||
               !ValidArray(desc->callbacks, desc->num_callbacks))
        issue(mod->source, "invalid descriptor arrays");
      else if (m_impl->mods_by_id.contains(desc->id))
        issue(mod->source, "duplicate mod ID");
      else {
        m_impl->mods_by_id.emplace(desc->id, mod.get());
        m_impl->mods.push_back(std::move(mod));
      }
    }
  }

  if (!report.issues.empty()) {
    Unload();
    return report;
  }

  std::unordered_map<Impl::Mod *, std::size_t> mod_indices;
  for (std::size_t i = 0; i < m_impl->mods.size(); ++i)
    mod_indices.emplace(m_impl->mods[i].get(), i);
  std::vector<std::vector<std::size_t>> edges(m_impl->mods.size());
  std::vector<std::size_t> indegree(m_impl->mods.size());

  for (std::size_t i = 0; i < m_impl->mods.size(); ++i) {
    const auto &mod = m_impl->mods[i];
    const auto *desc = mod->descriptor;
    std::unordered_set<std::string> seen;
    for (std::uint32_t d = 0; d < desc->num_dependencies; ++d) {
      const auto &dependency = desc->dependencies[d];
      if (!ValidName(dependency.id) || !seen.emplace(dependency.id).second) {
        issue(mod->source, "invalid or duplicate dependency");
        continue;
      }
      const auto found = m_impl->mods_by_id.find(dependency.id);
      if (found == m_impl->mods_by_id.end()) {
        if (!dependency.optional)
          issue(mod->source,
                "missing dependency " + std::string(dependency.id));
        continue;
      }
      if (!VersionAtLeast(found->second->descriptor->version,
                          dependency.minimum_version)) {
        issue(mod->source, "dependency version is too old for " +
                               std::string(dependency.id));
        continue;
      }
      const std::size_t dependency_index = mod_indices.at(found->second);
      edges[dependency_index].push_back(i);
      ++indegree[i];
    }
  }

  if (!report.issues.empty()) {
    Unload();
    return report;
  }

  std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>>
      ready;
  for (std::size_t i = 0; i < indegree.size(); ++i) {
    if (indegree[i] == 0)
      ready.push(i);
  }
  std::vector<std::size_t> order;
  order.reserve(m_impl->mods.size());
  while (!ready.empty()) {
    const std::size_t index = ready.top();
    ready.pop();
    for (const std::size_t dependent : edges[index]) {
      if (--indegree[dependent] == 0)
        ready.push(dependent);
    }
    order.push_back(index);
  }
  if (order.size() != m_impl->mods.size()) {
    issue("mods", "dependency cycle");
    Unload();
    return report;
  }
  std::vector<std::unique_ptr<Impl::Mod>> ordered;
  ordered.reserve(m_impl->mods.size());
  for (const std::size_t index : order)
    ordered.push_back(std::move(m_impl->mods[index]));
  m_impl->mods = std::move(ordered);
  m_impl->mods_by_id.clear();
  for (const auto &mod : m_impl->mods)
    m_impl->mods_by_id.emplace(mod->descriptor->id, mod.get());

  std::unordered_set<std::string> event_names;
  event_names.emplace(EventKey("*", "runtime_start"));
  for (const auto &mod : m_impl->mods) {
    const auto *desc = mod->descriptor;
    std::unordered_set<std::string> local_exports;
    std::unordered_set<std::string> local_events;
    for (std::uint32_t i = 0; i < desc->num_exports; ++i) {
      const auto &entry = desc->exports[i];
      if (!ValidName(entry.name) || !entry.function ||
          !local_exports.emplace(entry.name).second) {
        issue(mod->source, "invalid or duplicate export");
        continue;
      }
      m_impl->exports.emplace(Impl::ExportKey(desc->id, entry.name),
                              entry.function);
    }
    for (std::uint32_t i = 0; i < desc->num_events; ++i) {
      const auto &event = desc->events[i];
      if (!ValidName(event.name) || !local_events.emplace(event.name).second) {
        issue(mod->source, "invalid or duplicate event");
        continue;
      }
      event_names.emplace(EventKey(desc->id, event.name));
    }
  }

  for (const auto &mod : m_impl->mods) {
    const auto *desc = mod->descriptor;
    for (std::uint32_t i = 0; i < desc->num_imports; ++i) {
      const auto &entry = desc->imports[i];
      if (!ValidName(entry.dependency_id) || !ValidName(entry.name) ||
          !entry.slot) {
        issue(mod->source, "invalid import");
        continue;
      }
      const ModernGekkoModDependency *declared = nullptr;
      for (std::uint32_t d = 0; d < desc->num_dependencies; ++d) {
        if (std::strcmp(desc->dependencies[d].id, entry.dependency_id) == 0) {
          declared = &desc->dependencies[d];
          break;
        }
      }
      if (!declared) {
        issue(mod->source, "import provider is not a declared dependency");
        continue;
      }
      const auto dependency = m_impl->mods_by_id.find(entry.dependency_id);
      const auto function = m_impl->exports.find(
          Impl::ExportKey(entry.dependency_id, entry.name));
      if (dependency == m_impl->mods_by_id.end()) {
        *entry.slot = nullptr;
      } else if (function == m_impl->exports.end()) {
        issue(mod->source, "missing export " +
                               std::string(entry.dependency_id) + ":" +
                               entry.name);
      } else {
        *entry.slot = function->second;
      }
      m_impl->import_slots.push_back(entry.slot);
    }
    for (std::uint32_t i = 0; i < desc->num_patches; ++i) {
      const auto &patch = desc->patches[i];
      if ((patch.address & 3u) != 0u || !patch.function ||
          (patch.flags & ~MODERNGEKKO_MOD_PATCH_FORCE) != 0u) {
        issue(mod->source, "invalid patch");
        continue;
      }
      const auto found = m_impl->patches.find(patch.address);
      if (found != m_impl->patches.end() &&
          (patch.flags & MODERNGEKKO_MOD_PATCH_FORCE) == 0u) {
        issue(mod->source,
              "patch conflict at " + std::to_string(patch.address));
        continue;
      }
      m_impl->patches[patch.address] = patch.function;
    }
    for (std::uint32_t i = 0; i < desc->num_hooks; ++i) {
      const auto &hook = desc->hooks[i];
      if ((hook.address & 3u) != 0u || !hook.function ||
          hook.kind > MODERNGEKKO_MOD_HOOK_RETURN) {
        issue(mod->source, "invalid hook");
        continue;
      }
      auto &hooks = m_impl->hooks[hook.address];
      if (hook.kind == MODERNGEKKO_MOD_HOOK_ENTRY)
        hooks.entry.push_back(hook.function);
      else
        hooks.returning.push_back(hook.function);
    }
    for (std::uint32_t i = 0; i < desc->num_callbacks; ++i) {
      const auto &callback = desc->callbacks[i];
      const std::string provider =
          callback.dependency_id &&
                  std::strcmp(callback.dependency_id, ".") == 0
              ? desc->id
          : callback.dependency_id ? callback.dependency_id
                                   : "";
      if ((!ValidName(provider.c_str()) && provider != "*") ||
          !ValidName(callback.event_name) || !callback.function) {
        issue(mod->source, "invalid callback");
        continue;
      }
      if (provider != desc->id && provider != "*") {
        bool declared = false;
        for (std::uint32_t d = 0; d < desc->num_dependencies; ++d)
          declared |= provider == desc->dependencies[d].id;
        if (!declared) {
          issue(mod->source, "callback provider is not a declared dependency");
          continue;
        }
      }
      const std::string key = EventKey(provider, callback.event_name);
      if (!event_names.contains(key)) {
        issue(mod->source,
              "missing event " + provider + ":" + callback.event_name);
        continue;
      }
      m_impl->callbacks[key].push_back(callback.function);
    }
  }

  if (!report.issues.empty()) {
    Unload();
    return report;
  }

  // Hooks and patches are immutable after Load().  Build the compact MEM1
  // negative filter once, outside the native execution hot path.
  m_impl->host_call_pages.fill(0);
  for (const auto &entry : m_impl->patches)
    m_impl->MarkHostCallPage(entry.first);
  for (const auto &entry : m_impl->hooks)
    m_impl->MarkHostCallPage(entry.first);

  for (const auto &mod : m_impl->mods) {
    const auto *desc = mod->descriptor;
    m_impl->loaded.push_back(
        {desc->id, desc->version, desc->display_name, mod->source});
    if (desc->on_load)
      desc->on_load(&m_impl->host_api);
    mod->loaded = true;
  }
  report.loaded = m_impl->loaded;
  return report;
}

ModLoadReport ModManager::LoadDirectories(
    const std::vector<std::filesystem::path> &directories,
    const std::string &game_id) {
  ModLoadReport discovery;
  std::vector<ModSource> sources =
      DiscoverModSources(directories, &discovery.issues);
  if (!discovery.issues.empty())
    return discovery;
  return Load(sources, game_id);
}

std::vector<ModSource>
DiscoverModSources(const std::vector<std::filesystem::path> &directories,
                   std::vector<ModLoadIssue> *issues) {
  std::vector<std::filesystem::path> paths;
  const auto issue = [&](const std::filesystem::path &path,
                         std::string message) {
    if (issues)
      issues->push_back({path.string(), std::move(message)});
  };
  for (const auto &directory : directories) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
      continue;
    if (std::filesystem::is_regular_file(directory, ec)) {
      paths.push_back(directory);
      continue;
    }
    if (!std::filesystem::is_directory(directory, ec)) {
      issue(directory, "mod path is not a directory or library");
      continue;
    }
    if (directory.extension() == ".mgm") {
      const auto library = directory / ("mod" + LibrarySuffix());
      if (std::filesystem::is_regular_file(library, ec))
        paths.push_back(library);
      else
        issue(directory, "package has no platform mod library");
      continue;
    }
    for (std::filesystem::directory_iterator it(directory, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (it->is_directory(ec) && it->path().extension() == ".mgm") {
        const auto library = it->path() / ("mod" + LibrarySuffix());
        if (std::filesystem::is_regular_file(library, ec))
          paths.push_back(library);
        else
          issue(it->path(), "package has no platform mod library");
      } else if (it->is_regular_file(ec) && IsPackagedLibrary(it->path())) {
        paths.push_back(it->path());
      }
    }
    if (ec)
      issue(directory, "could not enumerate mod directory");
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  std::vector<ModSource> sources;
  sources.reserve(paths.size());
  for (auto &path : paths)
    sources.push_back(ModSource::DynamicPath(std::move(path)));
  return sources;
}

void ModManager::Unload() {
#if defined(MODERNGEKKO_MOH_PC_LAYER)
  loaded_font_files.clear();
  cfont_files.clear();
#endif
  for (auto it = m_impl->mods.rbegin(); it != m_impl->mods.rend(); ++it) {
    if ((*it) && (*it)->loaded && (*it)->descriptor &&
        (*it)->descriptor->on_unload)
      (*it)->descriptor->on_unload();
  }
  for (ModernGekkoModFunction *slot : m_impl->import_slots) {
    if (slot)
      *slot = nullptr;
  }
  m_impl->pending_returns.clear();
  m_impl->callbacks.clear();
  m_impl->dispatch_cache = {};
  m_impl->host_call_pages.fill(0);
  m_impl->hooks.clear();
  m_impl->patches.clear();
  m_impl->exports.clear();
  m_impl->mods_by_id.clear();
  m_impl->loaded.clear();
  m_impl->import_slots.clear();
  m_impl->mods.clear();
  m_impl->runtime_started = false;
}

// Extremely hot native-hostcall path.  Once runtime_start has fired and no
// dynamic return hook is pending, recurring patch-only PCs can be served
// entirely from the 256-entry direct cache.  Keep the large generic resolver
// out-of-line so this common path does not inherit its stack frame, unordered
// map machinery or hook-vector bookkeeping.
bool ModManager::Dispatch(CPUState *state, std::uint32_t address) {
  if (!state)
    return false;

  Impl &impl = *m_impl;
  if (impl.runtime_started && impl.pending_returns.empty()) {
    auto &cached = impl.dispatch_cache[
        (address >> 2u) & (Impl::DISPATCH_CACHE_SIZE - 1u)];

    if (cached.valid && cached.address == address && cached.hooks == nullptr) {
      if (!cached.patch)
        return false;
      cached.patch(state);
      return true;
    }
  }

  return impl.DispatchSlow(this, state, address);
}

#if defined(_MSC_VER)
#define MODERNGEKKO_DISPATCH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define MODERNGEKKO_DISPATCH_NOINLINE __attribute__((noinline))
#else
#define MODERNGEKKO_DISPATCH_NOINLINE
#endif

MODERNGEKKO_DISPATCH_NOINLINE bool
ModManager::Impl::DispatchSlow(ModManager *owner, CPUState *state,
                               std::uint32_t address) {
  if (!runtime_started) {
    runtime_started = true;
    owner->TriggerEvent("*", "runtime_start", state);
  }

  if (!pending_returns.empty() &&
      pending_returns.back().address == address &&
      pending_returns.back().stack_pointer == state->gpr[1]) {
    auto pending = std::move(pending_returns.back());
    pending_returns.pop_back();
    for (auto it = pending.functions.rbegin(); it != pending.functions.rend();
         ++it)
      InvokePreservingCpuState(state, *it);
  }

  // Dispatch is hit very frequently by native recomp hooks.  The hook/patch
  // tables are immutable after Load(), so cache the resolved entries by guest
  // address and avoid two unordered_map hashes on recurring hot addresses.
  auto &cached = dispatch_cache[
      (address >> 2u) & (Impl::DISPATCH_CACHE_SIZE - 1u)];
  const Impl::Hooks *address_hooks = nullptr;
  ModernGekkoModFunction address_patch = nullptr;

  if (cached.valid && cached.address == address) {
    address_hooks = cached.hooks;
    address_patch = cached.patch;
  } else {
    const auto found_hooks = hooks.find(address);
    const auto found_patch = patches.find(address);
    address_hooks = found_hooks != hooks.end() ? &found_hooks->second : nullptr;
    address_patch = found_patch != patches.end() ? found_patch->second : nullptr;
    cached.address = address;
    cached.hooks = address_hooks;
    cached.patch = address_patch;
    cached.valid = true;
  }

  if (address_hooks) {
    for (ModernGekkoModFunction function : address_hooks->entry)
      InvokePreservingCpuState(state, function);

    if (!address_hooks->returning.empty()) {
      if (pending_returns.size() >= 4096u)
        pending_returns.erase(pending_returns.begin());
      pending_returns.push_back(
          {state->lr, state->gpr[1], address_hooks->returning});
    }
  }
  if (!address_patch)
    return false;
  address_patch(state);
  return true;
}

#undef MODERNGEKKO_DISPATCH_NOINLINE

bool ModManager::TriggerEvent(const std::string &provider_id,
                              const std::string &event_name, CPUState *state) {
  if (!state)
    return false;
  const auto found = m_impl->callbacks.find(EventKey(provider_id, event_name));
  if (found == m_impl->callbacks.end())
    return false;
  const CPUState saved = *state;
  for (ModernGekkoModFunction function : found->second) {
    function(state);
    *state = saved;
  }
  return true;
}

ModernGekkoModFunction
ModManager::FindExport(const std::string &provider_id,
                       const std::string &export_name) const {
  const auto found =
      m_impl->exports.find(Impl::ExportKey(provider_id, export_name));
  return found == m_impl->exports.end() ? nullptr : found->second;
}

const std::vector<LoadedModInfo> &ModManager::GetLoadedMods() const {
  return m_impl->loaded;
}

bool ModManager::HandlesAddress(std::uint32_t address) const {
  // Dynamic return hooks must win over the static page filter: their return PC
  // can be anywhere, including a page with no registered entry hook/patch.
  if (!m_impl->pending_returns.empty() &&
      std::ranges::any_of(m_impl->pending_returns,
                          [address](const Impl::PendingReturn &pending) {
                            return pending.address == address;
                          }))
    return true;

  // Dispatch has already resolved recurring hook PCs in the common case.
  // Consult that tiny direct-mapped cache before touching either unordered_map.
  // pending_returns is checked above because it is dynamic and must override a
  // cached static miss.
  const auto &cached = m_impl->dispatch_cache[
      (address >> 2u) & (Impl::DISPATCH_CACHE_SIZE - 1u)];
  if (cached.valid && cached.address == address)
    return cached.hooks != nullptr || cached.patch != nullptr;

  // Common GMFE69 path: one cached bitmap load instead of two unordered_map
  // probes.  Exact lookup is retained for the relatively few candidate pages.
  if (!m_impl->HostCallPageMayContain(address))
    return false;
  return m_impl->patches.contains(address) || m_impl->hooks.contains(address);
}

bool ModManager::HandlesRange(std::uint32_t start, std::uint32_t end) const {
  if (start >= end)
    return false;
  const auto inside = [start, end](const auto &entry) {
    return entry.first >= start && entry.first < end;
  };
  if (std::ranges::any_of(m_impl->patches, inside) ||
      std::ranges::any_of(m_impl->hooks, inside))
    return true;
  return std::ranges::any_of(
      m_impl->pending_returns, [start, end](const Impl::PendingReturn &pending) {
        return pending.address >= start && pending.address < end;
      });
}

bool ModManager::Empty() const { return m_impl->mods.empty(); }

// The MOH PC-layer token handler carries sizeable locals (mouse/ADS state,
// logging buffers, etc.).  Keeping it in ModManager::HostCall forces clang to
// build a stack frame and stack-canary path even for the overwhelmingly common
// native guest-address case that simply tail-dispatches a mod hook.
#if defined(_MSC_VER)
#define MODERNGEKKO_HOSTCALL_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define MODERNGEKKO_HOSTCALL_NOINLINE __attribute__((noinline))
#else
#define MODERNGEKKO_HOSTCALL_NOINLINE
#endif
MODERNGEKKO_HOSTCALL_NOINLINE static bool
HandleMohPcLayerHostCall(CPUState *state, std::uint32_t address, void *user_data) {
#if defined(MODERNGEKKO_MOH_PC_LAYER)
  if (address == MOH_HOSTCALL_GAMEPLAY_ENTER || address == MOH_HOSTCALL_GAMEPLAY_EXIT) {
    const bool active = address == MOH_HOSTCALL_GAMEPLAY_ENTER;
    MohPcLayer::SetGameplayActive(active);
    return true;
  }
  if (address == MOH_HOSTCALL_MOUSE_LOOK) {
    // Port Carnivorous' Dolphin Mouse Injector camera offsets/behaviour directly
    // into ModernGekko.  The active CPlayerObject* comes from BeginUpdate's
    // live r31 rather than the external injector's version-specific global.
    constexpr std::uint32_t CAM_X = 0x29Cu;
    constexpr std::uint32_t CAM_Y = 0x2A0u;
    constexpr std::uint32_t FOV = 0x42Cu;
    constexpr std::uint32_t SENTRY_X = 0x41Cu;
    constexpr std::uint32_t SENTRY_Y = 0x420u;
    constexpr std::uint32_t SENTRY_Y_LIMIT = 0x418u;
    constexpr std::uint32_t SENTRY_FLAG = 0x6CCu;
    constexpr float TAU = 6.2831853f;
    constexpr float CROSSHAIR_Y = 1.450000048f;

    // This host-call is emitted from inside CPlayerObject::BeginUpdate at
    // 0x800A4CB4, before the function epilogue restores callee-saved registers.
    // r31 is the live CPlayerObject* (mr r31,r3 at 0x800A2838), so using it is
    // substantially more robust than Carnivorous' static g_pPlayers address.
    const std::uint32_t player = state ? state->gpr[31] : 0u;
    if (!IsMem1Address(player)) {
      static bool bad_player_logged = false;
      if (!bad_player_logged) {
        bad_player_logged = true;
        std::fprintf(stderr,
                     "[moh-pc] mouse delta received but player pointer is invalid: %08x\n",
                     player);
      }
      return true;
    }

    const float fov = ReadGuestF32(state, player + FOV);
    float yaw_delta = 0.0f;
    float pitch_delta = 0.0f;
    if (!MohPcLayer::ConsumeMouseLook(fov, &yaw_delta, &pitch_delta))
      return true;

    const std::uint32_t sentry_flag = ReadGuestU32(state, player + SENTRY_FLAG);
    const char* mode = nullptr;
    std::uint32_t x_addr = 0;
    std::uint32_t y_addr = 0;
    float x = 0.0f;
    float y = 0.0f;
    float y_limit = CROSSHAIR_Y;

    if (sentry_flag == 1u) {
      mode = "player";
      x_addr = player + CAM_X;
      y_addr = player + CAM_Y;
      x = ReadGuestF32(state, x_addr);
      y = ReadGuestF32(state, y_addr);
    } else if (sentry_flag == 21u) {
      mode = "sentry";
      x_addr = player + SENTRY_X;
      y_addr = player + SENTRY_Y;
      x = ReadGuestF32(state, x_addr);
      y = ReadGuestF32(state, y_addr);
      y_limit = ReadGuestF32(state, player + SENTRY_Y_LIMIT);
      if (!std::isfinite(y_limit) || y_limit <= 0.0f || y_limit > TAU)
        y_limit = CROSSHAIR_Y;
    } else {
      static std::uint32_t last_unknown_flag = 0xffffffffu;
      if (sentry_flag != last_unknown_flag) {
        last_unknown_flag = sentry_flag;
        std::fprintf(stderr,
                     "[moh-pc] mouse look waiting for supported player state: sentryflag=%u\n",
                     sentry_flag);
      }
      return true;
    }

    if (!std::isfinite(x) || !std::isfinite(y) || x < -TAU || x > TAU ||
        y < -y_limit || y > y_limit)
      return true;

    x += yaw_delta;
    y += pitch_delta;
    while (x <= -TAU)
      x += TAU;
    while (x >= TAU)
      x -= TAU;
    y = std::clamp(y, -y_limit, y_limit);

    WriteGuestF32(state, x_addr, x);
    WriteGuestF32(state, y_addr, y);

    static bool logged = false;
    if (!logged) {
      logged = true;
      std::fprintf(stderr,
                   "[moh-pc] native mouse injector active: mode=%s player=%08x "
                   "cam=(+%03x,+%03x) fov=%.2f first_delta=(%+.5f,%+.5f) rad\n",
                   mode, player, x_addr - player, y_addr - player, fov,
                   yaw_delta, pitch_delta);
    }
    return true;
  }
  if (address == MOH_HOSTCALL_SCENE_POSTPROCESS) {
    MohPcLayer::PreprocessSceneBefore2D();
    return true;
  }

  if (address == 0xFFFFF147u)
  {
    // 44fd renderer keeps a short host-side temporal hold.
    return true;
  }
  // Metadata calls are injected at ELF-verified load/constructor boundaries.
  if (address == 0xFFFFF143u)
  {
    std::string name;

    if (IsMem1Address(state->gpr[3]))
    {
      loaded_font_files.erase(state->gpr[3]);

      if (ReadGuestCString(state, state->gpr[0], &name))
      {
        const std::string font =
            CanonicalKnownPS3FontForGCAsset(name);

        if (!font.empty())
        {
          loaded_font_files[state->gpr[3]] = font;

          static unsigned font_load_logs = 0;
          if (font_load_logs < 32)
          {
            ++font_load_logs;
            std::fprintf(
                stderr,
                "[moh-ps3-font] verified font: GC=%s -> PS3=%s\n",
                name.c_str(),
                font.c_str());
          }
        }
      }
    }

    return true;
  }
  if (address == 0xFFFFF144u ||
      address == 0xFFFFF145u)
  {
    cfont_files.erase(
        state->gpr[3]);

    if (address ==
        0xFFFFF144u)
    {
      const auto it =
          loaded_font_files.find(
              state->gpr[0]);

      if (it !=
          loaded_font_files.end())
      {
        cfont_files[
            state->gpr[3]] =
            it->second;
      }
    }

    return true;
  }
  if (address == 0xFFFFF146u)
  {
    std::string name;
    if (!ReadGuestCString(state, state->gpr[0], &name)) return true;
    const int index = PS3Compass::NameIndex(name);
    const u32 file = state->gpr[3];
    if (index < 0 || !IsMem1Address(file) || file > 0x817FFFE8u ||
        ReadGuestU32(state, file) != 0x53485047u) return true;
    const u32 offset = ReadGuestU32(state, file + 20);
    if (offset < 24 || offset > 0x81800000u - file - 16) return true;
    const u32 shape = file + offset;
    const u32 wh = ReadGuestU32(state, shape + 4);
    const u32 w = wh >> 16, h = wh & 65535;
    const u32 type = ReadGuestU32(state, shape) >> 24;
    // SHPG CTexture::Set switch verified against the GMFE69 ELF.
    const int format = type == 20 ? 4 : type == 22 ? 6 : type == 24 ? 8 : type == 25 ? 9 : type == 30 ? 14 : -1;
    if (format < 0 || !w || !h || w > 1024 || h > 1024) return true;
    const auto size = TexDecoder_GetTextureSizeInBytes(w, h, static_cast<TextureFormat>(format));
    if (size <= 0 || static_cast<u32>(size) > 0x81800000u - shape - 16) return true;
    std::vector<u8> original(size);
    for (int i = 0; i < size; ++i)
      original[i] = static_cast<u8>(state->external_read(state, shape + 16 + i, 1));
    std::vector<u8> palette;
    u32 palette_format = 0;
    if (format == 8 || format == 9)
    {
      const u32 poff = ReadGuestU32(state, shape) & 0xFFFFFF;
      const u32 psize = format == 8 ? 32 : 512;
      if (poff < 16 || poff > 0x81800000u - shape - 16 - psize) return true;
      const u32 p = shape + poff;
      const u32 pt = ReadGuestU32(state, p) >> 24;
      if (pt < 48 || pt > 50) return true;
      palette_format = pt - 48;
      palette.resize(psize);
      for (u32 i = 0; i < psize; ++i)
        palette[i] = static_cast<u8>(state->external_read(state, p + 16 + i, 1));
    }
    PS3Compass::Register(index, shape + 16, w, h, format, std::move(original),
                         palette_format, std::move(palette));
    return true;
  }

  if (address == MOH_HOSTCALL_PS3_FONT_DRAW ||
      address == MOH_HOSTCALL_PS3_FONT_CENTERED)
  {
    state->gpr[0] = 0u;

    std::string exact_font;

    const auto font_it =
        cfont_files.find(
            state->gpr[3]);

    if (font_it !=
        cfont_files.end())
    {
      exact_font =
          font_it->second;
    }

    if (exact_font.empty())
    {
      std::uint32_t pc =
          state->lr;

      std::uint32_t sp =
          state->gpr[1];

      for (unsigned depth = 0;
           depth < 12;
           ++depth)
      {
        if ((pc >= 0x800BCE58u &&
             pc < 0x800BD590u) ||
            (pc >= 0x800BD590u &&
             pc < 0x800BD864u))
        {
          exact_font =
              "mohgamefont_72.sfn";
          break;
        }

        if (!IsMem1Address(sp) ||
            (sp & 3u) ||
            sp > 0x817FFFF7u)
        {
          break;
        }

        const std::uint32_t parent =
            ReadGuestU32(
                state,
                sp);

        if (parent <= sp ||
            !IsMem1Address(parent) ||
            parent > 0x817FFFF7u)
        {
          break;
        }

        sp =
            parent;

        pc =
            ReadGuestU32(
                state,
                sp + 4u);
      }
    }

    if (exact_font.empty())
    {
      return true;
    }

    if (!MohPcLayer::
            IsPS3FontBridgeReady(
                exact_font.c_str()))
    {
      return true;
    }

    const bool centered =
        address ==
        MOH_HOSTCALL_PS3_FONT_CENTERED;

    const double f1 =
        state->fpr[1];

    const double f2 =
        state->fpr[2];

    if (!std::isfinite(f1) ||
        (!centered &&
         !std::isfinite(f2)))
    {
      return true;
    }

    std::string text_value;
    int source_reg = -1;

    if (!FindGuestFontText(
            state,
            &text_value,
            &source_reg))
    {
      return true;
    }

    const float draw_x =
        centered ?
            320.0f :
            static_cast<float>(
                f1);

    const float draw_y =
        centered ?
            static_cast<float>(
                f1) :
            static_cast<float>(
                f2);

    constexpr std::uint32_t rgba =
        0xFFFFFFFFu;

    static unsigned log_count = 0;

    if (log_count < 48)
    {
      ++log_count;

      std::fprintf(
          stderr,
          "[moh-ps3-font] verified capture "
          "font=%s r%d=%08X x=%.2f y=%.2f centered=%d text=\"%s\"\n",
          exact_font.c_str(),
          source_reg,
          state->gpr[source_reg],
          draw_x,
          draw_y,
          centered ? 1 : 0,
          text_value.c_str());
    }

    const bool accepted =
        MohPcLayer::
            QueuePS3FontDraw(
                text_value.c_str(),
                draw_x,
                draw_y,
                centered,
                exact_font.c_str(),
                rgba);

    state->gpr[0] =
        accepted ?
            1u :
            0u;

    return true;
  }

  if (address == MOH_HOSTCALL_VP6_MOVIE_ON || address == MOH_HOSTCALL_VP6_MOVIE_OFF) {
    MohPcLayer::SetMovieActive(address == MOH_HOSTCALL_VP6_MOVIE_ON);
    return true;
  }
  if (address == MOH_HOSTCALL_ADS_STATE) {
    if (state)
      state->gpr[0] = F32Bits(MohPcLayer::GetAdsBlend());
    return true;
  }
  if (address == MOH_HOSTCALL_ADS_CROSSHAIR) {
    if (state &&
        (MohPcLayer::IsPcCrosshairEnabled() ||
         MohPcLayer::ShouldHideAdsCrosshair()))
    {
      // The native CS-style reticle is authoritative. Never allow the
      // original GMFE69 crosshair status to reach the HUD while enabled.
      state->gpr[3] = 0;
    }
    return true;
  }
  if (address == MOH_HOSTCALL_ADS_VIEWMODEL) {
    if (!state)
      return true;
    const float blend = MohPcLayer::GetAdsBlend();
    if (blend <= 0.0001f)
      return true;

    // UpdateWeaponTransforms has r30=this and has already assembled the local
    // translation vector at guest stack +0x30.  Modify the temporary vector,
    // never the animation-owned object fields, so releasing ADS restores the
    // exact original weapon animation automatically.
    const std::uint32_t weapon_object = state->gpr[30];
    const std::uint32_t stack = state->gpr[1];
    if (!IsMem1Address(weapon_object) || !IsMem1Address(stack))
      return true;

    const std::uint32_t weapon = ReadGuestU32(state, weapon_object + 0x30E4u);
    if (IsMem1Address(weapon))
      MohPcLayer::SetCurrentWeaponType(static_cast<int>(ReadGuestU32(state, weapon + 0x298u)));

    const std::uint32_t vec = stack + 0x30u;
    float x = ReadGuestF32(state, vec + 0u);
    float y = ReadGuestF32(state, vec + 4u);
    float z = ReadGuestF32(state, vec + 8u);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
      return true;

    const float center = std::clamp(blend * MohPcLayer::GetAdsCenterStrength(), 0.0f, 1.0f);
    x += (MohPcLayer::GetAdsTargetX() - x) * center;
    y += (MohPcLayer::GetAdsTargetY() - y) * center;
    z += MohPcLayer::GetAdsZOffset() * blend;
    WriteGuestF32(state, vec + 0u, x);
    WriteGuestF32(state, vec + 4u, y);
    WriteGuestF32(state, vec + 8u, z);

    static bool ads_logged = false;
    if (!ads_logged) {
      ads_logged = true;
      std::fprintf(stderr,
                   "[moh-pc] FPS ADS viewmodel hook active: object=%08x type=%d blend=%.2f\\n",
                   weapon_object, MohPcLayer::GetCurrentWeaponType(), blend);
    }
    return true;
  }


  if (address == MOH_HOSTCALL_FRAME_PRESENT) {
    MohPcLayer::ArmGameplayPresent(state->gpr[3]);
    return true;
  }

  // Reserved GMFE69 control tokens emitted directly by the native game module.
  // They toggle Dolphin's VI-frequency override only while the real gameplay
  // loop is active; shell/menu/FMVs/DVD loading remain at original timing.
  if (address == MOH_HOSTCALL_VI_GAMEPLAY_ON ||
      address == MOH_HOSTCALL_VI_GAMEPLAY_OFF) {
    const bool enable = address == MOH_HOSTCALL_VI_GAMEPLAY_ON;

    // v6.6:
    // Reuse the exact GameCube gameplay marker which already controls
    // gameplay-only VI/FPS/widescreen behaviour.
    MohPcLayer::SetGameplayDetected(
        enable);

    Config::SetCurrent(Config::MAIN_VI_OVERCLOCK_ENABLE, enable);
    std::fprintf(stderr,
                 "[moh-enh] gameplay VI overclock %s (factor %.4f)\n",
                 enable ? "ON" : "OFF",
                 Config::Get(Config::MAIN_VI_OVERCLOCK));
    return true;
  }

#endif
  return user_data &&
         static_cast<ModManager *>(user_data)->Dispatch(state, address);
}
#undef MODERNGEKKO_HOSTCALL_NOINLINE

bool ModManager::HostCall(CPUState *state, std::uint32_t address,
                          void *user_data) {
#if defined(MODERNGEKKO_MOH_PC_LAYER)
  // All native MOH PC-layer control tokens live in the compact F100..F161
  // window.  Ordinary guest PCs are nowhere near this range, so keep their
  // path leaf-like: one unsigned range check followed by a tail call into
  // Dispatch().  The expensive token implementation stays completely cold.
  constexpr std::uint32_t token_span =
      MOH_HOSTCALL_PS3_FONT_CENTERED - MOH_HOSTCALL_VI_GAMEPLAY_ON;
  const std::uint32_t token_offset =
      address - MOH_HOSTCALL_VI_GAMEPLAY_ON;
#if defined(__GNUC__) || defined(__clang__)
  if (__builtin_expect(token_offset <= token_span, 0))
#else
  if (token_offset <= token_span)
#endif
    return HandleMohPcLayerHostCall(state, address, user_data);
#endif

  return user_data &&
         static_cast<ModManager *>(user_data)->Dispatch(state, address);
}

bool ModManager::HostCallContains(std::uint32_t address, void *user_data) {
  return user_data &&
         static_cast<ModManager *>(user_data)->HandlesAddress(address);
}

bool ModManager::HostCallRangeContains(std::uint32_t start, std::uint32_t end,
                                       void *user_data) {
  return user_data &&
         static_cast<ModManager *>(user_data)->HandlesRange(start, end);
}
}
