#include "ImGuiSetup.h"

#include <android/native_window.h>

#include "Font.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_android.h"

namespace ImGuiSetup
{

static bool g_ContextReady = false;

void EnsureContext(ANativeWindow *platformWindow)
{
    if (g_ContextReady)
        return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTrickleEventQueue = false;
    io.Fonts->Clear();
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 1;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;
    fontConfig.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned int*>(Window_data),
        static_cast<int>(Window_size),
        45.0f,
        &fontConfig,
        io.Fonts->GetGlyphRangesChineseFull());

    ImGui::StyleColorsLight();
    ImGuiStyle &style = ImGui::GetStyle();
    style.AntiAliasedLines  = true;
    style.AntiAliasedFill   = true;
    style.FontScaleMain = 1.0f;
    style.ScaleAllSizes(1.25f);
    style.WindowRounding = 4.0f;
    ImGui_ImplAndroid_Init(platformWindow);

    g_ContextReady = true;
}

void TeardownContext()
{
    if (!g_ContextReady)
        return;

    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();

    g_ContextReady = false;
}

} // namespace ImGuiSetup
