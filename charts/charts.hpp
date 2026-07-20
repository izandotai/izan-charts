#pragma once
// izan-charts — a financial charting library on top of implot.
// implot ships the generic floor (lines, bars, axes); the financial
// storey is empty. This library adds candlesticks, an indicator
// engine and TradingView-style linked panes. Independent of any
// trading stack and of any window framework — implot is the only
// dependency.
//
// The design contract, TV 1:1:
//   Main pane: candles + EMA/SMA overlays + a shaded Bollinger band
//   + OHLC/indicator readouts.
//   Sub panes: volume (bull green, bear red) and MACD (DIF/DEA lines
//   plus histogram).
//   Interaction: dragging or scrolling takes the view over; "follow
//   latest" returns to the live edge; the following viewport eases
//   exponentially; the panes share one X axis (LinkAllX) while every
//   Y fits its own visible slice; the price axis sits on the right.

#include <imgui.h>
#include <implot.h>

#include <cstddef>
#include <vector>

namespace izan::charts {

// ---------- data ----------

struct Bar {
    double t = 0; // bar start, seconds
    double o = 0, h = 0, l = 0, c = 0;
    double v = 0; // volume
};

// tick → OHLCV aggregation with a ring capacity.
class Series {
public:
    explicit Series(double bar_seconds = 60.0, std::size_t cap = 1000)
        : bar_s_(bar_seconds)
        , cap_(cap)
    {
    }

    void push_tick(double t, double px, double qty = 0.0);
    void push_bar(const Bar& bar); // pre-aggregated history backfill

    const std::vector<Bar>& bars() const
    {
        return bars_;
    }

    double bar_seconds() const
    {
        return bar_s_;
    }

    void clear()
    {
        bars_.clear();
    }

private:
    double bar_s_;
    std::size_t cap_;
    std::vector<Bar> bars_;
};

// ---------- indicator engine ----------
// Recomputed in full every frame: correctness first, and a thousand
// bars cost well under a millisecond. Incremental updates can wait.

struct IndicatorSet {
    // Switches — the minimal set of a TV indicator manager.
    bool ema_fast = true; // EMA12
    bool ema_slow = true; // EMA26
    bool sma = false;     // SMA20
    bool boll = true;     // BOLL(20, 2σ)
    bool volume = true;   // volume pane
    bool macd = true;     // MACD(12,26,9) pane

    // Parameters.
    int ema_fast_n = 12, ema_slow_n = 26, sma_n = 20;
    int boll_n = 20;
    double boll_k = 2.0;
    int macd_fast = 12, macd_slow = 26, macd_signal = 9;

    // Results, index-aligned with the bars; NAN marks the not-enough-
    // data prefix and implot breaks the line there by itself.
    std::vector<double> x;
    std::vector<double> ema_fast_v, ema_slow_v, sma_v;
    std::vector<double> boll_mid, boll_up, boll_dn;
    std::vector<double> macd_dif, macd_dea, macd_hist;

    void compute(const std::vector<Bar>& bars);
};

// ---------- the chart ----------

// The classic TV palette.
struct Theme {
    ImVec4 bull { 0.149f, 0.651f, 0.604f, 1.0f };     // #26a69a
    ImVec4 bear { 0.937f, 0.325f, 0.314f, 1.0f };     // #ef5350
    ImVec4 ema_fast { 0.976f, 0.702f, 0.075f, 1.0f }; // orange EMA12
    ImVec4 ema_slow { 0.259f, 0.647f, 0.961f, 1.0f }; // blue EMA26
    ImVec4 sma { 0.729f, 0.408f, 0.784f, 1.0f };      // purple SMA20
    ImVec4 boll { 0.475f, 0.525f, 0.796f, 1.0f };     // slate BOLL
    float boll_fill_alpha = 0.10f;
    ImVec4 macd_dif { 0.259f, 0.647f, 0.961f, 1.0f };
    ImVec4 macd_dea { 0.976f, 0.702f, 0.075f, 1.0f };
};

// Event markers on the main pane (fills, signals, outcomes — whatever
// the application wants to pin to a moment and a price).
struct Marker {
    double t = 0;
    double y = 0;
    ImPlotMarker shape = ImPlotMarker_Circle;
    ImVec4 color { 1, 1, 1, 1 };
    float size = 7.0f;
    const char* legend = ""; // markers sharing a legend form a group
};

class Chart {
public:
    // Draw the full multi-pane chart inside the current ImGui window
    // (size -1 stretches to fill).
    void draw(const char* id, const Series& series, IndicatorSet& ind,
        const ImVec2& size = ImVec2(-1, -1));

    bool following() const
    {
        return follow_;
    }

    void set_follow(bool on)
    {
        follow_ = on;
        if (on) { // invalidate the viewport: next frame snaps into place
            vx1_ = vx0_;
            vy1_ = vy0_;
        }
    }

    // Seamless switching: for a window of frames every easing becomes
    // a teleport, so data that trickles in after a source switch
    // (history backfill, merges) cannot trigger stretch animation.
    // Default ~2s at 60fps.
    void snap_view(int frames = 120)
    {
        snap_frames_ = frames;
        set_follow(true);
    }

    Theme theme;
    std::vector<Marker> markers; // refilled by the caller every frame
    // A reference price line across the main pane (a strike, a mark —
    // 0 draws nothing). Set by the caller every frame.
    double ref_price = 0;
    ImVec4 ref_color = ImVec4(0.95f, 0.75f, 0.20f, 0.85f);
    // The follow banner's words. The library ships English; an app
    // with a catalog pours its own language in.
    const char* follow_label = "Follow latest";
    const char* paused_note = "(paused: dragging / scrolling)";

private:
    void draw_main_pane(const Series& s, const IndicatorSet& ind, bool bottom);
    void draw_volume_pane(
        const Series& s, const IndicatorSet& ind, bool bottom, bool switched);
    void draw_macd_pane(const Series& s, const IndicatorSet& ind);
    void update_view(const Series& s); // eased advance while following
    void takeover_check();             // drag/scroll → the user takes over

    bool follow_ = true;
    int snap_frames_ = 0; // >0: the seamless-switch window, all teleports
    const Series* last_series_ = nullptr; // source-switch detection
    double span_ = 0;          // viewport width in seconds; 0 = unset
    double vx0_ = 0, vx1_ = 0; // eased viewport, X shared by all panes
    double vy0_ = 0, vy1_ = 0; // main-pane Y easing state
};

}
