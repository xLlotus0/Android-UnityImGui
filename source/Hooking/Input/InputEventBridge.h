#pragma once

#include <android/input.h>
#include <functional>

namespace Hooking::InputEventBridge
{

using Callback = std::function<void(void* consumer, AInputEvent* event)>;

void Install(Callback callback);
void SetCallback(Callback callback);

} // namespace Hooking::InputEventBridge
