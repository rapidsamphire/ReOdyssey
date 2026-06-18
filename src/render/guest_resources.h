

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "generated/shader_cache.h"

#ifdef _WIN32
struct IDxcBlobEncoding;
#endif

namespace reodyssey::render {

enum class ResourceType {
  Texture,
  VolumeTexture,
  VertexBuffer,
  IndexBuffer,
  RenderTarget,
  DepthStencil,
  VertexDeclaration,
  VertexShader,
  PixelShader,
};

// Sentinel at offset 0 (where a real guest D3DResource has its Common flags),
// so hooks can tell our Guest* objects apart from genuine guest D3D resources
// created through paths we don't hook (e.g. XeInitD3DDevice internals).
inline constexpr uint32_t kReoResourceMagic = 0x52454F44; // 'REOD'

struct GuestResource {
  uint32_t magic = kReoResourceMagic;
  uint32_t refCount = 1;
  ResourceType type;

  explicit GuestResource(ResourceType t) : type(t) {}
};

// True only for pointers to our own guest-allocated Guest* objects.
inline bool IsReoResource(const void *p) {
  return p != nullptr &&
         *reinterpret_cast<const uint32_t *>(p) == kReoResourceMagic;
}

struct GuestBaseTexture;

struct PendingResolve {
  GuestBaseTexture *destination = nullptr;
  plume::RenderRect sourceRect{};
  uint32_t destX = 0;
  uint32_t destY = 0;
  bool hasSourceRect = false;
};

struct GuestBaseTexture : GuestResource {
  std::unique_ptr<plume::RenderTexture> textureHolder;
  plume::RenderTexture *texture = nullptr;
  std::unique_ptr<plume::RenderTextureView> textureView;
  uint32_t width = 0;
  uint32_t height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t descriptorIndex = 0;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  bool requiresHostInitialization = false;
  bool hostInitialized = true;
  GuestBaseTexture *sourceTexture = nullptr;
  uint32_t pendingResolveCount = 0;
  std::vector<PendingResolve> pendingResolves;

  explicit GuestBaseTexture(ResourceType t) : GuestResource(t) {}
};

// D3DFMT_* texture/volume texture.
struct GuestTexture : GuestBaseTexture {
  uint32_t depth = 0;
  uint32_t levels = 1;
  plume::RenderTextureViewDimension viewDimension =
      plume::RenderTextureViewDimension::TEXTURE_2D;
  void *mappedMemory = nullptr; // host pointer to guest-visible lock staging
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;

  GuestTexture() : GuestBaseTexture(ResourceType::Texture) {}
};

// Vertex/index buffer.
struct GuestBuffer : GuestResource {
  std::unique_ptr<plume::RenderBuffer> buffer;
  void *mappedMemory = nullptr; // host pointer to guest-visible lock staging
  uint32_t dataSize = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN; // index format
  uint32_t guestFormat = 0;
  bool lockedReadOnly = false;

  explicit GuestBuffer(ResourceType t) : GuestResource(t) {}
};

// Render target / depth-stencil surface.
struct GuestSurface : GuestBaseTexture {
  uint32_t guestFormat = 0;
  plume::RenderSampleCounts sampleCount = plume::RenderSampleCount::COUNT_1;
  // Framebuffers keyed by their (paired) color attachment; the backbuffer's
  // texture changes per frame, so the depth surface owns the cache.
  std::unordered_map<const plume::RenderTexture *,
                     std::unique_ptr<plume::RenderFramebuffer>>
      framebuffers;

  explicit GuestSurface(ResourceType t) : GuestBaseTexture(t) {}
};

struct GuestVertexElement {
  uint16_t stream;
  uint16_t offset;
  uint32_t type;
  uint8_t method;
  uint8_t usage;
  uint8_t usageIndex;
  uint8_t padding;
};

struct GuestVertexDeclaration : GuestResource {
  uint64_t hash = 0;
  std::unique_ptr<plume::RenderInputElement[]> inputElements;
  std::unique_ptr<GuestVertexElement[]> vertexElements;
  uint32_t inputElementCount = 0;
  uint32_t vertexElementCount = 0;
  // Derived during input-layout translation (render_state.cpp).
  uint32_t swappedTexcoords = 0;
  uint32_t swappedBlendWeights = 0;
  uint32_t indexVertexStream = 0;
  bool hasR11G11B10Normal = false;
  bool hasUByte4TangentBasis = false;
  bool vertexStreams[16]{};

  GuestVertexDeclaration() : GuestResource(ResourceType::VertexDeclaration) {}
};

// Vertex/pixel shader: maps a guest shader to its XenosRecomp-translated host
// shader via the generated shader cache (keyed by microcode hash).
struct GuestShader : GuestResource {
  std::mutex mutex;
  std::unique_ptr<plume::RenderShader> shader;
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderShader>>
      specializedShaders;
#ifdef _WIN32
  IDxcBlobEncoding *dxilLibraryBlob = nullptr;
#endif
  ShaderCacheEntry *shaderCacheEntry = nullptr;

  explicit GuestShader(ResourceType t) : GuestResource(t) {}
  ~GuestShader();
};

} // namespace reodyssey::render
