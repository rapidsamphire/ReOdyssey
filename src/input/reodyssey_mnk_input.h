#pragma once

#include <memory>

#include <rex/system/interfaces/input.h>

namespace reodyssey {

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

}  // namespace reodyssey
