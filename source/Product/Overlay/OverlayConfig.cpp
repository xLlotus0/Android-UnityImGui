#include "Product/Overlay/OverlayConfig.h"

namespace OverlayUi
{

RuntimeConfig& GetConfig()
{
    static RuntimeConfig config;
    return config;
}

} // namespace OverlayUi
