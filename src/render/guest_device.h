// render/guest_device.h

#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/types.h>

namespace reodyssey::render {

struct GuestSamplerState {
  rex::be<uint32_t> data[6];
};

struct GuestDevice {
  rex::be<uint64_t> dirtyFlags[8];

  rex::be<uint32_t> setRenderStateFunctions[0x65];
  uint32_t setSamplerStateFunctions[0x14];

  uint8_t padding224[0x25C];

  GuestSamplerState samplerStates[0x20];

  // Raw (big-endian) shader constant registers; byte-swapped at upload time.
  uint32_t vertexShaderFloatConstants[0x400];  // device + 0x780
  uint32_t pixelShaderFloatConstants[0x400];   // device + 0x1780

  rex::be<uint32_t> vertexShaderBoolConstants[0x4];
  rex::be<uint32_t> pixelShaderBoolConstants[0x4];

  rex::be<uint32_t> vertexShaderIntConstants[0x10];  // device + 0x27A0
  rex::be<uint32_t> pixelShaderIntConstants[0x10];   // device + 0x27E0

  uint8_t padding2820[0x604];
  rex::be<uint32_t> vertexDeclaration;
  uint8_t padding2E28[0x340];
  struct {
    rex::be<float> x;
    rex::be<float> y;
    rex::be<float> width;
    rex::be<float> height;
    rex::be<float> minZ;
    rex::be<float> maxZ;
  } viewport;
  uint8_t padding3180[0x2C80];
};

static_assert(sizeof(GuestDevice) == 0x5E00);
static_assert(offsetof(GuestDevice, vertexShaderBoolConstants) == 0x2780);
static_assert(offsetof(GuestDevice, pixelShaderBoolConstants) == 0x2790);
static_assert(offsetof(GuestDevice, vertexShaderIntConstants) == 0x27A0);
static_assert(offsetof(GuestDevice, pixelShaderIntConstants) == 0x27E0);
static_assert(offsetof(GuestDevice, vertexDeclaration) == 0x2E24);

struct GuestViewport {
  rex::be<uint32_t> x;
  rex::be<uint32_t> y;
  rex::be<uint32_t> width;
  rex::be<uint32_t> height;
  rex::be<float> minZ;
  rex::be<float> maxZ;
};

struct GuestRect {
  rex::be<int32_t> left;
  rex::be<int32_t> top;
  rex::be<int32_t> right;
  rex::be<int32_t> bottom;
};

struct GuestPoint {
  rex::be<int32_t> x;
  rex::be<int32_t> y;
};

enum GuestRenderState : uint32_t {
  D3DRS_ZENABLE = 40,
  D3DRS_ZFUNC = 44,
  D3DRS_ZWRITEENABLE = 48,
  D3DRS_CULLMODE = 56,
  D3DRS_ALPHABLENDENABLE = 60,
  D3DRS_SRCBLEND = 72,
  D3DRS_DESTBLEND = 76,
  D3DRS_BLENDOP = 80,
  D3DRS_SRCBLENDALPHA = 84,
  D3DRS_DESTBLENDALPHA = 88,
  D3DRS_BLENDOPALPHA = 92,
  D3DRS_ALPHATESTENABLE = 96,
  D3DRS_ALPHAREF = 100,
  D3DRS_SCISSORTESTENABLE = 200,
  D3DRS_SLOPESCALEDEPTHBIAS = 204,
  D3DRS_DEPTHBIAS = 208,
  D3DRS_COLORWRITEENABLE = 212,
};

enum GuestCullMode : uint32_t {
  D3DCULL_NONE = 0,
  D3DCULL_CW = 2,
  D3DCULL_NONE_2 = 4,
  D3DCULL_CCW = 6,
};

enum GuestBlendMode : uint32_t {
  D3DBLEND_ZERO = 0,
  D3DBLEND_ONE = 1,
  D3DBLEND_SRCCOLOR = 4,
  D3DBLEND_INVSRCCOLOR = 5,
  D3DBLEND_SRCALPHA = 6,
  D3DBLEND_INVSRCALPHA = 7,
  D3DBLEND_DESTCOLOR = 8,
  D3DBLEND_INVDESTCOLOR = 9,
  D3DBLEND_DESTALPHA = 10,
  D3DBLEND_INVDESTALPHA = 11,
};

enum GuestBlendOp : uint32_t {
  D3DBLENDOP_ADD = 0,
  D3DBLENDOP_SUBTRACT = 1,
  D3DBLENDOP_MIN = 2,
  D3DBLENDOP_MAX = 3,
  D3DBLENDOP_REVSUBTRACT = 4,
};

enum GuestCmpFunc : uint32_t {
  D3DCMP_NEVER = 0,
  D3DCMP_LESS = 1,
  D3DCMP_EQUAL = 2,
  D3DCMP_LESSEQUAL = 3,
  D3DCMP_GREATER = 4,
  D3DCMP_NOTEQUAL = 5,
  D3DCMP_GREATEREQUAL = 6,
  D3DCMP_ALWAYS = 7,
};

enum GuestPrimitiveType : uint32_t {
  D3DPT_POINTLIST = 1,
  D3DPT_LINELIST = 2,
  D3DPT_LINESTRIP = 3,
  D3DPT_TRIANGLELIST = 4,
  D3DPT_TRIANGLEFAN = 5,
  D3DPT_TRIANGLESTRIP = 6,
  D3DPT_QUADLIST = 13,
};

enum GuestDeclType : uint32_t {
  D3DDECLTYPE_FLOAT1 = 0x2C83A4,
  D3DDECLTYPE_FLOAT2 = 0x2C23A5,
  D3DDECLTYPE_FLOAT3 = 0x2A23B9,
  D3DDECLTYPE_FLOAT4 = 0x1A23A6,
  D3DDECLTYPE_D3DCOLOR = 0x182886,
  D3DDECLTYPE_UBYTE4 = 0x1A2286,
  D3DDECLTYPE_UBYTE4_2 = 0x1A2386,
  D3DDECLTYPE_SHORT2 = 0x2C2359,
  D3DDECLTYPE_SHORT4 = 0x1A235A,
  D3DDECLTYPE_UBYTE4N = 0x1A2086,
  D3DDECLTYPE_UBYTE4N_2 = 0x1A2186,
  D3DDECLTYPE_SHORT2N = 0x2C2159,
  D3DDECLTYPE_SHORT4N = 0x1A215A,
  D3DDECLTYPE_USHORT2N = 0x2C2059,
  D3DDECLTYPE_USHORT4N = 0x1A205A,
  D3DDECLTYPE_UINT1 = 0x2C82A1,
  D3DDECLTYPE_UDEC3 = 0x2A2287,
  D3DDECLTYPE_DEC3N = 0x2A2187,
  D3DDECLTYPE_DEC3N_2 = 0x2A2190,
  D3DDECLTYPE_DEC3N_3 = 0x2A2390,
  D3DDECLTYPE_FLOAT16_2 = 0x2C235F,
  D3DDECLTYPE_FLOAT16_4 = 0x1A2360,
  D3DDECLTYPE_UNUSED = 0xFFFFFFFF,
};

enum GuestDeclUsage : uint32_t {
  D3DDECLUSAGE_POSITION = 0,
  D3DDECLUSAGE_BLENDWEIGHT = 1,
  D3DDECLUSAGE_BLENDINDICES = 2,
  D3DDECLUSAGE_NORMAL = 3,
  D3DDECLUSAGE_PSIZE = 4,
  D3DDECLUSAGE_TEXCOORD = 5,
  D3DDECLUSAGE_TANGENT = 6,
  D3DDECLUSAGE_BINORMAL = 7,
  D3DDECLUSAGE_TESSFACTOR = 8,
  D3DDECLUSAGE_POSITIONT = 9,
  D3DDECLUSAGE_COLOR = 10,
  D3DDECLUSAGE_FOG = 11,
  D3DDECLUSAGE_DEPTH = 12,
  D3DDECLUSAGE_SAMPLE = 13,
};

enum GuestTextureFilterType : uint32_t {
  D3DTEXF_POINT = 0,
  D3DTEXF_LINEAR = 1,
  D3DTEXF_NONE = 2,
};

enum GuestTextureAddress : uint32_t {
  D3DTADDRESS_WRAP = 0,
  D3DTADDRESS_MIRROR = 1,
  D3DTADDRESS_CLAMP = 2,
  D3DTADDRESS_MIRRORONCE = 3,
  D3DTADDRESS_BORDER = 6,
};

constexpr uint32_t D3DCLEAR_TARGET = 0x1;
constexpr uint32_t D3DCLEAR_ZBUFFER = 0x10;
constexpr uint32_t D3DCLEAR_STENCIL = 0x20;

}  // namespace reodyssey::render
