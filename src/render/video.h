// render/video.h

#pragma once

#include <cstdint>

struct Video {
  // Host viewport dimensions (backbuffer size).
  static inline uint32_t s_viewportWidth = 1280;
  static inline uint32_t s_viewportHeight = 720;

  // Create the Plume interface/device/queue/swapchain for the given native
  // window. Safe to call once; returns false if no backend could be created.
  static bool Init(void* nativeWindowHandle, uint32_t width, uint32_t height);

  // Has Init() succeeded?
  static bool IsInitialized();

  // Acquire the next backbuffer, clear it, and present. This is the milestone-1
  // present path, driven by the guest's D3DDevice_Swap hook.
  static void Present();

  // Block until the GPU has finished all submitted work.
  static void WaitForGPU();

  // Release all host GPU resources.
  static void Shutdown();
};
