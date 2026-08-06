#include "charts/interaction.hpp"

#include <cstdlib>

namespace {

void require(bool condition)
{
    if (!condition)
        std::abort();
}

}

int main()
{
    using izan::charts::detail::viewport_takeover_requested;
    using izan::charts::detail::wheel_zoom_requested;

    require(wheel_zoom_requested(true, 1.0f, false));
    require(!wheel_zoom_requested(false, 1.0f, false));
    require(!wheel_zoom_requested(true, 1.0f, true));

    // Regression: immediately after reset/follow, the pointer can be over an
    // axis (inside FrameRect but outside PlotRect). Its wheel event must take
    // ownership of the viewport so the next follow frame cannot overwrite it.
    require(viewport_takeover_requested(false, true, false));
    require(viewport_takeover_requested(true, false, false));
    require(!viewport_takeover_requested(false, false, false));
    require(!viewport_takeover_requested(false, true, true));
}
