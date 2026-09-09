#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/RenderState.h"

namespace MOHFrontline::NativeHostRenderer
{
// Installs the first concrete NativeRender submitter.  Resources are created
// lazily on the GPU thread when the first matched PS3 MSH/DMF is submitted.
void Initialize();
void Shutdown();

// Experimental generic GC decoded-batch takeover. Disabled by default until
// TEV/UI/CPT semantics are reproduced; set MOH_NATIVE_GC_RENDER=1 to test.
bool GameCubeBatchTakeoverEnabled();

bool SubmitGameCubeDecodedBatch(const u8* vertex_data, u32 num_vertices,
                                const u16* indices, u32 num_indices,
                                const PortableVertexDeclaration& declaration,
                                PrimitiveType primitive);
}  // namespace MOHFrontline::NativeHostRenderer
