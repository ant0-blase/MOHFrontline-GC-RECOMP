#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/MOHFrontline/Engine/Filesystem/NativeAssetResolver.h"
#include "VideoCommon/PS3RemasterAssets.h"
#include "VideoCommon/PS3TextureDecoder.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/TextureInfo.h"
#include "VideoCommon/VideoConfig.h"

namespace PS3WorldCPT
{
namespace detail
{
struct Entry
{
  u64 gc_fnv1a = 0;
  u32 gc_width = 0;
  u32 gc_height = 0;
  u32 gc_format = 0;

  u32 ps3_offset = 0;
  u32 ps3_size = 0;
  u32 ps3_width = 0;
  u32 ps3_height = 0;
  u32 ps3_format = 0;
  u32 ps3_mips = 0;

  bool safe = false;
  float score = 0.0f;
  float margin = 0.0f;
};

// Generated from the user's matched GameCube/PS3 1_1 files.
//
// GC side:
//   DATA/1/1_1/1_1_ART.cpt
//   CPT material -> SHPG -> GX CMPR payload
//
// PS3 side:
//   data/1/1_1/1_1_ART_c*.cpt resource descriptors
//   -> absolute range in data/1/1_1/rsx.viv
//
// The table is keyed by the exact FNV1a64 of the GC GX payload together with
// its dimensions/format. 154/186 identities are enabled by default because
// their offline GC-vs-PS3 image match was strong enough. All 186 remain
// "known world" identities so an unsafe one falls back to GC instead of the
// broad texture fuzzy matcher.
inline constexpr std::array<Entry, 186> kLevel11 = {{
    {0x00A8529DC2FF2136ULL, 128u, 128u, 14u, 0x00EE3F00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000546f, 0.508143f},
    {0x018370C00F51E788ULL, 64u, 64u, 14u, 0x0263BE80u, 10936u, 128u, 128u, 0x86u, 8u, false, 0.620353f, 0.034560f},
    {0x06F4EE8542358906ULL, 64u, 64u, 14u, 0x01094D00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.152815f, 0.214172f},
    {0x0C2E3A85ADF6E9D5ULL, 128u, 128u, 14u, 0x02283780u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001849f, 0.460869f},
    {0x0EBF0B7A3C95E449ULL, 128u, 64u, 14u, 0x01B40280u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.006284f, 0.600906f},
    {0x0F8A2ECBE90F640CULL, 64u, 32u, 14u, 0x00D6DC00u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.006076f, 0.529108f},
    {0x0FB1575E4E86F42BULL, 128u, 64u, 14u, 0x01D76380u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000952f, 0.361086f},
    {0x1302FA9026512254ULL, 64u, 64u, 14u, 0x03214500u, 10936u, 128u, 128u, 0x86u, 8u, false, 0.677868f, 0.014347f},
    {0x132664E032BF5CCDULL, 64u, 64u, 14u, 0x02283780u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.557536f, 0.057579f},
    {0x185FAC9C1BF6DE10ULL, 128u, 128u, 14u, 0x01869F80u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000691f, 0.414250f},
    {0x19F82E6ECDA36328ULL, 64u, 64u, 14u, 0x024C7680u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.001611f, 0.319969f},
    {0x1E351D53EEBC3873ULL, 128u, 128u, 14u, 0x00EFEB00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000842f, 0.525207f},
    {0x226D9044350ECD64ULL, 64u, 64u, 14u, 0x01E0AC80u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.006092f, 0.648924f},
    {0x2483D47048468278ULL, 64u, 64u, 14u, 0x01FDAD80u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.002414f, 0.503535f},
    {0x260CD78088F3D10FULL, 128u, 64u, 14u, 0x0232DF80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.006141f, 0.421812f},
    {0x2626D65B52AA1C7EULL, 128u, 64u, 14u, 0x0105F700u, 87400u, 512u, 256u, 0x86u, 10u, true, 0.195996f, 0.196886f},
    {0x2BB97472800DC143ULL, 128u, 64u, 14u, 0x00F41900u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001602f, 0.526011f},
    {0x2D227981DF4D7336ULL, 128u, 64u, 14u, 0x0265A280u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000653f, 0.337408f},
    {0x2E0FDC73682D2C0FULL, 128u, 64u, 14u, 0x022A4E80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001613f, 0.533584f},
    {0x2EADEA9243467419ULL, 64u, 64u, 14u, 0x02196480u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.002203f, 0.314290f},
    {0x2F9C28FA9506CF8CULL, 32u, 8u, 14u, 0x01FD7180u, 712u, 64u, 16u, 0x86u, 7u, true, 0.064044f, 0.650075f},
    {0x302E280AC973E652ULL, 64u, 64u, 14u, 0x04EC3C00u, 21872u, 128u, 128u, 0x88u, 8u, false, 0.619670f, 0.010135f},
    {0x31B6C03CFFF5D016ULL, 64u, 32u, 14u, 0x00D6B100u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.002673f, 0.457623f},
    {0x327FD429C7B42B4FULL, 32u, 128u, 14u, 0x00DABE00u, 10952u, 64u, 256u, 0x86u, 9u, true, 0.005538f, 0.517941f},
    {0x32B374A28DC313C6ULL, 128u, 64u, 14u, 0x03E70780u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.002398f, 0.471537f},
    {0x34A770C33E6B1F47ULL, 64u, 64u, 14u, 0x01052100u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.094521f, 0.182815f},
    {0x385219F1DD19306EULL, 128u, 32u, 14u, 0x00E0AD00u, 43720u, 512u, 128u, 0x86u, 10u, true, 0.108502f, 0.870962f},
    {0x3C44559F64DD77B5ULL, 128u, 64u, 14u, 0x01DA8680u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001380f, 0.566681f},
    {0x3DD65BAD74E419D3ULL, 128u, 64u, 14u, 0x034B5300u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000701f, 0.123455f},
    {0x3E1B1550B381A1C3ULL, 128u, 16u, 14u, 0x00E08100u, 5512u, 256u, 32u, 0x86u, 9u, true, 0.012720f, 0.884953f},
    {0x3E275E843694055CULL, 128u, 128u, 14u, 0x040E5480u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.258817f, 0.221969f},
    {0x3E73FF207F122BD4ULL, 128u, 128u, 14u, 0x014BCD00u, 349552u, 512u, 512u, 0x88u, 10u, false, 0.084520f, 0.047666f},
    {0x3EF4B21F36ED3300ULL, 64u, 32u, 14u, 0x010A2300u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.011001f, 0.606664f},
    {0x4099C0F5D3A38467ULL, 64u, 64u, 14u, 0x00CC3F00u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.244264f, 0.243949f},
    {0x410AC30E946E07E8ULL, 128u, 64u, 14u, 0x02653780u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000918f, 0.344032f},
    {0x41C187A96BB72CCCULL, 32u, 16u, 14u, 0x01FEB980u, 1384u, 64u, 32u, 0x86u, 7u, true, 0.022498f, 0.184193f},
    {0x42004F9F167F91D2ULL, 128u, 64u, 14u, 0x01DECA80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.003220f, 0.522471f},
    {0x42639FDE9B1F11BBULL, 128u, 128u, 14u, 0x01E2A680u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.004195f, 0.350769f},
    {0x433394D0BB77F68EULL, 128u, 64u, 14u, 0x010A8400u, 87400u, 512u, 256u, 0x86u, 10u, false, 0.405244f, 0.287077f},
    {0x435EAD7DB35CBA64ULL, 128u, 64u, 14u, 0x00E93D00u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001821f, 0.465392f},
    {0x43F317EE440EF2CBULL, 64u, 64u, 14u, 0x00FE1F00u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.658582f, 0.034396f},
    {0x46013F35045CD8E4ULL, 128u, 16u, 14u, 0x036F4900u, 5512u, 256u, 32u, 0x86u, 9u, true, 0.015335f, 0.662337f},
    {0x474D02B3DA902DE9ULL, 16u, 64u, 14u, 0x00CDC200u, 2760u, 32u, 128u, 0x86u, 8u, true, 0.023523f, 0.641642f},
    {0x4885903F8BDF6189ULL, 128u, 64u, 14u, 0x02514580u, 21864u, 256u, 128u, 0x86u, 9u, false, 0.524950f, 0.000835f},
    {0x4953833DE5063B32ULL, 64u, 64u, 14u, 0x0370AC00u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.003933f, 0.691552f},
    {0x495CDAF875A2BA55ULL, 128u, 64u, 14u, 0x0218F980u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.002252f, 0.532388f},
    {0x4C4FA3D6ECED99C4ULL, 64u, 64u, 14u, 0x02587780u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.004852f, 0.376200f},
    {0x4CB829783A138D91ULL, 128u, 64u, 14u, 0x010C2F00u, 87400u, 512u, 256u, 0x86u, 10u, false, 0.379882f, 0.296858f},
    {0x4EBCF621861DD515ULL, 128u, 64u, 14u, 0x01E48880u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.011068f, 0.704025f},
    {0x502C4CB22791F7C5ULL, 64u, 64u, 14u, 0x00D70700u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.002092f, 0.185552f},
    {0x508ED290D54ABE48ULL, 128u, 128u, 14u, 0x00B7D000u, 174776u, 512u, 512u, 0x86u, 10u, true, 0.147877f, 0.241264f},
    {0x50A3FE0E0049C738ULL, 128u, 64u, 14u, 0x01DD5380u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.008554f, 0.578279f},
    {0x52DA1D927C2B5AEAULL, 64u, 32u, 14u, 0x03E56580u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.008534f, 0.549656f},
    {0x56D7DDDE12DC746BULL, 64u, 64u, 14u, 0x01E45280u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.005398f, 0.725906f},
    {0x5D7F8A036EE8175FULL, 64u, 64u, 14u, 0x0230FD80u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.658735f, 0.003779f},
    {0x5D875FA1C52E0419ULL, 64u, 64u, 14u, 0x018F6580u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.000808f, 0.196948f},
    {0x5E7F482BF610B96AULL, 128u, 128u, 14u, 0x03475A00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000446f, 0.374278f},
    {0x5EC7C1036B773BE7ULL, 128u, 64u, 14u, 0x034AE800u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000923f, 0.088103f},
    {0x5FAECD71CC8C54C4ULL, 128u, 128u, 14u, 0x04D99200u, 10936u, 128u, 128u, 0x86u, 8u, false, 0.554072f, 0.079192f},
    {0x62545B6995F25CA6ULL, 128u, 64u, 14u, 0x040CA980u, 87400u, 512u, 256u, 0x86u, 10u, true, 0.031249f, 0.303599f},
    {0x62EB72D5D57EAAAAULL, 64u, 64u, 14u, 0x01D97A80u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.002238f, 0.512096f},
    {0x640E9395A187BCE2ULL, 128u, 64u, 14u, 0x01DDF480u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.003687f, 0.524726f},
    {0x6886225DFF6560F9ULL, 64u, 64u, 14u, 0x018F2F80u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.001018f, 0.398554f},
    {0x68B890113585F64FULL, 64u, 64u, 14u, 0x00DB4A00u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.004674f, 0.551718f},
    {0x6B6D1275F06EE7E1ULL, 64u, 64u, 14u, 0x022FBC80u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.529319f, 0.045458f},
    {0x6C3D76A78316CA91ULL, 128u, 64u, 14u, 0x00CD5700u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.194096f, 0.405837f},
    {0x6F30F7DE7D45D902ULL, 64u, 64u, 14u, 0x022FBC80u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.594263f, 0.018215f},
    {0x6F8BA99AB0B26314ULL, 128u, 64u, 14u, 0x02646180u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000907f, 0.136940f},
    {0x719205391DEE0018ULL, 128u, 128u, 14u, 0x02276180u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000982f, 0.677739f},
    {0x71FBE9BBE0ED4EFCULL, 128u, 64u, 14u, 0x01D83980u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.002219f, 0.490267f},
    {0x72940506A3BD8A89ULL, 128u, 64u, 14u, 0x01E04180u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.006435f, 0.514062f},
    {0x72D337269276F7C4ULL, 128u, 64u, 14u, 0x01D7CE80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001485f, 0.106470f},
    {0x72D90FD69D29A936ULL, 128u, 64u, 14u, 0x022B2480u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001781f, 0.356800f},
    {0x73A2CCBB47885CEDULL, 64u, 64u, 14u, 0x01DF3580u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.005626f, 0.688288f},
    {0x7498934413E598C1ULL, 128u, 128u, 14u, 0x00D18400u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001690f, 0.444777f},
    {0x7B2DB6E59347C62CULL, 64u, 64u, 14u, 0x04D3F200u, 10936u, 128u, 128u, 0x86u, 8u, false, 0.660419f, 0.003813f},
    {0x7B9C8B8D93029B12ULL, 64u, 64u, 14u, 0x03214500u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.001792f, 0.525007f},
    {0x7BC7DB8D8DB0EB8EULL, 128u, 64u, 14u, 0x01DA1B80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000487f, 0.340131f},
    {0x7C3536F164483903ULL, 128u, 128u, 14u, 0x00D25A00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001255f, 0.442155f},
    {0x7D943FE1A305A551ULL, 64u, 64u, 14u, 0x036F1300u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.009357f, 0.596117f},
    {0x7DDFD657DE4A63DCULL, 64u, 64u, 14u, 0x01DDBE80u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.005041f, 0.545771f},
    {0x7EB484B8EC7D27ECULL, 64u, 64u, 14u, 0x01D6F780u, 10936u, 128u, 128u, 0x86u, 8u, false, 0.612940f, 0.005678f},
    {0x806AD9C74EECC97FULL, 64u, 32u, 14u, 0x01FEC480u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.006825f, 0.504893f},
    {0x82664473C3BA4CCDULL, 128u, 128u, 14u, 0x00EA1300u, 174776u, 512u, 512u, 0x86u, 10u, true, 0.163360f, 0.264695f},
    {0x843D3C86A532873AULL, 64u, 64u, 14u, 0x00F69B00u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.636282f, 0.005094f},
    {0x847237544EBF4A00ULL, 128u, 128u, 14u, 0x00F26D00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.007305f, 0.335136f},
    {0x859CBD61C2EBB402ULL, 128u, 128u, 14u, 0x00FC7300u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000621f, 0.518352f},
    {0x85E4868098E4660AULL, 128u, 64u, 14u, 0x018EC480u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001062f, 0.558885f},
    {0x86E629D85F6CE9C9ULL, 128u, 128u, 14u, 0x01DAF180u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001826f, 0.607588f},
    {0x872D44E439C82E59ULL, 64u, 64u, 14u, 0x00FB9D00u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.630605f, 0.008970f},
    {0x8939A192672B0AE0ULL, 128u, 128u, 14u, 0x00B7D000u, 174776u, 512u, 512u, 0x86u, 10u, false, 0.550039f, 0.022849f},
    {0x8BFC7B36A885DD28ULL, 128u, 128u, 14u, 0x00DD2B00u, 174776u, 512u, 512u, 0x86u, 10u, true, 0.168783f, 0.080723f},
    {0x8CEF0C92E6A167CBULL, 128u, 128u, 14u, 0x00C52800u, 174776u, 512u, 512u, 0x86u, 10u, false, 0.367087f, 0.029505f},
    {0x8D9D917452EAE513ULL, 128u, 128u, 14u, 0x00F19700u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.010495f, 0.238586f},
    {0x8FB6EC2D46C2752FULL, 128u, 128u, 14u, 0x02290D80u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001207f, 0.458185f},
    {0x91422365F0033D31ULL, 128u, 64u, 14u, 0x01D9B080u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.008958f, 0.095096f},
    {0x919740EAC4A70CEBULL, 128u, 128u, 14u, 0x01FDE380u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001327f, 0.690553f},
    {0x91F225E21E956746ULL, 128u, 16u, 14u, 0x00CD2B00u, 5512u, 256u, 32u, 0x86u, 9u, true, 0.013953f, 0.649333f},
    {0x9264753CD2D955ECULL, 64u, 64u, 14u, 0x04BA4B00u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.005062f, 0.573714f},
    {0x92E6AAD676264E81ULL, 16u, 32u, 14u, 0x010E7B00u, 1384u, 32u, 64u, 0x86u, 7u, true, 0.055916f, 0.745946f},
    {0x95D7BB679413D393ULL, 128u, 128u, 14u, 0x00F34300u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.007893f, 0.355615f},
    {0x98220BB17069028CULL, 64u, 64u, 14u, 0x03217B00u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.002203f, 0.601815f},
    {0x9D8B0F48B7DFE082ULL, 128u, 64u, 14u, 0x010DDA00u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.002879f, 0.578766f},
    {0x9EACB39A93EE64CDULL, 256u, 256u, 14u, 0x00C1D200u, 174776u, 512u, 512u, 0x86u, 10u, false, 0.423426f, 0.162925f},
    {0x9EED8C4FDDD4F67CULL, 128u, 64u, 14u, 0x018F9B80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.002150f, 0.668607f},
    {0xA01FBE8F1A5A8EA4ULL, 128u, 64u, 14u, 0x022F5180u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001997f, 0.475972f},
    {0xA33F128BB1888E81ULL, 128u, 64u, 14u, 0x01DF6B80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.002036f, 0.633943f},
    {0xA37CDA711879724EULL, 128u, 128u, 14u, 0x01D5B680u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001005f, 0.625203f},
    {0xA385D61377C71E4CULL, 128u, 128u, 14u, 0x02660D80u, 87408u, 256u, 256u, 0x88u, 9u, true, 0.003618f, 0.679234f},
    {0xA4FF14481BC6CD2AULL, 128u, 128u, 14u, 0x01E0E280u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.010892f, 0.689907f},
    {0xA760446377FE9EB9ULL, 128u, 64u, 14u, 0x0264CC80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000761f, 0.135290f},
    {0xAA00B0DDC174C443ULL, 128u, 128u, 14u, 0x00CDD800u, 174776u, 512u, 512u, 0x86u, 10u, true, 0.052650f, 0.508036f},
    {0xAAB589140AB85F85ULL, 128u, 128u, 14u, 0x00FB9D00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000649f, 0.525933f},
    {0xAB078CF9A9A942D6ULL, 64u, 64u, 14u, 0x04D67500u, 10936u, 128u, 128u, 0x86u, 8u, false, 0.612482f, 0.002496f},
    {0xAC5A0C92D5EBCAC9ULL, 64u, 64u, 14u, 0x01D72D80u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.002370f, 0.343747f},
    {0xAD74290555676934ULL, 128u, 64u, 14u, 0x00FF6000u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001490f, 0.459762f},
    {0xAE887EFD6AE3BD48ULL, 128u, 128u, 14u, 0x00E68500u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000542f, 0.463078f},
    {0xB03D24F059322385ULL, 64u, 32u, 14u, 0x01399C00u, 10960u, 128u, 64u, 0x88u, 8u, true, 0.008089f, 0.345952f},
    {0xB0DBD249F3D48C55ULL, 64u, 64u, 14u, 0x01FD7780u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.002382f, 0.421748f},
    {0xB120AF0A985B8F2DULL, 32u, 128u, 14u, 0x00D12E00u, 10952u, 64u, 256u, 0x86u, 9u, true, 0.002517f, 0.519748f},
    {0xB1377B1F51A54E1DULL, 128u, 64u, 14u, 0x0188B680u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.002277f, 0.313563f},
    {0xB29BB929923A4643ULL, 128u, 128u, 14u, 0x00FE1F00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000512f, 0.412335f},
    {0xB4B6E83D862EBF4DULL, 128u, 64u, 14u, 0x0107A200u, 87400u, 512u, 256u, 0x86u, 10u, false, 0.250395f, 0.058081f},
    {0xB7BC90E0F509BC20ULL, 128u, 64u, 14u, 0x01D90F80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.005913f, 0.561509f},
    {0xB826CF63D2C43BE9ULL, 64u, 64u, 14u, 0x00E68500u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.454011f, 0.124150f},
    {0xB96F2152225D9ED3ULL, 64u, 16u, 14u, 0x00CD1500u, 2760u, 128u, 32u, 0x86u, 8u, true, 0.030906f, 0.500441f},
    {0xBA634D40D319BE8AULL, 128u, 64u, 14u, 0x04B9E000u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.004513f, 0.486216f},
    {0xBC0A0177D4A1E2D6ULL, 256u, 128u, 14u, 0x00DB8000u, 87400u, 512u, 256u, 0x86u, 10u, false, 0.289296f, 0.384933f},
    {0xBC3C08FD77FE8C44ULL, 128u, 16u, 14u, 0x01E1B880u, 5512u, 256u, 32u, 0x86u, 9u, true, 0.007729f, 0.447697f},
    {0xBD6BD12C301E98ECULL, 64u, 64u, 14u, 0x01D6F780u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.003559f, 0.361398f},
    {0xBDBE8438596A91F0ULL, 128u, 64u, 14u, 0x00F4EF00u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001455f, 0.585009f},
    {0xBDDF67B486A28A87ULL, 128u, 64u, 14u, 0x022AB980u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001600f, 0.400822f},
    {0xBE214DF3F9C86639ULL, 128u, 64u, 14u, 0x00E89C00u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.003354f, 0.286380f},
    {0xBE9E866EBBA432B7ULL, 64u, 64u, 14u, 0x010E4500u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.004997f, 0.460469f},
    {0xBF61530CB1B9A810ULL, 128u, 64u, 14u, 0x01D68C80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.009502f, 0.532928f},
    {0xC4CD4924133511B5ULL, 64u, 32u, 14u, 0x04BF8800u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.015196f, 0.300783f},
    {0xC50C1D7C1BFDB15EULL, 128u, 128u, 14u, 0x00E7C600u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001050f, 0.554636f},
    {0xC8206B9FCC59F61CULL, 128u, 128u, 14u, 0x00D33000u, 174776u, 512u, 512u, 0x86u, 10u, true, 0.144327f, 0.178955f},
    {0xCBE1A2E7A2FD455FULL, 64u, 64u, 14u, 0x00DB1400u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.001988f, 0.577854f},
    {0xCCAF9F2DE7717F25ULL, 32u, 32u, 14u, 0x013A3D00u, 5488u, 64u, 64u, 0x88u, 7u, false, 0.800000f, 0.000000f},
    {0xD09D2E926698CAE8ULL, 128u, 64u, 14u, 0x00F55A00u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001080f, 0.642856f},
    {0xD0E77F22CB7DE973ULL, 64u, 64u, 14u, 0x03490600u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.009568f, 0.592390f},
    {0xD12EC7FF0CDF114FULL, 128u, 64u, 14u, 0x03005400u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001098f, 0.464532f},
    {0xD3C6138E015C0B2EULL, 128u, 128u, 14u, 0x02268B80u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001028f, 0.608548f},
    {0xD446B24115F80C48ULL, 128u, 128u, 14u, 0x02570080u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001685f, 0.480271f},
    {0xD4A2DDB58E514055ULL, 128u, 64u, 14u, 0x01D8A480u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000750f, 0.521895f},
    {0xD5F194413F9DB4DDULL, 64u, 32u, 14u, 0x0234F680u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.006704f, 0.592098f},
    {0xD708A721BED3BA64ULL, 128u, 64u, 14u, 0x01DFD680u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001905f, 0.647253f},
    {0xD9015DF273F25D35ULL, 128u, 64u, 14u, 0x03E63180u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001550f, 0.443132f},
    {0xDA5A1FBAD7033F1BULL, 128u, 128u, 14u, 0x00ED6900u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001289f, 0.643623f},
    {0xDBBC2ADA0C69F985ULL, 128u, 64u, 14u, 0x00E75B00u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.004159f, 0.282377f},
    {0xDCD5B34BD8A85B50ULL, 128u, 128u, 14u, 0x01E37C80u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.002100f, 0.352248f},
    {0xDCDEEDC61AA2FFECULL, 256u, 16u, 14u, 0x01E24F80u, 11016u, 512u, 32u, 0x86u, 10u, true, 0.014043f, 9.000000f},
    {0xDCF694387F12F52FULL, 128u, 64u, 14u, 0x03E69C80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001690f, 0.443898f},
    {0xDCF86A72337CE253ULL, 128u, 64u, 14u, 0x00CBD400u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001217f, 0.529248f},
    {0xDDD9DF6320FC25C0ULL, 128u, 128u, 14u, 0x022FBC80u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.002289f, 0.366748f},
    {0xDE3308374654618EULL, 128u, 64u, 14u, 0x02334A80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000996f, 0.451535f},
    {0xE32D73C6D094E9FDULL, 128u, 64u, 14u, 0x00FEF500u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001418f, 0.540122f},
    {0xE51F4ACE356A0A9DULL, 128u, 64u, 14u, 0x01DE5F80u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.002763f, 0.656511f},
    {0xE58EAB5283B943D0ULL, 128u, 64u, 14u, 0x0229E380u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001535f, 0.479833f},
    {0xE652AD0665659917ULL, 128u, 64u, 14u, 0x04D49300u, 21864u, 256u, 128u, 0x86u, 9u, false, 0.509025f, 0.075500f},
    {0xE6844E32B0190EE5ULL, 64u, 64u, 14u, 0x04D71600u, 10936u, 128u, 128u, 0x86u, 8u, false, 0.401315f, 0.193043f},
    {0xE6C8F3994B6194FEULL, 64u, 64u, 14u, 0x03E59080u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.005328f, 0.678925f},
    {0xE875CD43B64AC8F2ULL, 32u, 16u, 14u, 0x01FD6680u, 1384u, 64u, 32u, 0x86u, 7u, true, 0.028552f, 0.168270f},
    {0xE9BB54269AE630CBULL, 128u, 128u, 14u, 0x00EF1500u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000570f, 0.561427f},
    {0xEEF8638DDFBE882DULL, 128u, 128u, 14u, 0x00F0C100u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.013938f, 0.235789f},
    {0xEF51C3C01B7377D3ULL, 128u, 64u, 14u, 0x034BBE00u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000642f, 0.097100f},
    {0xEF902D0FCBB0AF95ULL, 64u, 64u, 14u, 0x01E4F380u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.008996f, 0.552337f},
    {0xEFAFD8B6CAAC053CULL, 128u, 128u, 14u, 0x02FB9100u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001998f, 0.682838f},
    {0xF01B7B377234E0D0ULL, 64u, 64u, 14u, 0x0258AD80u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.003836f, 0.383718f},
    {0xF26F80880E2DA924ULL, 64u, 64u, 14u, 0x01E52980u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.005412f, 0.554279f},
    {0xF364F9E7EA263A31ULL, 128u, 128u, 14u, 0x01DAF180u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.694160f, 0.007345f},
    {0xF3F16ACEE37551E5ULL, 64u, 64u, 14u, 0x010A4E00u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.003184f, 0.598547f},
    {0xF423BAC624E73882ULL, 128u, 128u, 14u, 0x018A6280u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001187f, 0.438924f},
    {0xF4EDB037F3FF974CULL, 64u, 32u, 14u, 0x00D73D00u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.013982f, 0.489935f},
    {0xF5198347E610A819ULL, 128u, 64u, 14u, 0x00F48400u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.001147f, 0.545736f},
    {0xF73AEE86EA4824AAULL, 128u, 128u, 14u, 0x036A2F00u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.001421f, 0.579053f},
    {0xF7E4944B56B5E5A2ULL, 64u, 64u, 14u, 0x02283780u, 43704u, 256u, 256u, 0x86u, 9u, false, 0.494144f, 0.039587f},
    {0xF91C918467A04272ULL, 128u, 64u, 14u, 0x02514580u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000837f, 0.358225f},
    {0xFA0CC0ABF326DD25ULL, 64u, 32u, 14u, 0x00D68600u, 5480u, 128u, 64u, 0x86u, 8u, true, 0.006757f, 0.504490f},
    {0xFB5F9E02778943E4ULL, 64u, 64u, 14u, 0x022B8F80u, 10936u, 128u, 128u, 0x86u, 8u, true, 0.002823f, 0.579822f},
    {0xFBB5F6F8140CF2D3ULL, 16u, 128u, 14u, 0x00000000u, 0u, 0u, 0u, 0x00u, 0u, false, 999.000000f, 0.000000f},
    {0xFD44777929DD1FBAULL, 128u, 64u, 14u, 0x00E9A800u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000997f, 0.000049f},
    {0xFD84AC9DA82288EFULL, 128u, 64u, 14u, 0x01B4C380u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.000828f, 0.086707f},
    {0xFDCC9FD6327BE6BCULL, 128u, 128u, 14u, 0x00FD4900u, 43704u, 256u, 256u, 0x86u, 9u, true, 0.000543f, 0.399196f},
    {0xFEBECA0D007BE91DULL, 128u, 64u, 14u, 0x01E1E480u, 21864u, 256u, 128u, 0x86u, 9u, true, 0.004273f, 0.475669f},
}};

inline u64 FNV1a64(const u8* data, std::size_t size)
{
  u64 hash = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < size; ++i)
  {
    hash ^= data[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

inline bool EnvEnabled(const char* name, bool fallback)
{
  const char* value = std::getenv(name);
  if (!value || !*value)
    return fallback;

  std::string text(value);
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (text == "0" || text == "false" || text == "off" || text == "no")
    return false;
  if (text == "1" || text == "true" || text == "on" || text == "yes")
    return true;
  return fallback;
}

inline bool Enabled()
{
  // Environment is process-start configuration. getenv() used to run for
  // every texture lookup and showed up directly in perf.
  static const bool enabled = EnvEnabled("MOH_PS3_WORLD_CPT", true);
  return enabled;
}

inline bool AllowAll()
{
  static const bool enabled = EnvEnabled("MOH_PS3_WORLD_CPT_ALL", false);
  return enabled;
}

inline bool TraceEnabled()
{
  static const bool enabled =
      EnvEnabled("MOH_PS3_RSX_TRACE", false) ||
      EnvEnabled("MOH_PS3_WORLD_CPT_TRACE", false);
  return enabled;
}

inline bool IsLevel11()
{
  return MOHFrontline::NativeAssets::GetCurrentLevel() == "1_1";
}

struct LookupCacheLine
{
  const u8* data = nullptr;
  u32 address = 0;
  u32 size = 0;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  u64 sample = 0;
  u64 hash = 0;
  const Entry* entry = nullptr;
  bool valid = false;
};

// Video-thread-local and allocation-free. Positive and negative lookups are
// cached because both are hot in TextureCacheBase::LoadImpl().
inline thread_local std::array<LookupCacheLine, 512> lookup_cache{};

inline u64 SampleSignature(const u8* data, std::size_t size)
{
  // Guest/TMEM storage may reuse the same pointer/address for another texture.
  // Sample four regions (up to 64 bytes total) before trusting a cached full
  // hash. A changed sample falls through to the exact whole-payload FNV1a64.
  u64 hash = 0x9E3779B97F4A7C15ULL ^ static_cast<u64>(size);
  if (!data || !size)
    return hash;

  const std::size_t third = size / 3;
  const std::array<std::size_t, 4> offsets = {
      0,
      third,
      third * 2,
      size > 16 ? size - 16 : 0,
  };

  for (const std::size_t offset : offsets)
  {
    const std::size_t count = std::min<std::size_t>(16, size - offset);
    for (std::size_t i = 0; i < count; ++i)
    {
      hash ^= data[offset + i];
      hash *= 0x100000001b3ULL;
    }
  }

  return hash;
}

inline std::size_t LookupCacheIndex(const TextureInfo& info, const u8* data, u32 size)
{
  u64 key =
      (static_cast<u64>(info.GetRawAddress()) << 32) ^
      static_cast<u64>(reinterpret_cast<std::uintptr_t>(data) >> 4) ^
      (static_cast<u64>(size) * 0x9E3779B185EBCA87ULL);
  key ^= key >> 33;
  key *= 0xC2B2AE3D27D4EB4FULL;
  key ^= key >> 29;
  return static_cast<std::size_t>(key) & (lookup_cache.size() - 1u);
}

inline const Entry* FindEntry(u64 hash, u32 width, u32 height, u32 format)
{
  // kLevel11 is generated in ascending gc_fnv1a order.
  const auto it = std::lower_bound(
      kLevel11.begin(), kLevel11.end(), hash,
      [](const Entry& entry, u64 wanted) { return entry.gc_fnv1a < wanted; });

  if (it != kLevel11.end() &&
      it->gc_fnv1a == hash &&
      it->gc_width == width &&
      it->gc_height == height &&
      it->gc_format == format)
  {
    return &*it;
  }

  return nullptr;
}

inline const Entry* Lookup(const TextureInfo& info, u64* out_hash = nullptr)
{
  // Cheap rejects first: don't touch level/env state for textures that cannot
  // be CPT world identities.
  if (info.GetTextureFormat() != TextureFormat::CMPR ||
      !info.GetData() || !info.GetTextureSize() ||
      !Enabled() || !IsLevel11())
  {
    return nullptr;
  }

  const bool trace = TraceEnabled();
  if (trace)
  {
    static bool active_logged = false;
    if (!active_logged)
    {
      active_logged = true;
      std::fprintf(stderr,
                   "[moh-ps3-world] CPT matcher ACTIVE: level=1_1 "
                   "TMEM textures accepted; lookup cache ON\n");
    }
  }

  const u8* const data = info.GetData();
  const u32 size = info.GetTextureSize();
  const u32 width = info.GetRawWidth();
  const u32 height = info.GetRawHeight();
  const u32 format = static_cast<u32>(info.GetTextureFormat());
  const u32 address = info.GetRawAddress();

  const u64 sample = SampleSignature(data, size);
  LookupCacheLine& line = lookup_cache[LookupCacheIndex(info, data, size)];

  if (line.valid &&
      line.data == data &&
      line.address == address &&
      line.size == size &&
      line.width == width &&
      line.height == height &&
      line.format == format &&
      line.sample == sample)
  {
    if (out_hash)
      *out_hash = line.hash;
    return line.entry;
  }

  // The expensive exact full-payload hash now only runs on a cache miss or
  // when the sampled payload indicates that reused guest storage changed.
  const u64 hash = FNV1a64(data, size);
  const Entry* const entry = FindEntry(hash, width, height, format);

  line.data = data;
  line.address = address;
  line.size = size;
  line.width = width;
  line.height = height;
  line.format = format;
  line.sample = sample;
  line.hash = hash;
  line.entry = entry;
  line.valid = true;

  if (out_hash)
    *out_hash = hash;

  if (!entry && trace)
  {
    static unsigned miss_logs = 0;
    if (miss_logs++ < 96)
    {
      std::fprintf(
          stderr,
          "[moh-ps3-world] CPT MISS: tmem=%d stage=%u addr=%08X "
          "GC=%ux%u fmt=%u size=%u hash=%016llX\n",
          info.IsFromTmem() ? 1 : 0,
          info.GetStage(),
          address,
          width, height, format,
          size,
          static_cast<unsigned long long>(hash));
    }
  }

  return entry;
}

inline void WriteBE32(std::vector<u8>* out, std::size_t offset, u32 value)
{
  (*out)[offset + 0] = static_cast<u8>(value >> 24);
  (*out)[offset + 1] = static_cast<u8>(value >> 16);
  (*out)[offset + 2] = static_cast<u8>(value >> 8);
  (*out)[offset + 3] = static_cast<u8>(value);
}

inline std::vector<u8> MakeGTF(const Entry& entry,
                               std::span<const u8> payload)
{
  if (!entry.ps3_size || payload.size() != entry.ps3_size ||
      !entry.ps3_width || !entry.ps3_height || !entry.ps3_mips ||
      (entry.ps3_format != 0x86u &&
       entry.ps3_format != 0x87u &&
       entry.ps3_format != 0x88u))
  {
    return {};
  }

  std::array<u8, 24> descriptor{};
  descriptor[0] = static_cast<u8>(entry.ps3_format);
  descriptor[1] = static_cast<u8>(entry.ps3_mips);
  descriptor[2] = 2;
  descriptor[3] = 0;
  descriptor[4] = 0;
  descriptor[5] = 0;
  descriptor[6] = 0xAA;
  descriptor[7] = 0xE4;
  descriptor[8] = static_cast<u8>(entry.ps3_width >> 8);
  descriptor[9] = static_cast<u8>(entry.ps3_width);
  descriptor[10] = static_cast<u8>(entry.ps3_height >> 8);
  descriptor[11] = static_cast<u8>(entry.ps3_height);
  descriptor[12] = 0;
  descriptor[13] = 1;

  std::vector<u8> gtf(48 + payload.size());
  WriteBE32(&gtf, 0, 0x02010100u);
  WriteBE32(&gtf, 4, entry.ps3_size);
  WriteBE32(&gtf, 8, 1u);
  WriteBE32(&gtf, 16, 20u);
  WriteBE32(&gtf, 20, entry.ps3_size);
  std::copy(descriptor.begin(), descriptor.end(), gtf.begin() + 24);
  std::copy(payload.begin(), payload.end(), gtf.begin() + 48);
  return gtf;
}

inline std::shared_ptr<VideoCommon::CustomTextureData>
BuildCustomTexture(const std::vector<PS3TextureDecoder::Level>& levels)
{
  if (levels.empty())
    return nullptr;

  auto result = std::make_shared<VideoCommon::CustomTextureData>();
  result->m_slices.emplace_back();

  for (const auto& source : levels)
  {
    if (!source.width || !source.height ||
        source.rgba.size() <
            std::size_t(source.width) * source.height * 4u)
    {
      return nullptr;
    }

    VideoCommon::CustomTextureData::ArraySlice::Level level;
    level.width = source.width;
    level.height = source.height;
    level.row_length = source.width;
    level.data.reset(source.rgba.size());
    std::copy(source.rgba.begin(), source.rgba.end(), level.data.begin());
    result->m_slices[0].m_levels.push_back(std::move(level));
  }

  return result;
}

struct Cache
{
  std::uint64_t generation = ~std::uint64_t(0);
  std::unordered_map<u64, std::shared_ptr<VideoCommon::CustomTextureData>> decoded;
  std::unordered_set<u64> failed;
  std::unordered_set<u64> logged;
};

inline std::mutex cache_mutex;
inline Cache cache;

inline void ResetCacheForGeneration()
{
  const auto generation = PS3RemasterAssets::GetIndexGeneration();
  if (cache.generation == generation)
    return;

  cache = {};
  cache.generation = generation;
}

inline std::shared_ptr<VideoCommon::CustomTextureData>
Decode(const Entry& entry, u64 gc_hash)
{
  std::scoped_lock lock(cache_mutex);
  ResetCacheForGeneration();

  if (const auto it = cache.decoded.find(gc_hash);
      it != cache.decoded.end())
  {
    return it->second;
  }

  if (cache.failed.contains(gc_hash))
    return nullptr;

  const auto rsx = MOHFrontline::NativeAssets::Resolve(
      "data/1/1_1/rsx.viv",
      MOHFrontline::NativeAssets::Domain::Container);

  if (!rsx || !rsx.asset ||
      entry.ps3_offset > rsx.asset->size ||
      entry.ps3_size > rsx.asset->size - entry.ps3_offset)
  {
    cache.failed.insert(gc_hash);
    return nullptr;
  }

  const auto payload = PS3RemasterAssets::ReadRange(
      *rsx.asset, entry.ps3_offset, entry.ps3_size);

  if (payload.size() != entry.ps3_size)
  {
    cache.failed.insert(gc_hash);
    return nullptr;
  }

  const auto gtf = MakeGTF(entry, payload);
  if (gtf.empty())
  {
    cache.failed.insert(gc_hash);
    return nullptr;
  }

  std::shared_ptr<VideoCommon::CustomTextureData> decoded;
  std::vector<PS3TextureDecoder::CompressedLevel> blocks;
  if (g_backend_info.bSupportsST3CTextures &&
      PS3TextureDecoder::DecodeCompressed(gtf, &blocks) && !blocks.empty())
  {
    decoded = std::make_shared<VideoCommon::CustomTextureData>();
    decoded->m_slices.emplace_back();
    for (const auto& source : blocks)
    {
      VideoCommon::CustomTextureData::ArraySlice::Level mip;
      mip.width = source.width;
      mip.height = source.height;
      mip.row_length = (source.width + 3u) & ~3u;
      mip.format = source.format == PS3TextureDecoder::BlockFormat::BC1 ? AbstractTextureFormat::DXT1 :
                   source.format == PS3TextureDecoder::BlockFormat::BC2 ? AbstractTextureFormat::DXT3 :
                                                                        AbstractTextureFormat::DXT5;
      mip.data.reset(source.blocks.size());
      std::copy(source.blocks.begin(), source.blocks.end(), mip.data.begin());
      decoded->m_slices[0].m_levels.push_back(std::move(mip));
    }
  }
  else
  {
    std::vector<PS3TextureDecoder::Level> levels;
    if (PS3TextureDecoder::Decode(gtf, &levels))
      decoded = BuildCustomTexture(levels);
  }
  if (!decoded)
  {
    cache.failed.insert(gc_hash);
    return nullptr;
  }

  cache.decoded.emplace(gc_hash, decoded);

  if (TraceEnabled() && cache.logged.insert(gc_hash).second)
  {
    std::fprintf(
        stderr,
        "[moh-ps3-world] CPT FINGERPRINT MATCH: level=1_1 "
        "GC=%ux%u fmt=%u hash=%016llX -> "
        "rsx.viv+0x%08X size=%u PS3=%ux%u fmt=0x%02X "
        "mips=%u score=%.4f margin=%.4f safe=%d\n",
        entry.gc_width,
        entry.gc_height,
        entry.gc_format,
        static_cast<unsigned long long>(gc_hash),
        entry.ps3_offset,
        entry.ps3_size,
        entry.ps3_width,
        entry.ps3_height,
        entry.ps3_format,
        entry.ps3_mips,
        entry.score,
        entry.margin,
        entry.safe ? 1 : 0);
  }

  return decoded;
}
}  // namespace detail

inline u64 ReplacementKey(const TextureInfo& info)
{
  u64 hash = 0;
  const detail::Entry* entry = detail::Lookup(info, &hash);
  if (!entry || (!entry->safe && !detail::AllowAll()) || !entry->ps3_size)
    return 0;

  constexpr u64 kWorldTag = 0x4350545053330000ULL;
  u64 key = kWorldTag ^ hash;
  key ^= static_cast<u64>(entry->ps3_offset) * 0x9E3779B185EBCA87ULL;
  key ^= static_cast<u64>(entry->ps3_width) << 32;
  key ^= static_cast<u64>(entry->ps3_height) << 16;
  key ^= static_cast<u64>(entry->ps3_format) << 8;
  key ^= static_cast<u64>(entry->ps3_mips);
  return key ? key : 1;
}

inline bool IsKnownWorldTexture(const TextureInfo& info)
{
  return detail::Lookup(info) != nullptr;
}

inline std::shared_ptr<VideoCommon::CustomTextureData>
Find(const TextureInfo& info)
{
  u64 hash = 0;
  const detail::Entry* entry = detail::Lookup(info, &hash);
  if (!entry)
    return nullptr;

  // One CPT texture in the supplied 1_1 set had no candidate at all.
  if (!entry->ps3_offset || !entry->ps3_size)
    return nullptr;

  // Default mode only applies mappings with strong image evidence. The rest
  // are still classified as CPT/world and are kept on the original GC path.
  if (!entry->safe && !detail::AllowAll())
    return nullptr;

  return detail::Decode(*entry, hash);
}

inline void NotifyUploaded(const TextureInfo& info)
{
  if (!detail::TraceEnabled())
    return;

  u64 hash = 0;
  const detail::Entry* entry = detail::Lookup(info, &hash);
  if (!entry || !entry->safe)
    return;

  static std::mutex upload_mutex;
  static std::unordered_set<u64> uploaded;
  const u64 key = ReplacementKey(info);
  {
    std::scoped_lock lock(upload_mutex);
    if (!uploaded.insert(key).second)
      return;
  }

  std::fprintf(stderr,
               "[moh-ps3-world] UPLOAD READY: level=1_1 "
               "GC=%ux%u fmt=%u hash=%016llX -> "
               "PS3=%ux%u fmt=0x%02X mips=%u rsx.viv+0x%08X\n",
               info.GetRawWidth(), info.GetRawHeight(),
               static_cast<unsigned>(info.GetTextureFormat()),
               static_cast<unsigned long long>(hash),
               entry->ps3_width, entry->ps3_height,
               entry->ps3_format, entry->ps3_mips, entry->ps3_offset);
}

}  // namespace PS3WorldCPT
