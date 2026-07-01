

#include "video.h"

#include <array>
#include <cassert>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include "render/guest_resources.h"
#include "render/render_internal.h"
#include "render/shaders/copy_ps.hlsl.dxil.h"
#include "render/shaders/copy_vs.hlsl.dxil.h"

using namespace plume;

namespace plume {
// Backend factories (defined in thirdparty/plume/plume_d3d12.cpp,
// plume_vulkan.cpp).
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
extern std::unique_ptr<RenderInterface>
CreateVulkanInterface(RenderWindow sdlWindow);
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
} // namespace plume

namespace reodyssey::render {
void BeginRenderStateFrame();          // render_state.cpp
void FlushPendingResolvesForPresent(); // render_state.cpp
void EnsureFrameStarted();             // defined below
} // namespace reodyssey::render

namespace {

constexpr RenderFormat kBackbufferFormat = RenderFormat::R8G8B8A8_UNORM;

std::unique_ptr<RenderInterface> g_interface;
std::unique_ptr<RenderDevice> g_device;
std::unique_ptr<RenderCommandQueue> g_queue;
std::unique_ptr<RenderSwapChain> g_swapChain;

constexpr uint32_t kFramesInFlight = 2;

struct FrameContext {
  std::unique_ptr<RenderCommandList> commandList;
  std::unique_ptr<RenderCommandFence> commandFence;
  std::unique_ptr<RenderCommandSemaphore> acquireSemaphore;
  std::unique_ptr<RenderCommandSemaphore> renderSemaphore;
  bool commandsInFlight = false;
};

std::array<FrameContext, kFramesInFlight> g_frames;
uint32_t g_frameSlot = 0;

// One framebuffer per swapchain backbuffer, keyed by texture index.
std::vector<std::unique_ptr<RenderFramebuffer>> g_framebuffers;

// Dedicated copy queue for synchronous resource uploads.
std::unique_ptr<RenderCommandQueue> g_copyQueue;
std::unique_ptr<RenderCommandList> g_copyCommandList;
std::unique_ptr<RenderCommandFence> g_copyFence;
std::mutex g_copyMutex;

std::unique_ptr<RenderDescriptorSet> g_textureDescriptorSet;
std::mutex g_descriptorMutex;
uint32_t g_descriptorCapacity = reodyssey::render::kNullTextureDescriptorCount;
std::vector<uint32_t> g_freedDescriptors;
struct PendingDescriptorFree {
  uint32_t index;
  uint32_t frameMask;
};
std::vector<PendingDescriptorFree> g_pendingDescriptorFrees;
std::array<std::unique_ptr<RenderTexture>,
           reodyssey::render::kNullTextureDescriptorCount>
    g_nullTextures;
std::array<std::unique_ptr<RenderTextureView>,
           reodyssey::render::kNullTextureDescriptorCount>
    g_nullTextureViews;

// Bindless sampler set + the graphics pipeline layout (XenosRecomp ABI).
std::unique_ptr<RenderDescriptorSet> g_samplerDescriptorSet;
std::unique_ptr<RenderSampler> g_defaultSampler;
std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;

std::unique_ptr<RenderShader> g_blitVertexShader;
std::unique_ptr<RenderShader> g_blitPixelShader;
std::unordered_map<uint32_t, std::unique_ptr<RenderPipeline>> g_blitPipelines;

bool g_initialized = false;
bool g_swapChainValid = false;
bool g_frameOpen = false;
uint32_t g_backBufferIndex = 0;
RenderWindow g_window{};

// The guest's final front-buffer surface to blit this frame (D3DDevice_Swap).
reodyssey::render::GuestBaseTexture *g_presentSource = nullptr;

void RebuildFramebuffers() {
  g_framebuffers.clear();
  const uint32_t count = g_swapChain->getTextureCount();
  g_framebuffers.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const RenderTexture *color = g_swapChain->getTexture(i);
    RenderFramebufferDesc desc(&color, 1);
    g_framebuffers[i] = g_device->createFramebuffer(desc);
  }
}

FrameContext &CurrentFrame() { return g_frames[g_frameSlot]; }

void WaitForFrame(FrameContext &frame) {
  if (!frame.commandsInFlight)
    return;
  g_queue->waitForCommandFence(frame.commandFence.get());
  frame.commandsInFlight = false;
}

void RetireFrame(uint32_t frameSlot) {
  std::lock_guard lock(g_descriptorMutex);
  const uint32_t frameBit = 1u << frameSlot;
  for (size_t i = 0; i < g_pendingDescriptorFrees.size();) {
    PendingDescriptorFree &pending = g_pendingDescriptorFrees[i];
    pending.frameMask &= ~frameBit;
    if (pending.frameMask == 0) {
      g_freedDescriptors.push_back(pending.index);
      pending = g_pendingDescriptorFrees.back();
      g_pendingDescriptorFrees.pop_back();
    } else {
      ++i;
    }
  }
}

} // namespace

bool Video::Init(void *nativeWindowHandle, uint32_t width, uint32_t height) {
  if (g_initialized) {
    return true;
  }

  g_window = reinterpret_cast<RenderWindow>(nativeWindowHandle);
  s_viewportWidth = width;
  s_viewportHeight = height;

  // Prefer D3D12 on Windows, fall back to Vulkan.
  using InterfaceFn = std::unique_ptr<RenderInterface> (*)();
  const std::array<InterfaceFn, 2> factories = {
      plume::CreateD3D12Interface,
      static_cast<InterfaceFn>([]() { return plume::CreateVulkanInterface(); }),
  };

  for (InterfaceFn factory : factories) {
    g_interface = factory();
    if (g_interface == nullptr) {
      continue;
    }
    g_device = g_interface->createDevice();
    if (g_device != nullptr) {
      break;
    }
    g_interface.reset();
  }

  if (g_device == nullptr) {
    return false;
  }

  g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
  for (FrameContext &frame : g_frames) {
    frame.commandList = g_queue->createCommandList();
    frame.commandFence = g_device->createCommandFence();
    frame.acquireSemaphore = g_device->createCommandSemaphore();
    frame.renderSemaphore = g_device->createCommandSemaphore();
    frame.commandsInFlight = false;
  }

  RenderSwapChainDesc swapChainDesc(g_window, kBackbufferFormat, 2);
  g_swapChain = g_queue->createSwapChain(swapChainDesc);
  g_swapChainValid = !g_swapChain->needsResize();
  if (g_swapChainValid) {
    RebuildFramebuffers();
  }

  g_copyQueue = g_device->createCommandQueue(RenderCommandListType::COPY);
  g_copyCommandList = g_copyQueue->createCommandList();
  g_copyFence = g_device->createCommandFence();

  {
    using reodyssey::render::kNullTexture2DDescriptor;
    using reodyssey::render::kNullTexture3DDescriptor;
    using reodyssey::render::kNullTextureCubeDescriptor;
    using reodyssey::render::kNullTextureDescriptorCount;
    using reodyssey::render::kSamplerDescriptorSize;
    using reodyssey::render::kTextureDescriptorSize;

    RenderDescriptorSetBuilder textureSetBuilder;
    textureSetBuilder.begin();
    textureSetBuilder.addTexture(0, kTextureDescriptorSize);
    textureSetBuilder.end(true, kTextureDescriptorSize);
    g_textureDescriptorSet = textureSetBuilder.create(g_device.get());

    for (uint32_t i = 0; i < kNullTextureDescriptorCount; ++i) {
      RenderTextureDesc desc;
      desc.width = 1;
      desc.height = 1;
      desc.depth = 1;
      desc.mipLevels = 1;
      desc.format = RenderFormat::R8_UNORM;

      RenderTextureViewDesc viewDesc;
      viewDesc.format = desc.format;
      viewDesc.componentMapping =
          RenderComponentMapping(RenderSwizzle::ZERO, RenderSwizzle::ZERO,
                                 RenderSwizzle::ZERO, RenderSwizzle::ZERO);
      viewDesc.mipLevels = 1;

      switch (i) {
      case kNullTexture2DDescriptor:
        desc.dimension = RenderTextureDimension::TEXTURE_2D;
        desc.arraySize = 1;
        viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
        break;
      case kNullTexture3DDescriptor:
        desc.dimension = RenderTextureDimension::TEXTURE_3D;
        desc.arraySize = 1;
        viewDesc.dimension = RenderTextureViewDimension::TEXTURE_3D;
        break;
      case kNullTextureCubeDescriptor:
        desc.dimension = RenderTextureDimension::TEXTURE_2D;
        desc.arraySize = 6;
        desc.flags = RenderTextureFlag::CUBE;
        viewDesc.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
        break;
      }

      g_nullTextures[i] = g_device->createTexture(desc);
      g_nullTextureViews[i] = g_nullTextures[i]->createTextureView(viewDesc);
      g_textureDescriptorSet->setTexture(i, g_nullTextures[i].get(),
                                         RenderTextureLayout::SHADER_READ,
                                         g_nullTextureViews[i].get());
    }

    RenderDescriptorSetBuilder samplerSetBuilder;
    samplerSetBuilder.begin();
    samplerSetBuilder.addSampler(0, kSamplerDescriptorSize);
    samplerSetBuilder.end(true, kSamplerDescriptorSize);
    g_samplerDescriptorSet = samplerSetBuilder.create(g_device.get());
    g_defaultSampler = g_device->createSampler(RenderSamplerDesc());
    g_samplerDescriptorSet->setSampler(0, g_defaultSampler.get());

    RenderPipelineLayoutBuilder layoutBuilder;
    layoutBuilder.begin(false, true);
    layoutBuilder.addDescriptorSet(textureSetBuilder);
    layoutBuilder.addDescriptorSet(textureSetBuilder);
    layoutBuilder.addDescriptorSet(textureSetBuilder);
    layoutBuilder.addDescriptorSet(samplerSetBuilder);
    // D3D12: VS/PS/shared constants as root CBVs; small PS push constant.
    layoutBuilder.addRootDescriptor(0, 4,
                                    RenderRootDescriptorType::CONSTANT_BUFFER);
    layoutBuilder.addRootDescriptor(1, 4,
                                    RenderRootDescriptorType::CONSTANT_BUFFER);
    layoutBuilder.addRootDescriptor(2, 4,
                                    RenderRootDescriptorType::CONSTANT_BUFFER);
    layoutBuilder.addPushConstant(3, 4, 4, RenderShaderStageFlag::PIXEL);
    layoutBuilder.end();
    g_pipelineLayout = layoutBuilder.create(g_device.get());
  }

  g_blitVertexShader = g_device->createShader(
      g_copy_vs_dxil, sizeof(g_copy_vs_dxil), "main", RenderShaderFormat::DXIL);
  g_blitPixelShader = g_device->createShader(
      g_copy_ps_dxil, sizeof(g_copy_ps_dxil), "main", RenderShaderFormat::DXIL);

  g_initialized = true;
  return true;
}

bool Video::IsInitialized() { return g_initialized; }

namespace reodyssey::render {

RenderInterface *Interface() { return g_interface.get(); }
RenderDevice *Device() { return g_device.get(); }

uint32_t CurrentFrameSlot() { return g_frameSlot; }

void SetPresentSource(GuestBaseTexture *frontBuffer) {
  g_presentSource = frontBuffer;
}

void EnsureFrameStarted() {
  if (g_frameOpen || !g_initialized)
    return;

  if (!g_swapChainValid || g_swapChain->needsResize()) {
    // Drain the GPU (and any pending presentation) before resizing.
    Video::WaitForGPU();
    g_swapChainValid = g_swapChain->resize();
    if (!g_swapChainValid)
      return;
    RebuildFramebuffers();
  }
  FrameContext &frame = CurrentFrame();
  WaitForFrame(frame);
  RetireFrame(g_frameSlot);
  if (!g_swapChain->acquireTexture(frame.acquireSemaphore.get(),
                                   &g_backBufferIndex)) {
    g_swapChainValid = false;
    return;
  }
  frame.commandList->begin();
  g_frameOpen =
      true; // set before BeginRenderStateFrame (it calls CommandList())
  BeginRenderStateFrame();
}

RenderCommandList *CommandList() {
  EnsureFrameStarted();
  return CurrentFrame().commandList.get();
}

RenderDescriptorSet *TextureDescriptorSet() {
  return g_textureDescriptorSet.get();
}

RenderDescriptorSet *SamplerDescriptorSet() {
  return g_samplerDescriptorSet.get();
}

RenderPipelineLayout *PipelineLayout() { return g_pipelineLayout.get(); }

RenderPipeline *GetBlitPipeline(RenderFormat format) {
  if (g_device == nullptr)
    return nullptr;
  auto &pipeline = g_blitPipelines[uint32_t(format)];
  if (pipeline == nullptr) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_blitVertexShader.get();
    desc.pixelShader = g_blitPixelShader.get();
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = g_device->createGraphicsPipeline(desc);
  }
  return pipeline.get();
}

uint32_t AllocTextureDescriptor() {
  std::lock_guard lock(g_descriptorMutex);
  if (!g_freedDescriptors.empty()) {
    uint32_t v = g_freedDescriptors.back();
    g_freedDescriptors.pop_back();
    return v;
  }
  return g_descriptorCapacity++;
}

void FreeTextureDescriptor(uint32_t index) {
  if (index < kNullTextureDescriptorCount)
    return;
  std::lock_guard lock(g_descriptorMutex);
  uint32_t frameMask = 0;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (g_frames[i].commandsInFlight)
      frameMask |= 1u << i;
  }
  if (g_frameOpen)
    frameMask |= 1u << g_frameSlot;

  if (frameMask == 0) {
    g_freedDescriptors.push_back(index);
    return;
  }
  g_pendingDescriptorFrees.push_back({index, frameMask});
}

void ExecuteUpload(const std::function<void(RenderCommandList *)> &record) {
  std::lock_guard lock(g_copyMutex);
  g_copyCommandList->begin();
  record(g_copyCommandList.get());
  g_copyCommandList->end();
  g_copyQueue->executeCommandLists(g_copyCommandList.get(), g_copyFence.get());
  g_copyQueue->waitForCommandFence(g_copyFence.get());
}

} // namespace reodyssey::render

void Video::Present() {
  if (!g_initialized) {
    return;
  }

  // The guest's draws/clears for this frame have already been recorded into the
  // open command list (targeting the guest's own render-target surfaces). Make
  // sure a frame is open even if the guest issued nothing.
  reodyssey::render::EnsureFrameStarted();
  if (!g_frameOpen) {
    return; // acquire failed this frame
  }

  reodyssey::render::FlushPendingResolvesForPresent();

  RenderTexture *backBuffer = g_swapChain->getTexture(g_backBufferIndex);
  RenderFramebuffer *framebuffer = g_framebuffers[g_backBufferIndex].get();

  FrameContext &frame = CurrentFrame();
  RenderCommandList *commandList = frame.commandList.get();

  commandList->setFramebuffer(nullptr);
  RenderPipeline *blitPipeline =
      reodyssey::render::GetBlitPipeline(kBackbufferFormat);
  const bool blit = g_presentSource != nullptr &&
                    g_presentSource->texture != nullptr &&
                    blitPipeline != nullptr;
  if (blit) {
    if (g_presentSource->descriptorIndex == 0) {
      g_presentSource->descriptorIndex =
          reodyssey::render::AllocTextureDescriptor();
    }
    g_textureDescriptorSet->setTexture(
        g_presentSource->descriptorIndex, g_presentSource->texture,
        RenderTextureLayout::SHADER_READ, g_presentSource->textureView.get());


    RenderTextureBarrier toBlit[] = {
        RenderTextureBarrier(g_presentSource->texture,
                             RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(backBuffer, RenderTextureLayout::COLOR_WRITE),
    };
    commandList->barriers(RenderBarrierStage::GRAPHICS, toBlit, 2);
    g_presentSource->layout = RenderTextureLayout::SHADER_READ;

    const uint32_t descriptorIndex = g_presentSource->descriptorIndex;
    commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
    commandList->setPipeline(blitPipeline);
    commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
    commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
    commandList->setGraphicsPushConstants(0, &descriptorIndex, 0,
                                          sizeof(descriptorIndex));
    commandList->setFramebuffer(framebuffer);
    commandList->setViewports(
        RenderViewport(0.0f, 0.0f, float(g_swapChain->getWidth()),
                       float(g_swapChain->getHeight())));
    commandList->setScissors(
        RenderRect(0, 0, g_swapChain->getWidth(), g_swapChain->getHeight()));
    commandList->drawInstanced(3, 1, 0, 0);
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(backBuffer, RenderTextureLayout::PRESENT));
  } else {
    // No usable front buffer: clear so we still present.
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(backBuffer, RenderTextureLayout::COLOR_WRITE));
    commandList->setFramebuffer(framebuffer);
    commandList->clearColor(0, RenderColor(0.10f, 0.20f, 0.40f, 1.0f));
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(backBuffer, RenderTextureLayout::PRESENT));
  }
  commandList->end();
  g_frameOpen = false;
  g_presentSource = nullptr;

  RenderCommandSemaphore *waitSemaphores[] = {frame.acquireSemaphore.get()};
  RenderCommandSemaphore *signalSemaphores[] = {frame.renderSemaphore.get()};
  const RenderCommandList *commandLists[] = {commandList};
  g_queue->executeCommandLists(commandLists, 1, waitSemaphores, 1,
                               signalSemaphores, 1,
                               frame.commandFence.get());
  frame.commandsInFlight = true;

  g_swapChainValid =
      g_swapChain->present(g_backBufferIndex, signalSemaphores, 1);
  g_frameSlot = (g_frameSlot + 1) % kFramesInFlight;
}

void Video::WaitForGPU() {
  if (!g_initialized) {
    return;
  }
  for (FrameContext &frame : g_frames)
    WaitForFrame(frame);
  for (uint32_t i = 0; i < kFramesInFlight; ++i)
    RetireFrame(i);

  assert(!g_frameOpen);
  FrameContext &frame = CurrentFrame();
  frame.commandList->begin();
  frame.commandList->end();
  g_queue->executeCommandLists(frame.commandList.get(),
                               frame.commandFence.get());
  frame.commandsInFlight = true;
  WaitForFrame(frame);
}

void Video::Shutdown() {
  if (!g_initialized) {
    return;
  }
  WaitForGPU();
  g_blitPipelines.clear();
  g_blitPixelShader.reset();
  g_blitVertexShader.reset();
  g_pipelineLayout.reset();
  g_defaultSampler.reset();
  g_samplerDescriptorSet.reset();
  g_textureDescriptorSet.reset();
  g_copyFence.reset();
  g_copyCommandList.reset();
  g_copyQueue.reset();
  g_framebuffers.clear();
  g_swapChain.reset();
  for (FrameContext &frame : g_frames) {
    frame.renderSemaphore.reset();
    frame.acquireSemaphore.reset();
    frame.commandFence.reset();
    frame.commandList.reset();
    frame.commandsInFlight = false;
  }
  g_queue.reset();
  g_device.reset();
  g_interface.reset();
  g_initialized = false;
}
