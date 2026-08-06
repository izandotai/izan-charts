#pragma once

namespace izan::charts::detail {

constexpr bool wheel_zoom_requested(
    bool frame_hovered, float mouse_wheel, bool ctrl) noexcept
{
    return frame_hovered && mouse_wheel != 0.0f && !ctrl;
}

constexpr bool viewport_takeover_requested(
    bool plot_dragged, bool wheel_zoom_active, bool ctrl) noexcept
{
    return !ctrl && (plot_dragged || wheel_zoom_active);
}

}
