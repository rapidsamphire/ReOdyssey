#pragma once

#include <cstdint>
#include <new>
#include <utility>

#include <rex/memory.h>
#include <rex/system/kernel_state.h>

namespace reodyssey::ghp {

inline auto* GuestMemory() { return rex::system::kernel_state()->memory(); }
inline uint8_t* GuestBase() { return GuestMemory()->virtual_membase(); }

// Raw guest-memory allocation; returns a guest virtual address (0 on failure).
inline uint32_t GuestAllocRaw(uint32_t size, uint32_t alignment = 0x10) {
  return GuestMemory()->SystemHeapAlloc(size, alignment);
}

inline void GuestFreeRaw(uint32_t guestAddress) {
  if (guestAddress) GuestMemory()->SystemHeapFree(guestAddress);
}

// host pointer -> guest virtual address.
inline uint32_t ToGuest(const void* host) {
  if (!host) return 0;
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(host) -
                               reinterpret_cast<uintptr_t>(GuestBase()));
}

// guest virtual address -> host pointer.
template <typename T>
inline T* ToHost(uint32_t guestAddress) {
  return guestAddress ? rex::memory::GuestPtr<T*>(GuestBase(), guestAddress) : nullptr;
}

// Construct a T inside guest memory; returns the host pointer (its guest address
// is ToGuest(result)). Returns nullptr if the allocation failed.
template <typename T, typename... Args>
inline T* GuestNew(Args&&... args) {
  constexpr uint32_t align = alignof(T) < 0x10 ? 0x10 : alignof(T);
  uint32_t addr = GuestAllocRaw(sizeof(T), align);
  if (!addr) return nullptr;
  T* host = ToHost<T>(addr);
  return new (host) T(std::forward<Args>(args)...);
}

// Destroy a guest-allocated T and free its memory.
template <typename T>
inline void GuestDelete(T* host) {
  if (!host) return;
  uint32_t addr = ToGuest(host);
  host->~T();
  GuestFreeRaw(addr);
}

}  // namespace reodyssey::ghp
