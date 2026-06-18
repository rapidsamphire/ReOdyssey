// ReOdyssey mouse/keyboard controller emulation.
//
// This is effectively just SDK MnK driver's XInput semantics, but deliberately leaves
// the host cursor visible and uncaptured so Lost Odyssey UI mouse support can actually hover over stuff

#include "reodyssey_mnk_input.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#if REX_PLATFORM_WIN32
#include <rex/input/xinput/xinput_input_driver.h>
#endif

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(int32_t, mnk_user_index);
REXCVAR_DECLARE(double, mnk_sensitivity);

REXCVAR_DECLARE(std::string, keybind_a);
REXCVAR_DECLARE(std::string, keybind_b);
REXCVAR_DECLARE(std::string, keybind_x);
REXCVAR_DECLARE(std::string, keybind_y);
REXCVAR_DECLARE(std::string, keybind_left_trigger);
REXCVAR_DECLARE(std::string, keybind_right_trigger);
REXCVAR_DECLARE(std::string, keybind_left_shoulder);
REXCVAR_DECLARE(std::string, keybind_right_shoulder);
REXCVAR_DECLARE(std::string, keybind_lstick_up);
REXCVAR_DECLARE(std::string, keybind_lstick_down);
REXCVAR_DECLARE(std::string, keybind_lstick_left);
REXCVAR_DECLARE(std::string, keybind_lstick_right);
REXCVAR_DECLARE(std::string, keybind_lstick_press);
REXCVAR_DECLARE(std::string, keybind_rstick_press);
REXCVAR_DECLARE(std::string, keybind_dpad_up);
REXCVAR_DECLARE(std::string, keybind_dpad_down);
REXCVAR_DECLARE(std::string, keybind_dpad_left);
REXCVAR_DECLARE(std::string, keybind_dpad_right);
REXCVAR_DECLARE(std::string, keybind_back);
REXCVAR_DECLARE(std::string, keybind_start);
REXCVAR_DECLARE(std::string, keybind_guide);

namespace reodyssey {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_GAMEPAD_BACK;
using rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
using rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
using rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
using rex::input::X_INPUT_GAMEPAD_DPAD_UP;
using rex::input::X_INPUT_GAMEPAD_GUIDE;
using rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
using rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
using rex::input::X_INPUT_GAMEPAD_START;
using rex::input::X_INPUT_GAMEPAD_X;
using rex::input::X_INPUT_GAMEPAD_Y;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_KEYSTROKE_KEYDOWN;
using rex::input::X_INPUT_KEYSTROKE_KEYUP;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;
using rex::ui::VirtualKey;

bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  VirtualKey vk = rex::ui::ParseVirtualKey(cvar_val);
  if (vk == VirtualKey::kNone) {
    return false;
  }

  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}

int16_t Clamp16(int32_t value) {
  return static_cast<int16_t>(
      std::clamp(value, static_cast<int32_t>(INT16_MIN),
                 static_cast<int32_t>(INT16_MAX)));
}

class ReodysseyMnkInputDriver final : public rex::input::InputDriver,
                                      public rex::ui::WindowInputListener,
                                      public rex::ui::WindowListener {
 public:
  explicit ReodysseyMnkInputDriver(rex::ui::Window* window,
                                   size_t window_z_order)
      : InputDriver(window, window_z_order) {}

  ~ReodysseyMnkInputDriver() override { DetachWindow(); }

  X_STATUS Setup() override {
    REXLOG_INFO("ReOdyssey MnK input driver initialized");
    return X_STATUS_SUCCESS;
  }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (!IsEnabled() || user_index != UserIndex()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }

    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->flags = 0;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
      out_caps->vibration.left_motor_speed = 0xFFFF;
      out_caps->vibration.right_motor_speed = 0xFFFF;
    }

    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override {
    if (!IsEnabled() || user_index != UserIndex()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }

    if (!is_active() || !has_focus_) {
      if (out_state) {
        std::memset(out_state, 0, sizeof(*out_state));
        out_state->packet_number = packet_number_;
      }
      return X_ERROR_SUCCESS;
    }

    std::lock_guard lock(state_mutex_);

    uint16_t buttons = 0;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_a)))
      buttons |= X_INPUT_GAMEPAD_A;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_b)))
      buttons |= X_INPUT_GAMEPAD_B;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_x)))
      buttons |= X_INPUT_GAMEPAD_X;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_y)))
      buttons |= X_INPUT_GAMEPAD_Y;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_left_shoulder)))
      buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_right_shoulder)))
      buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_press)))
      buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_press)))
      buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_back)))
      buttons |= X_INPUT_GAMEPAD_BACK;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_start)))
      buttons |= X_INPUT_GAMEPAD_START;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_guide)))
      buttons |= X_INPUT_GAMEPAD_GUIDE;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_up)))
      buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_down)))
      buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_left)))
      buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_right)))
      buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

    uint8_t left_trigger =
        IsBindPressed(key_down_, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
    uint8_t right_trigger =
        IsBindPressed(key_down_, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

    int32_t left_x = 0;
    int32_t left_y = 0;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_left)))
      left_x -= INT16_MAX;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_right)))
      left_x += INT16_MAX;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_up)))
      left_y += INT16_MAX;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_down)))
      left_y -= INT16_MAX;

    double sensitivity = REXCVAR_GET(mnk_sensitivity);
    constexpr double kBaseScale = 200.0;
    int32_t right_x = static_cast<int32_t>(mouse_dx_ * sensitivity * kBaseScale);
    int32_t right_y = static_cast<int32_t>(-mouse_dy_ * sensitivity * kBaseScale);
    mouse_dx_ = 0;
    mouse_dy_ = 0;

    packet_number_++;

    if (out_state) {
      out_state->packet_number = packet_number_;
      out_state->gamepad.buttons = buttons;
      out_state->gamepad.left_trigger = left_trigger;
      out_state->gamepad.right_trigger = right_trigger;
      out_state->gamepad.thumb_lx = Clamp16(left_x);
      out_state->gamepad.thumb_ly = Clamp16(left_y);
      out_state->gamepad.thumb_rx = Clamp16(right_x);
      out_state->gamepad.thumb_ry = Clamp16(right_y);
    }

    return X_ERROR_SUCCESS;
  }

  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    if (!IsEnabled() || user_index != UserIndex()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (!IsEnabled() || user_index != UserIndex()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }

    std::lock_guard lock(state_mutex_);
    if (keystroke_queue_.empty()) {
      return X_ERROR_EMPTY;
    }

    if (out_keystroke) {
      *out_keystroke = keystroke_queue_.front();
    }
    keystroke_queue_.pop();
    return X_ERROR_SUCCESS;
  }

  void OnWindowAvailable(rex::ui::Window* window) override {
    DetachWindow();
    attached_window_ = window;
    if (!attached_window_) {
      return;
    }

    attached_window_->AddInputListener(this, window_z_order());
    attached_window_->AddListener(this);
    has_focus_ = true;
    have_prev_mouse_ = false;
  }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!IsEnabled() || !has_focus_) {
      return;
    }

    std::lock_guard lock(state_mutex_);
    SetKeyState(static_cast<uint16_t>(e.virtual_key()), true);
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (!IsEnabled()) {
      return;
    }

    std::lock_guard lock(state_mutex_);
    SetKeyState(static_cast<uint16_t>(e.virtual_key()), false);
  }

  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (!IsEnabled() || !has_focus_) {
      return;
    }

    std::lock_guard lock(state_mutex_);
    switch (e.button()) {
      case rex::ui::MouseEvent::Button::kLeft:
        SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
        break;
      case rex::ui::MouseEvent::Button::kRight:
        SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
        break;
      case rex::ui::MouseEvent::Button::kMiddle:
        SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
        break;
      default:
        break;
    }
  }

  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (!IsEnabled()) {
      return;
    }

    std::lock_guard lock(state_mutex_);
    switch (e.button()) {
      case rex::ui::MouseEvent::Button::kLeft:
        SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
        break;
      case rex::ui::MouseEvent::Button::kRight:
        SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
        break;
      case rex::ui::MouseEvent::Button::kMiddle:
        SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
        break;
      default:
        break;
    }
  }

  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!IsEnabled() || !has_focus_) {
      return;
    }

    std::lock_guard lock(state_mutex_);
    int32_t x = e.x();
    int32_t y = e.y();
    if (have_prev_mouse_) {
      mouse_dx_ += x - prev_mouse_x_;
      mouse_dy_ += y - prev_mouse_y_;
    } else {
      have_prev_mouse_ = true;
    }
    prev_mouse_x_ = x;
    prev_mouse_y_ = y;
  }

  void OnClosing(rex::ui::UIEvent& e) override {
    if (e.target() == attached_window_) {
      DetachWindow();
    }
  }

  void OnLostFocus(rex::ui::UISetupEvent& e) override {
    if (e.target() != attached_window_) {
      return;
    }

    std::lock_guard lock(state_mutex_);
    has_focus_ = false;
    have_prev_mouse_ = false;
    std::memset(key_down_, 0, sizeof(key_down_));
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  }

  void OnGotFocus(rex::ui::UISetupEvent& e) override {
    if (e.target() != attached_window_) {
      return;
    }

    std::lock_guard lock(state_mutex_);
    has_focus_ = true;
    have_prev_mouse_ = false;
  }

 private:
  uint32_t UserIndex() const {
    return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
  }

  bool IsEnabled() const { return REXCVAR_GET(mnk_mode); }

  void DetachWindow() {
    if (!attached_window_) {
      return;
    }

    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }

  void SetKeyState(uint16_t vk, bool down) {
    if (vk < 256) {
      key_down_[vk] = down;
    }
  }

  rex::ui::Window* attached_window_ = nullptr;
  std::mutex state_mutex_;
  bool key_down_[256] = {};
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool have_prev_mouse_ = false;
  bool has_focus_ = true;
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;
  uint32_t packet_number_ = 0;
};

template <typename Driver>
void AddDriverIfSetupSucceeds(rex::input::InputSystem* input) {
  auto driver = std::make_unique<Driver>(nullptr, 0);
  if (driver->Setup() == X_STATUS_SUCCESS) {
    input->AddDriver(std::move(driver));
  }
}

}  // namespace

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  auto input = std::make_unique<rex::input::InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      AddDriverIfSetupSucceeds<rex::input::xinput::XinputInputDriver>(
          input.get());
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      AddDriverIfSetupSucceeds<rex::input::sdl::SDLInputDriver>(input.get());
    }

    AddDriverIfSetupSucceeds<ReodysseyMnkInputDriver>(input.get());
  }

  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(
      std::make_unique<rex::input::nop::NopInputDriver>(nullptr, nop_index));
  return input;
}

}  // namespace reodyssey
