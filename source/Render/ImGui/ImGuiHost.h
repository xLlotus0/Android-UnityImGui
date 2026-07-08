#pragma once

#include <functional>

#include "imgui/imgui.h"

namespace ImGuiHost
{

struct InitOptions
{
    bool secureCapture = false;
    std::function<void()> render;
};

bool Init(const InitOptions& opt);
void Clean();
void Tick();

bool IsInitialized();
ImVec2 GetDisplaySize();
void SetCaptureHiding(bool enabled);

} // namespace ImGuiHost
