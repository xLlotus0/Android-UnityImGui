#pragma once

#include <cstddef>

#include "imgui/imgui.h"

namespace ImGuiAndroidIme
{

bool InputText(const char* label, char* buffer, size_t bufferSize, ImGuiInputTextFlags flags = 0);
void SetText(const char* text);
void DeleteBackward();
void HideKeyboard();

} // namespace ImGuiAndroidIme
