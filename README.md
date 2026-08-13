# izan-charts

A financial charting library for native C++ apps, built on
[implot](https://github.com/epezent/implot). implot ships the generic
floor — lines, bars, axes; this library adds the financial storey:

- **Candlesticks** with TV-style bodies and wicks, doji-safe
- **Indicator engine** — EMA, SMA, Bollinger bands, MACD — recomputed
  in full each frame (a thousand bars cost well under a millisecond)
- **Linked panes** — main / volume / MACD sharing one X axis, each Y
  fitting its own visible slice, price axis on the right
- **The TradingView interaction contract** — wheel zooms X/Y together,
  Shift+wheel zooms time only, Alt+wheel zooms value only, Ctrl+double-click
  focuses/restores a pane without losing its viewport, switching indicators
  replaces the focused pane in place, and one click atomically follows the
  live edge again without exposing an invalid intermediate range
- **Fast antialiased curves** — integer-width, atlas-textured line rendering
  keeps EMA, Bollinger, MACD and auxiliary plots crisp without MSAA overhead
- **Optional MACD energy layer** — compares each one-minute histogram bar with
  its predecessor inside a host-supplied market window, marking bullish/bearish
  expansion, contraction and zero crosses without changing indicator values.
- **Optional Super MACD context layer** - overlays an application-fed bounded
  score, causal time-aware smoothing, momentum ticks, regimes, drawdown and
  effective zero-cross markers on an independent hidden Y axis. It cannot
  alter MACD values, fitting, zoom, pan, follow or reset state.
- **Event markers** and a reference-price line for the application to
  pin its own story onto the tape

No window framework, no network, no opinion about where data comes
from: feed `Series::push_tick` (or backfill with `push_bar`) and call
`Chart::draw` inside any ImGui window.

## Build

```
cmake -S . -B build -G Ninja
cmake --build build
build/izan_charts_smoke.exe   # a random walk through the full stack
```

Dependencies (imgui/glfw/freetype via
[izan-ui](https://github.com/izandotai/izan-ui)'s `izan_imgui`, and
implot, all pinned) are fetched and built from source; executables
link fully static.

## License

GPL-3.0-only.
