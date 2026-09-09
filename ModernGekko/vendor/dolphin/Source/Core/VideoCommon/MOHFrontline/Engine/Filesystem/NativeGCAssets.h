#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace MOHFrontline::NativeGCAssets
{
// Observe exact host-backed GameCube reads. Both compact C0FB/BE24 archives
// and the BIGF/BE32 archive used by 2_2/comp.viv are indexed natively.
// Nested MSH/CPT/SKL/DMF/GSH/GFN/etc accesses are reported with their real
// archive offsets; GFN and CPT/CDB payloads are also decoded/validated on host.
void ObserveHostRead(const std::filesystem::path& host_path, std::string_view guest_path,
                     std::uint64_t offset, std::size_t bytes);
void Shutdown();
}  // namespace MOHFrontline::NativeGCAssets
