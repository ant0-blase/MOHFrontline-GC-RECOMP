#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace MOHFrontline::NativeRender
{
enum class Mode
{
  Off,
  Shadow,
  PreferNative,
};

struct Vertex
{
  std::array<float, 3> position{};
  std::array<float, 3> normal{};
  std::array<float, 2> uv0{};
  std::array<u8, 4> color{255, 255, 255, 255};
  u8 matrix_index = 0;
};

struct DrawPacket
{
  bool skinned = false;
  bool has_texture = true;
  bool use_vertex_color = false;
  std::string source_name;
  std::string material_name;
  std::vector<Vertex> vertices;
  std::vector<u32> indices;

  // For PS3 DMF draws these are the authored/model -> GC local affine
  // transforms already prepared by PS3MeshPort.
  std::vector<std::array<float, 12>> model_to_gc_local;
};

using Submitter = bool (*)(const DrawPacket& packet, void* userdata);

Mode GetMode();
const char* ModeName(Mode mode);

void SetSubmitter(Submitter submitter, void* userdata = nullptr);
bool HasSubmitter();

bool KeepOriginalGCGeometry();

// Builds a host-neutral packet from the exact PS3 MSH/DMF draw currently
// associated with the GC renderer. Returns false for ordinary/unmatched GC
// draws, which keeps the existing GX path untouched.
bool BuildCurrentDraw(DrawPacket* out);

// Returns true only when PreferNative is selected AND a registered native
// backend accepted the packet. VertexManagerBase then skips its GX DrawIndexed.
// Shadow mode submits a copy to the backend but always keeps the GX draw.
bool TrySubmitCurrentDraw();
}  // namespace MOHFrontline::NativeRender
