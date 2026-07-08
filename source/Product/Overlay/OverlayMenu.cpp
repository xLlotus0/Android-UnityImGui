#include "Product/Overlay/OverlayMenu.h"

#include <algorithm>

#include "Integration/AndroidIme/ImGuiAndroidIme.h"
#include "Product/Overlay/OverlayConfig.h"
#include "Render/ImGui/ImGuiHost.h"
#include "imgui/imgui.h"

namespace OverlayUi
{

namespace
{

ImVec2 CalculateMenuSize()
{
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float menuWidth = 680.0f;
    float menuHeight = 280.0f;

    if (display.x > 0.0f)
    {
        float maxWidth = std::max(320.0f, display.x - 32.0f);
        menuWidth = std::min(760.0f, maxWidth);
    }

    if (display.y > 0.0f)
    {
        float maxHeight = std::max(220.0f, display.y - 32.0f);
        menuHeight = std::min(std::max(280.0f, display.y * 0.32f), maxHeight);
    }

    return ImVec2(menuWidth, menuHeight);
}

void RenderImeInput(RuntimeConfig& config)
{
    float clearButtonWidth = 120.0f;
    ImGui::SetNextItemWidth(-(clearButtonWidth + ImGui::GetStyle().ItemSpacing.x));
    ImGuiAndroidIme::InputText("##ImeText", config.imeText, sizeof(config.imeText));
    if (ImGui::Button("Clear", ImVec2(clearButtonWidth, 0.0f)))
    {
        config.imeText[0] = '\0';
        ImGuiAndroidIme::SetText(config.imeText);
    }
}

} // namespace

void RenderMenu()
{
    RuntimeConfig& config = GetConfig();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 maxSize(
        display.x > 0.0f ? std::max(320.0f, display.x - 32.0f) : 760.0f,
        display.y > 0.0f ? std::max(220.0f, display.y - 32.0f) : 480.0f);

    ImGui::SetNextWindowPos(ImVec2(32, 32), ImGuiCond_Once);
    ImGui::SetNextWindowSize(CalculateMenuSize(), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 220.0f), maxSize);
    if (ImGui::Begin("Menu", nullptr))
    {
        if (ImGui::Checkbox("防录屏", &config.secureCapture))
            ImGuiHost::SetCaptureHiding(config.secureCapture);

        ImGui::Separator();
        RenderImeInput(config);
        ImGui::Text("耗时 %.3fms/ (%.1fFPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
    }
    ImGui::End();
}

} // namespace OverlayUi
