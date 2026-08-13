#include "charts/macd_energy.hpp"

#include <cmath>
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
    using izan::charts::classify_macd_energy;
    using izan::charts::MacdEnergyState;

    require(
        classify_macd_energy(1.0, 1.5).state == MacdEnergyState::BullExpanding);
    require(classify_macd_energy(1.5, 1.0).state
        == MacdEnergyState::BullContracting);
    require(classify_macd_energy(-1.0, -1.5).state
        == MacdEnergyState::BearExpanding);
    require(classify_macd_energy(-1.5, -1.0).state
        == MacdEnergyState::BearContracting);
    require(classify_macd_energy(-0.1, 0.1).state == MacdEnergyState::CrossUp);
    require(
        classify_macd_energy(0.1, -0.1).state == MacdEnergyState::CrossDown);
    require(
        classify_macd_energy(1.0, 1.005).state == MacdEnergyState::FlatBull);
    require(
        classify_macd_energy(-1.0, -1.005).state == MacdEnergyState::FlatBear);
    require(!classify_macd_energy(std::nan(""), 1.0).available());

    const auto transition = classify_macd_energy(2.0, 3.0);
    require(std::abs(transition.histogram_delta - 1.0) < 1e-12);
    require(std::abs(transition.magnitude_change - 0.5) < 1e-12);

    const auto fresh_bar = izan::charts::macd_bar_countdown(120.0);
    require(fresh_bar.available() && fresh_bar.display_seconds == 60);
    require(std::abs(fresh_bar.elapsed_fraction) < 1e-12);
    const auto half_bar = izan::charts::macd_bar_countdown(150.0);
    require(half_bar.display_seconds == 30);
    require(std::abs(half_bar.elapsed_fraction - 0.5) < 1e-12);
    const auto last_tick = izan::charts::macd_bar_countdown(179.9);
    require(last_tick.display_seconds == 1);
    require(last_tick.elapsed_fraction > 0.99);
    require(!izan::charts::macd_bar_countdown(std::nan("")).available());
}
