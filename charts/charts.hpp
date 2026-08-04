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
//   Interaction: dragging or scrolling takes the view over; wheel zooms
//   both axes, Shift+wheel only X, and Alt+wheel only Y; "follow latest"
//   returns to the live edge; the following viewport eases exponentially;
//   the panes share one X axis (LinkAllX) while every Y fits its own visible
//   slice; the price axis sits on the right.

#include <imgui.h>
#include <implot.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
    bool vwap = false;    // session/backfill anchored VWAP
    bool volume = true;   // volume pane
    bool macd = true;     // MACD(12,26,9) pane
    bool rsi = false;     // RSI(14) pane
    bool atr = false;     // ATR(14) pane

    // Parameters.
    int ema_fast_n = 12, ema_slow_n = 26, sma_n = 20;
    int boll_n = 20;
    double boll_k = 2.0;
    int macd_fast = 12, macd_slow = 26, macd_signal = 9;
    int rsi_n = 14, atr_n = 14;

    // Results, index-aligned with the bars; NAN marks the not-enough-
    // data prefix and implot breaks the line there by itself.
    std::vector<double> x;
    std::vector<double> ema_fast_v, ema_slow_v, sma_v;
    std::vector<double> boll_mid, boll_up, boll_dn;
    std::vector<double> vwap_v;
    std::vector<double> macd_dif, macd_dea, macd_hist;
    std::vector<double> rsi_v, atr_v;

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

struct AuxiliaryLine {
    std::string label;
    std::vector<double> x;
    std::vector<double> y;
    ImVec4 color { 1, 1, 1, 1 };
};

// One application-defined, time-linked indicator pane. The chart owns no
// market-data semantics; callers can feed CVD, leverage, volatility, research,
// or another bounded history.
struct AuxiliaryPane {
    std::string title;
    std::vector<AuxiliaryLine> lines;
    std::vector<double> guides;
    double y_min = 0;
    double y_max = 0;
};

struct ResearchOverlay {
    bool available = false;
    std::string horizon;
    double window_start_t = 0;
    double window_end_t = 0;
    double anchor_price = 0;
    double expected_move_bps = 0;
    double fair_probability_up = 0;
    double market_probability_up = 0;
    double edge_up = 0;
    double confidence = 0;
    bool dynamic_cone_enabled = false;
    double cone_start_t = 0;
    double cone_end_t = 0;
    double cone_center_price = 0;
    double cone_expected_move_bps = 0;
};

// Independent application-fed layers. Each layer is optional and has no
// ownership of the market data that produced it, so one unavailable layer
// cannot disturb candles, indicators, or another overlay.
struct ExpiryRiskLayer {
    bool available = false;
    bool minimized = true;
    std::string title = "ANCHOR RISK";
    std::string help;
    double start_t = 0;
    double end_t = 0;
    double anchor_price = 0;
    double current_price = 0;
    double expected_move_bps = 0;
    double safety_distance = 0;
    double remaining_seconds = 0;
    bool direction_up = false;
    bool motion_available = false;
    double velocity_5s_bps_per_second = 0;
    double velocity_15s_bps_per_second = 0;
    double acceleration_bps_per_second = 0;
    bool giveback_available = false;
    double giveback = 0;
};

struct VenueRaceEntry {
    std::string label;
    double deviation_bps = 0;
    double lead_lag_1s = 0;
    double lead_lag_5s = 0;
    double lead_lag_15s = 0;
    bool lead_lag_1s_available = false;
    bool lead_lag_5s_available = false;
    bool lead_lag_15s_available = false;
};

struct VenueRaceLayer {
    bool available = false;
    bool minimized = true;
    std::string title = "VENUE RACE";
    std::string help;
    double consensus_mad_bps = 0;
    double agreement = 0;
    std::vector<VenueRaceEntry> entries;
};

struct RtdsPriceLayer {
    bool available = false;
    std::string title = "RTDS";
    std::string help;
    std::string source_key;
    std::int64_t window_start_ns = 0;
    double current_price = 0;
    double beat_price = 0;
    double pmt_reference_price = 0;
    double official_gap_bps = 0;
    double proxy_gap_bps = 0;
    double current_age_ms = 0;
    double reference_offset_ms = 0;
    bool fresh = false;
    bool proxy_available = false;
    bool direction_agrees = false;
    bool reference_exact = false;
};

enum class LeverageRegime : std::uint8_t {
    Neutral,
    LongBuild,
    ShortBuild,
    ShortCover,
    LongUnwind,
};

struct LeverageRegimePoint {
    double t = 0;
    LeverageRegime regime = LeverageRegime::Neutral;
    double intensity = 0;
    double price_impulse_bps = 0;
    double open_interest_delta_pct = 0;
};

struct LeverageRegimeLayer {
    bool available = false;
    bool minimized = true;
    std::string title = "PRICE x OI";
    std::string help;
    std::string source_key;
    std::uint64_t source_revision = 0;
    double through_t = 0;
    std::vector<LeverageRegimePoint> points;
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

    void zoom(double factor);
    void pan_bars(double bars);

    Theme theme;
    std::vector<Marker> markers; // refilled by the caller every frame
    std::optional<AuxiliaryPane> auxiliary;
    ResearchOverlay research;
    ExpiryRiskLayer expiry_risk;
    VenueRaceLayer venue_race;
    RtdsPriceLayer rtds_price;
    LeverageRegimeLayer leverage_regime;
    // A reference price line across the main pane (a strike, a mark —
    // 0 draws nothing). Set by the caller every frame.
    double ref_price = 0;
    ImVec4 ref_color = ImVec4(0.95f, 0.75f, 0.20f, 0.85f);
    // The follow banner's words. The library ships English; an app
    // with a catalog pours its own language in.
    const char* follow_label = "Follow latest";
    const char* paused_note = "(paused: dragging / scrolling)";

private:
    enum class FocusedPane : std::uint8_t {
        All,
        Main,
        Volume,
        Macd,
        Rsi,
        Atr,
        Auxiliary,
    };

    void draw_main_pane(const Series& s, const IndicatorSet& ind, bool bottom);
    void draw_volume_pane(
        const Series& s, const IndicatorSet& ind, bool bottom, bool switched);
    void draw_macd_pane(const Series& s, const IndicatorSet& ind);
    void draw_rsi_pane(const Series& s, const IndicatorSet& ind);
    void draw_atr_pane(const Series& s, const IndicatorSet& ind);
    void draw_auxiliary_pane(const Series& s, const AuxiliaryPane& pane);
    void draw_expiry_risk_fan(const ImPlotRect& limits);
    void draw_expiry_risk_hud();
    void draw_venue_race_layer();
    void draw_rtds_price_layer(const ImPlotRect& limits);
    void draw_leverage_regime_layer(const ImPlotRect& limits);
    void draw_last_price_tag();
    void update_view(const Series& s); // eased advance while following
    void takeover_check();             // drag/scroll → the user takes over

    bool follow_ = true;
    int snap_frames_ = 0; // >0: the seamless-switch window, all teleports
    int manual_view_frames_ = 0;
    bool research_minimized_ = true;
    FocusedPane focused_pane_ = FocusedPane::All;
    FocusedPane last_indicator_pane_ = FocusedPane::All;
    const Series* last_series_ = nullptr; // source-switch detection
    double span_ = 0;          // viewport width in seconds; 0 = unset
    double bar_seconds_ = 60;
    double data_span_ = 60;
    double vx0_ = 0, vx1_ = 0; // eased viewport, X shared by all panes
    double vy0_ = 0, vy1_ = 0; // main-pane Y easing state
    bool last_price_tag_visible_ = false;
    float last_price_tag_right_ = 0;
    float last_price_tag_y_ = 0;
    ImVec2 last_price_tag_clip_min_ {};
    ImVec2 last_price_tag_clip_max_ {};
    ImU32 last_price_tag_color_ = 0;
    char last_price_tag_text_[32] {};
};

}
