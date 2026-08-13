#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace izan::charts {

enum class MacdEnergyState {
    Unavailable,
    BullExpanding,
    BullContracting,
    BearExpanding,
    BearContracting,
    CrossUp,
    CrossDown,
    FlatBull,
    FlatBear,
};

struct MacdEnergyTransition {
    MacdEnergyState state = MacdEnergyState::Unavailable;
    double histogram_delta = std::numeric_limits<double>::quiet_NaN();
    double magnitude_delta = std::numeric_limits<double>::quiet_NaN();
    double magnitude_change = std::numeric_limits<double>::quiet_NaN();

    [[nodiscard]] bool available() const
    {
        return state != MacdEnergyState::Unavailable;
    }
};

// Classify one one-minute MACD histogram bar against its immediate predecessor.
// A small relative dead-band keeps floating-point noise from alternating the
// expansion/contraction glyph every frame while an open candle is almost flat.
[[nodiscard]] inline MacdEnergyTransition classify_macd_energy(
    double previous, double current, double flat_relative_tolerance = 0.015)
{
    MacdEnergyTransition result;
    if (!std::isfinite(previous) || !std::isfinite(current))
        return result;

    const double previous_magnitude = std::abs(previous);
    const double current_magnitude = std::abs(current);
    result.histogram_delta = current - previous;
    result.magnitude_delta = current_magnitude - previous_magnitude;
    result.magnitude_change = previous_magnitude > 1e-12
        ? result.magnitude_delta / previous_magnitude
        : (current_magnitude > 1e-12 ? 1.0 : 0.0);

    if (previous <= 0.0 && current > 0.0) {
        result.state = MacdEnergyState::CrossUp;
        return result;
    }
    if (previous >= 0.0 && current < 0.0) {
        result.state = MacdEnergyState::CrossDown;
        return result;
    }

    const double scale
        = std::max({ previous_magnitude, current_magnitude, 1e-12 });
    if (std::abs(result.magnitude_delta)
        <= scale * std::max(0.0, flat_relative_tolerance)) {
        result.state = current >= 0.0 ? MacdEnergyState::FlatBull
                                      : MacdEnergyState::FlatBear;
    } else if (current >= 0.0) {
        result.state = result.magnitude_delta > 0.0
            ? MacdEnergyState::BullExpanding
            : MacdEnergyState::BullContracting;
    } else {
        result.state = result.magnitude_delta > 0.0
            ? MacdEnergyState::BearExpanding
            : MacdEnergyState::BearContracting;
    }
    return result;
}

[[nodiscard]] inline const char* macd_energy_state_label(MacdEnergyState state)
{
    switch (state) {
    case MacdEnergyState::BullExpanding:
        return "BULL EXP";
    case MacdEnergyState::BullContracting:
        return "BULL FADE";
    case MacdEnergyState::BearExpanding:
        return "BEAR EXP";
    case MacdEnergyState::BearContracting:
        return "BEAR FADE";
    case MacdEnergyState::CrossUp:
        return "CROSS UP";
    case MacdEnergyState::CrossDown:
        return "CROSS DN";
    case MacdEnergyState::FlatBull:
        return "BULL FLAT";
    case MacdEnergyState::FlatBear:
        return "BEAR FLAT";
    case MacdEnergyState::Unavailable:
        break;
    }
    return "WAIT";
}

struct MacdBarCountdown {
    double remaining_seconds = 0.0;
    double elapsed_fraction = 0.0;
    int display_seconds = 0;

    [[nodiscard]] bool available() const noexcept
    {
        return display_seconds > 0;
    }
};

// One-minute MACD bars are aligned to Unix/UTC minute boundaries. The ring
// reports elapsed progress toward the next completed energy bar while the
// number in its center reports whole seconds remaining.
[[nodiscard]] inline MacdBarCountdown macd_bar_countdown(
    double current_time, double bar_seconds = 60.0) noexcept
{
    if (!std::isfinite(current_time) || !std::isfinite(bar_seconds)
        || current_time < 0.0 || bar_seconds <= 0.0)
        return {};
    const double elapsed = current_time
        - std::floor(current_time / bar_seconds) * bar_seconds;
    const double remaining = std::clamp(bar_seconds - elapsed, 0.0, bar_seconds);
    return {
        .remaining_seconds = remaining,
        .elapsed_fraction = std::clamp(elapsed / bar_seconds, 0.0, 1.0),
        .display_seconds = std::clamp(
            static_cast<int>(std::ceil(remaining - 1e-9)), 1,
            static_cast<int>(std::ceil(bar_seconds))),
    };
}

}
