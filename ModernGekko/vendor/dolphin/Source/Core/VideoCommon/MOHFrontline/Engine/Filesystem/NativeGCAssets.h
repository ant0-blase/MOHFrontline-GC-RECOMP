#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace MOHFrontline::NativeGCAssets
{
// Observe exact host-backed GameCube reads. Frontline compact VIV is decoded
// natively as C0 FB + BE16 count + repeated BE24 offset/BE24 size/NUL name.
// Nested MSH/CPT/SKL/DMF/GSH/GFN/etc accesses are reported with their real
// archive offsets; GFN payloads are additionally validated by the host parser.
void ObserveHostRead(const std::filesystem::path& host_path, std::string_view guest_path,
                     std::uint64_t offset, std::size_t bytes);
void Shutdown();
}  // namespace MOHFrontline::NativeGCAssets
