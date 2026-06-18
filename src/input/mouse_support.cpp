// Native mouse bridge for Lost Odyssey 

#include "generated/reodyssey_init.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

REXCVAR_DECLARE(bool, lo_mouse_debug);
REXCVAR_DECLARE(bool, lo_mouse_support);

namespace {

constexpr float kGuestUiWidth = 1280.0f;
constexpr float kGuestUiHeight = 720.0f;
constexpr uint64_t kHoverActivationWindowMs = 1000;
constexpr uint64_t kHoverTargetWindowMs = 100;

constexpr uint32_t kControllerId = 0;
constexpr uint32_t kPressedAmount = 255;
constexpr uint32_t kReleasedAmount = 0;
constexpr uint32_t kXboxTypeSAKeySlot = 10;
constexpr uint32_t kFNameXboxTypeSA = 0x8336A1D0;
constexpr uint32_t kLoUiActionActivate = 1;

constexpr uint32_t kScrollListVisibleStartOffset = 0x2C;
constexpr uint32_t kScrollListSelectedIndexOffset = 0x38;
constexpr uint32_t kScrollListItemArrayOffset = 0x44;
constexpr uint32_t kScrollListItemCountOffset = 0x48;
constexpr uint32_t kScrollListRowArrayOffset = 0x78;
constexpr uint32_t kScrollListVisibleRowCountOffset = 0x7C;

constexpr uint32_t kCompactListRowArrayOffset = 0x88;
constexpr uint32_t kCompactListVisibleRowCountOffset = 0x8C;

constexpr uint32_t kDirectRowsRowArrayOffset = 0x44;
constexpr uint32_t kDirectRowsVisibleEndOffset = 0x30;

constexpr uint32_t kChoiceMenuItemArrayOffset = 0xC0;
constexpr uint32_t kChoiceMenuItemCountOffset = 0xC4;
constexpr uint32_t kChoiceMenuSelectedIndexOffset = 0x25C;
constexpr uint32_t kChoiceMenuItemStride = 536;
constexpr uint32_t kChoiceMenuControlOffset = 332;

constexpr uint32_t kUiListRows36ItemArrayOffset = 0x98;
constexpr uint32_t kUiListRows32AItemArrayOffset = 0x9C;
constexpr uint32_t kUiListRows32BItemArrayOffset = 0x98;
constexpr uint32_t kGenericListItemBaseOffset = 0x3898;

constexpr uint32_t kScrollListRowStride = 204;
constexpr uint32_t kDirectRowsRowStride = 236;
constexpr uint32_t kScrollListRowXOffset = 0x04;
constexpr uint32_t kScrollListRowYOffset = 0x08;
constexpr uint32_t kScrollListRowWidthOffset = 0x0C;
constexpr uint32_t kScrollListRowHeightOffset = 0x10;
constexpr uint32_t kMaxHoverTargets = 128;

struct MousePosition {
  float x;
  float y;
};

enum class HoverTargetKind {
  kScrollList,
  kUiList,
  kGenericList,
};

struct HoverTarget {
  uint32_t row_addr;
  uint32_t list_addr;
  int32_t item_index;
  HoverTargetKind kind;
  uint32_t selected_index_offset;
};

uint64_t g_last_selectable_hover_ms = 0;
uint64_t g_last_scroll_list_hover_ms = 0;
uint64_t g_hover_targets_updated_ms = 0;
uint64_t g_last_debug_log_ms = 0;
uint64_t g_last_viewport_debug_log_ms = 0;
uint64_t g_last_hook_entry_debug_log_ms = 0;
uint64_t g_last_control_state_debug_log_ms = 0;
uint64_t g_last_ui_action_debug_log_ms = 0;
uint64_t g_last_activation_debug_log_ms = 0;
uint32_t g_hovered_scroll_list_addr = 0;
int32_t g_hovered_scroll_list_item_index = -1;
HoverTarget g_hover_targets[kMaxHoverTargets]{};
uint32_t g_hover_target_count = 0;
bool g_sent_mouse_activation_down = false;
bool g_mouse_left_down_previous = false;
bool g_mouse_activation_pending = false;
bool g_mouse_activation_consumed = false;
uint32_t g_pending_viewport = 0;
double g_pending_time = 0.0;

uint32_t LoadU32(uint32_t addr) {
  uint8_t *base = rex::system::kernel_state()->memory()->virtual_membase();
  return REX_LOAD_U32(addr);
}

uint64_t LoadU64(uint32_t addr) {
  uint8_t *base = rex::system::kernel_state()->memory()->virtual_membase();
  return REX_LOAD_U64(addr);
}

int32_t LoadS32(uint32_t addr) { return static_cast<int32_t>(LoadU32(addr)); }

float LoadFloat(uint32_t addr) { return std::bit_cast<float>(LoadU32(addr)); }

void StoreU32(uint32_t addr, uint32_t value) {
  uint8_t *base = rex::system::kernel_state()->memory()->virtual_membase();
  REX_STORE_U32(addr, value);
}

bool HasGuestMemory() {
  auto *kernel_state = rex::system::kernel_state();
  return kernel_state && kernel_state->memory() &&
         kernel_state->memory()->virtual_membase();
}

#if REX_PLATFORM_WIN32
struct WindowSearch {
  HWND window = nullptr;
  int area = 0;
  DWORD process_id = 0;
};

BOOL CALLBACK FindProcessWindow(HWND window, LPARAM param) {
  auto *search = reinterpret_cast<WindowSearch *>(param);

  DWORD window_process_id = 0;
  GetWindowThreadProcessId(window, &window_process_id);
  if (window_process_id != search->process_id || !IsWindowVisible(window)) {
    return TRUE;
  }

  RECT client_rect{};
  if (!GetClientRect(window, &client_rect)) {
    return TRUE;
  }

  const int width = client_rect.right - client_rect.left;
  const int height = client_rect.bottom - client_rect.top;
  const int area = width * height;
  if (width <= 0 || height <= 0 || area <= search->area) {
    return TRUE;
  }

  search->window = window;
  search->area = area;
  return TRUE;
}

HWND GetGameWindow() {
  const DWORD current_process_id = GetCurrentProcessId();

  HWND window = GetForegroundWindow();
  if (window) {
    DWORD window_process_id = 0;
    GetWindowThreadProcessId(window, &window_process_id);
    if (window_process_id == current_process_id) {
      return window;
    }
  }

  WindowSearch search{};
  search.process_id = current_process_id;
  EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
  return search.window;
}
#endif

bool TryGetMousePosition(MousePosition *position) {
#if REX_PLATFORM_WIN32
  HWND window = GetGameWindow();
  if (!window) {
    return false;
  }

  POINT point{};
  if (!GetCursorPos(&point) || !ScreenToClient(window, &point)) {
    return false;
  }

  RECT client_rect{};
  if (!GetClientRect(window, &client_rect)) {
    return false;
  }

  const int client_width = client_rect.right - client_rect.left;
  const int client_height = client_rect.bottom - client_rect.top;
  if (client_width <= 0 || client_height <= 0 || point.x < 0 || point.y < 0 ||
      point.x >= client_width || point.y >= client_height) {
    return false;
  }

  position->x = static_cast<float>(point.x) * kGuestUiWidth /
                static_cast<float>(client_width);
  position->y = static_cast<float>(point.y) * kGuestUiHeight /
                static_cast<float>(client_height);
  return true;
#else
  (void)position;
  return false;
#endif
}

uint64_t GetHostTimeMs() {
#if REX_PLATFORM_WIN32
  return GetTickCount64();
#else
  return 0;
#endif
}

bool IsLeftMouseDown() {
#if REX_PLATFORM_WIN32
  return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
  return false;
#endif
}

bool UpdateMouseButtonState(bool *pressed = nullptr) {
  const bool left_down = IsLeftMouseDown();
  if (pressed) {
    *pressed = left_down && !g_mouse_left_down_previous;
  }
  if (!left_down) {
    g_mouse_activation_pending = false;
    g_mouse_activation_consumed = false;
  }
  g_mouse_left_down_previous = left_down;
  return left_down;
}

bool TryActivateHoveredScrollList();

void QueueMouseActivation() {
  bool pressed = false;
  if (UpdateMouseButtonState(&pressed) && pressed &&
      !g_mouse_activation_consumed) {
    if (TryActivateHoveredScrollList()) {
      g_mouse_activation_pending = false;
      g_mouse_activation_consumed = true;
      return;
    }
    g_mouse_activation_pending = true;
  }
}

bool HasRecentSelectableHover() {
  const uint64_t now = GetHostTimeMs();
  return now != 0 && g_last_selectable_hover_ms != 0 &&
         now - g_last_selectable_hover_ms <= kHoverActivationWindowMs;
}

bool HasRecentScrollListHover() {
  const uint64_t now = GetHostTimeMs();
  return now != 0 && g_last_scroll_list_hover_ms != 0 &&
         now - g_last_scroll_list_hover_ms <= kHoverActivationWindowMs;
}

bool IsMouseActivationAction(uint32_t action_id) {
  return action_id == kLoUiActionActivate;
}

bool IsFinitePositive(float value) {
  return std::isfinite(value) && value > 0.0f;
}

bool IsInsideRow(const MousePosition &mouse, uint32_t row_addr) {
  const float x = LoadFloat(row_addr + kScrollListRowXOffset);
  const float y = LoadFloat(row_addr + kScrollListRowYOffset);
  const float width = LoadFloat(row_addr + kScrollListRowWidthOffset);
  const float height = LoadFloat(row_addr + kScrollListRowHeightOffset);

  if (!std::isfinite(x) || !std::isfinite(y) || !IsFinitePositive(width) ||
      !IsFinitePositive(height)) {
    return false;
  }

  return mouse.x >= x && mouse.y >= y && mouse.x < x + width &&
         mouse.y < y + height;
}

uint8_t *GetGuestMemoryBase() {
  auto *kernel_state = rex::system::kernel_state();
  if (!kernel_state || !kernel_state->memory()) {
    return nullptr;
  } 

  return kernel_state->memory()->virtual_membase();
}

rex::runtime::ThreadState *GetThreadStateWithContext() {
  auto *thread_state = rex::runtime::ThreadState::Get();
  if (!thread_state || !thread_state->context()) {
    return nullptr;
  }

  return thread_state;
}

uint32_t GuestAppMalloc(uint32_t size, uint32_t alignment) {
  auto *thread_state = GetThreadStateWithContext();
  uint8_t *base = GetGuestMemoryBase();
  if (!thread_state || !base) {
    return 0;
  }

  rex::CallFrame frame(*thread_state->context());
  frame.ctx.r3.u64 = size;
  frame.ctx.r4.u64 = alignment;
  rex_appMalloc_YAPAXKK_Z(frame.ctx, base);
  return frame.ctx.r3.u32;
}

void GuestAppFree(uint32_t addr) {
  if (addr == 0) {
    return;
  }

  auto *thread_state = GetThreadStateWithContext();
  uint8_t *base = GetGuestMemoryBase();
  if (!thread_state || !base) {
    return;
  }

  rex::CallFrame frame(*thread_state->context());
  frame.ctx.r3.u64 = addr;
  rex_appFree_YAXPAX_Z(frame.ctx, base);
}

bool CallScrollListSelectIndex(uint32_t list_addr, int32_t item_index) {
  if (list_addr == 0 || item_index < 0) {
    return false;
  }

  auto *thread_state = GetThreadStateWithContext();
  uint8_t *base = GetGuestMemoryBase();
  if (!thread_state || !base) {
    return false;
  }

  rex::CallFrame frame(*thread_state->context());
  frame.ctx.r3.u64 = list_addr;
  frame.ctx.r4.u64 = static_cast<uint32_t>(item_index);
  sub_828A3138(frame.ctx, base);
  return true;
}

int32_t CallScrollListActivateSelected(uint32_t list_addr) {
  if (list_addr == 0) {
    return -20;
  }

  auto *thread_state = GetThreadStateWithContext();
  uint8_t *base = GetGuestMemoryBase();
  if (!thread_state || !base) {
    return -20;
  }

  const uint32_t out_ptr = GuestAppMalloc(sizeof(uint32_t), alignof(uint32_t));
  if (out_ptr == 0) {
    return -20;
  }

  StoreU32(out_ptr, 0);
  rex::CallFrame frame(*thread_state->context());
  frame.ctx.r3.u64 = list_addr;
  frame.ctx.r4.u64 = out_ptr;
  sub_828DF688(frame.ctx, base);
  const int32_t result = frame.ctx.r3.s32;
  GuestAppFree(out_ptr);
  return result;
}

void ResetHoverTargets() {
  g_hover_target_count = 0;
  g_hover_targets_updated_ms = GetHostTimeMs();
}

void RegisterHoverTarget(uint32_t row_addr, uint32_t list_addr,
                         int32_t item_index, HoverTargetKind kind,
                         uint32_t selected_index_offset) {
  if (row_addr == 0 || list_addr == 0 || item_index < 0 ||
      selected_index_offset == 0 || g_hover_target_count >= kMaxHoverTargets) {
    return;
  }

  g_hover_targets[g_hover_target_count++] = {
      row_addr, list_addr, item_index, kind, selected_index_offset,
  };
}

void RegisterVisibleRows(uint32_t list_addr, HoverTargetKind kind,
                         int32_t visible_start, int32_t item_count,
                         int32_t visible_row_count, uint32_t row_array,
                         uint32_t row_stride, int32_t row_index_base,
                         uint32_t selected_index_offset) {
  ResetHoverTargets();
  if (list_addr == 0 || visible_start < 0 || item_count <= 0 ||
      visible_row_count <= 0 || row_array == 0 || row_stride == 0 ||
      row_index_base < 0 || selected_index_offset == 0) {
    return;
  }

  const int32_t clamped_row_count =
      std::min<int32_t>(visible_row_count, item_count - visible_start);
  for (int32_t row_index = 0; row_index < clamped_row_count; ++row_index) {
    const int32_t item_index = visible_start + row_index;
    if (item_index < 0 || item_index >= item_count) {
      continue;
    }

    const uint32_t row_addr =
        row_array +
        static_cast<uint32_t>(row_index_base + row_index) * row_stride;
    RegisterHoverTarget(row_addr, list_addr, item_index, kind,
                        selected_index_offset);
  }
}

const HoverTarget *FindHoverTarget(uint32_t row_addr) {
  if (row_addr == 0 || g_hover_target_count == 0) {
    return nullptr;
  }

  const uint64_t now = GetHostTimeMs();
  if (now != 0 && g_hover_targets_updated_ms != 0 &&
      now - g_hover_targets_updated_ms > kHoverTargetWindowMs) {
    return nullptr;
  }

  for (uint32_t i = g_hover_target_count; i > 0; --i) {
    const HoverTarget &target = g_hover_targets[i - 1];
    if (target.row_addr == row_addr) {
      return &target;
    }
  }

  return nullptr;
}

bool ApplyHoverTargetSelection(const HoverTarget &target) {
  if (target.list_addr == 0 || target.item_index < 0 ||
      target.selected_index_offset == 0) {
    return false;
  }

  if (target.kind == HoverTargetKind::kScrollList) {
    if (!CallScrollListSelectIndex(target.list_addr, target.item_index)) {
      StoreU32(target.list_addr + kScrollListSelectedIndexOffset,
               static_cast<uint32_t>(target.item_index));
    }

    const uint64_t now = GetHostTimeMs();
    g_last_selectable_hover_ms = now;
    g_last_scroll_list_hover_ms = now;
    g_hovered_scroll_list_addr = target.list_addr;
    g_hovered_scroll_list_item_index = target.item_index;
    return true;
  }

  StoreU32(target.list_addr + target.selected_index_offset,
           static_cast<uint32_t>(target.item_index));
  g_last_selectable_hover_ms = GetHostTimeMs();
  return true;
}

bool ApplyHoverTargetSelection(uint32_t row_addr) {
  const HoverTarget *target = FindHoverTarget(row_addr);
  return target != nullptr && ApplyHoverTargetSelection(*target);
}

void LogMouseDebug(uint32_t list_addr, const MousePosition &mouse,
                   int32_t visible_start, int32_t item_count,
                   int32_t visible_row_count, uint32_t item_array,
                   uint32_t row_array, uint32_t row_stride,
                   int32_t row_index_base, int32_t hit_row, bool selected) {
  if (!REXCVAR_GET(lo_mouse_debug)) {
    return;
  }

  const uint64_t now = GetHostTimeMs();
  if (now == 0 || now - g_last_debug_log_ms < 1000) {
    return;
  }
  g_last_debug_log_ms = now;

  float row_x = 0.0f;
  float row_y = 0.0f;
  float row_width = 0.0f;
  float row_height = 0.0f;
  if (row_array != 0 && visible_row_count > 0) {
    const uint32_t row0_addr =
        row_array + static_cast<uint32_t>(row_index_base) * row_stride;
    row_x = LoadFloat(row0_addr + kScrollListRowXOffset);
    row_y = LoadFloat(row0_addr + kScrollListRowYOffset);
    row_width = LoadFloat(row0_addr + kScrollListRowWidthOffset);
    row_height = LoadFloat(row0_addr + kScrollListRowHeightOffset);
  }

  REXLOG_INFO(
      "LO mouse hook list={:#x} mouse=({:.1f},{:.1f}) start={} selected={} "
      "count={} rows={} item_array={:#x} row_array={:#x} row0=({:.1f},{:.1f},"
      "{:.1f},{:.1f}) hit_row={} selected_now={}",
      list_addr, mouse.x, mouse.y, visible_start,
      LoadS32(list_addr + kScrollListSelectedIndexOffset), item_count,
      visible_row_count, item_array, row_array, row_x, row_y, row_width,
      row_height, hit_row, selected);
}

void HoverSelectRows(uint32_t list_addr, const MousePosition &mouse,
                     HoverTargetKind kind, int32_t visible_start,
                     int32_t item_count, int32_t visible_row_count,
                     uint32_t item_array, uint32_t row_array,
                     uint32_t row_stride, int32_t row_index_base,
                     uint32_t selected_index_offset) {
  RegisterVisibleRows(list_addr, kind, visible_start, item_count,
                      visible_row_count, row_array, row_stride, row_index_base,
                      selected_index_offset);
  if (visible_start < 0 || item_count <= 0 || visible_row_count <= 0 ||
      row_array == 0 || row_stride == 0 || row_index_base < 0 ||
      selected_index_offset == 0) {
    LogMouseDebug(list_addr, mouse, visible_start, item_count,
                  visible_row_count, item_array, row_array, row_stride,
                  row_index_base, -1, false);
    return;
  }

  int32_t hit_row = -1;
  bool selected = false;
  const int32_t clamped_row_count =
      std::min<int32_t>(visible_row_count, item_count - visible_start);
  for (int32_t row_index = 0; row_index < clamped_row_count; ++row_index) {
    const uint32_t row_addr =
        row_array +
        static_cast<uint32_t>(row_index_base + row_index) * row_stride;
    if (!IsInsideRow(mouse, row_addr)) {
      continue;
    }

    hit_row = row_index;
    const int32_t item_index = visible_start + row_index;
    if (item_index < 0 || item_index >= item_count) {
      LogMouseDebug(list_addr, mouse, visible_start, item_count,
                    visible_row_count, item_array, row_array, row_stride,
                    row_index_base, hit_row, false);
      return;
    }

    selected = ApplyHoverTargetSelection(HoverTarget{
        row_addr, list_addr, item_index, kind, selected_index_offset});
    LogMouseDebug(list_addr, mouse, visible_start, item_count,
                  visible_row_count, item_array, row_array, row_stride,
                  row_index_base, hit_row, selected);
    return;
  }

  LogMouseDebug(list_addr, mouse, visible_start, item_count, visible_row_count,
                item_array, row_array, row_stride, row_index_base, hit_row,
                selected);
}

void HoverSelectScrollList(uint32_t list_addr, const MousePosition &mouse) {
  HoverSelectRows(list_addr, mouse, HoverTargetKind::kScrollList,
                  LoadS32(list_addr + kScrollListVisibleStartOffset),
                  LoadS32(list_addr + kScrollListItemCountOffset),
                  LoadS32(list_addr + kScrollListVisibleRowCountOffset),
                  LoadU32(list_addr + kScrollListItemArrayOffset),
                  LoadU32(list_addr + kScrollListRowArrayOffset),
                  kScrollListRowStride, 0, kScrollListSelectedIndexOffset);
}

void HoverSelectUiList(uint32_t list_addr, const MousePosition &mouse,
                       uint32_t item_array_offset) {
  HoverSelectRows(list_addr, mouse, HoverTargetKind::kUiList,
                  LoadS32(list_addr + kScrollListVisibleStartOffset),
                  LoadS32(list_addr + kScrollListItemCountOffset),
                  LoadS32(list_addr + kScrollListVisibleRowCountOffset),
                  LoadU32(list_addr + item_array_offset),
                  LoadU32(list_addr + kScrollListRowArrayOffset),
                  kScrollListRowStride, 0, kScrollListSelectedIndexOffset);
}

void HoverSelectGenericList(uint32_t list_addr, const MousePosition &mouse) {
  HoverSelectRows(list_addr, mouse, HoverTargetKind::kGenericList,
                  LoadS32(list_addr + kScrollListVisibleStartOffset),
                  LoadS32(list_addr + kScrollListItemCountOffset),
                  LoadS32(list_addr + kScrollListVisibleRowCountOffset),
                  LoadU32(list_addr + kGenericListItemBaseOffset),
                  LoadU32(list_addr + kScrollListRowArrayOffset),
                  kScrollListRowStride, 0, kScrollListSelectedIndexOffset);
}

void HoverSelectCompactList(uint32_t list_addr, const MousePosition &mouse) {
  HoverSelectRows(list_addr, mouse, HoverTargetKind::kGenericList,
                  LoadS32(list_addr + kScrollListVisibleStartOffset),
                  LoadS32(list_addr + kScrollListItemCountOffset),
                  LoadS32(list_addr + kCompactListVisibleRowCountOffset),
                  LoadU32(list_addr + kScrollListItemArrayOffset),
                  LoadU32(list_addr + kCompactListRowArrayOffset),
                  kScrollListRowStride, 0, kScrollListSelectedIndexOffset);
}

void HoverSelectStandardRows204(uint32_t list_addr,
                                const MousePosition &mouse) {
  HoverSelectRows(list_addr, mouse, HoverTargetKind::kGenericList,
                  LoadS32(list_addr + kScrollListVisibleStartOffset),
                  LoadS32(list_addr + kScrollListItemCountOffset),
                  LoadS32(list_addr + kScrollListVisibleRowCountOffset), 0,
                  LoadU32(list_addr + kScrollListRowArrayOffset),
                  kScrollListRowStride, 0, kScrollListSelectedIndexOffset);
}

void HoverSelectDirectRows236(uint32_t list_addr, const MousePosition &mouse,
                              bool use_visible_range) {
  const int32_t item_count = LoadS32(list_addr + kScrollListItemCountOffset);
  const int32_t visible_start =
      use_visible_range ? LoadS32(list_addr + kScrollListVisibleStartOffset)
                        : 0;
  const int32_t visible_end =
      use_visible_range ? LoadS32(list_addr + kDirectRowsVisibleEndOffset)
                        : item_count;
  HoverSelectRows(list_addr, mouse, HoverTargetKind::kGenericList,
                  visible_start, item_count, visible_end - visible_start, 0,
                  LoadU32(list_addr + kDirectRowsRowArrayOffset),
                  kDirectRowsRowStride, visible_start,
                  kScrollListSelectedIndexOffset);
}

void HoverSelectChoiceMenu(uint32_t menu_addr, const MousePosition &mouse) {
  const int32_t item_count = LoadS32(menu_addr + kChoiceMenuItemCountOffset);
  const uint32_t item_array = LoadU32(menu_addr + kChoiceMenuItemArrayOffset);
  const uint32_t row_array =
      item_array == 0 ? 0 : item_array + kChoiceMenuControlOffset;
  HoverSelectRows(menu_addr, mouse, HoverTargetKind::kGenericList, 0,
                  item_count, item_count, item_array, row_array,
                  kChoiceMenuItemStride, 0, kChoiceMenuSelectedIndexOffset);
}

void InjectXboxTypeSA(uint32_t viewport, double time, bool pressed) {
  auto *thread_state = rex::runtime::ThreadState::Get();
  if (!thread_state || !thread_state->context()) {
    return;
  }

  if (!HasGuestMemory()) {
    return;
  }

  uint8_t *base = rex::system::kernel_state()->memory()->virtual_membase();
  rex::CallFrame frame(*thread_state->context());
  frame.ctx.r3.u64 = viewport;
  frame.ctx.r4.u64 = kXboxTypeSAKeySlot;
  frame.ctx.r5.u64 = kControllerId;
  frame.ctx.r6.u64 = LoadU64(kFNameXboxTypeSA);
  frame.ctx.r7.u64 = pressed ? kPressedAmount : kReleasedAmount;
  frame.ctx.f1.f64 = time;
  rex_PassKeyStateToViewportClient_FXenonViewport_QAAXHHVFName_HN_Z(frame.ctx,
                                                                    base);
}

void LogViewportDebug(uint32_t viewport, bool support_enabled,
                      bool debug_enabled, bool activate) {
  if (!support_enabled && !debug_enabled) {
    return;
  }

  const uint64_t now = GetHostTimeMs();
  if (now == 0 || now - g_last_viewport_debug_log_ms < 1000) {
    return;
  }
  g_last_viewport_debug_log_ms = now;

  REXLOG_INFO(
      "LO mouse viewport hook viewport={:#x} support={} debug={} activate={} "
      "recent_hover={} left_down={}",
      viewport, support_enabled, debug_enabled, activate,
      HasRecentSelectableHover(), IsLeftMouseDown());
}

void LogHoverHookEntry(const char *hook_name, uint32_t list_addr,
                       bool support_enabled, bool has_memory, bool mouse_ok,
                       const MousePosition &mouse) {
  if (!REXCVAR_GET(lo_mouse_debug)) {
    return;
  }

  const uint64_t now = GetHostTimeMs();
  if (now == 0 || now - g_last_hook_entry_debug_log_ms < 1000) {
    return;
  }
  g_last_hook_entry_debug_log_ms = now;

  REXLOG_INFO(
      "LO mouse hover hook entry hook={} list={:#x} support={} memory={} "
      "mouse_ok={} mouse=({:.1f},{:.1f})",
      hook_name, list_addr, support_enabled, has_memory, mouse_ok, mouse.x,
      mouse.y);
}

void LogControlStateDebug(uint32_t control_addr, uint32_t state_kind,
                          uint32_t state_value, bool hovered) {
  if (!REXCVAR_GET(lo_mouse_debug)) {
    return;
  }

  const uint64_t now = GetHostTimeMs();
  if (now == 0 || now - g_last_control_state_debug_log_ms < 1000) {
    return;
  }
  g_last_control_state_debug_log_ms = now;

  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  if (control_addr != 0 && HasGuestMemory()) {
    x = LoadFloat(control_addr + kScrollListRowXOffset);
    y = LoadFloat(control_addr + kScrollListRowYOffset);
    width = LoadFloat(control_addr + kScrollListRowWidthOffset);
    height = LoadFloat(control_addr + kScrollListRowHeightOffset);
  }

  REXLOG_INFO("LO mouse control state hook control={:#x} kind={} value={} "
              "bounds=({:.1f},{:.1f},{:.1f},{:.1f}) hovered={}",
              control_addr, state_kind, state_value, x, y, width, height,
              hovered);
}

void LogUiActionDebug(const char *hook_name, uint32_t action_id, bool forced) {
  if (!REXCVAR_GET(lo_mouse_debug)) {
    return;
  }

  const uint64_t now = GetHostTimeMs();
  if (!forced && (now == 0 || now - g_last_ui_action_debug_log_ms < 500)) {
    return;
  }
  g_last_ui_action_debug_log_ms = now;

  REXLOG_INFO(
      "LO mouse ui action hook hook={} action={} forced={} recent_hover={} "
      "left_down={} pending={} consumed={}",
      hook_name, action_id, forced, HasRecentSelectableHover(),
      IsLeftMouseDown(), g_mouse_activation_pending,
      g_mouse_activation_consumed);
}

void LogActivationDebug(uint32_t list_addr, int32_t item_index,
                        int32_t result) {
  if (!REXCVAR_GET(lo_mouse_debug)) {
    return;
  }

  const uint64_t now = GetHostTimeMs();
  if (now == 0 || now - g_last_activation_debug_log_ms < 250) {
    return;
  }
  g_last_activation_debug_log_ms = now;

  REXLOG_INFO("LO mouse activate scroll list list={:#x} item={} result={} "
              "recent_hover={} left_down={}",
              list_addr, item_index, result, HasRecentScrollListHover(),
              IsLeftMouseDown());
}

bool TryActivateHoveredScrollList() {
  if (!REXCVAR_GET(lo_mouse_support) || !HasRecentScrollListHover() ||
      g_hovered_scroll_list_addr == 0 || !HasGuestMemory()) {
    return false;
  }

  const int32_t item_index =
      LoadS32(g_hovered_scroll_list_addr + kScrollListSelectedIndexOffset);
  const int32_t item_count =
      LoadS32(g_hovered_scroll_list_addr + kScrollListItemCountOffset);
  if (item_index < 0 || item_index >= item_count ||
      item_index != g_hovered_scroll_list_item_index) {
    return false;
  }

  CallScrollListSelectIndex(g_hovered_scroll_list_addr, item_index);
  const int32_t result =
      CallScrollListActivateSelected(g_hovered_scroll_list_addr);
  LogActivationDebug(g_hovered_scroll_list_addr, item_index, result);
  return result >= 0;
}

bool TryForceMouseActivationAction(PPCRegister &r3, PPCRegister &r4,
                                   const char *hook_name) {
  const uint32_t action_id = r4.u32 & 0xFF;
  bool pressed = false;
  UpdateMouseButtonState(&pressed);

  if (REXCVAR_GET(lo_mouse_support) && IsMouseActivationAction(action_id) &&
      HasRecentSelectableHover() && IsLeftMouseDown() && pressed &&
      !g_mouse_activation_consumed) {
    if (TryActivateHoveredScrollList()) {
      g_mouse_activation_pending = false;
      g_mouse_activation_consumed = true;
      LogUiActionDebug(hook_name, action_id, false);
      return false;
    }
    g_mouse_activation_pending = true;
  }

  const bool forced =
      REXCVAR_GET(lo_mouse_support) && IsMouseActivationAction(action_id) &&
      HasRecentSelectableHover() && IsLeftMouseDown() &&
      g_mouse_activation_pending && !g_mouse_activation_consumed;
  LogUiActionDebug(hook_name, action_id, forced);
  if (!forced) {
    return false;
  }

  r3.u64 = 1;
  g_mouse_activation_pending = false;
  g_mouse_activation_consumed = true;
  return true;
}

} // namespace

REXCVAR_DEFINE_BOOL(
    lo_mouse_support, false, "Lost Odyssey/Input",
    "Use host mouse hover to select Lost Odyssey native scroll-list rows.");
REXCVAR_DEFINE_BOOL(lo_mouse_debug, false, "Lost Odyssey/Input",
                    "Log Lost Odyssey mouse hover hook diagnostics.");

void LoMouseSupportScrollListHoverHook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("scroll_list", r3.u32, support_enabled, has_memory,
                    mouse_ok, mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectScrollList(r3.u32, mouse);
}

void LoMouseSupportUiListRows36Hook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("rows36", r3.u32, support_enabled, has_memory, mouse_ok,
                    mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectUiList(r3.u32, mouse, kUiListRows36ItemArrayOffset);
}

void LoMouseSupportUiListRows32AHook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("rows32_a", r3.u32, support_enabled, has_memory, mouse_ok,
                    mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectUiList(r3.u32, mouse, kUiListRows32AItemArrayOffset);
}

void LoMouseSupportUiListRows32BHook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("rows32_b", r3.u32, support_enabled, has_memory, mouse_ok,
                    mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectUiList(r3.u32, mouse, kUiListRows32BItemArrayOffset);
}

void LoMouseSupportGenericListHoverHook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("generic_list", r3.u32, support_enabled, has_memory,
                    mouse_ok, mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectGenericList(r3.u32, mouse);
}

void LoMouseSupportCompactListHoverHook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("compact_list", r3.u32, support_enabled, has_memory,
                    mouse_ok, mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectCompactList(r3.u32, mouse);
}

void LoMouseSupportStandardRows204Hook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("standard_rows204", r3.u32, support_enabled, has_memory,
                    mouse_ok, mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectStandardRows204(r3.u32, mouse);
}

void LoMouseSupportDirectRows236RangeHook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("direct_rows236_range", r3.u32, support_enabled, has_memory,
                    mouse_ok, mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectDirectRows236(r3.u32, mouse, true);
}

void LoMouseSupportDirectRows236AllHook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("direct_rows236_all", r3.u32, support_enabled, has_memory,
                    mouse_ok, mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectDirectRows236(r3.u32, mouse, false);
}

void LoMouseSupportChoiceMenuHoverHook(PPCRegister &r3) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool has_memory = HasGuestMemory();
  MousePosition mouse{};
  const bool mouse_ok = TryGetMousePosition(&mouse);
  LogHoverHookEntry("choice_menu", r3.u32, support_enabled, has_memory,
                    mouse_ok, mouse);

  if (!support_enabled || r3.u32 == 0 || !has_memory || !mouse_ok) {
    return;
  }

  HoverSelectChoiceMenu(r3.u32, mouse);
}

void LoMouseSupportViewportHook(PPCRegister &r3, PPCRegister &f1) {
  const bool support_enabled = REXCVAR_GET(lo_mouse_support);
  const bool debug_enabled = REXCVAR_GET(lo_mouse_debug);
  if (support_enabled && HasRecentSelectableHover() && IsLeftMouseDown()) {
    QueueMouseActivation();
  } else {
    UpdateMouseButtonState();
  }

  const bool activate = support_enabled && HasRecentSelectableHover() &&
                        IsLeftMouseDown() && !g_mouse_activation_consumed;

  LogViewportDebug(r3.u32, support_enabled, debug_enabled, activate);

  if (!support_enabled || r3.u32 == 0) {
    return;
  }

  if (!activate && !g_sent_mouse_activation_down) {
    return;
  }

  InjectXboxTypeSA(r3.u32, f1.f64, activate);
  g_sent_mouse_activation_down = activate;
}

void LoMouseSupportViewportPreHook(PPCRegister &r3, PPCRegister &f1) {
  if (r3.u32 == 0) {
    g_pending_viewport = 0;
    return;
  }

  g_pending_viewport = r3.u32;
  g_pending_time = f1.f64;
}

void LoMouseSupportViewportPostHook() {
  if (g_pending_viewport == 0) {
    return;
  }

  PPCRegister r3{};
  PPCRegister f1{};
  r3.u64 = g_pending_viewport;
  f1.f64 = g_pending_time;
  g_pending_viewport = 0;

  LoMouseSupportViewportHook(r3, f1);
}

void LoMouseSupportControlStateDebugHook(PPCRegister &r3, PPCRegister &r4,
                                         PPCRegister &r5) {
  bool hovered = false;
  if (REXCVAR_GET(lo_mouse_support) && r3.u32 != 0 && HasGuestMemory() &&
      (r4.u32 == 1 || r4.u32 == 3)) {
    MousePosition mouse{};
    if (TryGetMousePosition(&mouse) && IsInsideRow(mouse, r3.u32)) {
      hovered = true;
      r5.u64 = 1;
      if (!ApplyHoverTargetSelection(r3.u32)) {
        g_last_selectable_hover_ms = GetHostTimeMs();
      }
      QueueMouseActivation();
    }
  }

  LogControlStateDebug(r3.u32, r4.u32, r5.u32, hovered);
}

bool LoMouseSupportUiActionConsumeHook(PPCRegister &r3, PPCRegister &r4) {
  return TryForceMouseActivationAction(r3, r4, "consume");
}

bool LoMouseSupportUiActionIsPressedHook(PPCRegister &r3, PPCRegister &r4) {
  return TryForceMouseActivationAction(r3, r4, "pressed");
}

bool LoMouseSupportUiActionIsActiveHook(PPCRegister &r3, PPCRegister &r4) {
  return TryForceMouseActivationAction(r3, r4, "active");
}
