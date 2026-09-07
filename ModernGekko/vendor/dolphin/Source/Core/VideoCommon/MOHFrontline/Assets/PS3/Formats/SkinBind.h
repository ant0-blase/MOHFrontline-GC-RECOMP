#pragma once

// CPU preload only. Matrices below are row-major, acting on column vectors.
// SKL supplies hierarchy/translations; DMF supplies bind Euler angles and the
// independently authored inverse-world matrices. Do not infer rotations from
// the zero-filled area following the SKL translation table.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace MOHFrontline::PS3::SkinBind
{
using Matrix = std::array<double, 16>;
using Vector = std::array<double, 3>;
inline Matrix Identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
inline Matrix Multiply(const Matrix& a, const Matrix& b)
{
  Matrix result{};
  for (unsigned r = 0; r < 4; ++r)
    for (unsigned c = 0; c < 4; ++c)
      for (unsigned k = 0; k < 4; ++k)
        result[r*4+c] += a[r*4+k] * b[k*4+c];
  return result;
}
inline Vector Transform(const Matrix& m, const Vector& p)
{
  Vector result{};
  for (unsigned r = 0; r < 3; ++r)
    result[r] = m[r*4]*p[0] + m[r*4+1]*p[1] + m[r*4+2]*p[2] + m[r*4+3];
  return result;
}
inline Matrix Rotation(unsigned axis, double angle)
{
  Matrix m = Identity();
  const unsigned a = (axis+1)%3, b = (axis+2)%3;
  m[a*4+a] = m[b*4+b] = std::cos(angle);
  m[a*4+b] = -std::sin(angle);
  m[b*4+a] = std::sin(angle);
  return m;
}
struct Reader
{
  std::span<const std::uint8_t> bytes;
  bool big_endian = true;
  bool Has(std::size_t offset, std::size_t size) const
  { return offset <= bytes.size() && size <= bytes.size()-offset; }
  std::uint32_t U32(std::size_t p) const
  {
    const auto value = (std::uint32_t(bytes[p])<<24)|(std::uint32_t(bytes[p+1])<<16)|(std::uint32_t(bytes[p+2])<<8)|bytes[p+3];
    return big_endian ? value : ((value>>24)|((value>>8)&0xff00)|((value<<8)&0xff0000)|(value<<24));
  }
  std::int16_t S16(std::size_t p) const
  { return std::bit_cast<std::int16_t>(std::uint16_t((bytes[p]<<8)|bytes[p+1])); }
  double Float(std::size_t p) const { return std::bit_cast<float>(U32(p)); }
  std::string Name(std::size_t p) const
  {
    std::size_t n = 0;
    while (n < 16 && bytes[p+n]) ++n;
    return std::string(reinterpret_cast<const char*>(bytes.data()+p), n);
  }
};
struct Joint
{
  std::string name;
  int parent = -1;
  Vector translation{};
};
struct Hierarchy
{
  bool valid = false;
  std::vector<Joint> joints;
};
inline Hierarchy DecodeHierarchy(std::span<const std::uint8_t> bytes)
{
  Reader r{bytes};
  Hierarchy out;
  if (!r.Has(0, 32)) return out;
  if (r.U32(0) == 0x314c4b53) r.big_endian = false;
  if (r.U32(0) != 0x534b4c31) return out;
  const auto word = r.U32(4);
  const auto count = (word & 0xffff) ? word & 0xffff : word >> 16;
  const auto names = r.U32(12);
  if (!count || count > 512 || !r.Has(names, std::size_t(count)*20) ||
      !r.Has(32, std::size_t(count)*16)) return out;
  std::unordered_map<std::uint32_t, unsigned> nodes;
  std::unordered_map<std::string, unsigned> unique_names;
  for (unsigned i = 0; i < count; ++i)
  {
    const auto node = r.U32(names+i*20);
    if (!r.Has(node, 8) || !nodes.emplace(node, i).second) return {};
    Joint joint;
    joint.name = r.Name(names+i*20+4);
    if (joint.name.empty() || !unique_names.emplace(joint.name, i).second) return {};
    for (unsigned a = 0; a < 3; ++a)
    {
      joint.translation[a] = r.Float(32+i*16+a*4);
      if (!std::isfinite(joint.translation[a])) return {};
    }
    out.joints.push_back(std::move(joint));
  }
  for (unsigned i = 0; i < count; ++i)
  {
    const auto node = r.U32(names+i*20);
    const auto depth = bytes[node+1];
    if (bytes[node+2] != i) return {};
    if (i == 0)
    {
      // The root's parent word is not a valid file pointer.
      if (depth != 0 || std::bit_cast<std::int32_t>(r.U32(32+i*16+12)) != -1)
        return {};
    }
    else
    {
      const auto parent = nodes.find(r.U32(node+4));
      if (parent == nodes.end() || parent->second >= i) return {};
      const auto parent_node = r.U32(names+parent->second*20);
      if (depth != bytes[parent_node+1]+1 || r.U32(32+i*16+12) != depth-1u)
        return {};
      out.joints[i].parent = static_cast<int>(parent->second);
    }
    // Node links include traversal successors, not only children. Validate
    // their storage and targets without inventing parent-child relationships.
    const auto links = bytes[node];
    if (!r.Has(node+8, std::size_t(links)*4)) return {};
    for (unsigned j = 0; j < links; ++j)
      if (!nodes.contains(r.U32(node+8+j*4))) return {};
  }
  out.valid = true;
  return out;
}
struct SkeletonBone
{
  std::string name;
  int parent = -1;
  Matrix local_bind{}, world_bind{}, inverse_bind{};
};
struct Binding
{
  bool valid = false;
  std::string reason;
  std::vector<SkeletonBone> bones;
  std::vector<int> ref_to_bone;
  double max_matrix_error = 0;
};
inline Binding Validate(std::span<const std::uint8_t> dmf, const Hierarchy& skeleton)
{
  Binding out;
  auto reject = [&](const char* reason) { out.reason = reason; return out; };
  Reader r{dmf};
  if (!skeleton.valid || !r.Has(0, 0x54) || r.U32(0) != 0x444d4600 ||
      dmf[4] != 5 || dmf[5] != 2) return reject("header/hierarchy");
  const auto count = r.U32(0x48), names = r.U32(0x4c), angles = r.U32(0x50);
  const std::size_t matrices = (std::size_t(angles)+std::size_t(count)*6+15)&~std::size_t(15);
  if (!count || count > 4096 || !r.Has(names, std::size_t(count)*16) ||
      !r.Has(angles, std::size_t(count)*6) || !r.Has(matrices, std::size_t(count)*64))
    return reject("bind tables");
  std::unordered_map<std::string, unsigned> refs;
  for (unsigned i = 0; i < count; ++i)
    if (!refs.emplace(r.Name(names+i*16), i).second) return reject("duplicate bone");
  out.ref_to_bone.assign(count, -1);
  for (unsigned i = 0; i < skeleton.joints.size(); ++i)
  {
    const auto& joint = skeleton.joints[i];
    const auto found = refs.find(joint.name);
    if (found == refs.end()) return reject("missing bind bone");
    const auto ref = found->second;
    SkeletonBone bone;
    bone.name = joint.name;
    bone.parent = joint.parent;
    bone.local_bind = Identity();
    // Proven GC order: Y, Z, X. Signed 16-bit angles encode a full turn.
    for (const auto axis : {1u, 2u, 0u})
      bone.local_bind = Multiply(bone.local_bind,
          Rotation(axis, r.S16(angles+ref*6+axis*2)*(6.2831853071795864769/65536.0)));
    for (unsigned a = 0; a < 3; ++a) bone.local_bind[a*4+3] = joint.translation[a];
    bone.world_bind = joint.parent < 0 ? bone.local_bind :
        Multiply(out.bones[joint.parent].world_bind, bone.local_bind);
    for (unsigned row = 0; row < 4; ++row)
      for (unsigned col = 0; col < 4; ++col)
      {
        const double value = r.Float(matrices+ref*64+(col*4+row)*4);
        if (!std::isfinite(value)) return reject("nonfinite inverse bind");
        bone.inverse_bind[row*4+col] = value;
      }
    const auto product = Multiply(bone.world_bind, bone.inverse_bind);
    for (unsigned j = 0; j < 16; ++j)
      out.max_matrix_error = std::max(out.max_matrix_error, std::abs(product[j]-Identity()[j]));
    out.ref_to_bone[ref] = static_cast<int>(i);
    out.bones.push_back(std::move(bone));
  }
  // Euler quantisation accumulates over the hierarchy; independent PS3 tables
  // on colt/grenade/soldier exhibit roughly 4e-4 to 6e-4 error, not exact zero.
  if (out.max_matrix_error > 0.002) return reject("world_bind * inverse_bind mismatch");
  out.valid = true;
  return out;
}
} // namespace MOHFrontline::PS3::SkinBind
