#pragma once
#include <span>
#include "VideoCommon/MOHFrontline/Engine/Renderer/Meshes/StaticMesh.h"
namespace MOHFrontline::PS3::MSH
{
bool Decode(std::span<const std::uint8_t> bytes, Meshes::StaticMesh* out);
}
