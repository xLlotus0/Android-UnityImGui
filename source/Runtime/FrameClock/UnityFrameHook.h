#pragma once

#include <functional>

namespace Runtime::UnityFrameHook
{

using TickCallback = std::function<void(float deltaTime)>;

bool Install(TickCallback callback);

} // namespace Runtime::UnityFrameHook
