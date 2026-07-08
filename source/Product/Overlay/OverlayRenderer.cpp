#include "Product/Overlay/OverlayRenderer.h"

#include "Product/Overlay/OverlayDraw.h"
#include "Product/Overlay/OverlayMenu.h"

namespace OverlayUi
{

void Render()
{
    RenderDrawLayer();
    RenderMenu();
}

} // namespace OverlayUi
