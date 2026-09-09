#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeGCAssets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCFont.h"
#include "VideoCommon/MOHFrontline/Assets/Formats/WorldFormats.h"
#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCCompartment.h"
#include "VideoCommon/MOHFrontline/Assets/GC/Formats/GCViv.h"
#include "VideoCommon/MOHFrontline/Engine/NativePCStatus.h"

namespace MOHFrontline::NativeGCAssets
{
namespace
{
struct Entry
{
  std::string name;
  std::string extension;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

struct VivIndex
{
  std::filesystem::path path;
  std::uint64_t file_size = 0;
  std::vector<Entry> entries;
};

std::mutex s_mutex;
std::unordered_map<std::string, std::shared_ptr<VivIndex>> s_vivs;
std::unordered_set<std::string> s_logged_entries;
std::unordered_set<std::string> s_inspected_fonts;
std::unordered_map<std::string, std::uint64_t> s_extension_counts;
std::uint64_t s_total_viv_entries = 0;

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string Extension(std::string_view name)
{
  const auto slash = name.find_last_of("/\\");
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash))
    return {};
  return Lower(std::string(name.substr(dot)));
}

const char* Kind(std::string_view ext)
{
  if (ext == ".viv") return "VIV-container";
  if (ext == ".msh") return "MSH-static-mesh";
  if (ext == ".dmf") return "DMF-skinned-mesh";
  if (ext == ".cpt") return "CPT-world-geometry";
  if (ext == ".cdb") return "CDB-world-collision";
  if (ext == ".skl") return "SKL-skeleton";
  if (ext == ".mvd") return "MVD-animation";
  if (ext == ".mpk") return "MPK-animation-pack";
  if (ext == ".gsh") return "GSH/SHPG-texture";
  if (ext == ".gfn") return "GFN/FNTG-font";
  if (ext == ".tpk") return "TPK-texture-pack";
  if (ext == ".psp") return "PSP-property-BSP";
  if (ext == ".bpd") return "BPD-level-properties";
  if (ext == ".scr") return "SCR-shell-script";
  if (ext == ".cbs") return "CBS-behaviour";
  if (ext == ".sin") return "SIN-instance";
  if (ext == ".cls") return "CLS-class-data";
  if (ext == ".dat") return "DAT-game-data";
  if (ext == ".som") return "SOM-sound-map";
  if (ext == ".aem") return "AEM-event-map";
  if (ext == ".abk") return "ABK-audio-bank";
  if (ext == ".ast") return "AST-audio-stream";
  if (ext == ".mus") return "MUS-music";
  if (ext == ".asf") return "ASF-EA-stream";
  if (ext == ".mpc") return "MPC-EA-movie";
  if (ext == ".emt") return "EMT-level-table";
  if (ext == ".bum") return "BUM-table";
  if (ext == ".mpf") return "MPF-level-data";
  if (ext == ".lfc") return "LFC-level-file";
  return "asset";
}

bool IsSemanticAsset(std::string_view ext)
{
  return ext == ".msh" || ext == ".dmf" || ext == ".cpt" || ext == ".cdb" ||
         ext == ".skl" || ext == ".mvd" || ext == ".mpk" || ext == ".gsh" ||
         ext == ".gfn" || ext == ".tpk" || ext == ".psp" || ext == ".bpd" ||
         ext == ".scr" || ext == ".cbs" || ext == ".sin";
}

bool ReadFileRange(const std::filesystem::path& path, std::uint64_t offset,
                   std::uint64_t size, std::vector<unsigned char>* out)
{
  if (!out || size == 0 || size > 64ull * 1024ull * 1024ull)
    return false;
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file)
    return false;
  out->resize(static_cast<std::size_t>(size));
  file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
  return static_cast<std::size_t>(file.gcount()) == out->size();
}

std::shared_ptr<VivIndex> ParseViv(const std::filesystem::path& path)
{
  std::error_code ec;
  const std::uint64_t file_size = std::filesystem::file_size(path, ec);
  if (ec || file_size < 6 || file_size > 1024ull * 1024ull * 1024ull)
    return {};

  const std::size_t probe_size = static_cast<std::size_t>(
      std::min<std::uint64_t>(file_size, 4ull * 1024ull * 1024ull));
  std::vector<unsigned char> bytes(probe_size);
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return {};
  stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(stream.gcount()));
  const auto entries = GCViv::Parse(bytes, file_size);
  if (!entries)
    return {};

  auto index = std::make_shared<VivIndex>();
  index->path = path;
  index->file_size = file_size;
  index->entries.reserve(entries->size());
  for (const auto& entry : *entries)
    index->entries.push_back({entry.name, Extension(entry.name), entry.offset, entry.size});

  std::sort(index->entries.begin(), index->entries.end(),
            [](const Entry& a, const Entry& b) {
              if (a.offset != b.offset) return a.offset < b.offset;
              return a.size < b.size;
            });
  return index;
}

std::shared_ptr<VivIndex> GetViv(const std::filesystem::path& path)
{
  const std::string key = Lower(path.lexically_normal().generic_string());
  {
    std::scoped_lock lock(s_mutex);
    if (const auto it = s_vivs.find(key); it != s_vivs.end())
      return it->second;
  }

  auto parsed = ParseViv(path);
  if (!parsed)
  {
    NativePCStatus::Fallback(NativePCStatus::Domain::VIV, path.generic_string(),
                             "C0FB/BIGF TOC parse rejected");
    return {};
  }

  std::unordered_map<std::string, std::uint64_t> local_counts;
  for (const Entry& entry : parsed->entries)
    ++local_counts[entry.extension];

  {
    std::scoped_lock lock(s_mutex);
    const auto [it, inserted] = s_vivs.emplace(key, parsed);
    if (!inserted)
      return it->second;
    s_total_viv_entries += parsed->entries.size();
    for (const auto& [ext, count] : local_counts)
      s_extension_counts[ext] += count;
  }

  char detail[512]{};
  std::snprintf(detail, sizeof(detail),
                "VIV host-indexed entries=%zu MSH=%llu CPT=%llu SKL=%llu DMF=%llu GSH=%llu GFN=%llu",
                parsed->entries.size(),
                static_cast<unsigned long long>(local_counts[".msh"]),
                static_cast<unsigned long long>(local_counts[".cpt"]),
                static_cast<unsigned long long>(local_counts[".skl"]),
                static_cast<unsigned long long>(local_counts[".dmf"]),
                static_cast<unsigned long long>(local_counts[".gsh"]),
                static_cast<unsigned long long>(local_counts[".gfn"]));
  NativePCStatus::Native(NativePCStatus::Domain::VIV, path.filename().generic_string(), detail);
  return parsed;
}

void InspectFont(const std::filesystem::path& container, const Entry& entry)
{
  if (entry.extension != ".gfn" || entry.size == 0)
    return;
  const std::string key = Lower(container.generic_string() + "::" + entry.name);
  {
    std::scoped_lock lock(s_mutex);
    if (!s_inspected_fonts.emplace(key).second)
      return;
  }
  std::vector<unsigned char> bytes;
  if (!ReadFileRange(container, entry.offset, entry.size, &bytes) ||
      !GCFont::Inspect(entry.name, bytes))
  {
    NativePCStatus::Fallback(NativePCStatus::Domain::Font, entry.name,
                             "GFN/FNTG host parser rejected");
  }
}

void InspectCompartment(const std::filesystem::path& container, const Entry& entry)
{
  if (entry.extension != ".cpt" && entry.extension != ".cdb" &&
      entry.extension != ".bpd" && entry.extension != ".psp")
    return;
  std::vector<unsigned char> bytes;
  bool valid = ReadFileRange(container, entry.offset, entry.size, &bytes);
  std::size_t decoded = 0;
  if (valid && entry.extension == ".cpt")
  {
    const auto cpt = GCCompartment::CPT::Parse(bytes);
    valid = cpt.has_value();
    if (cpt)
    {
      for (std::uint32_t i = 0; valid && i < cpt->tables[2].count; ++i)
      {
        const auto geometry = cpt->Geometry(i);
        valid = geometry.has_value();
        if (!geometry)
          break;
        for (std::uint32_t j = 0; valid && j < cpt->data.U16(*geometry + 2); ++j)
        {
          GCCompartment::Vertex vertex;
          valid = cpt->DecodeVertex(*geometry, j, &vertex);
          decoded += valid;
        }
      }
    }
  }
  else if (valid && entry.extension == ".cdb")
  {
    const auto cdb = GCCompartment::CDB::Parse(bytes);
    valid = cdb.has_value();
    if (cdb)
    {
      for (std::uint32_t i = 0; valid && i < cdb->triangles.count; ++i)
      {
        std::array<std::array<float, 3>, 3> triangle;
        valid = cdb->DecodeTriangle(i, &triangle);
        decoded += valid;
      }
    }
  }
  else if (valid && entry.extension == ".bpd")
  {
    const auto bpd = WorldFormats::BPD::ParseWithEndian(bytes, true);
    valid = bpd.has_value();
    if (bpd)
      decoded = bpd->properties.size();
  }
  else if (valid)
  {
    const auto psp = WorldFormats::PSP::Parse(bytes, true);
    valid = psp.has_value();
    if (psp)
      decoded = psp->nodes.size() + psp->leaves.size();
  }
  const char* units = entry.extension == ".cpt" ? "strip vertices" :
                      entry.extension == ".cdb" ? "collision triangles" :
                      entry.extension == ".bpd" ? "properties" : "BSP nodes/leaves";
  const std::string detail = valid ?
      "host file decoder validated " + std::to_string(decoded) + " " + units +
          "; live execution=static-recomp/GX" :
      "host world decoder rejected file layout; semantic handling=static-recomp";
  if (valid)
    NativePCStatus::Native(NativePCStatus::Domain::AssetCPU, entry.name, detail);
  else
    NativePCStatus::Fallback(NativePCStatus::Domain::AssetCPU, entry.name, detail);
}

void LogAsset(const std::filesystem::path& container, const Entry& entry)
{
  const std::string key = Lower(container.generic_string() + "::" + entry.name);
  {
    std::scoped_lock lock(s_mutex);
    if (!s_logged_entries.emplace(key).second)
      return;
  }

  std::string detail = Kind(entry.extension);
  detail += " | VIV offset=0x";
  char offset_text[32]{};
  std::snprintf(offset_text, sizeof(offset_text), "%llx",
                static_cast<unsigned long long>(entry.offset));
  detail += offset_text;
  detail += " size=" + std::to_string(entry.size);
  if (IsSemanticAsset(entry.extension))
    detail += " | host bytes -> static-recomp PC semantic parser";
  NativePCStatus::Native(NativePCStatus::Domain::AssetCPU, entry.name, detail);

  InspectFont(container, entry);
  InspectCompartment(container, entry);
}
}  // namespace

void ObserveHostRead(const std::filesystem::path& host_path, std::string_view guest_path,
                     std::uint64_t offset, std::size_t bytes)
{
  if (host_path.empty())
    return;

  const std::string ext = Extension(host_path.filename().generic_string());
  if (ext != ".viv")
  {
    std::string detail = Kind(ext);
    detail += " | direct host file -> native/static-recomp PC";
    NativePCStatus::Native(NativePCStatus::Domain::AssetCPU, guest_path, detail);
    if (ext == ".gfn")
    {
      std::vector<unsigned char> data;
      std::error_code ec;
      const auto size = std::filesystem::file_size(host_path, ec);
      if (!ec && size && ReadFileRange(host_path, 0, size, &data))
        GCFont::Inspect(guest_path, data);
    }
    return;
  }

  const auto index = GetViv(host_path);
  if (!index || bytes == 0 || offset >= index->file_size)
    return;

  const std::uint64_t read_end = std::min<std::uint64_t>(
      index->file_size, offset + std::min<std::uint64_t>(bytes, index->file_size - offset));
  for (const Entry& entry : index->entries)
  {
    if (entry.size == 0)
      continue;
    const std::uint64_t entry_end = entry.offset + entry.size;
    if (entry_end <= offset)
      continue;
    if (entry.offset >= read_end)
      break;
    LogAsset(host_path, entry);
  }
}

void Shutdown()
{
  std::scoped_lock lock(s_mutex);
  std::fprintf(stderr,
               "[NATIVE-PC] GC asset inventory: VIV=%zu nested=%llu "
               "MSH=%llu CPT=%llu SKL=%llu DMF=%llu GSH=%llu GFN=%llu "
               "MVD=%llu MPK=%llu SCR=%llu CBS=%llu SIN=%llu\n",
               s_vivs.size(), static_cast<unsigned long long>(s_total_viv_entries),
               static_cast<unsigned long long>(s_extension_counts[".msh"]),
               static_cast<unsigned long long>(s_extension_counts[".cpt"]),
               static_cast<unsigned long long>(s_extension_counts[".skl"]),
               static_cast<unsigned long long>(s_extension_counts[".dmf"]),
               static_cast<unsigned long long>(s_extension_counts[".gsh"]),
               static_cast<unsigned long long>(s_extension_counts[".gfn"]),
               static_cast<unsigned long long>(s_extension_counts[".mvd"]),
               static_cast<unsigned long long>(s_extension_counts[".mpk"]),
               static_cast<unsigned long long>(s_extension_counts[".scr"]),
               static_cast<unsigned long long>(s_extension_counts[".cbs"]),
               static_cast<unsigned long long>(s_extension_counts[".sin"]));
  s_vivs.clear();
  s_logged_entries.clear();
  s_inspected_fonts.clear();
  s_extension_counts.clear();
  s_total_viv_entries = 0;
}
}  // namespace MOHFrontline::NativeGCAssets
