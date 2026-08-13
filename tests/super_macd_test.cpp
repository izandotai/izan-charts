#include "charts/super_macd.hpp"

#include <cassert>
#include <cmath>
#include <vector>

int main()
{
    using izan::charts::SuperMacdMomentumPoint;
    std::vector<SuperMacdMomentumPoint> points{
        { .t = 1.0, .score = 0.0 },
        { .t = 2.0, .score = 100.0 },
        { .t = 3.0, .score = 100.0 },
    };
    const auto trace = izan::charts::super_macd_causal_smoothing(points, 1.0);
    assert(trace.size() == points.size());
    assert(trace[0] == 0.0);
    assert(trace[1] > 60.0 && trace[1] < 65.0);
    assert(trace[2] > trace[1] && trace[2] < 100.0);

    // Appending a future observation cannot rewrite earlier displayed values.
    const double second_before = trace[1];
    points.push_back({ .t = 4.0, .score = -100.0 });
    const auto extended =
        izan::charts::super_macd_causal_smoothing(points, 1.0);
    assert(std::abs(extended[1] - second_before) < 1e-12);

    // Duplicate/out-of-order event time carries the last causal state.
    points.push_back({ .t = 4.0, .score = 100.0 });
    const auto duplicate =
        izan::charts::super_macd_causal_smoothing(points, 1.0);
    assert(duplicate.back() == duplicate[duplicate.size() - 2]);

    const auto macd = izan::charts::super_macd_trace(points, 0.5, 2.0);
    assert(macd.fast.size() == points.size());
    assert(macd.slow.size() == points.size());
    assert(macd.histogram.size() == points.size());
    assert(macd.fast[1] > macd.slow[1]);
    assert(macd.histogram[1] > 0.0);
    assert(std::abs(macd.histogram[1]
                    - (macd.fast[1] - macd.slow[1])) < 1e-12);

    // A short freshly-started history is magnified inside a much wider
    // shared MACD time axis; once it covers the axis, no inset is needed.
    std::vector<SuperMacdMomentumPoint> short_history{
        { .t = 90.0, .score = 10.0 },
        { .t = 100.0, .score = 20.0 },
    };
    assert(izan::charts::super_macd_needs_magnifier(
        short_history, 0.0, 300.0));
    assert(!izan::charts::super_macd_needs_magnifier(
        short_history, 90.0, 100.0));

    // Visible-axis scaling expands ordinary scores without changing them and
    // remains bounded for extreme inputs.
    const double normal_extent = izan::charts::super_macd_visible_extent(
        points, 1.0, 3.0);
    assert(normal_extent == 105.0); // the visible sample includes +100
    std::vector<SuperMacdMomentumPoint> ordinary{
        { .t = 1.0, .score = -18.0 },
        { .t = 2.0, .score = -24.0 },
    };
    const double ordinary_extent = izan::charts::super_macd_visible_extent(
        ordinary, 1.0, 2.0);
    assert(ordinary_extent > 29.0 && ordinary_extent < 30.0);
}
