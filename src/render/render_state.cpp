#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <rex/hash.h> // XXH3_64bits

#include "render/guest_device.h"
#include "render/guest_heap.h"
#include "render/guest_resources.h"
#include "render/render_internal.h"
#include "render/render_state.h"

// Spec-constant bits (XenosRecomp shared header).
#define SPEC_CONSTANT_R11G11B10_NORMAL (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST (1 << 1)
#define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
#define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
#define SPEC_CONSTANT_REVERSE_Z (1 << 4)
#define SPEC_CONSTANT_UNPACK_UBYTE4_BASIS (1 << 6)

using namespace plume;

namespace reodyssey::render {

void BindTextureDescriptor(uint32_t index, GuestBaseTexture *texture,
                           RenderTextureViewDimension viewDimension);
void EnsureShaderResourceDescriptor(GuestBaseTexture *texture);

namespace {

#pragma pack(push, 1)
struct PipelineState {
  GuestShader *vertexShader = nullptr;
  GuestShader *pixelShader = nullptr;
  GuestVertexDeclaration *vertexDeclaration = nullptr;
  bool instancing = false;
  bool zEnable = true;
  bool zWriteEnable = true;
  RenderBlend srcBlend = RenderBlend::ONE;
  RenderBlend destBlend = RenderBlend::ZERO;
  RenderCullMode cullMode = RenderCullMode::NONE;
  RenderComparisonFunction zFunc = RenderComparisonFunction::LESS;
  bool alphaBlendEnable = false;
  RenderBlendOperation blendOp = RenderBlendOperation::ADD;
  float slopeScaledDepthBias = 0.0f;
  int32_t depthBias = 0;
  RenderBlend srcBlendAlpha = RenderBlend::ONE;
  RenderBlend destBlendAlpha = RenderBlend::ZERO;
  RenderBlendOperation blendOpAlpha = RenderBlendOperation::ADD;
  uint32_t colorWriteEnable = uint32_t(RenderColorWriteEnable::ALL);
  RenderPrimitiveTopology primitiveTopology =
      RenderPrimitiveTopology::TRIANGLE_LIST;
  uint8_t vertexStrides[16]{};
  RenderFormat renderTargetFormat{};
  RenderFormat depthStencilFormat{};
  RenderSampleCounts sampleCount = RenderSampleCount::COUNT_1;
  bool enableAlphaToCoverage = false;
  bool depthClipEnabled = true;
  uint32_t specConstants = 0;

  bool stencilEnable = false;
  uint8_t stencilReadMask = 0xFF;
  uint8_t stencilWriteMask = 0xFF;
  uint8_t stencilRef = 0;
  RenderComparisonFunction stencilFrontFunc = RenderComparisonFunction::ALWAYS;
  RenderStencilOp stencilFrontFail = RenderStencilOp::KEEP;
  RenderStencilOp stencilFrontDepthFail = RenderStencilOp::KEEP;
  RenderStencilOp stencilFrontPass = RenderStencilOp::KEEP;
  RenderComparisonFunction stencilBackFunc = RenderComparisonFunction::ALWAYS;
  RenderStencilOp stencilBackFail = RenderStencilOp::KEEP;
  RenderStencilOp stencilBackDepthFail = RenderStencilOp::KEEP;
  RenderStencilOp stencilBackPass = RenderStencilOp::KEEP;
};
#pragma pack(pop)
struct SharedConstants {
  uint32_t texture2DIndices[16]{};
  uint32_t texture3DIndices[16]{};
  uint32_t textureCubeIndices[16]{};
  uint32_t samplerIndices[16]{};
  uint32_t booleans{};
  uint32_t swappedTexcoords{};
  uint32_t swappedNormals{};
  uint32_t swappedBinormals{};
  uint32_t swappedTangents{};
  uint32_t swappedBlendWeights{};
  float halfPixelOffsetX{};
  float halfPixelOffsetY{};
  float clipPlane[4]{};
  uint32_t clipPlaneEnabled{};
  float alphaThreshold{};
  uint32_t conditionalSurveyIndex{};
  uint32_t conditionalRenderingIndex{};
};

struct DirtyStates {
  bool renderTargetAndDepthStencil;
  bool viewport;
  bool pipelineState;
  bool scissorRect;
  uint8_t vertexStreamFirst;
  uint8_t vertexStreamLast;
  bool indices;

  explicit DirtyStates(bool value)
      : renderTargetAndDepthStencil(value), viewport(value),
        pipelineState(value), scissorRect(value),
        vertexStreamFirst(value ? 0 : 255), vertexStreamLast(value ? 15 : 0),
        indices(value) {}
};

GuestBaseTexture *g_renderTarget;
GuestBaseTexture *g_implicitRenderTarget;
GuestSurface *g_depthStencil;
GuestBaseTexture *g_lastTouchedRenderTarget;
GuestSurface *g_implicitDepthStencil;
RenderFramebuffer *g_framebuffer;
RenderViewport g_viewport(0.0f, 0.0f, 1280.0f, 720.0f);
PipelineState g_pipelineState;
SharedConstants g_sharedConstants;
GuestTexture *g_textures[16];
bool g_scissorTestEnable = false;
bool g_sharedConstantsInitialized = false;
RenderRect g_scissorRect;
RenderVertexBufferView g_vertexBufferViews[16];
RenderInputSlot g_inputSlots[16];
RenderIndexBufferView g_indexBufferView({}, 0, RenderFormat::R16_UINT);
DirtyStates g_dirtyStates(true);
std::unique_ptr<RenderBuffer> g_dummyVertexBuffer;
// Set by FlushRenderState: whether a valid graphics pipeline is bound. Draws
// skip when false (e.g. a guest shader missing from the translated cache).
bool g_pipelineBound = false;

struct GuestClipPlane {
  rex::be<float> x;
  rex::be<float> y;
  rex::be<float> z;
  rex::be<float> w;
};

constexpr size_t kGuestClipPlanesOffset = 0x2820;
constexpr size_t kGuestClipPlaneEnableOffset = 0x2944;
constexpr size_t kGuestScissorEnableOffset = 0x2E48;
constexpr uint32_t kGuestClipPlaneMask = 0x3F;

GuestClipPlane *ClipPlanes(GuestDevice *device) {
  return reinterpret_cast<GuestClipPlane *>(
      reinterpret_cast<uint8_t *>(device) + kGuestClipPlanesOffset);
}

uint32_t ClipPlaneEnableMask(GuestDevice *device) {
  auto *value = reinterpret_cast<rex::be<uint32_t> *>(
      reinterpret_cast<uint8_t *>(device) + kGuestClipPlaneEnableOffset);
  return value->get() & kGuestClipPlaneMask;
}

bool ScissorTestEnabled(GuestDevice *device) {
  auto *value = reinterpret_cast<rex::be<uint32_t> *>(
      reinterpret_cast<uint8_t *>(device) + kGuestScissorEnableOffset);
  return value->get() != 0;
}

std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> g_pipelines;
std::unordered_map<uint64_t,
                   std::pair<uint32_t, std::unique_ptr<RenderSampler>>>
    g_samplerStates;
std::unordered_map<GuestShader *, GuestVertexDeclaration *>
    g_vertexShaderDeclarations;
std::unordered_map<RenderTexture *, std::unique_ptr<RenderFramebuffer>>
    g_colorFramebuffers;
std::unordered_set<GuestBaseTexture *> g_pendingStretchRectSurfaces;
std::unique_ptr<GuestVertexDeclaration> g_texturedQuadDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_simpleElementDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_materialVertexDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_dynamicMeshVertexDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_batchedTriangleVertexDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_gpuSkin40VertexDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_screenQuadDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_particleSpriteDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_particleSpriteDynamicDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_particleSubUVDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_particleSubUVDynamicDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_particleBeamTrailDeclaration;
std::unique_ptr<GuestVertexDeclaration> g_particleBeamTrailDynamicDeclaration;

enum class PipelineRejectReason {
  None,
  NoAttachments,
  MissingVertexShader,
  MissingVertexShaderCache,
  MissingHostVertexShader,
  MissingVertexDeclaration,
  CreateFailed,
};

PipelineRejectReason g_lastPipelineRejectReason = PipelineRejectReason::None;

constexpr uint64_t kSimpleElementVertexShaderHash = 0xE5478DE588433B53ull;

bool SceneReverseZ();

bool IsSimpleElementVertexStride(uint32_t vertexStride) {
  return vertexStride == 44 || vertexStride == 48;
}

uint32_t ReadGuestDeviceU32(GuestDevice *device, size_t offset) {
  if (device == nullptr)
    return 0;
  return reinterpret_cast<const rex::be<uint32_t> *>(
             reinterpret_cast<const uint8_t *>(device) + offset)
      ->get();
}

const char *KnownDeclarationName(GuestVertexDeclaration *declaration) {
  if (declaration == nullptr)
    return "null";
  if (g_simpleElementDeclaration &&
      declaration == g_simpleElementDeclaration.get())
    return "SimpleElement";
  if (g_texturedQuadDeclaration &&
      declaration == g_texturedQuadDeclaration.get())
    return "TexturedQuad";
  if (g_materialVertexDeclaration &&
      declaration == g_materialVertexDeclaration.get())
    return "MaterialTile";
  if (g_dynamicMeshVertexDeclaration &&
      declaration == g_dynamicMeshVertexDeclaration.get())
    return "DynamicMesh";
  if (g_batchedTriangleVertexDeclaration &&
      declaration == g_batchedTriangleVertexDeclaration.get())
    return "BatchedTriangle";
  if (g_gpuSkin40VertexDeclaration &&
      declaration == g_gpuSkin40VertexDeclaration.get())
    return "GpuSkin40";
  if (g_screenQuadDeclaration && declaration == g_screenQuadDeclaration.get())
    return "ScreenQuad";
  if (g_particleSpriteDeclaration &&
      declaration == g_particleSpriteDeclaration.get())
    return "ParticleSprite";
  if (g_particleSpriteDynamicDeclaration &&
      declaration == g_particleSpriteDynamicDeclaration.get())
    return "ParticleSpriteDynamic";
  if (g_particleSubUVDeclaration &&
      declaration == g_particleSubUVDeclaration.get())
    return "ParticleSubUV";
  if (g_particleSubUVDynamicDeclaration &&
      declaration == g_particleSubUVDynamicDeclaration.get())
    return "ParticleSubUVDynamic";
  if (g_particleBeamTrailDeclaration &&
      declaration == g_particleBeamTrailDeclaration.get())
    return "ParticleBeamTrail";
  if (g_particleBeamTrailDynamicDeclaration &&
      declaration == g_particleBeamTrailDynamicDeclaration.get())
    return "ParticleBeamTrailDynamic";
  return "Guest";
}

template <typename T> void SetDirtyValue(bool &dirty, T &dest, const T &src);

bool IsShadowIndexedUPDraw(uint32_t primitiveType, uint32_t minVertexIndex,
                           uint32_t numVertices, uint32_t numPrimitives,
                           uint32_t indexStride, const void *vertexData,
                           uint32_t vertexStride) {
  return primitiveType == D3DPT_TRIANGLELIST && minVertexIndex == 0 &&
         numVertices == 8 && numPrimitives == 12 && indexStride == 2 &&
         vertexStride == 12 && vertexData != nullptr;
}

void SyncShadowIndexedUPColorWrite(GuestDevice *device, uint32_t primitiveType,
                                   uint32_t minVertexIndex,
                                   uint32_t numVertices, uint32_t numPrimitives,
                                   uint32_t indexStride, const void *vertexData,
                                   uint32_t vertexStride) {
  if (!IsShadowIndexedUPDraw(primitiveType, minVertexIndex, numVertices,
                             numPrimitives, indexStride, vertexData,
                             vertexStride)) {
    return;
  }

  bool dirty = false;
  SetDirtyValue(dirty, g_pipelineState.colorWriteEnable,
                ReadGuestDeviceU32(device, 11852) & 0xF);
  if (g_pipelineState.colorWriteEnable == 0) {
    SetDirtyValue(dirty, g_pipelineState.alphaBlendEnable, false);
  }
  if (dirty) {
    g_dirtyStates.pipelineState = true;
    g_dirtyStates.renderTargetAndDepthStencil = true;
  }
}

template <typename T> void SetDirtyValue(bool &dirty, T &dest, const T &src) {
  if (dest != src) {
    dest = src;
    dirty = true;
  }
}

// ---------------------------------------------------------------------------
// Transient per-frame upload allocator (CONSTANT|VERTEX|INDEX upload heap).
// ---------------------------------------------------------------------------

struct UploadResult {
  RenderBuffer *buffer;
  uint64_t offset;
  uint8_t *memory;
};

struct UploadAllocator {
  static constexpr uint64_t kBufferSize = 16 * 1024 * 1024;

  struct Buffer {
    std::unique_ptr<RenderBuffer> buffer;
    uint8_t *memory = nullptr;
  };
  std::vector<Buffer> buffers;
  uint32_t index = 0;
  uint64_t offset = 0;

  void reset() {
    index = 0;
    offset = 0;
  }

  UploadResult allocate(uint64_t size, uint64_t alignment) {
    offset = (offset + alignment - 1) & ~(alignment - 1);
    if (offset + size > kBufferSize) {
      ++index;
      offset = 0;
    }
    if (buffers.size() <= index)
      buffers.resize(index + 1);

    Buffer &buf = buffers[index];
    if (buf.buffer == nullptr) {
      buf.buffer = Device()->createBuffer(RenderBufferDesc::UploadBuffer(
          kBufferSize, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX |
                           RenderBufferFlag::INDEX));
      buf.memory = reinterpret_cast<uint8_t *>(buf.buffer->map());
    }
    uint64_t at = offset;
    offset += size;
    return {buf.buffer.get(), at, buf.memory + at};
  }

  template <bool ByteSwap, typename T>
  UploadResult allocateCopy(const T *src, uint64_t size, uint64_t alignment) {
    UploadResult result = allocate(size, alignment);
    if constexpr (ByteSwap) {
      T *dst = reinterpret_cast<T *>(result.memory);
      for (uint64_t i = 0; i < size / sizeof(T); ++i)
        dst[i] = std::byteswap(src[i]);
    } else {
      std::memcpy(result.memory, src, size);
    }
    return result;
  }
};

UploadAllocator g_uploadAllocator;

UploadResult UploadGuestVertexData(const void *data, uint32_t size,
                                   uint64_t alignment) {
  UploadResult result = g_uploadAllocator.allocate(size, alignment);
  const uint8_t *srcBytes = reinterpret_cast<const uint8_t *>(data);
  uint8_t *dstBytes = result.memory;

  const uint32_t dwordCount = size / sizeof(uint32_t);
  const uint32_t *src = reinterpret_cast<const uint32_t *>(srcBytes);
  uint32_t *dst = reinterpret_cast<uint32_t *>(dstBytes);
  for (uint32_t i = 0; i < dwordCount; ++i)
    dst[i] = std::byteswap(src[i]);

  const uint32_t swappedSize = dwordCount * sizeof(uint32_t);
  if (swappedSize != size) {
    std::memcpy(dstBytes + swappedSize, srcBytes + swappedSize,
                size - swappedSize);
  }
  return result;
}

void MarkVertexStreamDirty(uint8_t index) {
  g_dirtyStates.vertexStreamFirst =
      std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, index);
  g_dirtyStates.vertexStreamLast =
      std::max<uint8_t>(g_dirtyStates.vertexStreamLast, index);
}

void EnsureInputSlotIndices() {
  for (uint32_t i = 0; i < std::size(g_inputSlots); ++i)
    g_inputSlots[i].index = i;
}

void EnsureDummyVertexStream() {
  constexpr uint8_t kDummyStream = 15;
  constexpr uint32_t kDefaultVertexInput = 0;

  if (g_pipelineState.vertexDeclaration == nullptr ||
      !g_pipelineState.vertexDeclaration->usesDummyVertexStream) {
    return;
  }

  if (g_dummyVertexBuffer == nullptr) {
    g_dummyVertexBuffer = Device()->createBuffer(RenderBufferDesc::UploadBuffer(
        sizeof(kDefaultVertexInput), RenderBufferFlag::VERTEX));
    if (g_dummyVertexBuffer != nullptr) {
      void *mapped = g_dummyVertexBuffer->map();
      std::memcpy(mapped, &kDefaultVertexInput, sizeof(kDefaultVertexInput));
      const RenderRange written(0, sizeof(kDefaultVertexInput));
      g_dummyVertexBuffer->unmap(0, &written);
    }
  }

  if (g_dummyVertexBuffer == nullptr)
    return;

  bool dirty = false;
  SetDirtyValue(dirty, g_vertexBufferViews[kDummyStream].buffer,
                g_dummyVertexBuffer->at(0));
  SetDirtyValue(dirty, g_vertexBufferViews[kDummyStream].size,
                uint32_t(sizeof(kDefaultVertexInput)));
  SetDirtyValue(dirty, g_inputSlots[kDummyStream].stride, 0u);
  if (dirty)
    MarkVertexStreamDirty(kDummyStream);
}

void SetRootDescriptor(const UploadResult &allocation, uint32_t index) {
  CommandList()->setGraphicsRootDescriptor(
      allocation.buffer->at(allocation.offset), index);
}

// ---------------------------------------------------------------------------
// Per-frame upload cache for genuine guest D3D vertex/index buffers (UE3 RHI
// builds those headers itself; the data lives in guest physical memory and is
// re-uploaded at most once per frame, keyed on its host pointer).
// ---------------------------------------------------------------------------

uint64_t g_frameIndex = 0;

struct GuestDataUpload {
  uint64_t frame = ~0ull;
  uint32_t size = 0;
  RenderBufferReference ref;
};

std::unordered_map<const void *, GuestDataUpload> g_guestVertexUploads;
std::unordered_map<const void *, GuestDataUpload> g_guestIndexUploads;

// ---------------------------------------------------------------------------
// Barriers
// ---------------------------------------------------------------------------

std::unordered_map<RenderTexture *, RenderTextureLayout> g_barrierMap;
std::vector<RenderTextureBarrier> g_barriers;
std::unordered_set<RenderTexture *> g_initializedAttachments;
std::unordered_set<RenderTexture *> g_pendingAttachmentDiscards;

RenderSampleCounts GetSampleCount(GuestBaseTexture *texture);

void MarkAttachmentInitialized(RenderTexture *texture) {
  if (texture != nullptr)
    g_initializedAttachments.insert(texture);
}

void MarkAttachmentInitialized(GuestBaseTexture *texture) {
  if (texture != nullptr) {
    texture->hostInitialized = true;
    MarkAttachmentInitialized(texture->texture);
  }
}

void QueueAttachmentDiscard(RenderTexture *texture) {
  if (texture == nullptr || g_initializedAttachments.contains(texture))
    return;
  g_initializedAttachments.insert(texture);
  g_pendingAttachmentDiscards.insert(texture);
}

void DiscardIfNeeded(RenderCommandList *commandList, RenderTexture *texture) {
  if (texture == nullptr || g_initializedAttachments.contains(texture))
    return;
  g_initializedAttachments.insert(texture);
  commandList->discardTexture(texture);
}

bool RequiresValidContents(RenderTextureLayout layout) {
  switch (layout) {
  case RenderTextureLayout::COPY_DEST:
  case RenderTextureLayout::RESOLVE_DEST:
    return false;
  default:
    return true;
  }
}

void QueueHostInitializationIfNeeded(GuestBaseTexture *texture,
                                     RenderTextureLayout layout) {
  if (texture == nullptr || texture->texture == nullptr ||
      !texture->requiresHostInitialization || texture->hostInitialized ||
      !RequiresValidContents(layout)) {
    return;
  }
  texture->hostInitialized = true;
  QueueAttachmentDiscard(texture->texture);
}

void AddBarrier(GuestBaseTexture *texture, RenderTextureLayout layout) {
  if (texture != nullptr && texture->texture != nullptr) {
    QueueHostInitializationIfNeeded(texture, layout);
    if (texture->layout == layout)
      return;
    g_barrierMap[texture->texture] = layout;
    texture->layout = layout;
  }
}

void FlushBarriers() {
  if (g_barrierMap.empty() && g_pendingAttachmentDiscards.empty())
    return;
  for (auto &[texture, layout] : g_barrierMap)
    g_barriers.emplace_back(texture, layout);
  RenderCommandList *commandList = CommandList();
  if (!g_barriers.empty()) {
    commandList->barriers(
        RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY, g_barriers);
  }
  for (RenderTexture *texture : g_pendingAttachmentDiscards)
    commandList->discardTexture(texture);
  g_pendingAttachmentDiscards.clear();
  g_barrierMap.clear();
  g_barriers.clear();
}

bool AddPendingStretchRectBarriers(GuestBaseTexture *surface) {
  if (surface == nullptr || surface->pendingResolves.empty())
    return false;

  RenderSampleCounts sampleCount = GetSampleCount(surface);
  AddBarrier(surface, sampleCount != RenderSampleCount::COUNT_1
                          ? RenderTextureLayout::RESOLVE_SOURCE
                          : RenderTextureLayout::COPY_SOURCE);
  for (const PendingResolve &resolve : surface->pendingResolves) {
    GuestBaseTexture *destination = resolve.destination;
    AddBarrier(destination, sampleCount != RenderSampleCount::COUNT_1
                                ? RenderTextureLayout::RESOLVE_DEST
                                : RenderTextureLayout::COPY_DEST);
  }
  return true;
}

struct ResolveBlitScratch {
  std::unique_ptr<RenderTexture> texture;
  std::unique_ptr<RenderFramebuffer> framebuffer;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
};
std::unordered_map<uint64_t, ResolveBlitScratch> g_resolveBlitScratch;

bool BlitFormatConvertedResolve(RenderCommandList *commandList,
                                GuestBaseTexture *source,
                                GuestBaseTexture *destination) {
  RenderPipeline *pipeline = GetBlitPipeline(destination->format);
  if (pipeline == nullptr || source->textureView == nullptr)
    return false;

  const uint64_t key = (uint64_t(destination->format) << 40) |
                       (uint64_t(destination->width) << 20) |
                       uint64_t(destination->height);
  ResolveBlitScratch &scratch = g_resolveBlitScratch[key];
  if (scratch.texture == nullptr) {
    RenderTextureDesc desc;
    desc.dimension = RenderTextureDimension::TEXTURE_2D;
    desc.width = destination->width;
    desc.height = destination->height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = destination->format;
    desc.flags = RenderTextureFlag::RENDER_TARGET;
    scratch.texture = Device()->createTexture(desc);
    if (scratch.texture == nullptr) {
      g_resolveBlitScratch.erase(key);
      return false;
    }
    const RenderTexture *color = scratch.texture.get();
    scratch.framebuffer =
        Device()->createFramebuffer(RenderFramebufferDesc(&color, 1));
  }

  EnsureShaderResourceDescriptor(source);
  AddBarrier(source, RenderTextureLayout::SHADER_READ);
  FlushBarriers();
  if (scratch.layout != RenderTextureLayout::COLOR_WRITE) {
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(scratch.texture.get(),
                             RenderTextureLayout::COLOR_WRITE));
    scratch.layout = RenderTextureLayout::COLOR_WRITE;
  }
  DiscardIfNeeded(commandList, scratch.texture.get());

  commandList->setPipeline(pipeline);
  commandList->setFramebuffer(scratch.framebuffer.get());
  commandList->setViewports(RenderViewport(
      0.0f, 0.0f, float(destination->width), float(destination->height)));
  commandList->setScissors(
      RenderRect(0, 0, destination->width, destination->height));
  const uint32_t descriptorIndex = source->descriptorIndex;
  commandList->setGraphicsPushConstants(0, &descriptorIndex, 0,
                                        sizeof(descriptorIndex));
  commandList->drawInstanced(3, 1, 0, 0);

  commandList->barriers(RenderBarrierStage::COPY,
                        RenderTextureBarrier(scratch.texture.get(),
                                             RenderTextureLayout::COPY_SOURCE));
  scratch.layout = RenderTextureLayout::COPY_SOURCE;
  commandList->copyTexture(destination->texture, scratch.texture.get());
  MarkAttachmentInitialized(destination);

  // The injected draw clobbered the framebuffer/viewport/scissor bindings;
  // force the next guest flush to rebind them.
  g_framebuffer = nullptr;
  g_dirtyStates.renderTargetAndDepthStencil = true;
  g_dirtyStates.viewport = true;
  g_dirtyStates.scissorRect = true;
  return true;
}

RenderRect FullResolveRect(GuestBaseTexture *source) {
  return RenderRect(0, 0, int32_t(source->width), int32_t(source->height));
}

bool IsFullSurfaceResolve(GuestBaseTexture *source,
                          GuestBaseTexture *destination,
                          const PendingResolve &resolve) {
  if (resolve.destX != 0 || resolve.destY != 0)
    return false;
  if (!resolve.hasSourceRect)
    return source->width == destination->width &&
           source->height == destination->height;
  return resolve.sourceRect == FullResolveRect(source) &&
         source->width == destination->width &&
         source->height == destination->height;
}

bool ClipResolveRegion(GuestBaseTexture *source, GuestBaseTexture *destination,
                       const PendingResolve &resolve, RenderRect &srcRect,
                       uint32_t &dstX, uint32_t &dstY) {
  srcRect =
      resolve.hasSourceRect ? resolve.sourceRect : FullResolveRect(source);
  dstX = resolve.destX;
  dstY = resolve.destY;

  srcRect.left = std::clamp(srcRect.left, int32_t(0), int32_t(source->width));
  srcRect.top = std::clamp(srcRect.top, int32_t(0), int32_t(source->height));
  srcRect.right = std::clamp(srcRect.right, srcRect.left,
                             int32_t(source->width));
  srcRect.bottom = std::clamp(srcRect.bottom, srcRect.top,
                              int32_t(source->height));

  const uint32_t srcWidth = uint32_t(srcRect.right - srcRect.left);
  const uint32_t srcHeight = uint32_t(srcRect.bottom - srcRect.top);
  if (srcWidth == 0 || srcHeight == 0 || dstX >= destination->width ||
      dstY >= destination->height)
    return false;

  const uint32_t copyWidth = std::min(srcWidth, destination->width - dstX);
  const uint32_t copyHeight = std::min(srcHeight, destination->height - dstY);
  srcRect.right = srcRect.left + int32_t(copyWidth);
  srcRect.bottom = srcRect.top + int32_t(copyHeight);
  return copyWidth != 0 && copyHeight != 0;
}

void CompletePendingResolve(GuestBaseTexture *source,
                            GuestBaseTexture *destination) {
  if (destination == nullptr)
    return;
  if (destination->pendingResolveCount > 0)
    --destination->pendingResolveCount;
  if (destination->pendingResolveCount == 0 &&
      destination->sourceTexture == source) {
    destination->sourceTexture = nullptr;
  }
}

void ExecutePendingStretchRects(GuestBaseTexture *surface) {
  if (surface == nullptr || surface->pendingResolves.empty())
    return;

  RenderSampleCounts sampleCount = GetSampleCount(surface);
  RenderCommandList *commandList = CommandList();
  std::vector<PendingResolve> pending = std::move(surface->pendingResolves);
  surface->pendingResolves.clear();
  for (const PendingResolve &resolve : pending) {
    GuestBaseTexture *destination = resolve.destination;
    if (destination == nullptr || destination->texture == nullptr) {
      CompletePendingResolve(surface, destination);
      continue;
    }
    RenderRect srcRect;
    uint32_t dstX = 0;
    uint32_t dstY = 0;
    if (!ClipResolveRegion(surface, destination, resolve, srcRect, dstX,
                           dstY)) {
      CompletePendingResolve(surface, destination);
      continue;
    }

    const bool fullSurface = IsFullSurfaceResolve(surface, destination, resolve);
    AddBarrier(destination, sampleCount != RenderSampleCount::COUNT_1
                                ? RenderTextureLayout::RESOLVE_DEST
                                : RenderTextureLayout::COPY_DEST);
    FlushBarriers();
    if (sampleCount != RenderSampleCount::COUNT_1) {
      if (fullSurface) {
        commandList->resolveTexture(destination->texture, surface->texture);
      } else {
        commandList->resolveTextureRegion(destination->texture, dstX, dstY,
                                          surface->texture, &srcRect);
      }
      MarkAttachmentInitialized(destination);
    } else if (surface->format != destination->format) {
      if (fullSurface &&
          !BlitFormatConvertedResolve(commandList, surface, destination)) {
      } else if (!fullSurface) {
      }
    } else {
      AddBarrier(surface, RenderTextureLayout::COPY_SOURCE);
      FlushBarriers();
      if (fullSurface) {
        commandList->copyTexture(destination->texture, surface->texture);
      } else {
        const RenderTextureCopyLocation dst =
            RenderTextureCopyLocation::Subresource(destination->texture, 0);
        const RenderTextureCopyLocation src =
            RenderTextureCopyLocation::Subresource(surface->texture, 0);
        const RenderBox srcBox(srcRect.left, srcRect.top, srcRect.right,
                               srcRect.bottom);
        commandList->copyTextureRegion(dst, src, dstX, dstY, 0, &srcBox);
      }
      MarkAttachmentInitialized(destination);
    }
    AddBarrier(destination, RenderTextureLayout::SHADER_READ);
    CompletePendingResolve(surface, destination);
    for (uint32_t i = 0; i < std::size(g_textures); ++i) {
      if (static_cast<GuestBaseTexture *>(g_textures[i]) == destination) {
        BindTextureDescriptor(i, destination,
                              RenderTextureViewDimension::TEXTURE_2D);
      }
    }
  }
  g_pendingStretchRectSurfaces.erase(surface);
}

void FlushPendingStretchRects(GuestBaseTexture *renderTarget,
                              GuestSurface *depthStencil) {
  std::vector<GuestBaseTexture *> surfaces(g_pendingStretchRectSurfaces.begin(),
                                           g_pendingStretchRectSurfaces.end());
  if (renderTarget != nullptr)
    surfaces.emplace_back(renderTarget);
  if (depthStencil != nullptr)
    surfaces.emplace_back(depthStencil);

  bool addedAny = false;
  for (GuestBaseTexture *surface : surfaces)
    addedAny |= AddPendingStretchRectBarriers(surface);
  if (!addedAny)
    return;

  FlushBarriers();
  for (GuestBaseTexture *surface : surfaces)
    ExecutePendingStretchRects(surface);
  FlushBarriers();
}

// ---------------------------------------------------------------------------
// Enum translation
// ---------------------------------------------------------------------------

RenderBlend ConvertBlendMode(uint32_t v) {
  switch (v) {
  case D3DBLEND_ZERO:
    return RenderBlend::ZERO;
  case D3DBLEND_ONE:
    return RenderBlend::ONE;
  case D3DBLEND_SRCCOLOR:
    return RenderBlend::SRC_COLOR;
  case D3DBLEND_INVSRCCOLOR:
    return RenderBlend::INV_SRC_COLOR;
  case D3DBLEND_SRCALPHA:
    return RenderBlend::SRC_ALPHA;
  case D3DBLEND_INVSRCALPHA:
    return RenderBlend::INV_SRC_ALPHA;
  case D3DBLEND_DESTCOLOR:
    return RenderBlend::DEST_COLOR;
  case D3DBLEND_INVDESTCOLOR:
    return RenderBlend::INV_DEST_COLOR;
  case D3DBLEND_DESTALPHA:
    return RenderBlend::DEST_ALPHA;
  case D3DBLEND_INVDESTALPHA:
    return RenderBlend::INV_DEST_ALPHA;
  default:
    return RenderBlend::ZERO;
  }
}

RenderBlendOperation ConvertBlendOp(uint32_t v) {
  switch (v) {
  case D3DBLENDOP_ADD:
    return RenderBlendOperation::ADD;
  case D3DBLENDOP_SUBTRACT:
    return RenderBlendOperation::SUBTRACT;
  case D3DBLENDOP_REVSUBTRACT:
    return RenderBlendOperation::REV_SUBTRACT;
  case D3DBLENDOP_MIN:
    return RenderBlendOperation::MIN;
  case D3DBLENDOP_MAX:
    return RenderBlendOperation::MAX;
  default:
    return RenderBlendOperation::ADD;
  }
}


constexpr uint32_t kD3DFMT_D24FS8 = 0x1A220197u;

bool SceneReverseZ() {
  return g_depthStencil != nullptr &&
         g_depthStencil->guestFormat == kD3DFMT_D24FS8;
}

RenderComparisonFunction FlipCmpFunc(RenderComparisonFunction f) {
  switch (f) {
  case RenderComparisonFunction::LESS:
    return RenderComparisonFunction::GREATER;
  case RenderComparisonFunction::LESS_EQUAL:
    return RenderComparisonFunction::GREATER_EQUAL;
  case RenderComparisonFunction::GREATER:
    return RenderComparisonFunction::LESS;
  case RenderComparisonFunction::GREATER_EQUAL:
    return RenderComparisonFunction::LESS_EQUAL;
  default:
    return f; // NEVER/EQUAL/NOT_EQUAL/ALWAYS are unaffected by Z direction
  }
}

RenderComparisonFunction ConvertCmpFunc(uint32_t v) {
  switch (v) {
  case D3DCMP_NEVER:
    return RenderComparisonFunction::NEVER;
  case D3DCMP_LESS:
    return RenderComparisonFunction::LESS;
  case D3DCMP_EQUAL:
    return RenderComparisonFunction::EQUAL;
  case D3DCMP_LESSEQUAL:
    return RenderComparisonFunction::LESS_EQUAL;
  case D3DCMP_GREATER:
    return RenderComparisonFunction::GREATER;
  case D3DCMP_NOTEQUAL:
    return RenderComparisonFunction::NOT_EQUAL;
  case D3DCMP_GREATEREQUAL:
    return RenderComparisonFunction::GREATER_EQUAL;
  case D3DCMP_ALWAYS:
    return RenderComparisonFunction::ALWAYS;
  default:
    return RenderComparisonFunction::NEVER;
  }
}

// Xenos RB_DEPTHCONTROL stencil-op fields hold EStencilOp values (1:1 with the
// 360 _D3DSTENCILOP enum, per TranslateStencilOp @ 0x824de838).
RenderStencilOp ConvertStencilOp(uint32_t v) {
  switch (v) {
  case 0: // SO_Keep
    return RenderStencilOp::KEEP;
  case 1: // SO_Zero
    return RenderStencilOp::ZERO;
  case 2: // SO_Replace
    return RenderStencilOp::REPLACE;
  case 3: // SO_SaturatedIncrement
    return RenderStencilOp::INCREMENT_AND_CLAMP;
  case 4: // SO_SaturatedDecrement
    return RenderStencilOp::DECREMENT_AND_CLAMP;
  case 5: // SO_Invert
    return RenderStencilOp::INVERT;
  case 6: // SO_Increment (wrap)
    return RenderStencilOp::INCREMENT_AND_WRAP;
  case 7: // SO_Decrement (wrap)
    return RenderStencilOp::DECREMENT_AND_WRAP;
  default:
    return RenderStencilOp::KEEP;
  }
}

RenderPrimitiveTopology ConvertPrimitiveType(uint32_t v) {
  switch (v) {
  case D3DPT_POINTLIST:
    return RenderPrimitiveTopology::POINT_LIST;
  case D3DPT_LINELIST:
    return RenderPrimitiveTopology::LINE_LIST;
  case D3DPT_LINESTRIP:
    return RenderPrimitiveTopology::LINE_STRIP;
  case D3DPT_TRIANGLELIST:
  case D3DPT_QUADLIST:
    return RenderPrimitiveTopology::TRIANGLE_LIST;
  case D3DPT_TRIANGLESTRIP:
    return RenderPrimitiveTopology::TRIANGLE_STRIP;
  case D3DPT_TRIANGLEFAN:
    return RenderPrimitiveTopology::TRIANGLE_LIST;
  default:
    return RenderPrimitiveTopology::TRIANGLE_LIST;
  }
}

RenderTextureAddressMode ConvertAddressMode(uint32_t v) {
  switch (v) {
  case D3DTADDRESS_WRAP:
    return RenderTextureAddressMode::WRAP;
  case D3DTADDRESS_MIRROR:
    return RenderTextureAddressMode::MIRROR;
  case D3DTADDRESS_CLAMP:
  case 4: // Xenos kClampToHalfway; no exact host equivalent.
    return RenderTextureAddressMode::CLAMP;
  case D3DTADDRESS_MIRRORONCE:
  case 5: // Xenos kMirrorClampToHalfway; no exact host equivalent.
  case 7: // Xenos kMirrorClampToBorder; D3D9/D3D12 normalize to mirror-once.
    return RenderTextureAddressMode::MIRROR_ONCE;
  case D3DTADDRESS_BORDER:
    return RenderTextureAddressMode::BORDER;
  default:
    return RenderTextureAddressMode::WRAP;
  }
}

RenderFilter ConvertFilter(uint32_t v) {
  switch (v) {
  case D3DTEXF_POINT:
  case D3DTEXF_NONE:
    return RenderFilter::NEAREST;
  case D3DTEXF_LINEAR:
    return RenderFilter::LINEAR;
  default:
    return RenderFilter::NEAREST;
  }
}

RenderBorderColor ConvertBorderColor(uint32_t v) {
  return v == 1 ? RenderBorderColor::OPAQUE_WHITE
                : RenderBorderColor::TRANSPARENT_BLACK;
}

const char *ConvertDeclUsage(uint32_t usage) {
  switch (usage) {
  case D3DDECLUSAGE_POSITION:
    return "POSITION";
  case D3DDECLUSAGE_BLENDWEIGHT:
    return "BLENDWEIGHT";
  case D3DDECLUSAGE_BLENDINDICES:
    return "BLENDINDICES";
  case D3DDECLUSAGE_NORMAL:
    return "NORMAL";
  case D3DDECLUSAGE_PSIZE:
    return "PSIZE";
  case D3DDECLUSAGE_TEXCOORD:
    return "TEXCOORD";
  case D3DDECLUSAGE_TANGENT:
    return "TANGENT";
  case D3DDECLUSAGE_BINORMAL:
    return "BINORMAL";
  case D3DDECLUSAGE_TESSFACTOR:
    return "TESSFACTOR";
  case D3DDECLUSAGE_POSITIONT:
    return "POSITIONT";
  case D3DDECLUSAGE_COLOR:
    return "COLOR";
  case D3DDECLUSAGE_FOG:
    return "FOG";
  case D3DDECLUSAGE_DEPTH:
    return "DEPTH";
  case D3DDECLUSAGE_SAMPLE:
    return "SAMPLE";
  default:
    return "UNKNOWN";
  }
}

RenderFormat ConvertDeclType(uint32_t type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return RenderFormat::R32_FLOAT;
  case D3DDECLTYPE_FLOAT2:
    return RenderFormat::R32G32_FLOAT;
  case D3DDECLTYPE_FLOAT3:
    return RenderFormat::R32G32B32_FLOAT;
  case D3DDECLTYPE_FLOAT4:
    return RenderFormat::R32G32B32A32_FLOAT;
  case D3DDECLTYPE_D3DCOLOR:
    return RenderFormat::B8G8R8A8_UNORM;
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_UBYTE4_2:
    return RenderFormat::R8G8B8A8_UINT;
  case D3DDECLTYPE_SHORT2:
    return RenderFormat::R16G16_SINT;
  case D3DDECLTYPE_SHORT4:
    return RenderFormat::R16G16B16A16_SINT;
  case D3DDECLTYPE_UBYTE4N:
  case D3DDECLTYPE_UBYTE4N_2:
    return RenderFormat::R8G8B8A8_UNORM;
  case D3DDECLTYPE_SHORT2N:
    return RenderFormat::R16G16_SNORM;
  case D3DDECLTYPE_SHORT4N:
    return RenderFormat::R16G16B16A16_SNORM;
  case D3DDECLTYPE_USHORT2N:
    return RenderFormat::R16G16_UNORM;
  case D3DDECLTYPE_USHORT4N:
    return RenderFormat::R16G16B16A16_UNORM;
  case D3DDECLTYPE_UINT1:
    return RenderFormat::R32_UINT;
  case D3DDECLTYPE_DEC3N_2:
  case D3DDECLTYPE_DEC3N_3:
    return RenderFormat::R32_UINT;
  case D3DDECLTYPE_FLOAT16_2:
    return RenderFormat::R16G16_FLOAT;
  case D3DDECLTYPE_FLOAT16_4:
    return RenderFormat::R16G16B16A16_FLOAT;
  default:
    return RenderFormat::UNKNOWN;
  }
}

RenderFormat ConvertPositionDeclType(uint32_t type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return RenderFormat::R32_UINT;
  case D3DDECLTYPE_FLOAT2:
    return RenderFormat::R32G32_UINT;
  case D3DDECLTYPE_FLOAT3:
    return RenderFormat::R32G32B32_UINT;
  case D3DDECLTYPE_FLOAT4:
    return RenderFormat::R32G32B32A32_UINT;
  default:
    return ConvertDeclType(type);
  }
}


struct SemanticLocation {
  uint32_t usage;
  uint32_t usageIndex;
  uint32_t location;
};

constexpr SemanticLocation kSemanticLocations[] = {
    {D3DDECLUSAGE_POSITION, 0, 0},      {D3DDECLUSAGE_NORMAL, 0, 1},
    {D3DDECLUSAGE_TANGENT, 0, 2},       {D3DDECLUSAGE_BINORMAL, 0, 3},
    {D3DDECLUSAGE_POSITION, 1, 4},      {D3DDECLUSAGE_TEXCOORD, 0, 13},
    {D3DDECLUSAGE_TEXCOORD, 1, 14},     {D3DDECLUSAGE_TEXCOORD, 2, 15},
    {D3DDECLUSAGE_TEXCOORD, 3, 16},     {D3DDECLUSAGE_COLOR, 0, 17},
    {D3DDECLUSAGE_COLOR, 1, 18},        {D3DDECLUSAGE_BLENDWEIGHT, 0, 19},
    {D3DDECLUSAGE_BLENDINDICES, 0, 20}, {D3DDECLUSAGE_TEXCOORD, 4, 21},
    {D3DDECLUSAGE_TEXCOORD, 5, 22},     {D3DDECLUSAGE_TEXCOORD, 6, 23},
    {D3DDECLUSAGE_TEXCOORD, 7, 24},
};

uint32_t LookupLocation(uint32_t usage, uint32_t usageIndex) {
  for (const auto &loc : kSemanticLocations)
    if (loc.usage == usage && loc.usageIndex == usageIndex)
      return loc.location;
  return ~0u;
}

void CompleteVertexDeclaration(GuestVertexDeclaration *decl) {
  if (decl->inputElements != nullptr || decl->vertexElements == nullptr)
    return;

  std::vector<RenderInputElement> inputElements;

  for (uint32_t i = 0; i < decl->vertexElementCount; ++i) {
    const GuestVertexElement &e = decl->vertexElements[i];
    if (e.stream == 0xFF || e.type == D3DDECLTYPE_UNUSED)
      break;
    if (e.usage == D3DDECLUSAGE_POSITION && e.usageIndex == 2)
      continue;

    RenderInputElement &ie = inputElements.emplace_back();
    ie.semanticName = ConvertDeclUsage(e.usage);
    ie.semanticIndex = e.usageIndex;
    ie.location = LookupLocation(e.usage, e.usageIndex);
    ie.format = ConvertDeclType(e.type);
    ie.slotIndex = e.stream;
    ie.alignedByteOffset = e.offset;

    switch (e.usage) {
    case D3DDECLUSAGE_POSITION:
      if (e.usageIndex == 0) {
        ie.format = ConvertPositionDeclType(e.type);
      }
      if (e.usageIndex == 1)
        decl->indexVertexStream = e.stream;
      break;
    case D3DDECLUSAGE_NORMAL:
    case D3DDECLUSAGE_TANGENT:
    case D3DDECLUSAGE_BINORMAL:
      if (e.type == D3DDECLTYPE_FLOAT3)
        ie.format = RenderFormat::R32G32B32_UINT;
      else {
        if (e.type == D3DDECLTYPE_UBYTE4 || e.type == D3DDECLTYPE_UBYTE4_2) {
          ie.format = RenderFormat::R8G8B8A8_UNORM;
          decl->hasUByte4TangentBasis = true;
        }
        decl->hasR11G11B10Normal = true;
      }
      break;
    case D3DDECLUSAGE_TEXCOORD:
      switch (e.type) {
      case D3DDECLTYPE_SHORT2:
      case D3DDECLTYPE_SHORT4:
      case D3DDECLTYPE_SHORT2N:
      case D3DDECLTYPE_SHORT4N:
      case D3DDECLTYPE_USHORT2N:
      case D3DDECLTYPE_USHORT4N:
      case D3DDECLTYPE_FLOAT16_2:
      case D3DDECLTYPE_FLOAT16_4:
        decl->swappedTexcoords |= 1u << e.usageIndex;
        break;
      }
      break;
    case D3DDECLUSAGE_BLENDWEIGHT:
      break;
    }
    decl->vertexStreams[e.stream] = true;
  }

  bool hasDummyElements = false;
  auto addDummyElement = [&](uint32_t usage, uint32_t usageIndex) {
    const uint32_t location = LookupLocation(usage, usageIndex);
    for (const RenderInputElement &ie : inputElements)
      if (ie.location == location)
        return;
    RenderFormat format = RenderFormat::R32_FLOAT;
    switch (usage) {
    case D3DDECLUSAGE_NORMAL:
    case D3DDECLUSAGE_TANGENT:
    case D3DDECLUSAGE_BINORMAL:
    case D3DDECLUSAGE_BLENDINDICES:
      format = RenderFormat::R32_UINT;
      break;
    }
    inputElements.emplace_back(ConvertDeclUsage(usage), usageIndex, location,
                               format, 15, 0);
    hasDummyElements = true;
  };
  addDummyElement(D3DDECLUSAGE_POSITION, 0);
  addDummyElement(D3DDECLUSAGE_NORMAL, 0);
  addDummyElement(D3DDECLUSAGE_TANGENT, 0);
  addDummyElement(D3DDECLUSAGE_BINORMAL, 0);
  for (uint32_t i = 0; i < 8; ++i)
    addDummyElement(D3DDECLUSAGE_TEXCOORD, i);
  addDummyElement(D3DDECLUSAGE_COLOR, 0);
  addDummyElement(D3DDECLUSAGE_COLOR, 1);
  addDummyElement(D3DDECLUSAGE_BLENDWEIGHT, 0);
  addDummyElement(D3DDECLUSAGE_BLENDINDICES, 0);
  decl->usesDummyVertexStream = hasDummyElements;
  if (hasDummyElements)
    decl->vertexStreams[15] = true;

  decl->inputElementCount = uint32_t(inputElements.size());
  decl->inputElements =
      std::make_unique<RenderInputElement[]>(inputElements.size());
  std::copy(inputElements.begin(), inputElements.end(),
            decl->inputElements.get());
}

// ---------------------------------------------------------------------------
// Pipeline build + cache
// ---------------------------------------------------------------------------

void SanitizePipelineState(PipelineState &ps) {
  if (!ps.zEnable) {
    ps.zWriteEnable = false;
    ps.zFunc = RenderComparisonFunction::LESS;
    ps.slopeScaledDepthBias = 0.0f;
    ps.depthBias = 0;
  }
  if (!ps.stencilEnable) {
    // Canonicalize so depth-only / stencil-off states collapse to one key.
    ps.stencilReadMask = 0xFF;
    ps.stencilWriteMask = 0xFF;
    ps.stencilRef = 0;
    ps.stencilFrontFunc = RenderComparisonFunction::ALWAYS;
    ps.stencilBackFunc = RenderComparisonFunction::ALWAYS;
    ps.stencilFrontFail = ps.stencilFrontDepthFail = ps.stencilFrontPass =
        RenderStencilOp::KEEP;
    ps.stencilBackFail = ps.stencilBackDepthFail = ps.stencilBackPass =
        RenderStencilOp::KEEP;
  }
  // The depth-stencil attachment is needed whenever depth OR stencil is in use;
  // only drop it (and the PSO's DSV) when neither is.
  if (!ps.zEnable && !ps.stencilEnable) {
    ps.depthStencilFormat = RenderFormat::UNKNOWN;
  }
  if (!ps.colorWriteEnable) {
    ps.alphaBlendEnable = false;
    ps.renderTargetFormat = RenderFormat::UNKNOWN;
  }
  if (!ps.alphaBlendEnable) {
    ps.srcBlend = RenderBlend::ONE;
    ps.destBlend = RenderBlend::ZERO;
    ps.blendOp = RenderBlendOperation::ADD;
    ps.srcBlendAlpha = RenderBlend::ONE;
    ps.destBlendAlpha = RenderBlend::ZERO;
    ps.blendOpAlpha = RenderBlendOperation::ADD;
  }

  if (ps.vertexDeclaration != nullptr) {
    for (uint32_t i = 0; i < 16; ++i)
      if (!ps.vertexDeclaration->vertexStreams[i])
        ps.vertexStrides[i] = 0;
  }

  uint32_t mask = 0;
  if (ps.vertexShader && ps.vertexShader->shaderCacheEntry)
    mask |= ps.vertexShader->shaderCacheEntry->spec_constants_mask;
  if (ps.pixelShader && ps.pixelShader->shaderCacheEntry)
    mask |= ps.pixelShader->shaderCacheEntry->spec_constants_mask;
  ps.specConstants &= mask;
}

std::unique_ptr<RenderPipeline>
CreateGraphicsPipeline(const PipelineState &ps) {
  if (ps.vertexShader == nullptr) {
    g_lastPipelineRejectReason = PipelineRejectReason::MissingVertexShader;
    return nullptr;
  }
  if (ps.vertexShader->shaderCacheEntry == nullptr) {
    g_lastPipelineRejectReason = PipelineRejectReason::MissingVertexShaderCache;
    return nullptr;
  }
  RenderShader *vertexShader = LoadShader(ps.vertexShader, ps.specConstants);
  if (vertexShader == nullptr) {
    g_lastPipelineRejectReason = PipelineRejectReason::MissingHostVertexShader;
    return nullptr;
  }
  if (ps.vertexDeclaration == nullptr) {
    g_lastPipelineRejectReason = PipelineRejectReason::MissingVertexDeclaration;
    return nullptr;
  }

  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = PipelineLayout();
  desc.vertexShader = vertexShader;
  desc.pixelShader =
      ps.pixelShader ? LoadShader(ps.pixelShader, ps.specConstants) : nullptr;
  desc.depthFunction = ps.zFunc;
  desc.depthEnabled = ps.zEnable;
  desc.depthWriteEnabled = ps.zWriteEnable;
  desc.depthBias = ps.depthBias;
  desc.slopeScaledDepthBias = ps.slopeScaledDepthBias;
  desc.depthClipEnabled = ps.depthClipEnabled;
  desc.primitiveTopology = ps.primitiveTopology;
  desc.cullMode = ps.cullMode;
  desc.renderTargetFormat[0] = ps.renderTargetFormat;
  desc.renderTargetBlend[0].blendEnabled = ps.alphaBlendEnable;
  desc.renderTargetBlend[0].srcBlend = ps.srcBlend;
  desc.renderTargetBlend[0].dstBlend = ps.destBlend;
  desc.renderTargetBlend[0].blendOp = ps.blendOp;
  desc.renderTargetBlend[0].srcBlendAlpha = ps.srcBlendAlpha;
  desc.renderTargetBlend[0].dstBlendAlpha = ps.destBlendAlpha;
  desc.renderTargetBlend[0].blendOpAlpha = ps.blendOpAlpha;
  desc.renderTargetBlend[0].renderTargetWriteMask =
      uint8_t(ps.colorWriteEnable);
  desc.renderTargetCount =
      ps.renderTargetFormat != RenderFormat::UNKNOWN ? 1 : 0;
  desc.depthTargetFormat = ps.depthStencilFormat;
  desc.stencilEnabled = ps.stencilEnable;
  desc.stencilReadMask = ps.stencilReadMask;
  desc.stencilWriteMask = ps.stencilWriteMask;
  desc.stencilReference = ps.stencilRef;
  desc.stencilFrontFace.compareFunction = ps.stencilFrontFunc;
  desc.stencilFrontFace.failOp = ps.stencilFrontFail;
  desc.stencilFrontFace.depthFailOp = ps.stencilFrontDepthFail;
  desc.stencilFrontFace.passOp = ps.stencilFrontPass;
  desc.stencilBackFace.compareFunction = ps.stencilBackFunc;
  desc.stencilBackFace.failOp = ps.stencilBackFail;
  desc.stencilBackFace.depthFailOp = ps.stencilBackDepthFail;
  desc.stencilBackFace.passOp = ps.stencilBackPass;
  desc.multisampling.sampleCount = ps.sampleCount;
  desc.alphaToCoverageEnabled = ps.enableAlphaToCoverage;
  desc.inputElements = ps.vertexDeclaration->inputElements.get();
  desc.inputElementsCount = ps.vertexDeclaration->inputElementCount;

  RenderSpecConstant specConstant{};
  specConstant.value = ps.specConstants;
  if (ps.specConstants != 0) {
    desc.specConstants = &specConstant;
    desc.specConstantsCount = 1;
  }

  RenderInputSlot inputSlots[16]{};
  uint32_t inputSlotIndices[16]{};
  uint32_t inputSlotCount = 0;
  for (uint32_t i = 0; i < ps.vertexDeclaration->inputElementCount; ++i) {
    const RenderInputElement &ie = ps.vertexDeclaration->inputElements[i];
    uint32_t &slotIndex = inputSlotIndices[ie.slotIndex];
    if (slotIndex == 0)
      slotIndex = ++inputSlotCount;

    RenderInputSlot &slot = inputSlots[slotIndex - 1];
    slot.index = ie.slotIndex;
    slot.stride = ps.vertexStrides[ie.slotIndex];
    slot.classification =
        (ps.instancing && ie.slotIndex != 0 && ie.slotIndex != 15)
            ? RenderInputSlotClassification::PER_INSTANCE_DATA
            : RenderInputSlotClassification::PER_VERTEX_DATA;
  }
  desc.inputSlots = inputSlots;
  desc.inputSlotsCount = inputSlotCount;

  auto pipeline = Device()->createGraphicsPipeline(desc);
  if (pipeline == nullptr)
    g_lastPipelineRejectReason = PipelineRejectReason::CreateFailed;
  return pipeline;
}

RenderPipeline *GetPipeline(PipelineState ps) {
  SanitizePipelineState(ps);
  g_lastPipelineRejectReason = PipelineRejectReason::None;
  if (ps.renderTargetFormat == RenderFormat::UNKNOWN &&
      ps.depthStencilFormat == RenderFormat::UNKNOWN) {
    g_lastPipelineRejectReason = PipelineRejectReason::NoAttachments;
    return nullptr;
  }
  uint64_t hash = XXH3_64bits(&ps, sizeof(ps));
  auto &pipeline = g_pipelines[hash];
  if (pipeline == nullptr)
    pipeline = CreateGraphicsPipeline(ps);
  return pipeline.get();
}

// ---------------------------------------------------------------------------
// Framebuffer + viewport
// ---------------------------------------------------------------------------

RenderSampleCounts GetSampleCount(GuestBaseTexture *texture) {
  if (texture != nullptr && (texture->type == ResourceType::RenderTarget ||
                             texture->type == ResourceType::DepthStencil))
    return static_cast<GuestSurface *>(texture)->sampleCount;
  return RenderSampleCount::COUNT_1;
}


struct OversizedColorScratch {
  std::unique_ptr<RenderTexture> texture;
};
std::unordered_map<uint64_t, OversizedColorScratch> g_oversizedColorScratch;

RenderTexture *GetOversizedDepthColorTarget(uint32_t width, uint32_t height,
                                            RenderFormat format) {
  const uint64_t key =
      (uint64_t(format) << 40) | (uint64_t(width) << 20) | uint64_t(height);
  OversizedColorScratch &scratch = g_oversizedColorScratch[key];
  if (scratch.texture == nullptr) {
    RenderTextureDesc desc;
    desc.dimension = RenderTextureDimension::TEXTURE_2D;
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = format;
    desc.flags = RenderTextureFlag::RENDER_TARGET;
    scratch.texture = Device()->createTexture(desc);
    if (scratch.texture == nullptr) {
      g_oversizedColorScratch.erase(key);
      return nullptr;
    }
    CommandList()->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(scratch.texture.get(),
                             RenderTextureLayout::COLOR_WRITE));
  }
  return scratch.texture.get();
}

void SetFramebuffer(GuestBaseTexture *renderTarget, GuestSurface *depthStencil,
                    bool forClear) {
  if (!forClear && !g_dirtyStates.renderTargetAndDepthStencil)
    return;

  GuestSurface *container = nullptr;
  RenderTexture *key = nullptr;
  if (renderTarget && depthStencil) {
    container = depthStencil;
    key = renderTarget->texture;
  } else if (renderTarget) {
    key = renderTarget->texture;
  } else if (depthStencil) {
    container = depthStencil;
    key = nullptr;
  }

  RenderCommandList *commandList = CommandList();
  if (container != nullptr) {
    auto &framebuffer = container->framebuffers[key];
    if (framebuffer == nullptr) {
      RenderFramebufferDesc desc;
      const RenderTexture *color =
          renderTarget ? renderTarget->texture : nullptr;
      if (renderTarget && depthStencil &&
          (depthStencil->height > renderTarget->height ||
           depthStencil->width > renderTarget->width)) {
        RenderTexture *scratch = GetOversizedDepthColorTarget(
            depthStencil->width, depthStencil->height, renderTarget->format);
        if (scratch != nullptr)
          color = scratch;
      }
      if (renderTarget) {
        desc.colorAttachments = &color;
        desc.colorAttachmentsCount = 1;
      }
      if (depthStencil)
        desc.depthAttachment = depthStencil->texture;
      framebuffer = Device()->createFramebuffer(desc);
    }
    if (g_framebuffer != framebuffer.get()) {
      commandList->setFramebuffer(framebuffer.get());
      g_framebuffer = framebuffer.get();
    }
  } else if (renderTarget != nullptr) {
    auto &framebuffer = g_colorFramebuffers[renderTarget->texture];
    if (framebuffer == nullptr) {
      const RenderTexture *color = renderTarget->texture;
      RenderFramebufferDesc desc(&color, 1);
      framebuffer = Device()->createFramebuffer(desc);
    }
    if (g_framebuffer != framebuffer.get()) {
      commandList->setFramebuffer(framebuffer.get());
      g_framebuffer = framebuffer.get();
    }
  } else if (g_framebuffer != nullptr) {
    commandList->setFramebuffer(nullptr);
    g_framebuffer = nullptr;
  }

  if (g_framebuffer != nullptr) {
    g_sharedConstants.halfPixelOffsetX =
        1.0f / float(g_framebuffer->getWidth());
    g_sharedConstants.halfPixelOffsetY =
        -1.0f / float(g_framebuffer->getHeight());
  }

  g_dirtyStates.renderTargetAndDepthStencil = forClear;
}

void FlushViewport() {
  RenderCommandList *commandList = CommandList();
  if (g_dirtyStates.viewport) {
    RenderViewport vp = g_viewport;
    if (SceneReverseZ()) {
      vp.minDepth = 1.0f;
      vp.maxDepth = 0.0f;
    }
    commandList->setViewports(vp);
    g_dirtyStates.viewport = false;
  }
  if (g_dirtyStates.scissorRect) {
    RenderRect rect =
        g_scissorTestEnable
            ? g_scissorRect
            : RenderRect(int32_t(g_viewport.x), int32_t(g_viewport.y),
                         int32_t(g_viewport.x + g_viewport.width),
                         int32_t(g_viewport.y + g_viewport.height));
    commandList->setScissors(rect);
    g_dirtyStates.scissorRect = false;
  }
}

// ---------------------------------------------------------------------------
// Per-draw constant + sampler upload
// ---------------------------------------------------------------------------

void FlushSamplerStates(GuestDevice *device) {
  for (uint32_t i = 0; i < 16; ++i) {
    const GuestSamplerState &s = device->samplerStates[i];
    uint32_t data0 = s.data[0].get();
    uint32_t data3 = s.data[3].get();
    uint32_t data5 = s.data[5].get();

    RenderSamplerDesc desc{};
    desc.addressU = ConvertAddressMode((data0 >> 10) & 0x7);
    desc.addressV = ConvertAddressMode((data0 >> 13) & 0x7);
    desc.addressW = ConvertAddressMode((data0 >> 16) & 0x7);
    desc.magFilter = ConvertFilter((data3 >> 19) & 0x3);
    desc.minFilter = ConvertFilter((data3 >> 21) & 0x3);
    desc.mipmapMode = RenderMipmapMode(ConvertFilter((data3 >> 23) & 0x3));
    desc.maxAnisotropy = 16;
    desc.anisotropyEnabled = false;
    desc.borderColor = ConvertBorderColor(data5 & 0x3);

    uint64_t hash = XXH3_64bits(&desc, sizeof(desc));
    auto &[descriptorIndex, sampler] = g_samplerStates[hash];
    if (sampler == nullptr) {
      descriptorIndex = uint32_t(g_samplerStates.size());
      sampler = Device()->createSampler(desc);
      SamplerDescriptorSet()->setSampler(descriptorIndex - 1, sampler.get());
    }
    g_sharedConstants.samplerIndices[i] = descriptorIndex - 1;
  }
}

void FlushRenderState(GuestDevice *device) {
  EnsureInputSlotIndices();
  EnsureDummyVertexStream();

  GuestBaseTexture *renderTarget =
      g_pipelineState.colorWriteEnable ? g_renderTarget : nullptr;
  GuestSurface *depthStencil =
      (g_pipelineState.zEnable || g_pipelineState.stencilEnable)
          ? g_depthStencil
          : nullptr;
  if (depthStencil == nullptr &&
      (g_pipelineState.zEnable || g_pipelineState.stencilEnable) &&
      g_renderTarget && g_implicitDepthStencil != nullptr &&
      g_implicitDepthStencil->width == g_renderTarget->width &&
      g_implicitDepthStencil->height == g_renderTarget->height) {
    depthStencil = g_implicitDepthStencil;
    SetDirtyValue(g_dirtyStates.pipelineState,
                  g_pipelineState.depthStencilFormat, depthStencil->format);
    g_dirtyStates.renderTargetAndDepthStencil = true;
  }

  FlushPendingStretchRects(renderTarget, depthStencil);

  AddBarrier(renderTarget, RenderTextureLayout::COLOR_WRITE);
  AddBarrier(depthStencil, RenderTextureLayout::DEPTH_WRITE);
  FlushBarriers();

  SetFramebuffer(renderTarget, depthStencil, false);
  FlushViewport();

  RenderCommandList *commandList = CommandList();

  PipelineState pipelineState = g_pipelineState;
  if (depthStencil == nullptr) {
    pipelineState.zEnable = false;
    pipelineState.zWriteEnable = false;
    pipelineState.stencilEnable = false;
    pipelineState.depthStencilFormat = RenderFormat::UNKNOWN;
  }
  RenderPipeline *pipeline = GetPipeline(pipelineState);
  if (pipeline != nullptr)
    commandList->setPipeline(pipeline);
  g_pipelineBound = (pipeline != nullptr);

  // Booleans and sampler indices feed the shared constants (16 bits each in
  // the XenosRecomp ABI).
  g_sharedConstants.booleans =
      (device->vertexShaderBoolConstants[0].get() & 0xFFFF) |
      ((device->pixelShaderBoolConstants[0].get() & 0xFFFF) << 16);
  FlushSamplerStates(device);

  // Constants are byte-swapped out of guest memory each draw (no dirty-range
  // tracking; the guest writes them directly to device memory).
  SetRootDescriptor(g_uploadAllocator.allocateCopy<true>(
                        device->vertexShaderFloatConstants,
                        sizeof(device->vertexShaderFloatConstants), 0x100),
                    0);
  SetRootDescriptor(
      g_uploadAllocator.allocateCopy<true>(device->pixelShaderFloatConstants,
                                           0x380 * sizeof(uint32_t), 0x100),
      1);
  SetRootDescriptor(g_uploadAllocator.allocateCopy<false>(
                        &g_sharedConstants, sizeof(g_sharedConstants), 0x100),
                    2);

  if (g_dirtyStates.vertexStreamFirst <= g_dirtyStates.vertexStreamLast) {
    commandList->setVertexBuffers(
        g_dirtyStates.vertexStreamFirst,
        g_vertexBufferViews + g_dirtyStates.vertexStreamFirst,
        g_dirtyStates.vertexStreamLast - g_dirtyStates.vertexStreamFirst + 1,
        g_inputSlots + g_dirtyStates.vertexStreamFirst);
  }
  if (g_dirtyStates.indices && g_indexBufferView.buffer.ref != nullptr)
    commandList->setIndexBuffer(&g_indexBufferView);

  if (g_pipelineBound && renderTarget != nullptr)
    g_lastTouchedRenderTarget = renderTarget;

  g_dirtyStates = DirtyStates(false);
}

void SetAlphaTestMode(bool enable) {
  uint32_t specConstants = enable ? SPEC_CONSTANT_ALPHA_TEST : 0;
  specConstants |=
      g_pipelineState.specConstants &
      ~(SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.specConstants,
                specConstants);
}

// QUADLIST / TRIANGLEFAN re-index helper (builds 16-bit index data on demand).
template <uint32_t PrimitiveType> struct PrimitiveIndexData {
  std::vector<uint16_t> indexData;

  uint32_t prepare(uint32_t guestPrimCount) {
    uint32_t primCount;
    uint32_t indexCountPerPrimitive;
    if constexpr (PrimitiveType == D3DPT_TRIANGLEFAN) {
      primCount = guestPrimCount - 2;
      indexCountPerPrimitive = 3;
    } else {
      primCount = guestPrimCount / 4;
      indexCountPerPrimitive = 6;
    }
    uint32_t indexCount = primCount * indexCountPerPrimitive;

    if (indexData.size() < indexCount) {
      size_t oldPrimCount = indexData.size() / indexCountPerPrimitive;
      indexData.resize(indexCount);
      for (size_t i = oldPrimCount; i < primCount; ++i) {
        if constexpr (PrimitiveType == D3DPT_TRIANGLEFAN) {
          indexData[i * 3 + 0] = 0;
          indexData[i * 3 + 1] = uint16_t(i + 1);
          indexData[i * 3 + 2] = uint16_t(i + 2);
        } else {
          indexData[i * 6 + 0] = uint16_t(i * 4 + 0);
          indexData[i * 6 + 1] = uint16_t(i * 4 + 1);
          indexData[i * 6 + 2] = uint16_t(i * 4 + 2);
          indexData[i * 6 + 3] = uint16_t(i * 4 + 0);
          indexData[i * 6 + 4] = uint16_t(i * 4 + 2);
          indexData[i * 6 + 5] = uint16_t(i * 4 + 3);
        }
      }
    }

    UploadResult allocation = g_uploadAllocator.allocateCopy<false>(
        indexData.data(), indexCount * 2, 2);
    g_indexBufferView.buffer = allocation.buffer->at(allocation.offset);
    g_indexBufferView.size = indexCount * 2;
    g_indexBufferView.format = RenderFormat::R16_UINT;
    g_dirtyStates.indices = true;
    return indexCount;
  }
};

PrimitiveIndexData<D3DPT_TRIANGLEFAN> g_triangleFanIndexData;
PrimitiveIndexData<D3DPT_QUADLIST> g_quadIndexData;

void SetPrimitiveType(uint32_t primitiveType) {
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.primitiveTopology,
                ConvertPrimitiveType(primitiveType));
}

void SyncVertexDeclarationFromDevice(GuestDevice *device) {
  if (device == nullptr)
    return;
  uint32_t guestDeclaration = device->vertexDeclaration.get();
  if (guestDeclaration == 0)
    return;
  GuestVertexDeclaration *declaration =
      ghp::ToHost<GuestVertexDeclaration>(guestDeclaration);
  if (!IsReoResource(declaration))
    declaration = LookupVertexDeclarationAlias(guestDeclaration);
  SetVertexDeclaration(device, declaration);
}

void RestoreVertexDeclarationForShader(GuestDevice *device) {
  if (g_pipelineState.vertexDeclaration != nullptr ||
      g_pipelineState.vertexShader == nullptr)
    return;
  auto it = g_vertexShaderDeclarations.find(g_pipelineState.vertexShader);
  if (it != g_vertexShaderDeclarations.end())
    SetVertexDeclaration(device, it->second);
}

GuestVertexDeclaration *SimpleElementDeclaration() {
  if (g_simpleElementDeclaration != nullptr)
    return g_simpleElementDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 4;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(5);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {
      0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[2] = {0, 24, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_COLOR,
                             0, 0};
  decl->vertexElements[3] = {0, 40, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR,
                             1, 0};
  decl->vertexElements[4] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_simpleElementDeclaration = std::move(decl);
  return g_simpleElementDeclaration.get();
}

GuestVertexDeclaration *TexturedQuadDeclaration() {
  if (g_texturedQuadDeclaration != nullptr)
    return g_texturedQuadDeclaration.get();

  // UE3 canvas/text vertex (20 bytes): FVector2D Position, FColor Color,
  // FVector2D UV.
  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 3;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(4);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {0, 8, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR,
                             0, 0};
  decl->vertexElements[2] = {
      0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[3] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_texturedQuadDeclaration = std::move(decl);
  return g_texturedQuadDeclaration.get();
}

GuestVertexDeclaration *MaterialVertexDeclaration() {
  if (g_materialVertexDeclaration != nullptr)
    return g_materialVertexDeclaration.get();

  // UE3 FMaterialTileVertex (32 bytes): FVector Position, FPackedNormal
  // TangentX, FPackedNormal TangentZ, DWORD Color, FVector2D UV.
  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 5;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(6);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {
      0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_TANGENT, 0, 0};
  decl->vertexElements[2] = {
      0, 16, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_NORMAL, 0, 0};
  decl->vertexElements[3] = {0, 20, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR,
                             0, 0};
  decl->vertexElements[4] = {
      0, 24, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[5] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_materialVertexDeclaration = std::move(decl);
  return g_materialVertexDeclaration.get();
}

GuestVertexDeclaration *DynamicMeshVertexDeclaration() {
  if (g_dynamicMeshVertexDeclaration != nullptr)
    return g_dynamicMeshVertexDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 5;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(6);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {
      0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[2] = {
      0, 20, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_TANGENT, 0, 0};
  decl->vertexElements[3] = {
      0, 24, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_NORMAL, 0, 0};
  decl->vertexElements[4] = {0, 28, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR,
                             0, 0};
  decl->vertexElements[5] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_dynamicMeshVertexDeclaration = std::move(decl);
  return g_dynamicMeshVertexDeclaration.get();
}

GuestVertexDeclaration *BatchedTriangleVertexDeclaration() {
  if (g_batchedTriangleVertexDeclaration != nullptr)
    return g_batchedTriangleVertexDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 6;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(7);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {
      0, 16, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_TANGENT, 0, 0};
  decl->vertexElements[2] = {
      0, 20, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BINORMAL, 0, 0};
  decl->vertexElements[3] = {
      0, 24, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_NORMAL, 0, 0};
  decl->vertexElements[4] = {0, 28, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR,
                             0, 0};
  decl->vertexElements[5] = {
      0, 32, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[6] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_batchedTriangleVertexDeclaration = std::move(decl);
  return g_batchedTriangleVertexDeclaration.get();
}

GuestVertexDeclaration *GpuSkin40VertexDeclaration() {
  if (g_gpuSkin40VertexDeclaration != nullptr)
    return g_gpuSkin40VertexDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 7;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(8);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {
      0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_TANGENT, 0, 0};
  decl->vertexElements[2] = {
      0, 16, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_NORMAL, 0, 0};
  decl->vertexElements[3] = {
      0, 20, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BINORMAL, 0, 0};
  decl->vertexElements[4] = {
      0, 24, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[5] = {
      0, 32, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0, 0};
  decl->vertexElements[6] = {
      0, 36, D3DDECLTYPE_UBYTE4N, 0, D3DDECLUSAGE_BLENDWEIGHT, 0, 0};
  decl->vertexElements[7] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_gpuSkin40VertexDeclaration = std::move(decl);
  return g_gpuSkin40VertexDeclaration.get();
}

GuestVertexDeclaration *ScreenQuadDeclaration() {
  if (g_screenQuadDeclaration != nullptr)
    return g_screenQuadDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 4;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(5);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {
      0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[2] = {0, 24, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR,
                             0, 0};
  decl->vertexElements[3] = {
      0, 24, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 1, 0};
  decl->vertexElements[4] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_screenQuadDeclaration = std::move(decl);
  return g_screenQuadDeclaration.get();
}

GuestVertexDeclaration *ParticleSpriteDeclaration() {
  if (g_particleSpriteDeclaration != nullptr)
    return g_particleSpriteDeclaration.get();

  // Xbox UE3 has PARTICLES_USE_INDEXED_SPRITES enabled:
  // FParticleSpriteVertex is 60 bytes and omits per-corner TEXCOORD0.
  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 5;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(6);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL,
                             0, 0};
  decl->vertexElements[2] = {0, 24, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TANGENT,
                             0, 0};
  decl->vertexElements[3] = {
      0, 36, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0, 0};
  decl->vertexElements[4] = {
      0, 44, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1, 0};
  decl->vertexElements[5] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_particleSpriteDeclaration = std::move(decl);
  return g_particleSpriteDeclaration.get();
}

GuestVertexDeclaration *ParticleSpriteDynamicDeclaration() {
  if (g_particleSpriteDynamicDeclaration != nullptr)
    return g_particleSpriteDynamicDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 6;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(7);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL,
                             0, 0};
  decl->vertexElements[2] = {0, 24, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TANGENT,
                             0, 0};
  decl->vertexElements[3] = {
      0, 36, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0, 0};
  decl->vertexElements[4] = {
      0, 44, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1, 0};
  decl->vertexElements[5] = {
      0, 60, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 3, 0};
  decl->vertexElements[6] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_particleSpriteDynamicDeclaration = std::move(decl);
  return g_particleSpriteDynamicDeclaration.get();
}

GuestVertexDeclaration *ParticleSubUVDeclaration() {
  if (g_particleSubUVDeclaration != nullptr)
    return g_particleSubUVDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 7;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(8);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL,
                             0, 0};
  decl->vertexElements[2] = {0, 24, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TANGENT,
                             0, 0};
  decl->vertexElements[3] = {
      0, 36, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0, 0};
  decl->vertexElements[4] = {
      0, 44, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1, 0};
  decl->vertexElements[5] = {
      0, 60, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[6] = {
      0, 76, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 2, 0};
  decl->vertexElements[7] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_particleSubUVDeclaration = std::move(decl);
  return g_particleSubUVDeclaration.get();
}

GuestVertexDeclaration *ParticleSubUVDynamicDeclaration() {
  if (g_particleSubUVDynamicDeclaration != nullptr)
    return g_particleSubUVDynamicDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 8;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(9);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL,
                             0, 0};
  decl->vertexElements[2] = {0, 24, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TANGENT,
                             0, 0};
  decl->vertexElements[3] = {
      0, 36, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0, 0};
  decl->vertexElements[4] = {
      0, 44, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1, 0};
  decl->vertexElements[5] = {
      0, 60, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[6] = {
      0, 76, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 2, 0};
  decl->vertexElements[7] = {
      0, 92, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 3, 0};
  decl->vertexElements[8] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_particleSubUVDynamicDeclaration = std::move(decl);
  return g_particleSubUVDynamicDeclaration.get();
}

GuestVertexDeclaration *ParticleBeamTrailDeclaration() {
  if (g_particleBeamTrailDeclaration != nullptr)
    return g_particleBeamTrailDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 6;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(7);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL,
                             0, 0};
  decl->vertexElements[2] = {0, 24, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TANGENT,
                             0, 0};
  decl->vertexElements[3] = {
      0, 36, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0, 0};
  decl->vertexElements[4] = {
      0, 44, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1, 0};
  decl->vertexElements[5] = {
      0, 60, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[6] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_particleBeamTrailDeclaration = std::move(decl);
  return g_particleBeamTrailDeclaration.get();
}

GuestVertexDeclaration *ParticleBeamTrailDynamicDeclaration() {
  if (g_particleBeamTrailDynamicDeclaration != nullptr)
    return g_particleBeamTrailDynamicDeclaration.get();

  auto decl = std::make_unique<GuestVertexDeclaration>();
  decl->vertexElementCount = 7;
  decl->vertexElements = std::make_unique<GuestVertexElement[]>(8);
  decl->vertexElements[0] = {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION,
                             0, 0};
  decl->vertexElements[1] = {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL,
                             0, 0};
  decl->vertexElements[2] = {0, 24, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TANGENT,
                             0, 0};
  decl->vertexElements[3] = {
      0, 36, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0, 0};
  decl->vertexElements[4] = {
      0, 44, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1, 0};
  decl->vertexElements[5] = {
      0, 60, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0, 0};
  decl->vertexElements[6] = {
      0, 76, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 2, 0};
  decl->vertexElements[7] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0, 0};
  decl->vertexStreams[0] = true;

  g_particleBeamTrailDynamicDeclaration = std::move(decl);
  return g_particleBeamTrailDynamicDeclaration.get();
}

GuestVertexDeclaration *
SelectParticleVertexDeclarationByStride(uint32_t primitiveType,
                                        uint32_t vertexStride) {
  const bool triangleList = primitiveType == D3DPT_TRIANGLELIST;
  const bool triangleStrip = primitiveType == D3DPT_TRIANGLESTRIP;

  if (triangleStrip) {
    if (vertexStride == 76)
      return ParticleBeamTrailDeclaration();
    if (vertexStride == 92)
      return ParticleBeamTrailDynamicDeclaration();
  } else if (triangleList) {
    if (vertexStride == 60)
      return ParticleSpriteDeclaration();
    if (vertexStride == 76)
      return ParticleSpriteDynamicDeclaration();
    if (vertexStride == 92)
      return ParticleSubUVDeclaration();
    if (vertexStride == 108)
      return ParticleSubUVDynamicDeclaration();
  }

  return nullptr;
}

bool SelectVertexDeclaration(GuestDevice *device,
                             GuestVertexDeclaration *substitute,
                             GuestVertexDeclaration **previous) {
  if (substitute == nullptr || g_pipelineState.vertexDeclaration == substitute)
    return false;

  *previous = g_pipelineState.vertexDeclaration;
  SetVertexDeclaration(device, substitute);
  return true;
}


GuestVertexDeclaration *SelectStride32QuadDeclaration(const void *vertexData,
                                                      uint32_t minVertexIndex,
                                                      uint32_t numVertices) {
  if (vertexData == nullptr)
    return MaterialVertexDeclaration();
  auto dwordAt = [vertexData, minVertexIndex](uint32_t vertex, uint32_t word) {
    const uint32_t *v = reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(vertexData) +
        size_t(minVertexIndex + vertex) * 32);
    return std::byteswap(v[word]);
  };
  const uint32_t *v = reinterpret_cast<const uint32_t *>(
      reinterpret_cast<const uint8_t *>(vertexData) +
      size_t(minVertexIndex) * 32);
  const uint32_t stride = 32 / 4;
  bool pos4 = std::byteswap(v[3]) == 0x3F800000u;
  if (numVertices > 1)
    pos4 = pos4 && std::byteswap(v[stride + 3]) == 0x3F800000u;
  if (pos4 && numVertices >= 4) {
    bool words16Vary = false;
    bool words24Vary = false;
    const uint32_t count = std::min<uint32_t>(numVertices, 4);
    const uint32_t word4 = dwordAt(0, 4);
    const uint32_t word5 = dwordAt(0, 5);
    const uint32_t word6 = dwordAt(0, 6);
    const uint32_t word7 = dwordAt(0, 7);
    for (uint32_t i = 1; i < count; ++i) {
      words16Vary |= dwordAt(i, 4) != word4 || dwordAt(i, 5) != word5;
      words24Vary |= dwordAt(i, 6) != word6 || dwordAt(i, 7) != word7;
    }
    if (!words16Vary && words24Vary)
      return MaterialVertexDeclaration();
  }
  return pos4 ? ScreenQuadDeclaration() : MaterialVertexDeclaration();
}

bool IsLegacyTexturedQuadDeclaration(GuestVertexDeclaration *declaration) {
  if (declaration == nullptr || declaration->vertexElementCount < 3)
    return false;

  const GuestVertexElement &position = declaration->vertexElements[0];
  const GuestVertexElement &color = declaration->vertexElements[1];
  const GuestVertexElement &texcoord = declaration->vertexElements[2];
  return position.stream == 0 && position.offset == 0 &&
         position.type == D3DDECLTYPE_FLOAT2 &&
         position.usage == D3DDECLUSAGE_POSITION && position.usageIndex == 0 &&
         color.stream == 0 && color.offset == 8 &&
         color.type == D3DDECLTYPE_D3DCOLOR &&
         color.usage == D3DDECLUSAGE_COLOR && color.usageIndex == 0 &&
         texcoord.stream == 0 && texcoord.offset == 12 &&
         texcoord.type == D3DDECLTYPE_FLOAT2 &&
         texcoord.usage == D3DDECLUSAGE_TEXCOORD && texcoord.usageIndex == 0;
}

bool SelectIndexedMeshDeclarationForStaleCanvasDecl(
    GuestDevice *device, uint32_t primitiveType, uint32_t indexCount,
    uint32_t vertexStride, GuestVertexDeclaration **previous) {
  if (primitiveType != D3DPT_TRIANGLELIST || vertexStride != 40)
    return false;
  if (!IsLegacyTexturedQuadDeclaration(g_pipelineState.vertexDeclaration))
    return false;

  GuestVertexDeclaration *declaration = indexCount == 336
                                            ? BatchedTriangleVertexDeclaration()
                                            : DynamicMeshVertexDeclaration();
  *previous = g_pipelineState.vertexDeclaration;
  SetVertexDeclaration(device, declaration);
  return true;
}

bool IsGpuSkin40DeclarationWithPackedTexcoord(
    GuestVertexDeclaration *declaration) {
  if (declaration == nullptr)
    return false;

  bool hasPosition = false;
  bool hasPackedTexcoord = false;
  bool hasBlendIndices = false;
  bool hasBlendWeight = false;
  for (uint32_t i = 0; i < declaration->vertexElementCount; ++i) {
    const GuestVertexElement &e = declaration->vertexElements[i];
    if (e.stream == 0xFF || e.type == D3DDECLTYPE_UNUSED)
      break;
    hasPosition |= e.stream == 0 && e.offset == 0 &&
                   e.type == D3DDECLTYPE_FLOAT3 &&
                   e.usage == D3DDECLUSAGE_POSITION && e.usageIndex == 0;
    hasPackedTexcoord |= e.stream == 0 && e.offset == 12 &&
                         e.type == D3DDECLTYPE_FLOAT2 &&
                         e.usage == D3DDECLUSAGE_TEXCOORD && e.usageIndex == 0;
    hasBlendIndices |=
        e.stream == 0 && e.offset == 32 && e.type == D3DDECLTYPE_UBYTE4 &&
        e.usage == D3DDECLUSAGE_BLENDINDICES && e.usageIndex == 0;
    hasBlendWeight |= e.stream == 0 && e.offset == 36 &&
                      e.type == D3DDECLTYPE_UBYTE4N &&
                      e.usage == D3DDECLUSAGE_BLENDWEIGHT && e.usageIndex == 0;
  }
  return hasPosition && hasPackedTexcoord && hasBlendIndices && hasBlendWeight;
}

bool SelectGpuSkin40Declaration(GuestDevice *device, uint32_t primitiveType,
                                uint32_t vertexStride,
                                GuestVertexDeclaration **previous) {
  if (primitiveType != D3DPT_TRIANGLELIST || vertexStride != 40)
    return false;
  if (!IsGpuSkin40DeclarationWithPackedTexcoord(
          g_pipelineState.vertexDeclaration))
    return false;

  *previous = g_pipelineState.vertexDeclaration;
  SetVertexDeclaration(device, GpuSkin40VertexDeclaration());
  return true;
}

bool SelectTexturedQuadVertexDeclaration(GuestDevice *device,
                                         uint32_t primitiveType,
                                         uint32_t primitiveCount,
                                         GuestVertexDeclaration **previous) {
  if (!((primitiveType == D3DPT_TRIANGLEFAN ||
         primitiveType == D3DPT_TRIANGLESTRIP) &&
        (primitiveCount == 4 || primitiveCount == 6))) {
    return false;
  }
  if (!IsLegacyTexturedQuadDeclaration(g_pipelineState.vertexDeclaration))
    return false;

  *previous = g_pipelineState.vertexDeclaration;
  SetVertexDeclaration(device, TexturedQuadDeclaration());
  return true;
}

bool SelectShaderVertexDeclaration(GuestDevice *device, uint32_t vertexStride,
                                   GuestVertexDeclaration **previous) {
  *previous = nullptr;
  if (g_pipelineState.vertexShader == nullptr ||
      g_pipelineState.vertexShader->shaderCacheEntry == nullptr) {
    return false;
  }

  const uint64_t hash = g_pipelineState.vertexShader->shaderCacheEntry->hash;
  if (hash == kSimpleElementVertexShaderHash &&
      IsSimpleElementVertexStride(vertexStride)) {
    *previous = g_pipelineState.vertexDeclaration;
    SetVertexDeclaration(device, SimpleElementDeclaration());
    return true;
  }
  return false;
}


bool SelectQuadVertexDeclarationByStride(GuestDevice *device,
                                         uint32_t primitiveType,
                                         uint32_t primitiveCount,
                                         uint32_t vertexStride,
                                         GuestVertexDeclaration **previous,
                                         const void *vertexData = nullptr) {
  const bool singleQuad = (primitiveType == D3DPT_TRIANGLEFAN ||
                           primitiveType == D3DPT_TRIANGLESTRIP) &&
                          (primitiveCount == 4 || primitiveCount == 6);
  const bool textBatch = vertexStride == 20 &&
                         primitiveType == D3DPT_TRIANGLELIST &&
                         primitiveCount >= 6 && primitiveCount % 6 == 0;
  if (!singleQuad && !textBatch) {
    return false;
  }
  GuestVertexDeclaration *substitute = nullptr;
  if (vertexStride == 20) {
    substitute = TexturedQuadDeclaration();
  } else if (vertexStride == 32) {
    // With guest data available (UP draws), sniff apart the two 32-byte quad
    // structs; otherwise assume FMaterialTileVertex.
    substitute =
        vertexData != nullptr
            ? SelectStride32QuadDeclaration(vertexData, 0, primitiveCount)
            : MaterialVertexDeclaration();
  } else if (IsSimpleElementVertexStride(vertexStride)) {
    substitute = SimpleElementDeclaration();
  }
  if (substitute == nullptr || g_pipelineState.vertexDeclaration == substitute)
    return false;

  *previous = g_pipelineState.vertexDeclaration;
  SetVertexDeclaration(device, substitute);
  return true;
}

// Instancing: the index buffer is fed as a vertex stream (POSITION1 stream).
uint32_t CheckInstancing() {
  uint32_t indexCount = 0;
  if (g_pipelineState.vertexDeclaration == nullptr)
    return 0;
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.instancing,
                g_pipelineState.vertexDeclaration->indexVertexStream != 0);
  if (g_pipelineState.instancing)
    indexCount = g_vertexBufferViews[g_pipelineState.vertexDeclaration
                                         ->indexVertexStream]
                     .size /
                 4;
  return indexCount;
}

void UnsetInstancingStream() {
  if (g_pipelineState.vertexDeclaration == nullptr)
    return;
  bool dirty = false;
  uint32_t index = g_pipelineState.vertexDeclaration->indexVertexStream;
  SetDirtyValue(dirty, g_vertexBufferViews[index].buffer,
                RenderBufferReference{});
  SetDirtyValue(dirty, g_vertexBufferViews[index].size, 0u);
  SetDirtyValue(dirty, g_inputSlots[index].stride, 0u);
  if (dirty) {
    g_dirtyStates.vertexStreamFirst =
        std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, index);
    g_dirtyStates.vertexStreamLast =
        std::max<uint8_t>(g_dirtyStates.vertexStreamLast, index);
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

void UpdateClipPlaneConstants(GuestDevice *device) {
  const uint32_t enabledMask = ClipPlaneEnableMask(device);
  g_sharedConstants.clipPlaneEnabled = enabledMask != 0 ? 1 : 0;
  if (enabledMask == 0)
    return;

  const uint32_t planeIndex = std::countr_zero(enabledMask);
  const GuestClipPlane &plane = ClipPlanes(device)[planeIndex];
  g_sharedConstants.clipPlane[0] = plane.x.get();
  g_sharedConstants.clipPlane[1] = plane.y.get();
  g_sharedConstants.clipPlane[2] = plane.z.get();
  g_sharedConstants.clipPlane[3] = plane.w.get();
}

void FlushPendingResolvesForPresent() {
  FlushPendingStretchRects(g_renderTarget, g_depthStencil);
}

void BeginRenderStateFrame() {
  g_uploadAllocator.reset();
  ++g_frameIndex; // invalidates the per-frame guest vertex/index upload caches
  EnsureInputSlotIndices();
  g_framebuffer = nullptr;
  g_dirtyStates = DirtyStates(true);
  if (!g_sharedConstantsInitialized) {
    for (uint32_t i = 0; i < std::size(g_sharedConstants.texture2DIndices);
         ++i) {
      g_sharedConstants.texture2DIndices[i] = kNullTexture2DDescriptor;
      g_sharedConstants.texture3DIndices[i] = kNullTexture3DDescriptor;
      g_sharedConstants.textureCubeIndices[i] = kNullTextureCubeDescriptor;
    }
    g_sharedConstantsInitialized = true;
  }

  RenderCommandList *commandList = CommandList();
  commandList->setGraphicsPipelineLayout(PipelineLayout());
  commandList->setGraphicsDescriptorSet(TextureDescriptorSet(), 0);
  commandList->setGraphicsDescriptorSet(TextureDescriptorSet(), 1);
  commandList->setGraphicsDescriptorSet(TextureDescriptorSet(), 2);
  commandList->setGraphicsDescriptorSet(SamplerDescriptorSet(), 3);
}

void SetDepthState(uint32_t zEnable, uint32_t zWriteEnable, uint32_t cmpFunc) {
  const bool ze = zEnable != 0;
  if (g_pipelineState.zEnable != ze)
    g_dirtyStates.renderTargetAndDepthStencil = true;
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zEnable, ze);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zWriteEnable,
                zWriteEnable != 0);
  RenderComparisonFunction zf = ConvertCmpFunc(cmpFunc);
  if (SceneReverseZ())
    zf = FlipCmpFunc(zf);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zFunc, zf);
}

void SetStencilState(const GuestStencilState &s) {
  if (g_pipelineState.stencilEnable != s.enable)
    g_dirtyStates.renderTargetAndDepthStencil = true;
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilEnable,
                s.enable);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilFrontFunc,
                ConvertCmpFunc(s.frontFunc));
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilFrontFail,
                ConvertStencilOp(s.frontFail));
  SetDirtyValue(g_dirtyStates.pipelineState,
                g_pipelineState.stencilFrontDepthFail,
                ConvertStencilOp(s.frontDepthFail));
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilFrontPass,
                ConvertStencilOp(s.frontPass));

  const uint32_t backFunc = s.twoSided ? s.backFunc : s.frontFunc;
  const uint32_t backFail = s.twoSided ? s.backFail : s.frontFail;
  const uint32_t backDepthFail =
      s.twoSided ? s.backDepthFail : s.frontDepthFail;
  const uint32_t backPass = s.twoSided ? s.backPass : s.frontPass;
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilBackFunc,
                ConvertCmpFunc(backFunc));
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilBackFail,
                ConvertStencilOp(backFail));
  SetDirtyValue(g_dirtyStates.pipelineState,
                g_pipelineState.stencilBackDepthFail,
                ConvertStencilOp(backDepthFail));
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilBackPass,
                ConvertStencilOp(backPass));

  SetDirtyValue<uint8_t>(g_dirtyStates.pipelineState,
                         g_pipelineState.stencilRef, uint8_t(s.ref));
  SetDirtyValue<uint8_t>(g_dirtyStates.pipelineState,
                         g_pipelineState.stencilReadMask, uint8_t(s.readMask));
  SetDirtyValue<uint8_t>(g_dirtyStates.pipelineState,
                         g_pipelineState.stencilWriteMask,
                         uint8_t(s.writeMask));
}

void SetRenderState(GuestDevice *device, uint32_t state, uint32_t value) {
  switch (state) {
  case D3DRS_ZENABLE:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zEnable,
                  value != 0);
    g_dirtyStates.renderTargetAndDepthStencil |= g_dirtyStates.pipelineState;
    break;
  case D3DRS_ZWRITEENABLE:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zWriteEnable,
                  value != 0);
    break;
  case D3DRS_ALPHATESTENABLE:
    SetAlphaTestMode(value != 0);
    break;
  case D3DRS_SRCBLEND:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.srcBlend,
                  ConvertBlendMode(value));
    break;
  case D3DRS_DESTBLEND:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.destBlend,
                  ConvertBlendMode(value));
    break;
  case D3DRS_CULLMODE: {
    RenderCullMode cull = RenderCullMode::NONE;
    switch (value) {
    case D3DCULL_NONE:
    case D3DCULL_NONE_2:
      cull = RenderCullMode::NONE;
      break;
    case D3DCULL_CW:
      cull = RenderCullMode::FRONT;
      break;
    case D3DCULL_CCW:
      cull = RenderCullMode::BACK;
      break;
    }
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.cullMode, cull);
    break;
  }
  case D3DRS_ZFUNC: {
    RenderComparisonFunction zf = ConvertCmpFunc(value);
    if (SceneReverseZ())
      zf = FlipCmpFunc(zf);
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zFunc, zf);
    break;
  }
  case D3DRS_ALPHAREF:
    g_sharedConstants.alphaThreshold = float(value) / 256.0f;
    break;
  case D3DRS_ALPHABLENDENABLE:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.alphaBlendEnable,
                  value != 0);
    break;
  case D3DRS_BLENDOP:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.blendOp,
                  ConvertBlendOp(value));
    break;
  case D3DRS_SCISSORTESTENABLE:
    SetDirtyValue(g_dirtyStates.scissorRect, g_scissorTestEnable, value != 0);
    break;
  case D3DRS_SLOPESCALEDEPTHBIAS:
    SetDirtyValue(g_dirtyStates.pipelineState,
                  g_pipelineState.slopeScaledDepthBias,
                  *reinterpret_cast<float *>(&value));
    break;
  case D3DRS_DEPTHBIAS:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthBias,
                  int32_t(*reinterpret_cast<float *>(&value) * (1 << 24)));
    break;
  case D3DRS_SRCBLENDALPHA:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.srcBlendAlpha,
                  ConvertBlendMode(value));
    break;
  case D3DRS_DESTBLENDALPHA:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.destBlendAlpha,
                  ConvertBlendMode(value));
    break;
  case D3DRS_BLENDOPALPHA:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.blendOpAlpha,
                  ConvertBlendOp(value));
    break;
  case D3DRS_COLORWRITEENABLE:
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.colorWriteEnable,
                  value);
    g_dirtyStates.renderTargetAndDepthStencil |= g_dirtyStates.pipelineState;
    break;
  default:
    break;
  }
}

void SetViewportEnable(GuestDevice * /*device*/, uint32_t value) {
  // The Xenos ViewportEnable render state maps to PA_CL_CLIP_CNTL.clip_disable.
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthClipEnabled,
                value != 0);
}

void EnsureShaderResourceDescriptor(GuestBaseTexture *texture) {
  if (texture == nullptr || texture->texture == nullptr)
    return;
  if (texture->descriptorIndex == 0)
    texture->descriptorIndex = AllocTextureDescriptor();
  TextureDescriptorSet()->setTexture(texture->descriptorIndex, texture->texture,
                                     RenderTextureLayout::SHADER_READ,
                                     texture->textureView.get());
}

void BindTextureDescriptor(uint32_t index, GuestBaseTexture *texture,
                           RenderTextureViewDimension viewDimension) {
  AddBarrier(texture, RenderTextureLayout::SHADER_READ);
  EnsureShaderResourceDescriptor(texture);

  g_sharedConstants.texture2DIndices[index] =
      (texture && viewDimension == RenderTextureViewDimension::TEXTURE_2D)
          ? texture->descriptorIndex
          : kNullTexture2DDescriptor;
  g_sharedConstants.texture3DIndices[index] =
      (texture && viewDimension == RenderTextureViewDimension::TEXTURE_3D)
          ? texture->descriptorIndex
          : kNullTexture3DDescriptor;
  g_sharedConstants.textureCubeIndices[index] =
      (texture && viewDimension == RenderTextureViewDimension::TEXTURE_CUBE)
          ? texture->descriptorIndex
          : kNullTextureCubeDescriptor;
}

void SetTexture(GuestDevice * /*device*/, uint32_t index,
                GuestTexture *texture) {
  GuestBaseTexture *boundTexture = texture;
  RenderTextureViewDimension viewDimension =
      texture ? texture->viewDimension : RenderTextureViewDimension::UNKNOWN;

  if (texture != nullptr && texture->sourceTexture != nullptr &&
      GetSampleCount(texture->sourceTexture) == RenderSampleCount::COUNT_1) {
    boundTexture = texture->sourceTexture;
    viewDimension = RenderTextureViewDimension::TEXTURE_2D;
  } else if (texture != nullptr && texture->pendingResolveCount != 0) {
    FlushPendingStretchRects(nullptr, nullptr);
  }

  BindTextureDescriptor(index, boundTexture, viewDimension);
  g_textures[index] = texture;
}

void SetVertexShader(GuestDevice *device, GuestShader *shader) {
  SyncVertexDeclarationFromDevice(device);
  if (shader != nullptr && g_pipelineState.vertexDeclaration != nullptr)
    g_vertexShaderDeclarations[shader] = g_pipelineState.vertexDeclaration;
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexShader,
                shader);
  RestoreVertexDeclarationForShader(device);
}

void SetPixelShader(GuestDevice *device, GuestShader *shader) {
  SyncVertexDeclarationFromDevice(device);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.pixelShader,
                shader);
}

void SetVertexDeclaration(GuestDevice * /*device*/,
                          GuestVertexDeclaration *declaration) {
  if (declaration != nullptr) {
    CompleteVertexDeclaration(declaration);
    g_sharedConstants.swappedTexcoords = declaration->swappedTexcoords;
    g_sharedConstants.swappedBlendWeights = declaration->swappedBlendWeights;
    uint32_t specConstants = g_pipelineState.specConstants;
    if (declaration->hasR11G11B10Normal)
      specConstants |= SPEC_CONSTANT_R11G11B10_NORMAL;
    else
      specConstants &= ~SPEC_CONSTANT_R11G11B10_NORMAL;
    if (declaration->hasUByte4TangentBasis)
      specConstants |= SPEC_CONSTANT_UNPACK_UBYTE4_BASIS;
    else
      specConstants &= ~SPEC_CONSTANT_UNPACK_UBYTE4_BASIS;
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.specConstants,
                  specConstants);
  }
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexDeclaration,
                declaration);
}

void SetStreamSource(GuestDevice *device, uint32_t index, GuestBuffer *buffer,
                     uint32_t offset, uint32_t stride) {
  SyncVertexDeclarationFromDevice(device);

  SetDirtyValue(g_dirtyStates.pipelineState,
                g_pipelineState.vertexStrides[index],
                uint8_t(buffer ? stride : 0));

  bool dirty = false;
  SetDirtyValue(dirty, g_vertexBufferViews[index].buffer,
                buffer ? buffer->buffer->at(offset) : RenderBufferReference{});
  SetDirtyValue(dirty, g_vertexBufferViews[index].size,
                buffer ? (buffer->dataSize - offset) : 0u);
  SetDirtyValue(dirty, g_inputSlots[index].stride, buffer ? stride : 0u);
  if (dirty) {
    g_dirtyStates.vertexStreamFirst =
        std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, index);
    g_dirtyStates.vertexStreamLast =
        std::max<uint8_t>(g_dirtyStates.vertexStreamLast, index);
  }
}

void SetIndices(GuestDevice * /*device*/, GuestBuffer *buffer) {
  SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer,
                buffer ? buffer->buffer->at(0) : RenderBufferReference{});
  SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.format,
                buffer ? buffer->format : RenderFormat::R16_UINT);
  SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.size,
                buffer ? buffer->dataSize : 0u);
}

void SetStreamSourceGuestData(GuestDevice *device, uint32_t index,
                              const void *data, uint32_t size,
                              uint32_t stride) {
  SyncVertexDeclarationFromDevice(device);

  SetDirtyValue(g_dirtyStates.pipelineState,
                g_pipelineState.vertexStrides[index], uint8_t(stride));

  GuestDataUpload &entry = g_guestVertexUploads[data];
  if (entry.frame != g_frameIndex || entry.size != size) {
    UploadResult result = UploadGuestVertexData(data, size, 0x10);
    entry.frame = g_frameIndex;
    entry.size = size;
    entry.ref = result.buffer->at(result.offset);
  }

  bool dirty = false;
  SetDirtyValue(dirty, g_vertexBufferViews[index].buffer, entry.ref);
  SetDirtyValue(dirty, g_vertexBufferViews[index].size, size);
  SetDirtyValue(dirty, g_inputSlots[index].stride, stride);
  if (dirty) {
    g_dirtyStates.vertexStreamFirst =
        std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, index);
    g_dirtyStates.vertexStreamLast =
        std::max<uint8_t>(g_dirtyStates.vertexStreamLast, index);
  }
}

void SetIndicesGuestData(GuestDevice * /*device*/, const void *data,
                         uint32_t size, uint32_t indexStride) {
  GuestDataUpload &entry = g_guestIndexUploads[data];
  if (entry.frame != g_frameIndex || entry.size != size) {
    UploadResult result;
    if (indexStride == 4) {
      result = g_uploadAllocator.allocateCopy<true>(
          reinterpret_cast<const uint32_t *>(data), size & ~3u, 4);
    } else {
      result = g_uploadAllocator.allocateCopy<true>(
          reinterpret_cast<const uint16_t *>(data), size & ~1u, 2);
    }
    entry.frame = g_frameIndex;
    entry.size = size;
    entry.ref = result.buffer->at(result.offset);
  }

  SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, entry.ref);
  SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.format,
                indexStride == 4 ? RenderFormat::R32_UINT
                                 : RenderFormat::R16_UINT);
  SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.size, size);
}

void SetViewport(GuestDevice *device, GuestViewport *viewport) {
  // D3D9 validation: a zero-sized viewport is INVALIDCALL and leaves the
  // state unchanged.
  if (viewport->width.get() == 0 || viewport->height.get() == 0)
    return;
  SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.x,
                       float(viewport->x.get()));
  SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.y,
                       float(viewport->y.get()));
  SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.width,
                       float(viewport->width.get()));
  SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.height,
                       float(viewport->height.get()));
  SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.minDepth,
                       viewport->minZ.get());
  SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.maxDepth,
                       viewport->maxZ.get());

  uint32_t specConstants = g_pipelineState.specConstants;
  if (viewport->minZ.get() > viewport->maxZ.get())
    specConstants |= SPEC_CONSTANT_REVERSE_Z;
  else
    specConstants &= ~SPEC_CONSTANT_REVERSE_Z;
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.specConstants,
                specConstants);

  g_dirtyStates.scissorRect |= g_dirtyStates.viewport;
}

void SetScissorRect(GuestDevice *device, GuestRect *rect) {
  SetDirtyValue(g_dirtyStates.scissorRect, g_scissorTestEnable,
                ScissorTestEnabled(device));
  SetDirtyValue<int32_t>(g_dirtyStates.scissorRect, g_scissorRect.top,
                         rect->top.get());
  SetDirtyValue<int32_t>(g_dirtyStates.scissorRect, g_scissorRect.left,
                         rect->left.get());
  SetDirtyValue<int32_t>(g_dirtyStates.scissorRect, g_scissorRect.bottom,
                         rect->bottom.get());
  SetDirtyValue<int32_t>(g_dirtyStates.scissorRect, g_scissorRect.right,
                         rect->right.get());
}

void SetRenderTargetInternal(GuestBaseTexture *renderTarget) {
  SetDirtyValue(g_dirtyStates.renderTargetAndDepthStencil, g_renderTarget,
                renderTarget);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.renderTargetFormat,
                renderTarget ? renderTarget->format : RenderFormat::UNKNOWN);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.sampleCount,
                GetSampleCount(renderTarget));
  SetAlphaTestMode(
      (g_pipelineState.specConstants &
       (SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE)) != 0);

  // D3D9/Xenon semantics: SetRenderTarget resets the viewport to cover the
  // whole surface.
  if (renderTarget != nullptr && renderTarget->width != 0 &&
      renderTarget->height != 0) {
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.x, 0.0f);
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.y, 0.0f);
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.width,
                         float(renderTarget->width));
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.height,
                         float(renderTarget->height));
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.minDepth, 0.0f);
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.maxDepth, 1.0f);
    g_dirtyStates.scissorRect |= g_dirtyStates.viewport;
  }
}

void SetRenderTarget(GuestDevice * /*device*/, uint32_t index,
                     GuestBaseTexture *renderTarget) {
  if (index != 0)
    return;
  SetRenderTargetInternal(renderTarget ? renderTarget : g_implicitRenderTarget);
}

void SetImplicitRenderTarget(GuestBaseTexture *renderTarget) {
  g_implicitRenderTarget = renderTarget;
  SetRenderTargetInternal(renderTarget);
}

void SetDepthStencilSurface(GuestDevice * /*device*/,
                            GuestSurface *depthStencil) {
  SetDirtyValue(g_dirtyStates.renderTargetAndDepthStencil, g_depthStencil,
                depthStencil);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthStencilFormat,
                depthStencil ? depthStencil->format : RenderFormat::UNKNOWN);
  // viewport on the next flush.
  g_dirtyStates.viewport = true;
  if (depthStencil != nullptr)
    g_implicitDepthStencil = depthStencil;
}

void Clear(GuestDevice * /*device*/, uint32_t flags, const float *color,
           float z) {
  FlushPendingStretchRects(g_renderTarget, g_depthStencil);

  AddBarrier(g_renderTarget, RenderTextureLayout::COLOR_WRITE);
  AddBarrier(g_depthStencil, RenderTextureLayout::DEPTH_WRITE);
  FlushBarriers();

  bool onePass = (g_renderTarget == nullptr) || (g_depthStencil == nullptr) ||
                 (g_renderTarget->width == g_depthStencil->width &&
                  g_renderTarget->height == g_depthStencil->height);
  if (onePass)
    SetFramebuffer(g_renderTarget, g_depthStencil, true);


  RenderRect clearRect(int32_t(g_viewport.x), int32_t(g_viewport.y),
                       int32_t(g_viewport.x + g_viewport.width),
                       int32_t(g_viewport.y + g_viewport.height));
  if (g_scissorTestEnable) {
    clearRect.left = std::max(clearRect.left, g_scissorRect.left);
    clearRect.top = std::max(clearRect.top, g_scissorRect.top);
    clearRect.right = std::min(clearRect.right, g_scissorRect.right);
    clearRect.bottom = std::min(clearRect.bottom, g_scissorRect.bottom);
  }

  RenderCommandList *commandList = CommandList();
  if (g_renderTarget != nullptr && (flags & D3DCLEAR_TARGET) != 0) {
    if (!onePass)
      SetFramebuffer(g_renderTarget, nullptr, true);
    commandList->clearColor(0,
                            RenderColor(color[0], color[1], color[2], color[3]),
                            &clearRect, 1);
    MarkAttachmentInitialized(g_renderTarget);
    g_lastTouchedRenderTarget = g_renderTarget;
  }
  const bool clearDepth = (flags & D3DCLEAR_ZBUFFER) != 0;
  const bool clearStencil = (flags & D3DCLEAR_STENCIL) != 0;
  if (g_depthStencil != nullptr && (clearDepth || clearStencil)) {
    if (!onePass)
      SetFramebuffer(nullptr, g_depthStencil, true);
    const float depthValue = SceneReverseZ() ? (1.0f - z) : z;
    commandList->clearDepthStencil(clearDepth, clearStencil, depthValue, 0,
                                   &clearRect, 1);
    MarkAttachmentInitialized(g_depthStencil);
    g_implicitDepthStencil = g_depthStencil;
  }
}

void StretchRect(GuestDevice * /*device*/, uint32_t flags,
                 const GuestRect *source, GuestBaseTexture *destination,
                 const GuestPoint *destPoint) {
  const bool isDepthStencil = (flags & 0x4) != 0;
  GuestBaseTexture *surface =
      isDepthStencil ? static_cast<GuestBaseTexture *>(g_depthStencil)
                     : g_renderTarget;
  if (!isDepthStencil && surface != nullptr &&
      g_lastTouchedRenderTarget != nullptr &&
      surface != g_lastTouchedRenderTarget &&
      surface->width == g_lastTouchedRenderTarget->width &&
      surface->height == g_lastTouchedRenderTarget->height &&
      surface->format == g_lastTouchedRenderTarget->format) {
    surface = g_lastTouchedRenderTarget;
  }
  if (surface == nullptr || surface->texture == nullptr ||
      destination == nullptr || destination->texture == nullptr)
    return;
  if (surface->texture == destination->texture)
    return;

  PendingResolve resolve;
  resolve.destination = destination;
  resolve.hasSourceRect = source != nullptr;
  if (source != nullptr) {
    resolve.sourceRect =
        RenderRect(source->left.get(), source->top.get(), source->right.get(),
                   source->bottom.get());
  }
  if (destPoint != nullptr) {
    resolve.destX = uint32_t(std::max(destPoint->x.get(), int32_t(0)));
    resolve.destY = uint32_t(std::max(destPoint->y.get(), int32_t(0)));
  }

  const RenderSampleCounts sampleCount = GetSampleCount(surface);
  const bool fullSurface = IsFullSurfaceResolve(surface, destination, resolve);
  destination->sourceTexture =
      fullSurface && sampleCount == RenderSampleCount::COUNT_1 &&
              surface->format == destination->format
          ? surface
          : nullptr;
  ++destination->pendingResolveCount;
  surface->pendingResolves.emplace_back(resolve);
  g_pendingStretchRectSurfaces.emplace(surface);

  for (uint32_t i = 0; i < std::size(g_textures); ++i) {
    if (static_cast<GuestBaseTexture *>(g_textures[i]) == destination) {
      if (destination->sourceTexture != nullptr)
        BindTextureDescriptor(i, surface,
                              RenderTextureViewDimension::TEXTURE_2D);
      else
        BindTextureDescriptor(i, destination,
                              RenderTextureViewDimension::TEXTURE_2D);
    }
  }
}

void DrawPrimitive(GuestDevice *device, uint32_t primitiveType,
                   uint32_t startVertex, uint32_t primitiveCount) {
  SyncVertexDeclarationFromDevice(device);
  RestoreVertexDeclarationForShader(device);
  GuestVertexDeclaration *previousDeclaration = nullptr;
  bool restoreDeclaration = SelectShaderVertexDeclaration(
      device, g_inputSlots[0].stride, &previousDeclaration);
  if (!restoreDeclaration) {
    restoreDeclaration =
        SelectVertexDeclaration(device,
                                SelectParticleVertexDeclarationByStride(
                                    primitiveType, g_inputSlots[0].stride),
                                &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectGpuSkin40Declaration(
        device, primitiveType, g_inputSlots[0].stride, &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectIndexedMeshDeclarationForStaleCanvasDecl(
        device, primitiveType, primitiveCount, g_inputSlots[0].stride,
        &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectQuadVertexDeclarationByStride(
        device, primitiveType, primitiveCount, g_inputSlots[0].stride,
        &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectTexturedQuadVertexDeclaration(
        device, primitiveType, primitiveCount, &previousDeclaration);
  }
  SetPrimitiveType(primitiveType);

  uint32_t indexCount = CheckInstancing();
  if (indexCount > 0) {
    auto &view = g_vertexBufferViews[g_pipelineState.vertexDeclaration
                                         ->indexVertexStream];
    SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, view.buffer);
    SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.size, view.size);
    SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.format,
                  RenderFormat::R32_UINT);
    UnsetInstancingStream();
  }

  uint32_t convertedIndexCount = 0;
  if (indexCount == 0) {
    if (primitiveType == D3DPT_QUADLIST)
      convertedIndexCount = g_quadIndexData.prepare(primitiveCount);
    else if (primitiveType == D3DPT_TRIANGLEFAN)
      convertedIndexCount = g_triangleFanIndexData.prepare(primitiveCount);
  }

  FlushRenderState(device);
  if (!g_pipelineBound) {
    if (restoreDeclaration)
      SetVertexDeclaration(device, previousDeclaration);
    return;
  }

  RenderCommandList *commandList = CommandList();
  if (indexCount > 0)
    commandList->drawIndexedInstanced(indexCount, primitiveCount / indexCount,
                                      0, 0, 0);
  else if (convertedIndexCount > 0)
    commandList->drawIndexedInstanced(convertedIndexCount, 1, 0,
                                      int32_t(startVertex), 0);
  else
    commandList->drawInstanced(primitiveCount, 1, startVertex, 0);
  if (restoreDeclaration)
    SetVertexDeclaration(device, previousDeclaration);
}

void DrawIndexedPrimitive(GuestDevice *device, uint32_t primitiveType,
                          int32_t baseVertexIndex, uint32_t startIndex,
                          uint32_t primitiveCount) {
  SyncVertexDeclarationFromDevice(device);
  RestoreVertexDeclarationForShader(device);
  GuestVertexDeclaration *previousDeclaration = nullptr;
  bool restoreDeclaration = SelectShaderVertexDeclaration(
      device, g_inputSlots[0].stride, &previousDeclaration);
  if (!restoreDeclaration) {
    restoreDeclaration =
        SelectVertexDeclaration(device,
                                SelectParticleVertexDeclarationByStride(
                                    primitiveType, g_inputSlots[0].stride),
                                &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectGpuSkin40Declaration(
        device, primitiveType, g_inputSlots[0].stride, &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectIndexedMeshDeclarationForStaleCanvasDecl(
        device, primitiveType, primitiveCount, g_inputSlots[0].stride,
        &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectQuadVertexDeclarationByStride(
        device, primitiveType, primitiveCount, g_inputSlots[0].stride,
        &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectTexturedQuadVertexDeclaration(
        device, primitiveType, primitiveCount, &previousDeclaration);
  }
  uint32_t indexCount = CheckInstancing();
  if (indexCount > 0)
    UnsetInstancingStream();

  SetPrimitiveType(primitiveType);
  FlushRenderState(device);
  if (!g_pipelineBound) {
    if (restoreDeclaration)
      SetVertexDeclaration(device, previousDeclaration);
    return;
  }

  CommandList()->drawIndexedInstanced(primitiveCount, 1, startIndex,
                                      baseVertexIndex, 0);
  if (restoreDeclaration)
    SetVertexDeclaration(device, previousDeclaration);
}

void DrawPrimitiveUP(GuestDevice *device, uint32_t primitiveType,
                     uint32_t primitiveCount, void *vertexStreamZeroData,
                     uint32_t vertexStreamZeroStride) {
  SyncVertexDeclarationFromDevice(device);
  RestoreVertexDeclarationForShader(device);
  GuestVertexDeclaration *previousDeclaration = nullptr;
  bool restoreDeclaration = SelectShaderVertexDeclaration(
      device, vertexStreamZeroStride, &previousDeclaration);
  if (!restoreDeclaration) {
    restoreDeclaration =
        SelectVertexDeclaration(device,
                                SelectParticleVertexDeclarationByStride(
                                    primitiveType, vertexStreamZeroStride),
                                &previousDeclaration);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectQuadVertexDeclarationByStride(
        device, primitiveType, primitiveCount, vertexStreamZeroStride,
        &previousDeclaration, vertexStreamZeroData);
  }
  if (!restoreDeclaration) {
    restoreDeclaration = SelectTexturedQuadVertexDeclaration(
        device, primitiveType, primitiveCount, &previousDeclaration);
  }
  CheckInstancing();
  if (g_pipelineState.instancing)
    UnsetInstancingStream();

  SetPrimitiveType(primitiveType);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexStrides[0],
                uint8_t(vertexStreamZeroStride));

  uint32_t vertexDataSize = primitiveCount * vertexStreamZeroStride;
  UploadResult allocation =
      UploadGuestVertexData(vertexStreamZeroData, vertexDataSize, 0x4);

  g_vertexBufferViews[0].size = vertexDataSize;
  g_vertexBufferViews[0].buffer = allocation.buffer->at(allocation.offset);
  g_inputSlots[0].stride = vertexStreamZeroStride;
  g_dirtyStates.vertexStreamFirst = 0;

  uint32_t indexCount = 0;
  if (primitiveType == D3DPT_QUADLIST)
    indexCount = g_quadIndexData.prepare(primitiveCount);
  else if (primitiveType == D3DPT_TRIANGLEFAN)
    indexCount = g_triangleFanIndexData.prepare(primitiveCount);

  FlushRenderState(device);
  if (!g_pipelineBound) {
    if (restoreDeclaration)
      SetVertexDeclaration(device, previousDeclaration);
    return;
  }

  if (indexCount != 0)
    CommandList()->drawIndexedInstanced(indexCount, 1, 0, 0, 0);
  else
    CommandList()->drawInstanced(primitiveCount, 1, 0, 0);
  if (restoreDeclaration)
    SetVertexDeclaration(device, previousDeclaration);
}

static uint32_t IndexCountForPrimitive(uint32_t primitiveType,
                                       uint32_t primitiveCount) {
  switch (primitiveType) {
  case D3DPT_POINTLIST:
    return primitiveCount;
  case D3DPT_LINELIST:
    return primitiveCount * 2;
  case D3DPT_LINESTRIP:
    return primitiveCount + 1;
  case D3DPT_TRIANGLELIST:
    return primitiveCount * 3;
  case D3DPT_TRIANGLESTRIP:
  case D3DPT_TRIANGLEFAN:
    return primitiveCount + 2;
  case D3DPT_QUADLIST:
    return primitiveCount * 4;
  default:
    return primitiveCount * 3;
  }
}

void DrawIndexedPrimitiveUP(GuestDevice *device, uint32_t primitiveType,
                            uint32_t minVertexIndex, uint32_t numVertices,
                            uint32_t numPrimitives, const void *indexData,
                            uint32_t indexStride, const void *vertexData,
                            uint32_t vertexStride) {
  SyncVertexDeclarationFromDevice(device);
  RestoreVertexDeclarationForShader(device);
  GuestVertexDeclaration *previousDeclaration = nullptr;
  bool restoreDeclaration =
      SelectShaderVertexDeclaration(device, vertexStride, &previousDeclaration);
  if (!restoreDeclaration) {
    restoreDeclaration = SelectVertexDeclaration(
        device,
        SelectParticleVertexDeclarationByStride(primitiveType, vertexStride),
        &previousDeclaration);
  }
  if (!restoreDeclaration) {
    GuestVertexDeclaration *substitute = nullptr;
    if (IsSimpleElementVertexStride(vertexStride))
      substitute = SimpleElementDeclaration();
    else if (vertexStride == 20)
      substitute = TexturedQuadDeclaration();
    else if (vertexStride == 32)
      substitute = SelectStride32QuadDeclaration(vertexData, minVertexIndex,
                                                 numVertices);
    if (substitute != nullptr &&
        g_pipelineState.vertexDeclaration != substitute) {
      previousDeclaration = g_pipelineState.vertexDeclaration;
      SetVertexDeclaration(device, substitute);
      restoreDeclaration = true;
    }
  }
  CheckInstancing();
  if (g_pipelineState.instancing)
    UnsetInstancingStream();

  SetPrimitiveType(primitiveType);
  SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexStrides[0],
                uint8_t(vertexStride));

  const uint8_t *vertexSrc = reinterpret_cast<const uint8_t *>(vertexData) +
                             size_t(minVertexIndex) * vertexStride;
  uint32_t vertexDataSize = numVertices * vertexStride;
  UploadResult va = UploadGuestVertexData(vertexSrc, vertexDataSize, 0x4);
  g_vertexBufferViews[0].size = vertexDataSize;
  g_vertexBufferViews[0].buffer = va.buffer->at(va.offset);
  g_inputSlots[0].stride = vertexStride;
  g_dirtyStates.vertexStreamFirst = 0;

  uint32_t indexCount = IndexCountForPrimitive(primitiveType, numPrimitives);
  UploadResult ia;
  if (indexStride == 4) {
    ia = g_uploadAllocator.allocateCopy<true>(
        reinterpret_cast<const uint32_t *>(indexData), indexCount * 4, 4);
    g_indexBufferView.format = RenderFormat::R32_UINT;
  } else {
    ia = g_uploadAllocator.allocateCopy<true>(
        reinterpret_cast<const uint16_t *>(indexData), indexCount * 2, 2);
    g_indexBufferView.format = RenderFormat::R16_UINT;
  }
  g_indexBufferView.buffer = ia.buffer->at(ia.offset);
  g_indexBufferView.size = indexCount * indexStride;
  g_dirtyStates.indices = true;

  SyncShadowIndexedUPColorWrite(device, primitiveType, minVertexIndex,
                                numVertices, numPrimitives, indexStride,
                                vertexData, vertexStride);
  FlushRenderState(device);
  if (!g_pipelineBound) {
    if (restoreDeclaration)
      SetVertexDeclaration(device, previousDeclaration);
    return;
  }

  CommandList()->drawIndexedInstanced(indexCount, 1, 0,
                                      -int32_t(minVertexIndex), 0);
  if (restoreDeclaration)
    SetVertexDeclaration(device, previousDeclaration);
}

} // namespace reodyssey::render
