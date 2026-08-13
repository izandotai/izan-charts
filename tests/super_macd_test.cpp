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
}
