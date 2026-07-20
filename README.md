# izan-charts

A financial charting library for native C++ apps, built on
[implot](https://github.com/epezent/implot). implot ships the generic
floor — lines, bars, axes; this library adds the financial storey:

- **Candlesticks** with TV-style bodies and wicks, doji-safe
- **Indicator engine** — EMA, SMA, Bollinger bands, MACD — recomputed
  in full each frame (a thousand bars cost well under a millisecond)
- **Linked panes** — main / volume / MACD sharing one X axis, each Y
  fitting its own visible slice, price axis on the right
- **The TradingView interaction contract** — drag or scroll to take
  the view over, one click to follow the live edge again, eased
  viewport, seamless source switching without stretch animation
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
