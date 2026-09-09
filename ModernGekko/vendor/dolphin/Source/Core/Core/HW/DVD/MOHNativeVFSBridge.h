#pragma once

#include <span>
#include <string_view>

#include "Common/CommonTypes.h"

namespace DVD
{
// Core owns the physical/FST read path, while the MOH native VFS lives in
// VideoCommon. Keep the dependency one-way by exposing only a tiny callback ABI.
using MOHNativeVFSReadCallback =
    bool (*)(std::string_view guest_path, u64 file_offset, std::span<u8> destination);

void SetMOHNativeVFSReadCallback(MOHNativeVFSReadCallback callback);

// Returns true only when a host-side GC-compatible file supplied the complete
// requested range. False means "use the original disc read".
bool TryMOHNativeVFSRead(std::string_view guest_path, u64 file_offset,
                         std::span<u8> destination);
}  // namespace DVD
