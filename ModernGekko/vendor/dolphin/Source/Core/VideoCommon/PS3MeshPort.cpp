#include "VideoCommon/PS3MeshPort.h"
#include "VideoCommon/PS3AssetPort.h"
#include "VideoCommon/MOHFrontline/Assets/PS3/Formats/MSH.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace PS3MeshPort
{
namespace
{
u16 BE16(const u8* p)
{
  return static_cast<u16>((u16(p[0]) << 8) | u16(p[1]));
}

u32 BE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u32 LE32(const u8* p)
{
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}
}  // namespace

bool ParseMSHv8(std::span<const u8> bytes, StaticMesh* out)
{
  if (!PS3AssetPort::IsMSHEnabled())
    return false;

  const bool ok = MOHFrontline::PS3::MSH::Decode(bytes, out);
  static unsigned logs = 0;
  if (ok && out && logs++ < 64)
  {
    std::fprintf(stderr, "[moh-ps3-msh] decoded static MSH: submeshes=%zu\n",
                 out->submeshes.size());
  }
  return ok;
}

DMFInfo InspectDMF(std::span<const u8> bytes)
{
  DMFInfo out;

  if (!PS3AssetPort::IsDMFEnabled())
    return out;

  if (bytes.size() < 0x5c ||
      std::memcmp(bytes.data(), "DMF\\0", 4) != 0)
  {
    return out;
  }

  // Frontline PS3 remaster DMF revision seen in the RPCS3 dump:
  // bytes +04 = 05 02 00 00 -> revision 0x0502 + flags 0x0000.
  const u32 ps3_revision =
      (u32(bytes[4]) << 8) |
      u32(bytes[5]);

  const u32 ps3_flags =
      (u32(bytes[6]) << 8) |
      u32(bytes[7]);

  if (ps3_revision >= 0x0100 &&
      ps3_revision <= 0x0FFF &&
      ps3_flags <= 0x00FF)
  {
    out.version =
        ps3_revision;

    out.mesh_count =
        BE32(bytes.data() + 0x20);

    out.material_count =
        BE32(bytes.data() + 0x28);

    // +0x48 = count.
    // +0x4C is the pointer/table field and MUST NOT be read as a count.
    out.bone_ref_count =
        BE32(bytes.data() + 0x48);
  }
  else
  {
    // Preserve conservative support for the older documented DMF family.
    const u32 version_le =
        LE32(bytes.data() + 4);

    const u32 version_be =
        BE32(bytes.data() + 4);

    if (version_le > 0 &&
        version_le < 64)
    {
      out.version =
          version_le;

      out.mesh_count =
          LE32(bytes.data() + 0x20);

      out.material_count =
          LE32(bytes.data() + 0x28);

      out.bone_ref_count =
          LE32(bytes.data() + 0x4c);
    }
    else if (version_be > 0 &&
             version_be < 64)
    {
      out.version =
          version_be;

      out.mesh_count =
          BE32(bytes.data() + 0x20);

      out.material_count =
          BE32(bytes.data() + 0x28);

      out.bone_ref_count =
          BE32(bytes.data() + 0x4c);
    }
    else
    {
      return out;
    }
  }

  char name[9]{};

  std::memcpy(
      name,
      bytes.data() + 0x0c,
      8);

  out.model_name =
      name;

  if (out.mesh_count > 65535 ||
      out.material_count > 65535 ||
      out.bone_ref_count > 4096)
  {
    return {};
  }

  out.valid = true;

  return out;
}

bool IsHostRenderable(const StaticMesh& mesh)
{
  if (mesh.submeshes.empty())
    return false;

  for (const auto& sub : mesh.submeshes)
  {
    if (sub.position_uv.size() != sub.vertex_count || sub.indices.empty() || !sub.vertex_stride)
      return false;

    // Require a position declaration. Semantic 0 is the position field in
    // the PS3 MSH v8 files observed in Frontline.
    const bool has_position =
        std::any_of(sub.attributes.begin(), sub.attributes.end(),
                    [](const Attribute& a) { return a.semantic == 0 && a.components >= 3; });
    if (!has_position)
      return false;
  }

  return true;
}
}  // namespace PS3MeshPort
