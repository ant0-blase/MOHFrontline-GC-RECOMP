#include "moderngekko/mod_loader.hpp"
#if defined(MODERNGEKKO_MOH_PC_LAYER)
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "VideoCommon/MohPcLayer.h"
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
constexpr std::uint32_t MOH_HOSTCALL_VI_GAMEPLAY_ON = 0xFFFFF100u;
constexpr std::uint32_t MOH_HOSTCALL_VI_GAMEPLAY_OFF = 0xFFFFF101u;
constexpr std::uint32_t MOH_HOSTCALL_GAMEPLAY_ENTER = 0xFFFFF110u;
constexpr std::uint32_t MOH_HOSTCALL_GAMEPLAY_EXIT = 0xFFFFF111u;
constexpr std::uint32_t MOH_HOSTCALL_MOUSE_LOOK = 0xFFFFF120u;
constexpr std::uint32_t MOH_HOSTCALL_ADS_VIEWMODEL = 0xFFFFF130u;
constexpr std::uint32_t MOH_HOSTCALL_ADS_STATE = 0xFFFFF131u;
constexpr std::uint32_t MOH_HOSTCALL_ADS_CROSSHAIR = 0xFFFFF132u;
constexpr std::uint32_t MOH_HOSTCALL_VP6_MOVIE_ON = 0xFFFFF140u;
constexpr std::uint32_t MOH_HOSTCALL_VP6_MOVIE_OFF = 0xFFFFF141u;

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

bool ModManager::Dispatch(CPUState *state, std::uint32_t address) {
  if (!state)
    return false;
  if (!m_impl->runtime_started) {
    m_impl->runtime_started = true;
    TriggerEvent("*", "runtime_start", state);
  }
  if (!m_impl->pending_returns.empty() &&
      m_impl->pending_returns.back().address == address &&
      m_impl->pending_returns.back().stack_pointer == state->gpr[1]) {
    auto pending = std::move(m_impl->pending_returns.back());
    m_impl->pending_returns.pop_back();
    for (auto it = pending.functions.rbegin(); it != pending.functions.rend();
         ++it) {
      const CPUState saved = *state;
      (*it)(state);
      *state = saved;
    }
  }

  // Dispatch is hit very frequently by native recomp hooks.  The hook/patch
  // tables are immutable after Load(), so cache the resolved entries by guest
  // address and avoid two unordered_map hashes on recurring hot addresses.
  auto &cached = m_impl->dispatch_cache[
      (address >> 2u) & (Impl::DISPATCH_CACHE_SIZE - 1u)];
  const Impl::Hooks *address_hooks = nullptr;
  ModernGekkoModFunction address_patch = nullptr;

  if (cached.valid && cached.address == address) {
    address_hooks = cached.hooks;
    address_patch = cached.patch;
  } else {
    const auto hooks = m_impl->hooks.find(address);
    const auto patch = m_impl->patches.find(address);
    address_hooks = hooks != m_impl->hooks.end() ? &hooks->second : nullptr;
    address_patch = patch != m_impl->patches.end() ? patch->second : nullptr;
    cached.address = address;
    cached.hooks = address_hooks;
    cached.patch = address_patch;
    cached.valid = true;
  }

  if (address_hooks) {
    for (ModernGekkoModFunction function : address_hooks->entry) {
      const CPUState saved = *state;
      function(state);
      *state = saved;
    }
    if (!address_hooks->returning.empty()) {
      if (m_impl->pending_returns.size() >= 4096u)
        m_impl->pending_returns.erase(m_impl->pending_returns.begin());
      m_impl->pending_returns.push_back(
          {state->lr, state->gpr[1], address_hooks->returning});
    }
  }
  if (!address_patch)
    return false;
  address_patch(state);
  return true;
}

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

bool ModManager::HostCall(CPUState *state, std::uint32_t address,
                          void *user_data) {
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
    if (state && MohPcLayer::ShouldHideAdsCrosshair())
      state->gpr[3] = 0;
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

  // Reserved GMFE69 control tokens emitted directly by the native game module.
  // They toggle Dolphin's VI-frequency override only while the real gameplay
  // loop is active; shell/menu/FMVs/DVD loading remain at original timing.
  if (address == MOH_HOSTCALL_VI_GAMEPLAY_ON ||
      address == MOH_HOSTCALL_VI_GAMEPLAY_OFF) {
    const bool enable = address == MOH_HOSTCALL_VI_GAMEPLAY_ON;
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
