#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/MOHFrontline/Engine/Renderer/Meshes/StaticMesh.h"

namespace PS3MeshPort
{
using Attribute = MOHFrontline::Meshes::Attribute;
using Submesh = MOHFrontline::Meshes::Submesh;
using StaticMesh = MOHFrontline::Meshes::StaticMesh;

struct DMFInfo
{
  bool valid = false;
  u32 version = 0;
  u32 mesh_count = 0;
  u32 material_count = 0;
  u32 bone_ref_count = 0;
  std::string model_name;
};

bool ParseMSHv8(std::span<const u8> bytes, StaticMesh* out);
DMFInfo InspectDMF(std::span<const u8> bytes);

// Static MSH can be consumed by a host renderer. It must NOT be copied into
// the GameCube MSH loader because GC and PS3 layouts are different.
bool IsHostRenderable(const StaticMesh& mesh);
}  // namespace PS3MeshPort
