#pragma once

namespace OverlayUi
{

struct RuntimeConfig
{
    bool secureCapture = true;
    char imeText[256] = "";
};

RuntimeConfig& GetConfig();

} // namespace OverlayUi
