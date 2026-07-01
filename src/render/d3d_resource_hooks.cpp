// render/d3d_resource_hooks.cpp

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <rex/hash.h> // pulls in xxhash (XXH3_64bits)
#include <rex/logging.h>
#include "generated/shader_cache.h"
#include "render/guest_heap.h"
#include "render/guest_resources.h"
#include "render/render_internal.h"

using namespace plume;
using namespace reodyssey::ghp;

namespace reodyssey::render {

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

// Xenos D3DFORMAT (the GuestFormat enum values) -> Plume RenderFormat.
RenderFormat ConvertFormat(uint32_t d3dFormat) {
  switch (d3dFormat) {
  case 0x1A22AB60: // D3DFMT_A16B16G16R16F
  case 0x1A2201BF: // D3DFMT_A16B16G16R16F_2
    return RenderFormat::R16G16B16A16_FLOAT;
  case 0x1A200186: // D3DFMT_A8B8G8R8
    return RenderFormat::R8G8B8A8_UNORM;
  case 0x18280186: // D3DFMT_A8R8G8B8
  case 0x28280086: // D3DFMT_X8R8G8B8
    return RenderFormat::B8G8R8A8_UNORM;
  case 0x1A220197: // D3DFMT_D24FS8
  case 0x2D200196: // D3DFMT_D24S8
    return RenderFormat::D32_FLOAT_S8_UINT;
  case 0x2D22AB9F: // D3DFMT_G16R16F
  case 0x2D20AB8D: // D3DFMT_G16R16F_2
    return RenderFormat::R16G16_FLOAT;
  case 1: // D3DFMT_INDEX16
    return RenderFormat::R16_UINT;
  case 6: // D3DFMT_INDEX32
    return RenderFormat::R32_UINT;
  case 0x28000102: // D3DFMT_L8
  case 0x28000002: // D3DFMT_L8_2
    return RenderFormat::R8_UNORM;
  }

  switch (d3dFormat & 0x3F) {
  case 22: // k_24_8 (D24S8 variants)
  case 23: // k_24_8_FLOAT (D24FS8 variants)
    return RenderFormat::D32_FLOAT_S8_UINT;
  }

  return RenderFormat::R8G8B8A8_UNORM;
}

static uint32_t FormatBytes(RenderFormat format) {
  switch (format) {
  case RenderFormat::R16G16B16A16_FLOAT:
  case RenderFormat::D32_FLOAT_S8_UINT: // R32G8X24-typeless: 8 bytes/texel
    return 8;
  case RenderFormat::R8G8B8A8_UNORM:
  case RenderFormat::B8G8R8A8_UNORM:
  case RenderFormat::R16G16_FLOAT:
  case RenderFormat::D32_FLOAT:
  case RenderFormat::R32_UINT:
    return 4;
  case RenderFormat::R16_UINT:
    return 2;
  case RenderFormat::R8_UNORM:
    return 1;
  default:
    return 4;
  }
}

static RenderHeapType BufferHeapType() {
  return Device()->getCapabilities().gpuUploadHeap ? RenderHeapType::GPU_UPLOAD
                                                   : RenderHeapType::DEFAULT;
}

// D3D12 requires copy row pitch aligned to 256 bytes.
static uint32_t ComputeTexturePitch(const GuestTexture *texture) {
  uint32_t bpp = FormatBytes(texture->format);
  uint32_t pitch = texture->width * bpp;
  return (pitch + 255u) & ~255u;
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

GuestBuffer *CreateVertexBuffer(uint32_t length) {
  auto *buffer = GuestNew<GuestBuffer>(ResourceType::VertexBuffer);
  buffer->buffer = Device()->createBuffer(RenderBufferDesc::VertexBuffer(
      length, BufferHeapType(), RenderBufferFlag::INDEX));
  buffer->dataSize = length;
  return buffer;
}

GuestBuffer *CreateIndexBuffer(uint32_t length, uint32_t format) {
  auto *buffer = GuestNew<GuestBuffer>(ResourceType::IndexBuffer);
  buffer->buffer = Device()->createBuffer(
      RenderBufferDesc::IndexBuffer(length, BufferHeapType()));
  buffer->dataSize = length;
  buffer->guestFormat = format;
  buffer->format = ConvertFormat(format);
  return buffer;
}

// Lock returns a guest-visible staging pointer that the guest writes (BE) into.
static uint32_t LockBuffer(GuestBuffer *buffer, uint32_t flags) {
  buffer->lockedReadOnly = (flags & 0x10) != 0;
  if (buffer->mappedMemory == nullptr) {
    uint32_t addr = GuestAllocRaw(buffer->dataSize, 0x10);
    buffer->mappedMemory = ToHost<void>(addr);
  }
  return ToGuest(buffer->mappedMemory);
}

// Byte-swap the guest staging contents into the host buffer.
template <typename T> static void UploadBufferSwapped(GuestBuffer *buffer) {
  if (buffer->lockedReadOnly || buffer->mappedMemory == nullptr)
    return;

  const T *src = reinterpret_cast<const T *>(buffer->mappedMemory);
  const size_t count = buffer->dataSize / sizeof(T);

  auto swapInto = [&](T *dest) {
    for (size_t i = 0; i < count; ++i)
      dest[i] = std::byteswap(src[i]);
  };

  if (Device()->getCapabilities().gpuUploadHeap) {
    swapInto(reinterpret_cast<T *>(buffer->buffer->map()));
    buffer->buffer->unmap();
  } else {
    auto upload = Device()->createBuffer(
        RenderBufferDesc::UploadBuffer(buffer->dataSize));
    swapInto(reinterpret_cast<T *>(upload->map()));
    upload->unmap();
    RenderBuffer *dst = buffer->buffer.get();
    RenderBuffer *srcBuf = upload.get();
    const uint64_t size = buffer->dataSize;
    ExecuteUpload([&](RenderCommandList *cl) {
      cl->copyBufferRegion(dst->at(0), srcBuf->at(0), size);
    });
  }
}

uint32_t LockVertexBuffer(GuestBuffer *buffer, uint32_t flags) {
  return LockBuffer(buffer, flags);
}
void UnlockVertexBuffer(GuestBuffer *buffer) {
  UploadBufferSwapped<uint32_t>(buffer);
}

uint32_t LockIndexBuffer(GuestBuffer *buffer, uint32_t flags) {
  return LockBuffer(buffer, flags);
}
void UnlockIndexBuffer(GuestBuffer *buffer) {
  if (buffer->guestFormat == 6 /* D3DFMT_INDEX32 */)
    UploadBufferSwapped<uint32_t>(buffer);
  else
    UploadBufferSwapped<uint16_t>(buffer);
}

// ---------------------------------------------------------------------------
// Textures / surfaces
// ---------------------------------------------------------------------------

GuestTexture *CreateTexture(uint32_t width, uint32_t height, uint32_t depth,
                            uint32_t levels, uint32_t usage, uint32_t format,
                            uint32_t /*pool*/, uint32_t type) {
  const bool volume = (type == 17);
  auto *texture = GuestNew<GuestTexture>();
  texture->type = volume ? ResourceType::VolumeTexture : ResourceType::Texture;

  RenderTextureDesc desc;
  desc.dimension = volume ? RenderTextureDimension::TEXTURE_3D
                          : RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = depth;
  desc.mipLevels = levels;
  desc.arraySize = 1;
  desc.format = ConvertFormat(format);
  if (RenderFormatIsDepth(desc.format)) {
    desc.flags = RenderTextureFlag::DEPTH_TARGET;
  } else if (volume) {
    desc.flags = RenderTextureFlag::NONE;
  } else {
    desc.flags = RenderTextureFlag::RENDER_TARGET;
  }

  texture->textureHolder = Device()->createTexture(desc);
  texture->texture = texture->textureHolder.get();

  RenderTextureViewDesc viewDesc;
  viewDesc.format = desc.format;
  viewDesc.dimension = volume ? RenderTextureViewDimension::TEXTURE_3D
                              : RenderTextureViewDimension::TEXTURE_2D;
  viewDesc.mipLevels = levels;
  switch (format) {
  case 0x1A220197: // D3DFMT_D24FS8
  case 0x2D200196: // D3DFMT_D24S8
  case 0x28000102: // D3DFMT_L8
  case 0x28000002: // D3DFMT_L8_2
    viewDesc.componentMapping =
        RenderComponentMapping(RenderSwizzle::R, RenderSwizzle::R,
                               RenderSwizzle::R, RenderSwizzle::ONE);
    break;
  case 0x28280086: // D3DFMT_X8R8G8B8
    viewDesc.componentMapping =
        RenderComponentMapping(RenderSwizzle::G, RenderSwizzle::B,
                               RenderSwizzle::A, RenderSwizzle::ONE);
    break;
  default:
    break;
  }
  texture->textureView = texture->texture->createTextureView(viewDesc);

  texture->width = width;
  texture->height = height;
  texture->depth = depth;
  texture->levels = levels;
  texture->format = desc.format;
  texture->requiresHostInitialization =
      desc.flags == RenderTextureFlag::RENDER_TARGET ||
      desc.flags == RenderTextureFlag::DEPTH_TARGET;
  texture->hostInitialized = !texture->requiresHostInitialization;
  texture->viewDimension = viewDesc.dimension;
  texture->descriptorIndex = AllocTextureDescriptor();
  TextureDescriptorSet()->setTexture(texture->descriptorIndex, texture->texture,
                                     RenderTextureLayout::SHADER_READ,
                                     texture->textureView.get());
  return texture;
}

GuestSurface *CreateSurface(uint32_t width, uint32_t height, uint32_t format,
                            uint32_t /*multiSample*/) {
  RenderTextureDesc desc;
  desc.dimension = RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = ConvertFormat(format);
  const bool depth = RenderFormatIsDepth(desc.format);
  desc.flags = depth ? RenderTextureFlag::DEPTH_TARGET
                     : RenderTextureFlag::RENDER_TARGET;

  auto *surface = GuestNew<GuestSurface>(depth ? ResourceType::DepthStencil
                                               : ResourceType::RenderTarget);
  surface->textureHolder = Device()->createTexture(desc);
  surface->texture = surface->textureHolder.get();
  RenderTextureViewDesc viewDesc;
  viewDesc.format = desc.format;
  viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
  viewDesc.mipLevels = 1;
  surface->textureView = surface->texture->createTextureView(viewDesc);
  surface->width = width;
  surface->height = height;
  surface->format = desc.format;
  surface->guestFormat = format;
  surface->requiresHostInitialization = true;
  surface->hostInitialized = false;
  surface->descriptorIndex = AllocTextureDescriptor();
  TextureDescriptorSet()->setTexture(surface->descriptorIndex, surface->texture,
                                     RenderTextureLayout::SHADER_READ,
                                     surface->textureView.get());
  return surface;
}

// LockTextureRect returns pitch + a guest-visible staging pointer.
void LockTextureRect(GuestTexture *texture, uint32_t *outPitch,
                     uint32_t *outBits) {
  uint32_t pitch = ComputeTexturePitch(texture);
  uint32_t slicePitch = pitch * texture->height;
  if (texture->mappedMemory == nullptr) {
    uint32_t addr = GuestAllocRaw(slicePitch, 0x10);
    texture->mappedMemory = ToHost<void>(addr);
  }
  if (outPitch)
    *outPitch = pitch;
  if (outBits)
    *outBits = ToGuest(texture->mappedMemory);
}

void UnlockTextureRect(GuestTexture *texture) {
  if (texture->mappedMemory == nullptr)
    return;
  uint32_t pitch = ComputeTexturePitch(texture);
  uint32_t slicePitch = pitch * texture->height;

  auto upload =
      Device()->createBuffer(RenderBufferDesc::UploadBuffer(slicePitch));
  std::memcpy(upload->map(), texture->mappedMemory, slicePitch);
  upload->unmap();

  RenderTexture *dst = texture->texture;
  RenderBuffer *src = upload.get();
  const RenderFormat fmt = texture->format;
  const uint32_t w = texture->width, h = texture->height;
  const uint32_t rowTexels = pitch / FormatBytes(fmt);
  ExecuteUpload([&](RenderCommandList *cl) {
    cl->barriers(RenderBarrierStage::COPY,
                 RenderTextureBarrier(dst, RenderTextureLayout::COPY_DEST));
    cl->copyTextureRegion(RenderTextureCopyLocation::Subresource(dst, 0),
                          RenderTextureCopyLocation::PlacedFootprint(
                              src, fmt, w, h, 1, rowTexels));
  });
  texture->hostInitialized = true;
}

// ---------------------------------------------------------------------------
// Vertex declaration (input-element translation deferred to pipeline build)
// ---------------------------------------------------------------------------

static GuestVertexDeclaration *
CreateVertexDeclarationFromElements(GuestVertexElement *guestElements,
                                    uint32_t count, bool hasTerminator) {
  auto *decl = GuestNew<GuestVertexDeclaration>();
  decl->vertexElementCount = count;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(count + 1);
  for (uint32_t i = 0; i <= count; ++i) {
    GuestVertexElement &d = decl->vertexElements[i];
    if (i == count && !hasTerminator) {
      d.stream = 0xFF;
      d.offset = 0;
      d.type = 0xFFFFFFFF;
      d.method = 0;
      d.usage = 0;
      d.usageIndex = 0;
      d.padding = 0;
    } else {
      const auto &s = guestElements[i];
      d.stream = std::byteswap(s.stream);
      d.offset = std::byteswap(s.offset);
      d.type = std::byteswap(s.type);
      d.method = s.method;
      d.usage = s.usage;
      d.usageIndex = s.usageIndex;
      d.padding = s.padding;
    }
    if (i < count && d.stream < 16)
      decl->vertexStreams[d.stream] = true;
  }
  // RenderInputElement translation + hashing happens at pipeline-build time.
  return decl;
}

// Count guest vertex elements up to the D3DDECL_END terminator (stream 0xFF).
GuestVertexDeclaration *
CreateVertexDeclaration(GuestVertexElement *guestElements) {
  // Guest elements are big-endian; count + copy with byte-swap.
  auto readU16 = [](uint16_t v) { return std::byteswap(v); };

  uint32_t count = 0;
  while (readU16(reinterpret_cast<uint16_t *>(&guestElements[count])[0]) !=
         0xFF) {
    ++count;
    if (count > 64)
      break; // safety
  }

  return CreateVertexDeclarationFromElements(guestElements, count, true);
}

static std::mutex g_vertexDeclarationAliasMutex;
static std::unordered_map<uint32_t, GuestVertexDeclaration *>
    g_vertexDeclarationAliases;

void RegisterVertexDeclarationAlias(uint32_t guestAddress,
                                    GuestVertexDeclaration *declaration) {
  if (!guestAddress || declaration == nullptr)
    return;
  std::lock_guard lock(g_vertexDeclarationAliasMutex);
  g_vertexDeclarationAliases[guestAddress] = declaration;
}

GuestVertexDeclaration *LookupVertexDeclarationAlias(uint32_t guestAddress) {
  if (!guestAddress)
    return nullptr;
  std::lock_guard lock(g_vertexDeclarationAliasMutex);
  auto it = g_vertexDeclarationAliases.find(guestAddress);
  if (it != g_vertexDeclarationAliases.end())
    return it->second;

  uint8_t *raw = ToHost<uint8_t>(guestAddress);
  uint32_t count = reinterpret_cast<rex::be<uint32_t> *>(raw + 24)->get();
  if (count == 0 || count > 64)
    return nullptr;

  auto *elements = reinterpret_cast<GuestVertexElement *>(raw + 52);
  GuestVertexDeclaration *declaration =
      CreateVertexDeclarationFromElements(elements, count, false);
  g_vertexDeclarationAliases[guestAddress] = declaration;
  return declaration;
}

// ---------------------------------------------------------------------------
// Descriptor getters (read-back of buffer/surface metadata)
// ---------------------------------------------------------------------------

void GetVertexBufferDesc(GuestBuffer *buffer, uint32_t *outSize) {
  if (outSize)
    *outSize = buffer->dataSize;
}

void GetIndexBufferDesc(GuestBuffer *buffer, uint32_t *outFormat,
                        uint32_t *outSize) {
  if (outFormat)
    *outFormat = buffer->guestFormat;
  if (outSize)
    *outSize = buffer->dataSize;
}

// 

static ShaderCacheEntry *FindShaderCacheEntry(uint64_t hash) {
  ShaderCacheEntry *end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  ShaderCacheEntry *it = std::lower_bound(
      g_shaderCacheEntries, end, hash,
      [](const ShaderCacheEntry &lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

static GuestShader *CreateShaderFromFunction(const uint32_t *function,
                                             ResourceType type) {
  // Guest microcode is big-endian; size = header words [1] + [2].
  uint32_t size = std::byteswap(function[1]) + std::byteswap(function[2]);
  uint64_t hash = XXH3_64bits(function, size);

  if (ShaderCacheEntry *entry = FindShaderCacheEntry(hash)) {
    if (entry->guest_shader == nullptr) {
      auto *shader = GuestNew<GuestShader>(type);
      shader->shaderCacheEntry = entry;
      entry->guest_shader = reinterpret_cast<::GuestShader *>(shader);
      return shader;
    }
    return reinterpret_cast<GuestShader *>(entry->guest_shader);
  }
  // Dump the raw ShaderContainer so XenosRecomp can translate it offline.
  static std::unordered_set<uint64_t> s_dumped;
  if (s_dumped.insert(hash).second) {
    std::filesystem::create_directories("missed_shaders");
    char path[64];
    std::snprintf(path, sizeof(path), "missed_shaders/%016llX.bin",
                  static_cast<unsigned long long>(hash));
    if (FILE *f = std::fopen(path, "wb")) {
      std::fwrite(function, 1, size, f);
      std::fclose(f);
    }
    REXGPU_WARN(
        "Shader cache MISS: hash=0x{:016X} size={} type={} dumped to {}",
        hash, size, int(type), path);
  }
  return GuestNew<GuestShader>(type);
}

GuestShader *CreateVertexShader(const uint32_t *function) {
  return CreateShaderFromFunction(function, ResourceType::VertexShader);
}

GuestShader *CreatePixelShader(const uint32_t *function) {
  return CreateShaderFromFunction(function, ResourceType::PixelShader);
}


namespace {
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t width,
                       uint32_t bytesPerElement) {
  uint32_t alignedWidth = (width + 31) & ~31u;
  uint32_t logBpp = (bytesPerElement >> 2) +
                    ((bytesPerElement >> 1) >> (bytesPerElement >> 2));
  uint32_t macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBpp + 7);
  uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
  uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                    ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
  return (((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
          ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
         logBpp;
}

struct XenosTextureInfo {
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t baseAddress = 0; // guest byte address of mip 0
  uint32_t pitchTexels = 0; // row pitch in texels
  uint32_t blockDim = 1;    // 4 for BC formats
  uint32_t bytesPerBlock = 4;
  uint32_t endian = 0; // 0 none, 1 = 8-in-16, 2 = 8-in-32
  bool tiled = false;
  bool packedMips = false;
  bool valid = false;
};


void GetPackedBaseOffsetBlocks(const XenosTextureInfo &info, uint32_t &outX,
                               uint32_t &outY) {
  outX = 0;
  outY = 0;
  if (!info.packedMips)
    return;
  const uint32_t log2Width = uint32_t(std::bit_width(info.width - 1));
  const uint32_t log2Height = uint32_t(std::bit_width(info.height - 1));
  if (std::min(log2Width, log2Height) > 4)
    return; // min dimension > 16 texels: base level not packed
  uint32_t xTexels = 0, yTexels = 0;
  if (log2Width > log2Height)
    yTexels = 16; // wider than tall: laid out vertically
  else
    xTexels = 16; // taller than wide (or square): laid out horizontally
  outX = xTexels / info.blockDim;
  outY = yTexels / info.blockDim;
}

// GPUTEXTURE_FETCH_CONSTANT (6 dwords at header +0x1C, big-endian).
XenosTextureInfo ParseTextureFetchConstant(const rex::be<uint32_t> *fc) {
  XenosTextureInfo info;
  const uint32_t fc0 = fc[0].get();
  const uint32_t fc1 = fc[1].get();
  const uint32_t fc2 = fc[2].get();

  info.pitchTexels = ((fc0 >> 22) & 0x1FF) * 32;
  info.tiled = (fc0 >> 31) != 0;
  const uint32_t gpuFormat = fc1 & 0x3F;
  // SignX (bits 2-3 of fetch constant word 0); GPUSIGN_GAMMA == 3 means the
  // colour channels are sRGB and must be linearised on sample.
  const bool gamma = ((fc0 >> 2) & 0x3) == 3;
  info.endian = (fc1 >> 6) & 0x3;
  info.baseAddress = ((fc1 >> 12) & 0xFFFFF) << 12;
  info.width = (fc2 & 0x1FFF) + 1;
  info.height = ((fc2 >> 13) & 0x1FFF) + 1;
  info.packedMips = ((fc[5].get() >> 11) & 0x1) != 0;

  switch (gpuFormat) {
  case 2: // k_8 (L8/A8)
    info.format = RenderFormat::R8_UNORM;
    info.bytesPerBlock = 1;
    break;
  case 6: // k_8_8_8_8 (A8R8G8B8 cooked; 8-in-32 swap yields BGRA bytes)
    info.format = RenderFormat::B8G8R8A8_UNORM;
    info.bytesPerBlock = 4;
    break;
  case 18: // k_DXT1
    info.format = gamma ? RenderFormat::BC1_UNORM_SRGB : RenderFormat::BC1_UNORM;
    info.blockDim = 4;
    info.bytesPerBlock = 8;
    break;
  case 19: // k_DXT2_3
    info.format = gamma ? RenderFormat::BC2_UNORM_SRGB : RenderFormat::BC2_UNORM;
    info.blockDim = 4;
    info.bytesPerBlock = 16;
    break;
  case 20: // k_DXT4_5
    info.format = gamma ? RenderFormat::BC3_UNORM_SRGB : RenderFormat::BC3_UNORM;
    info.blockDim = 4;
    info.bytesPerBlock = 16;
    break;
  case 26: // k_16_16_16_16
    info.format = RenderFormat::R16G16B16A16_UNORM;
    info.bytesPerBlock = 8;
    break;
  case 32: // k_16_16_16_16_FLOAT (scene-color resolve targets)
    info.format = RenderFormat::R16G16B16A16_FLOAT;
    info.bytesPerBlock = 8;
    break;
  default: {
    return info;
  }
  }
  if (info.pitchTexels == 0)
    info.pitchTexels = info.width;
  info.valid = info.width != 0 && info.height != 0 && info.baseAddress != 0;
  return info;
}

void EndianSwapBuffer(uint8_t *data, size_t size, uint32_t endian) {
  if (endian == 1) {
    auto *p = reinterpret_cast<uint16_t *>(data);
    for (size_t i = 0; i < size / 2; ++i)
      p[i] = std::byteswap(p[i]);
  } else if (endian == 2) {
    auto *p = reinterpret_cast<uint32_t *>(data);
    for (size_t i = 0; i < size / 4; ++i)
      p[i] = std::byteswap(p[i]);
  }
}

bool UploadGuestTextureData(GuestTexture *texture,
                            const XenosTextureInfo &info) {
  if (texture == nullptr || texture->texture == nullptr || !info.valid)
    return false;
  if (texture->width != info.width || texture->height != info.height ||
      texture->format != info.format) {
    return false;
  }

  const uint32_t wBlocks = (info.width + info.blockDim - 1) / info.blockDim;
  const uint32_t hBlocks = (info.height + info.blockDim - 1) / info.blockDim;
  const uint32_t pitchBlocks =
      std::max(wBlocks, info.pitchTexels / info.blockDim);

  // Small textures live at a block offset inside their packed 32x32 tile.
  uint32_t packedX = 0, packedY = 0;
  GetPackedBaseOffsetBlocks(info, packedX, packedY);

  std::vector<uint8_t> linear(size_t(wBlocks) * hBlocks * info.bytesPerBlock);
  const uint8_t *src = ToHost<uint8_t>(info.baseAddress);
  if (info.tiled) {
    for (uint32_t by = 0; by < hBlocks; ++by) {
      for (uint32_t bx = 0; bx < wBlocks; ++bx) {
        const uint32_t element = TiledOffset2D(bx + packedX, by + packedY,
                                               pitchBlocks, info.bytesPerBlock);
        std::memcpy(
            linear.data() + (size_t(by) * wBlocks + bx) * info.bytesPerBlock,
            src + size_t(element) * info.bytesPerBlock, info.bytesPerBlock);
      }
    }
  } else {
    for (uint32_t by = 0; by < hBlocks; ++by) {
      std::memcpy(linear.data() + size_t(by) * wBlocks * info.bytesPerBlock,
                  src + (size_t(by + packedY) * pitchBlocks + packedX) *
                            info.bytesPerBlock,
                  size_t(wBlocks) * info.bytesPerBlock);
    }
  }
  EndianSwapBuffer(linear.data(), linear.size(), info.endian);

  const uint32_t srcRowPitch = wBlocks * info.bytesPerBlock;
  const uint32_t dstRowPitch = (srcRowPitch + 255u) & ~255u;
  auto upload = Device()->createBuffer(
      RenderBufferDesc::UploadBuffer(size_t(dstRowPitch) * hBlocks));
  auto *mapped = reinterpret_cast<uint8_t *>(upload->map());
  for (uint32_t by = 0; by < hBlocks; ++by) {
    std::memcpy(mapped + size_t(by) * dstRowPitch,
                linear.data() + size_t(by) * srcRowPitch, srcRowPitch);
  }
  upload->unmap();

  RenderTexture *dst = texture->texture;
  RenderBuffer *srcBuf = upload.get();
  const uint32_t rowTexels = (dstRowPitch / info.bytesPerBlock) * info.blockDim;
  ExecuteUpload([&](RenderCommandList *cl) {
    cl->barriers(RenderBarrierStage::COPY,
                 RenderTextureBarrier(dst, RenderTextureLayout::COPY_DEST));
    cl->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(dst, 0),
        RenderTextureCopyLocation::PlacedFootprint(
            srcBuf, info.format, info.width, info.height, 1, rowTexels));
  });
  texture->hostInitialized = true;
  return true;
}

std::mutex g_guestTextureAliasMutex;
std::unordered_map<uint32_t, GuestTexture *> g_guestTextureAliases;
std::vector<std::unique_ptr<GuestTexture>> g_guestTextureStorage;

} // namespace

GuestTexture *TranslateGuestTexture(void *guestHeader, bool uploadGuestData) {
  const uint32_t guestAddress = ToGuest(guestHeader);

  const auto *header = reinterpret_cast<const rex::be<uint32_t> *>(guestHeader);
  if ((header[0].get() & 0xF) != 3) {
    return nullptr;
  }
  XenosTextureInfo info = ParseTextureFetchConstant(header + 7);
  if (!info.valid) {
    return nullptr;
  }

  {
    std::lock_guard lock(g_guestTextureAliasMutex);
    auto it = g_guestTextureAliases.find(info.baseAddress);
    if (it != g_guestTextureAliases.end() && it->second != nullptr) {
      GuestTexture *cached = it->second;
      // Same memory redescribed with a different shape: drop the stale entry.
      if (cached->width == info.width && cached->height == info.height &&
          cached->format == info.format) {
        return cached;
      }
      g_guestTextureAliases.erase(it);
    }
  }

  GuestTexture *result = nullptr;
  {
    {
      auto textureStorage = std::make_unique<GuestTexture>();
      GuestTexture *texture = textureStorage.get();
      texture->type = ResourceType::Texture;

      RenderTextureDesc desc;
      desc.dimension = RenderTextureDimension::TEXTURE_2D;
      desc.width = info.width;
      desc.height = info.height;
      desc.depth = 1;
      desc.mipLevels = 1;
      desc.arraySize = 1;
      desc.format = info.format;
      desc.flags = RenderTextureFlag::NONE;
      texture->textureHolder = Device()->createTexture(desc);
      texture->texture = texture->textureHolder.get();

      RenderTextureViewDesc viewDesc;
      viewDesc.format = info.format;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
      viewDesc.mipLevels = 1;
      if (info.format == RenderFormat::R8_UNORM) {
        viewDesc.componentMapping =
            RenderComponentMapping(RenderSwizzle::R, RenderSwizzle::R,
                                   RenderSwizzle::R, RenderSwizzle::ONE);
      }
      texture->textureView = texture->texture->createTextureView(viewDesc);
      texture->width = info.width;
      texture->height = info.height;
      texture->depth = 1;
      texture->levels = 1;
      texture->format = info.format;
      texture->requiresHostInitialization = false;
      texture->hostInitialized = true;
      texture->viewDimension = RenderTextureViewDimension::TEXTURE_2D;
      texture->descriptorIndex = AllocTextureDescriptor();
      TextureDescriptorSet()->setTexture(
          texture->descriptorIndex, texture->texture,
          RenderTextureLayout::SHADER_READ, texture->textureView.get());

      if (uploadGuestData)
        UploadGuestTextureData(texture, info);
      std::lock_guard lock(g_guestTextureAliasMutex);
      result = texture;
      g_guestTextureStorage.push_back(std::move(textureStorage));
      g_guestTextureAliases[info.baseAddress] = result;
    }
  }
  return result;
}

namespace {

std::mutex g_guestSurfaceAliasMutex;
std::unordered_map<uint32_t, GuestSurface *> g_guestSurfaceAliases;

} // namespace

GuestBaseTexture *TranslateGuestSurface(void *guestHeader) {
  const uint32_t guestAddress = ToGuest(guestHeader);
  const auto *header = reinterpret_cast<const rex::be<uint32_t> *>(guestHeader);

  const uint32_t common = header[0].get();
  if ((common & 0x40000000) != 0) {
    const uint32_t parentAddress = header[6].get(); // +0x18
    const uint32_t level = header[7].get() >> 28;   // +0x1C
    void *parentHost =
        parentAddress != 0 ? ToHost<void>(parentAddress) : nullptr;
    if (parentHost != nullptr && IsReoResource(parentHost) && level == 0) {
      auto *parent = static_cast<GuestBaseTexture *>(parentHost);
      if (parent->texture != nullptr)
        return parent;
    }
    return nullptr;
  }

  const uint32_t sizeDword = header[9].get(); // +0x24
  const uint32_t width = ((sizeDword >> 18) & 0x3FFF) + 1;
  const uint32_t height = ((sizeDword >> 3) & 0x7FFF) + 1;
  const uint32_t msaaType = (header[6].get() >> 16) & 3; // +0x18 top halfword
  const uint32_t guestFormat = header[10].get();         // +0x28
  if (width <= 1 || height <= 1 || width > 8192 || height > 8192) {
    return nullptr;
  }

  const RenderFormat format = ConvertFormat(guestFormat);
  const RenderSampleCounts sampleCount =
      msaaType == 2   ? RenderSampleCount::COUNT_4
      : msaaType == 1 ? RenderSampleCount::COUNT_2
                      : RenderSampleCount::COUNT_1;

  {
    std::lock_guard lock(g_guestSurfaceAliasMutex);
    auto it = g_guestSurfaceAliases.find(guestAddress);
    if (it != g_guestSurfaceAliases.end()) {
      GuestSurface *cached = it->second;
      if (cached->width == width && cached->height == height &&
          cached->format == format && cached->sampleCount == sampleCount) {
        return cached;
      }
      g_guestSurfaceAliases.erase(it); // header rebuilt with a new desc
    }
  }

  const bool depth = RenderFormatIsDepth(format);
  RenderTextureDesc desc;
  desc.dimension = RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.multisampling.sampleCount = sampleCount;
  desc.format = format;
  desc.flags = depth ? RenderTextureFlag::DEPTH_TARGET
                     : RenderTextureFlag::RENDER_TARGET;

  auto *surface = GuestNew<GuestSurface>(depth ? ResourceType::DepthStencil
                                               : ResourceType::RenderTarget);
  surface->textureHolder = Device()->createTexture(desc);
  surface->texture = surface->textureHolder.get();
  surface->width = width;
  surface->height = height;
  surface->format = format;
  surface->guestFormat = guestFormat;
  surface->sampleCount = sampleCount;
  surface->requiresHostInitialization = true;
  surface->hostInitialized = false;
  if (sampleCount == RenderSampleCount::COUNT_1) {
    RenderTextureViewDesc viewDesc;
    viewDesc.format = format;
    viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    viewDesc.mipLevels = 1;
    surface->textureView = surface->texture->createTextureView(viewDesc);
    surface->descriptorIndex = AllocTextureDescriptor();
    TextureDescriptorSet()->setTexture(
        surface->descriptorIndex, surface->texture,
        RenderTextureLayout::SHADER_READ, surface->textureView.get());
  }

  std::lock_guard lock(g_guestSurfaceAliasMutex);
  g_guestSurfaceAliases[guestAddress] = surface;
  return surface;
}

static std::mutex g_shaderAliasMutex;
static std::unordered_map<uint32_t, GuestShader *> g_shaderAliases;

void RegisterShaderAlias(uint32_t guestAddress, GuestShader *shader) {
  if (!guestAddress || shader == nullptr)
    return;
  std::lock_guard lock(g_shaderAliasMutex);
  g_shaderAliases[guestAddress] = shader;
}

GuestShader *LookupShaderAlias(uint32_t guestAddress) {
  if (!guestAddress)
    return nullptr;
  std::lock_guard lock(g_shaderAliasMutex);
  auto it = g_shaderAliases.find(guestAddress);
  return it != g_shaderAliases.end() ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// shitty DDS texture loading 
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kPitchAlignment = 0x100;
constexpr uint32_t kPlacementAlignment = 0x200;

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) |
         (uint32_t(d) << 24);
}

struct DdsInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  uint32_t mipCount = 1;
  uint32_t headerSize = 128;
  RenderFormat format = RenderFormat::R8G8B8A8_UNORM;
  uint32_t blockWidth = 1;
  uint32_t blockHeight = 1;
  uint32_t bytesPerBlock = 4;
  bool valid = false;
};

DdsInfo ParseDds(const uint8_t *data, uint32_t size) {
  DdsInfo info;
  if (size < 128 ||
      *reinterpret_cast<const uint32_t *>(data) != FourCC('D', 'D', 'S', ' '))
    return info;

  auto rd = [&](uint32_t off) {
    return *reinterpret_cast<const uint32_t *>(data + off);
  };
  info.height = rd(12);
  info.width = rd(16);
  info.depth = rd(24) ? rd(24) : 1;
  info.mipCount = rd(28) ? rd(28) : 1;

  const uint32_t pfFlags = rd(80);
  const uint32_t fourCC = rd(84);

  auto setBlock = [&](RenderFormat fmt, uint32_t bpb) {
    info.format = fmt;
    info.blockWidth = info.blockHeight = 4;
    info.bytesPerBlock = bpb;
  };

  if (pfFlags & 0x4) { // DDPF_FOURCC
    if (fourCC == FourCC('D', 'X', 'T', '1')) {
      setBlock(RenderFormat::BC1_UNORM, 8);
    } else if (fourCC == FourCC('D', 'X', 'T', '3')) {
      setBlock(RenderFormat::BC2_UNORM, 16);
    } else if (fourCC == FourCC('D', 'X', 'T', '5')) {
      setBlock(RenderFormat::BC3_UNORM, 16);
    } else if (fourCC == FourCC('A', 'T', 'I', '2') ||
               fourCC == FourCC('B', 'C', '5', 'U')) {
      setBlock(RenderFormat::BC5_UNORM, 16);
    } else if (fourCC == FourCC('D', 'X', '1', '0')) {
      info.headerSize = 148;
      const uint32_t dxgi = rd(128); // DDS_HEADER_DXT10.dxgiFormat
      switch (dxgi) {
      case 71:
        setBlock(RenderFormat::BC1_UNORM, 8);
        break; // BC1_UNORM
      case 74:
        setBlock(RenderFormat::BC2_UNORM, 16);
        break; // BC2_UNORM
      case 77:
        setBlock(RenderFormat::BC3_UNORM, 16);
        break; // BC3_UNORM
      case 83:
        setBlock(RenderFormat::BC5_UNORM, 16);
        break; // BC5_UNORM
      case 98:
        setBlock(RenderFormat::BC7_UNORM, 16);
        break; // BC7_UNORM
      case 28:
        info.format = RenderFormat::R8G8B8A8_UNORM;
        info.bytesPerBlock = 4;
        break;
      case 87:
        info.format = RenderFormat::B8G8R8A8_UNORM;
        info.bytesPerBlock = 4;
        break;
      default:
        info.format = RenderFormat::R8G8B8A8_UNORM;
        info.bytesPerBlock = 4;
        break;
      }
    } else {
      info.format = RenderFormat::R8G8B8A8_UNORM;
      info.bytesPerBlock = 4;
    }
  } else {
    // Uncompressed RGB(A). 32-bit assumed (B8G8R8A8 is the common D3D9 layout).
    info.format = RenderFormat::B8G8R8A8_UNORM;
    info.bytesPerBlock = 4;
  }

  info.valid = info.width != 0 && info.height != 0;
  return info;
}

} // namespace

GuestTexture *LoadTextureFromMemory(const uint8_t *data, uint32_t size) {
  DdsInfo dds = ParseDds(data, size);
  if (!dds.valid) {
    // Unrecognized container: hand back a 1x1 texture so the guest has a valid
    // handle instead of crashing.
    return CreateTexture(1, 1, 1, 1, 0, 0x1A200186 /*A8B8G8R8*/, 0, 0);
  }

  auto *texture = GuestNew<GuestTexture>();
  texture->type = ResourceType::Texture;

  RenderTextureDesc desc;
  desc.dimension = RenderTextureDimension::TEXTURE_2D;
  desc.width = dds.width;
  desc.height = dds.height;
  desc.depth = dds.depth;
  desc.mipLevels = dds.mipCount;
  desc.arraySize = 1;
  desc.format = dds.format;
  desc.flags = RenderTextureFlag::NONE;

  texture->textureHolder = Device()->createTexture(desc);
  texture->texture = texture->textureHolder.get();

  RenderTextureViewDesc viewDesc;
  viewDesc.format = dds.format;
  viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
  viewDesc.mipLevels = dds.mipCount;
  texture->textureView = texture->texture->createTextureView(viewDesc);

  texture->width = dds.width;
  texture->height = dds.height;
  texture->depth = dds.depth;
  texture->levels = dds.mipCount;
  texture->format = dds.format;
  texture->requiresHostInitialization = false;
  texture->hostInitialized = true;
  texture->viewDimension = RenderTextureViewDimension::TEXTURE_2D;
  texture->descriptorIndex = AllocTextureDescriptor();
  TextureDescriptorSet()->setTexture(texture->descriptorIndex, texture->texture,
                                     RenderTextureLayout::SHADER_READ,
                                     texture->textureView.get());

  // Lay out the mip chain into an upload buffer with aligned placed footprints.
  struct Slice {
    uint32_t width, height, srcOffset, dstOffset, srcRowPitch, dstRowPitch,
        rowCount;
  };
  std::vector<Slice> slices;
  uint32_t srcOff = 0, dstOff = 0;
  for (uint32_t mip = 0; mip < dds.mipCount; ++mip) {
    Slice s;
    s.width = std::max(1u, dds.width >> mip);
    s.height = std::max(1u, dds.height >> mip);
    s.srcOffset = srcOff;
    s.dstOffset = dstOff;
    uint32_t blocksW = (s.width + dds.blockWidth - 1) / dds.blockWidth;
    s.srcRowPitch = blocksW * dds.bytesPerBlock;
    s.dstRowPitch =
        (s.srcRowPitch + kPitchAlignment - 1) & ~(kPitchAlignment - 1);
    s.rowCount = (s.height + dds.blockHeight - 1) / dds.blockHeight;
    srcOff += s.srcRowPitch * s.rowCount;
    dstOff += (s.dstRowPitch * s.rowCount + kPlacementAlignment - 1) &
              ~(kPlacementAlignment - 1);
    slices.push_back(s);
  }
  if (dds.headerSize + srcOff > size)
    return texture; // truncated data; skip upload

  auto upload = Device()->createBuffer(RenderBufferDesc::UploadBuffer(dstOff));
  auto *mapped = reinterpret_cast<uint8_t *>(upload->map());
  for (const Slice &s : slices) {
    const uint8_t *src = data + dds.headerSize + s.srcOffset;
    uint8_t *dst = mapped + s.dstOffset;
    for (uint32_t r = 0; r < s.rowCount; ++r) {
      std::memcpy(dst, src, s.srcRowPitch);
      src += s.srcRowPitch;
      dst += s.dstRowPitch;
    }
  }
  upload->unmap();

  RenderTexture *dstTex = texture->texture;
  RenderBuffer *srcBuf = upload.get();
  const RenderFormat fmt = dds.format;
  const uint32_t blockW = dds.blockWidth, bpb = dds.bytesPerBlock;
  ExecuteUpload([&](RenderCommandList *cl) {
    cl->barriers(RenderBarrierStage::COPY,
                 RenderTextureBarrier(dstTex, RenderTextureLayout::COPY_DEST));
    for (uint32_t i = 0; i < slices.size(); ++i) {
      const Slice &s = slices[i];
      uint32_t rowTexels = (s.dstRowPitch / bpb) * blockW;
      cl->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(dstTex, i),
          RenderTextureCopyLocation::PlacedFootprint(
              srcBuf, fmt, s.width, s.height, 1, rowTexels, s.dstOffset));
    }
  });

  return texture;
}

} // namespace reodyssey::render
