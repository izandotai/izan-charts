#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace izan::charts {

enum class CompositeMomentumRegime : std::uint8_t {
    Unavailable = 0,
    Warming,
    Weak,
    Building,
    Persistent,
    Fading,
    Choppy,
    Transitional,
};

struct SuperMacdMomentumPoint {
    double t = 0;
    double score = 0;
    double velocity_5s_points_per_second = 0;
    double same_direction_residence_30s = 0;
    double same_direction_duration_seconds = 0;
    double drawdown_from_segment_peak_points = 0;
    std::uint32_t effective_zero_crossings_60s = 0;
    int direction = 0;
    CompositeMomentumRegime regime = CompositeMomentumRegime::Unavailable;
    bool available = false;
};

struct SuperMacdLayer {
    bool enabled = false;
    double effective_deadband_points = 5.0;
    // Time-aware EMA constants.  The source is an irregular, high-rate score,
    // so seconds are stable while sample-count periods are not.
    double fast_time_constant_seconds = 2.5;
    double slow_time_constant_seconds = 9.0;
    double signal_time_constant_seconds = 4.0;
    // Raw score remains available for diagnostics, but the default visual is
    // the familiar zero-centred DIF/DEA/HIST MACD vocabulary.
    bool show_raw_diagnostic = false;
    double magnifier_seconds = 120.0;
    std::vector<SuperMacdMomentumPoint> points;
};

struct SuperMacdTrace {
    std::vector<double> dif;
    std::vector<double> signal;
    std::vector<double> histogram;
};

// A time-aware, strictly causal EMA.  The returned point at index i only uses
// observations [0, i], so the visual trace cannot leak a future score.
inline std::vector<double> super_macd_causal_smoothing(
    std::span<const SuperMacdMomentumPoint> points,
    double time_constant_seconds)
{
    std::vector<double> result(points.size(), 0.0);
    if (points.empty())
        return result;
    const double tau = std::isfinite(time_constant_seconds)
        ? std::max(0.05, time_constant_seconds)
        : 3.0;
    result.front() = std::clamp(points.front().score, -100.0, 100.0);
    for (std::size_t index = 1; index < points.size(); ++index) {
        const double score = std::clamp(points[index].score, -100.0, 100.0);
        const double elapsed = points[index].t - points[index - 1].t;
        if (!std::isfinite(elapsed) || elapsed <= 0.0) {
            result[index] = result[index - 1];
            continue;
        }
        const double alpha = 1.0 - std::exp(-elapsed / tau);
        result[index] = result[index - 1]
            + std::clamp(alpha, 0.0, 1.0) * (score - result[index - 1]);
    }
    return result;
}

inline std::vector<double> super_macd_causal_value_smoothing(
    std::span<const SuperMacdMomentumPoint> points,
    std::span<const double> values,
    double time_constant_seconds)
{
    std::vector<double> result(values.size(), 0.0);
    if (points.size() != values.size() || values.empty())
        return result;
    const double tau = std::isfinite(time_constant_seconds)
        ? std::max(0.05, time_constant_seconds)
        : 4.0;
    result.front() = std::isfinite(values.front()) ? values.front() : 0.0;
    for (std::size_t index = 1; index < values.size(); ++index) {
        const double elapsed = points[index].t - points[index - 1].t;
        if (!std::isfinite(elapsed) || elapsed <= 0.0
            || !std::isfinite(values[index])) {
            result[index] = result[index - 1];
            continue;
        }
        const double alpha = 1.0 - std::exp(-elapsed / tau);
        result[index] = result[index - 1]
            + std::clamp(alpha, 0.0, 1.0)
                * (values[index] - result[index - 1]);
    }
    return result;
}

// Convert the real-time composite direction score into actual MACD
// semantics. DIF is fast EMA(score) - slow EMA(score), DEA is a causal EMA
// of DIF, and the histogram is DIF - DEA. All three therefore share the same
// zero-axis vocabulary; the absolute score level never displaces the curves.
// No resampling or future observation is used.
inline SuperMacdTrace super_macd_trace(
    std::span<const SuperMacdMomentumPoint> points,
    double fast_time_constant_seconds = 2.5,
    double slow_time_constant_seconds = 9.0,
    double signal_time_constant_seconds = 4.0)
{
    const double fast_tau = std::max(0.05,
        std::isfinite(fast_time_constant_seconds)
            ? fast_time_constant_seconds
            : 2.5);
    const double slow_tau = std::max(fast_tau + 0.05,
        std::isfinite(slow_time_constant_seconds)
            ? slow_time_constant_seconds
            : 9.0);
    const double signal_tau = std::max(0.05,
        std::isfinite(signal_time_constant_seconds)
            ? signal_time_constant_seconds
            : 4.0);
    SuperMacdTrace result;
    const auto fast = super_macd_causal_smoothing(points, fast_tau);
    const auto slow = super_macd_causal_smoothing(points, slow_tau);
    result.dif.resize(points.size(), 0.0);
    for (std::size_t index = 0; index < points.size(); ++index)
        result.dif[index] = fast[index] - slow[index];
    result.signal = super_macd_causal_value_smoothing(
        points, result.dif, signal_tau);
    result.histogram.resize(points.size(), 0.0);
    for (std::size_t index = 0; index < points.size(); ++index)
        result.histogram[index] = result.dif[index] - result.signal[index];
    return result;
}

inline bool super_macd_needs_magnifier(
    std::span<const SuperMacdMomentumPoint> points,
    double visible_min_t,
    double visible_max_t,
    double minimum_coverage = 0.35)
{
    if (points.size() < 2 || !std::isfinite(visible_min_t)
        || !std::isfinite(visible_max_t) || visible_max_t <= visible_min_t)
        return false;
    const double first = std::max(points.front().t, visible_min_t);
    const double last = std::min(points.back().t, visible_max_t);
    if (last <= first)
        return false;
    const double coverage = (last - first) / (visible_max_t - visible_min_t);
    return coverage < std::clamp(minimum_coverage, 0.05, 0.95);
}

// Choose a readable hidden-axis extent without changing the score itself.
// The axis remains symmetric around zero so UP/DOWN semantics stay stable,
// while ordinary +/-10..30 point paths are not crushed into a few pixels.
inline double super_macd_visible_extent(
    std::span<const SuperMacdMomentumPoint> points,
    double visible_min_t,
    double visible_max_t,
    double fast_time_constant_seconds = 2.5,
    double slow_time_constant_seconds = 9.0,
    double signal_time_constant_seconds = 4.0,
    double minimum_extent_points = 2.0)
{
    const auto trace = super_macd_trace(points, fast_time_constant_seconds,
        slow_time_constant_seconds, signal_time_constant_seconds);
    double maximum_strength = 0.0;
    bool has_visible_point = false;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        if (!std::isfinite(point.t) || point.t < visible_min_t
            || point.t > visible_max_t)
            continue;
        maximum_strength = std::max(maximum_strength,
            std::abs(trace.dif[index]));
        maximum_strength = std::max(maximum_strength,
            std::abs(trace.signal[index]));
        maximum_strength = std::max(maximum_strength,
            std::abs(trace.histogram[index]) * 1.5);
        has_visible_point = true;
    }
    if (!has_visible_point) {
        for (std::size_t index = 0; index < points.size(); ++index) {
            maximum_strength = std::max(maximum_strength,
                std::abs(trace.dif[index]));
            maximum_strength = std::max(maximum_strength,
                std::abs(trace.signal[index]));
            maximum_strength = std::max(maximum_strength,
                std::abs(trace.histogram[index]) * 1.5);
        }
    }
    const double minimum = std::clamp(
        std::isfinite(minimum_extent_points) ? minimum_extent_points : 2.0,
        0.5, 105.0);
    return std::clamp(maximum_strength * 1.25, minimum, 105.0);
}

} // namespace izan::charts
