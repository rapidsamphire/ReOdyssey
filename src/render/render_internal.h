// render/render_internal.h
// Shared internals between the render translation units 

#pragma once

#include <cstdint>
#include <functional>

#include <plume_render_interface.h>

namespace reodyssey::render {

// Bindless table sizes (match the shader-side descriptor arrays / ABI).
inline constexpr uint32_t kTextureDescriptorSize = 65536;
inline constexpr uint32_t kSamplerDescriptorSize = 1024;

inline constexpr uint32_t kNullTexture2DDescriptor = 0;
inline constexpr uint32_t kNullTexture3DDescriptor = 1;
inline constexpr uint32_t kNullTextureCubeDescriptor = 2;
inline constexpr uint32_t kNullTextureDescriptorCount = 3;

// The active Plume interface and device created by Video::Init(), or nullptr.
plume::RenderInterface *Interface();
plume::RenderDevice *Device();

struct GuestBaseTexture;
void SetPresentSource(GuestBaseTexture *frontBuffer);

plume::RenderDescriptorSet *TextureDescriptorSet();

plume::RenderDescriptorSet *SamplerDescriptorSet();

plume::RenderPipelineLayout *PipelineLayout();

plume::RenderPipeline *GetBlitPipeline(plume::RenderFormat format);

uint32_t CurrentFrameSlot();

uint32_t AllocTextureDescriptor();
void FreeTextureDescriptor(uint32_t index);

void ExecuteUpload(
    const std::function<void(plume::RenderCommandList *)> &record);
plume::RenderCommandList *CommandList();

struct GuestShader;
plume::RenderShader *LoadShader(GuestShader *guestShader,
                                uint32_t specConstants = 0);

struct GuestVertexDeclaration;
GuestVertexDeclaration *LookupVertexDeclarationAlias(uint32_t guestAddress);

} // namespace reodyssey::render
