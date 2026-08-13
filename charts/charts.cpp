#include "charts/charts.hpp"
#include "charts/interaction.hpp"

#include <implot_internal.h> // BeginItem/EndItem/FitPoint for custom glyphs

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace izan::charts {

// ---------- Series ----------

void Series::push_tick(double t, double px, double qty)
{
    if (bars_.empty() || t - bars_.back().t >= bar_s_) {
        Bar b;
        // Snap to the bar grid. Data gaps do NOT mint fake timestamps
        // — a gap stays a gap on the time axis, the TV semantic.
        b.t = std::floor(t / bar_s_) * bar_s_;
        b.o = b.h = b.l = b.c = px;
        b.v = qty;
        bars_.push_back(b);
        if (bars_.size() > cap_)
            bars_.erase(bars_.begin());
    } else {
        Bar& b = bars_.back();
        b.c = px;
        if (px > b.h)
            b.h = px;
        if (px < b.l)
            b.l = px;
        b.v += qty;
    }
}

void Series::push_bar(const Bar& bar)
{
    bars_.push_back(bar);
    if (bars_.size() > cap_)
        bars_.erase(bars_.begin());
}

// ---------- indicator engine ----------

namespace {

    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    struct WheelZoomIntent {
        bool active = false;
        bool x_only = false;
        bool y_only = false;
    };

    WheelZoomIntent wheel_zoom_intent()
    {
        const ImGuiIO& io = ImGui::GetIO();
        // IsPlotHovered() calls ImPlot::SetupLock(). This helper runs before
        // SetupAxes() so using the public query here makes every subsequent
        // Setup* call invalid. BeginPlot() has already established FrameRect;
        // use that non-locking geometry for the wheel-routing decision.
        const ImPlotPlot* plot = ImPlot::GetCurrentPlot();
        // Require the plot's own window to be the direct hover target. A
        // child window may intentionally sit above the plot (for example an
        // interactive mini-chart overlay); its wheel input must not also
        // transfer or zoom the obscured parent chart.
        const bool frame_hovered = plot != nullptr
            && plot->FrameRect.Contains(io.MousePos)
            && ImGui::IsWindowHovered(ImGuiHoveredFlags_None);
        WheelZoomIntent result;
        result.active = detail::wheel_zoom_requested(
            frame_hovered, io.MouseWheel, io.KeyCtrl);
        result.x_only = result.active && io.KeyShift && !io.KeyAlt;
        result.y_only = result.active && io.KeyAlt && !io.KeyShift;
        return result;
    }

    ImPlotAxisFlags interactive_x_flags(
        ImPlotAxisFlags flags, const WheelZoomIntent& zoom)
    {
        return zoom.y_only ? flags | ImPlotAxisFlags_Lock : flags;
    }

    ImPlotAxisFlags interactive_y_flags(
        ImPlotAxisFlags flags, const WheelZoomIntent& zoom)
    {
        return zoom.x_only ? flags | ImPlotAxisFlags_Lock : flags;
    }

    void ema(const std::vector<Bar>& bars, int n, std::vector<double>& out)
    {
        out.assign(bars.size(), kNaN);
        if (static_cast<int>(bars.size()) < n || n <= 0)
            return;
        const double k = 2.0 / (n + 1);
        double seed = 0;
        for (int i = 0; i < n; ++i)
            seed += bars[static_cast<std::size_t>(i)].c;
        seed /= n;
        out[static_cast<std::size_t>(n - 1)] = seed;
        for (std::size_t i = static_cast<std::size_t>(n); i < bars.size(); ++i)
            out[i] = bars[i].c * k + out[i - 1] * (1 - k);
    }

    void ema_of(const std::vector<double>& src, int n, std::vector<double>& out)
    {
        out.assign(src.size(), kNaN);
        const double k = 2.0 / (n + 1);
        int have = 0;
        double seed = 0;
        for (std::size_t i = 0; i < src.size(); ++i) {
            if (std::isnan(src[i]))
                continue;
            if (have < n) {
                seed += src[i];
                if (++have == n)
                    out[i] = seed / n;
            } else {
                out[i] = src[i] * k + out[i - 1] * (1 - k);
            }
        }
    }

    void sma_std(const std::vector<Bar>& bars, int n, std::vector<double>& mid,
        std::vector<double>* stddev)
    {
        mid.assign(bars.size(), kNaN);
        if (stddev)
            stddev->assign(bars.size(), kNaN);
        if (static_cast<int>(bars.size()) < n || n <= 0)
            return;
        double sum = 0, sq = 0;
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const double c = bars[i].c;
            sum += c;
            sq += c * c;
            if (i >= static_cast<std::size_t>(n)) {
                const double d = bars[i - static_cast<std::size_t>(n)].c;
                sum -= d;
                sq -= d * d;
            }
            if (i + 1 >= static_cast<std::size_t>(n)) {
                const double m = sum / n;
                mid[i] = m;
                if (stddev) {
                    const double var = std::max(0.0, sq / n - m * m);
                    (*stddev)[i] = std::sqrt(var);
                }
            }
        }
    }

    void compute_rsi(
        const std::vector<Bar>& bars, int n, std::vector<double>& out)
    {
        out.assign(bars.size(), kNaN);
        if (n <= 0 || bars.size() <= static_cast<std::size_t>(n))
            return;
        double gain = 0, loss = 0;
        for (int i = 1; i <= n; ++i) {
            const double change = bars[static_cast<std::size_t>(i)].c
                - bars[static_cast<std::size_t>(i - 1)].c;
            gain += std::max(0.0, change);
            loss += std::max(0.0, -change);
        }
        gain /= n;
        loss /= n;
        const auto value = [](double g, double l) {
            if (l <= 0)
                return g <= 0 ? 50.0 : 100.0;
            return 100.0 - 100.0 / (1.0 + g / l);
        };
        out[static_cast<std::size_t>(n)] = value(gain, loss);
        for (std::size_t i = static_cast<std::size_t>(n + 1);
             i < bars.size(); ++i) {
            const double change = bars[i].c - bars[i - 1].c;
            gain = (gain * (n - 1) + std::max(0.0, change)) / n;
            loss = (loss * (n - 1) + std::max(0.0, -change)) / n;
            out[i] = value(gain, loss);
        }
    }

    void compute_atr(
        const std::vector<Bar>& bars, int n, std::vector<double>& out)
    {
        out.assign(bars.size(), kNaN);
        if (n <= 0 || bars.size() <= static_cast<std::size_t>(n))
            return;
        std::vector<double> tr(bars.size(), 0);
        for (std::size_t i = 1; i < bars.size(); ++i)
            tr[i] = std::max({ bars[i].h - bars[i].l,
                std::fabs(bars[i].h - bars[i - 1].c),
                std::fabs(bars[i].l - bars[i - 1].c) });
        double value = 0;
        for (int i = 1; i <= n; ++i)
            value += tr[static_cast<std::size_t>(i)];
        value /= n;
        out[static_cast<std::size_t>(n)] = value;
        for (std::size_t i = static_cast<std::size_t>(n + 1);
             i < bars.size(); ++i) {
            value = (value * (n - 1) + tr[i]) / n;
            out[i] = value;
        }
    }

}

void IndicatorSet::compute(const std::vector<Bar>& bars)
{
    x.resize(bars.size());
    for (std::size_t i = 0; i < bars.size(); ++i)
        x[i] = bars[i].t;

    if (ema_fast)
        ema(bars, ema_fast_n, ema_fast_v);
    if (ema_slow)
        ema(bars, ema_slow_n, ema_slow_v);
    if (sma)
        sma_std(bars, sma_n, sma_v, nullptr);
    if (boll) {
        std::vector<double> sd;
        sma_std(bars, boll_n, boll_mid, &sd);
        boll_up.assign(bars.size(), kNaN);
        boll_dn.assign(bars.size(), kNaN);
        for (std::size_t i = 0; i < bars.size(); ++i) {
            if (std::isnan(boll_mid[i]))
                continue;
            boll_up[i] = boll_mid[i] + boll_k * sd[i];
            boll_dn[i] = boll_mid[i] - boll_k * sd[i];
        }
    }
    if (vwap) {
        vwap_v.assign(bars.size(), kNaN);
        double notional = 0, volume_sum = 0;
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const double typical = (bars[i].h + bars[i].l + bars[i].c) / 3.0;
            notional += typical * bars[i].v;
            volume_sum += bars[i].v;
            if (volume_sum > 0)
                vwap_v[i] = notional / volume_sum;
        }
    }
    if (macd) {
        std::vector<double> f, s;
        ema(bars, macd_fast, f);
        ema(bars, macd_slow, s);
        macd_dif.assign(bars.size(), kNaN);
        for (std::size_t i = 0; i < bars.size(); ++i)
            if (!std::isnan(f[i]) && !std::isnan(s[i]))
                macd_dif[i] = f[i] - s[i];
        ema_of(macd_dif, macd_signal, macd_dea);
        macd_hist.assign(bars.size(), kNaN);
        for (std::size_t i = 0; i < bars.size(); ++i)
            if (!std::isnan(macd_dif[i]) && !std::isnan(macd_dea[i]))
                macd_hist[i] = (macd_dif[i] - macd_dea[i]) * 2.0;
    }
    if (rsi)
        compute_rsi(bars, rsi_n, rsi_v);
    if (atr)
        compute_atr(bars, atr_n, atr_v);
}

// ---------- drawing primitives ----------

namespace {

    int fmt_hms(double value, char* buf, int size, void*)
    {
        const int sec
            = static_cast<int>(std::fmod(value, 86400.0) + 86400.0) % 86400;
        return std::snprintf(buf, static_cast<std::size_t>(size),
            "%02d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
    }

    // Candles, TV style: a body plus a 1px wick. `t` is the bar's
    // START; the body is centered inside [t, t+bar).
    void plot_candles(const char* id, const std::vector<Bar>& bars,
        double bar_s, const ImVec4& bull, const ImVec4& bear)
    {
        if (bars.empty())
            return;
        if (!ImPlot::BeginItem(id))
            return;
        if (ImPlot::GetCurrentPlot()->FitThisFrame) {
            for (const Bar& b : bars) {
                ImPlot::FitPoint(ImPlotPoint(b.t, b.l));
                ImPlot::FitPoint(ImPlotPoint(b.t + bar_s, b.h));
            }
        }
        ImDrawList* dl = ImPlot::GetPlotDrawList();
        const double half = bar_s * 0.5, body = bar_s * 0.36;
        const ImU32 cb = ImGui::GetColorU32(bull),
                    cr = ImGui::GetColorU32(bear);
        for (const Bar& b : bars) {
            const bool up = b.c >= b.o;
            const ImU32 col = up ? cb : cr;
            const double mid = b.t + half;
            const ImVec2 w0 = ImPlot::PlotToPixels(mid, b.h);
            const ImVec2 w1 = ImPlot::PlotToPixels(mid, b.l);
            dl->AddLine(w0, w1, col, 1.0f);
            const ImVec2 b0 = ImPlot::PlotToPixels(mid - body, up ? b.c : b.o);
            const ImVec2 b1 = ImPlot::PlotToPixels(mid + body, up ? b.o : b.c);
            // The body keeps at least one pixel: a doji stays visible.
            if (std::fabs(b1.y - b0.y) < 1.0f)
                dl->AddLine(ImVec2(b0.x, b0.y), ImVec2(b1.x, b0.y), col, 1.0f);
            else
                dl->AddRectFilled(b0, b1, col);
        }
        ImPlot::EndItem();
    }

    // Volume columns — bull green, bear red, body-width matched to
    // the candles.
    void plot_volume(const char* id, const std::vector<Bar>& bars, double bar_s,
        const ImVec4& bull, const ImVec4& bear)
    {
        if (bars.empty())
            return;
        if (!ImPlot::BeginItem(id))
            return;
        if (ImPlot::GetCurrentPlot()->FitThisFrame) {
            for (const Bar& b : bars) {
                ImPlot::FitPoint(ImPlotPoint(b.t, 0));
                ImPlot::FitPoint(ImPlotPoint(b.t + bar_s, b.v));
            }
        }
        ImDrawList* dl = ImPlot::GetPlotDrawList();
        const double half = bar_s * 0.5, body = bar_s * 0.36;
        for (const Bar& b : bars) {
            ImVec4 col = b.c >= b.o ? bull : bear;
            col.w = 0.55f;
            const double mid = b.t + half;
            const ImVec2 p0 = ImPlot::PlotToPixels(mid - body, b.v);
            const ImVec2 p1 = ImPlot::PlotToPixels(mid + body, 0.0);
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(col));
        }
        ImPlot::EndItem();
    }

    // MACD histogram — positive green, negative red, drawn on bar
    // centers the way TV does.
    void plot_hist(const char* id, const std::vector<double>& x,
        const std::vector<double>& h, double bar_s, const ImVec4& bull,
        const ImVec4& bear)
    {
        if (x.empty())
            return;
        if (!ImPlot::BeginItem(id))
            return;
        if (ImPlot::GetCurrentPlot()->FitThisFrame) {
            for (std::size_t i = 0; i < x.size(); ++i)
                if (!std::isnan(h[i]))
                    ImPlot::FitPoint(ImPlotPoint(x[i], h[i]));
        }
        ImDrawList* dl = ImPlot::GetPlotDrawList();
        const double half = bar_s * 0.5, body = bar_s * 0.30;
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (std::isnan(h[i]))
                continue;
            ImVec4 col = h[i] >= 0 ? bull : bear;
            col.w = 0.85f;
            const double mid = x[i] + half;
            const ImVec2 p0 = ImPlot::PlotToPixels(mid - body, h[i]);
            const ImVec2 p1 = ImPlot::PlotToPixels(mid + body, 0.0);
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(col));
        }
        ImPlot::EndItem();
    }

    void plot_line(const char* id, const std::vector<double>& x,
        const std::vector<double>& y, const ImVec4& col, float weight)
    {
        if (x.empty() || y.size() != x.size())
            return;
        // ImPlot's fast antialiasing path uses the font atlas' baked line
        // texture. It is both cheaper and cleaner than geometry AA, but its
        // samples are indexed by an integer pixel width. Normalize authored
        // fractional widths so the selected texture and geometry agree.
        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
        draw_list->Flags |= ImDrawListFlags_AntiAliasedLines
            | ImDrawListFlags_AntiAliasedLinesUseTex;
        ImPlotSpec spec;
        spec.LineColor = col;
        spec.LineWeight = std::max(1.0f, std::round(weight));
        ImPlot::PlotLine(
            id, x.data(), y.data(), static_cast<int>(x.size()), spec);
    }

    // Array extrema over the visible slice [x0,x1], NANs skipped.
    void minmax_visible(const std::vector<double>& x,
        const std::vector<double>& y, double x0, double x1, double& lo,
        double& hi)
    {
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (x[i] < x0 || x[i] > x1 || std::isnan(y[i]))
                continue;
            lo = std::min(lo, y[i]);
            hi = std::max(hi, y[i]);
        }
    }

}

// ---------- Chart ----------

void Chart::takeover_check(bool wheel_zoom_active)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool plot_dragged = ImPlot::IsPlotHovered()
        && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    // IsPlotHovered() only covers the data canvas. ImPlot also accepts wheel
    // zoom over the X/Y axis regions, so use the pre-setup frame hit-test
    // propagated by wheel_zoom_intent() to relinquish follow mode there too.
    if (detail::viewport_takeover_requested(
            plot_dragged, wheel_zoom_active, io.KeyCtrl)) {
        follow_ = false;
        follow_request_pending_ = false;
    }
    // The main pane is not present while an indicator pane is focused. Keep
    // the shared X viewport authoritative from whichever pane is actually
    // visible, otherwise changing the selected indicator restores stale
    // coordinates and appears to jump back to an earlier chart position.
    if (!follow_) {
        const ImPlotRect limits = ImPlot::GetPlotLimits();
        if (std::isfinite(limits.X.Min) && std::isfinite(limits.X.Max)
            && limits.X.Max > limits.X.Min) {
            vx0_ = limits.X.Min;
            vx1_ = limits.X.Max;
            span_ = vx1_ - vx0_;
        }
    }
}

void Chart::update_view(const Series& s)
{
    const auto& bars = s.bars();
    if (bars.empty())
        return;
    if (follow_request_pending_) {
        follow_ = true;
        follow_request_pending_ = false;
    }
    const double bar_s = s.bar_seconds();
    bar_seconds_ = bar_s;
    data_span_ = std::max(
        bar_s, bars.back().t + bar_s - bars.front().t);
    if (!std::isfinite(span_) || span_ <= 0)
        span_ = bar_s * 120; // 120 bars per screen by default
    if (follow_) {
        const double minimum_span = bar_s * 12.0;
        const double maximum_span = std::max(
            minimum_span, data_span_ + bar_s * 12.0);
        span_ = std::clamp(span_, minimum_span, maximum_span);
    }
    const float dt = ImGui::GetIO().DeltaTime;
    // Inside the seamless-switch window every easing teleports; the
    // frame counter burns here, exactly once per frame.
    const bool snap_this_frame = snap_frames_ > 0;
    if (snap_this_frame)
        --snap_frames_;
    const double alpha = snap_this_frame
        ? 1.0
        : 1.0 - std::exp(-double(dt) * 12.0);
    const double tx1 = bars.back().t + bar_s * 6.0; // TV's right margin
    double tx0 = tx1 - span_;
    const double first = bars.front().t;
    if (tx0 < first)
        tx0 = first; // less than a screen of data leaves no dead space
    if (!std::isfinite(vx0_) || !std::isfinite(vx1_) || vx1_ <= vx0_) {
        vx0_ = tx0;
        vx1_ = tx1;
    }
    if (!follow_)
        return;
    vx0_ += (tx0 - vx0_) * alpha;
    vx1_ += (tx1 - vx1_) * alpha;
}

void Chart::zoom(double factor)
{
    if (!(factor > 0) || span_ <= 0)
        return;
    const double minimum = bar_seconds_ * 12.0;
    const double maximum
        = std::max(minimum, data_span_ + bar_seconds_ * 12.0);
    const double next = std::clamp(span_ * factor, minimum, maximum);
    const double anchor = follow_ ? vx1_ : (vx0_ + vx1_) * 0.5;
    span_ = next;
    if (follow_)
        vx0_ = vx1_ - span_;
    else {
        vx0_ = anchor - span_ * 0.5;
        vx1_ = anchor + span_ * 0.5;
        manual_view_frames_ = 2;
    }
}

void Chart::pan_bars(double bars)
{
    if (span_ <= 0)
        return;
    follow_ = false;
    follow_request_pending_ = false;
    const double delta = bars * bar_seconds_;
    vx0_ += delta;
    vx1_ += delta;
    manual_view_frames_ = 2;
}

static ImVec4 readable_overlay_accent(ImVec4 color)
{
    const ImVec4 background
        = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const float luminance = background.x * 0.2126f
        + background.y * 0.7152f + background.z * 0.0722f;
    if (luminance > 0.55f) {
        color.x *= 0.68f;
        color.y *= 0.68f;
        color.z *= 0.68f;
    }
    return color;
}

void Chart::draw_expiry_risk_fan(const ImPlotRect& limits)
{
    const auto& layer = expiry_risk;
    if (!layer.available || !(layer.current_price > 0)
        || !(layer.anchor_price > 0) || !(layer.expected_move_bps > 0)
        || !(layer.end_t > layer.start_t))
        return;
    const double x0 = std::max(limits.X.Min, layer.start_t);
    const double x1 = std::min(limits.X.Max, layer.end_t);
    if (!(x1 > x0))
        return;

    constexpr int segments = 64;
    const double duration = layer.end_t - layer.start_t;
    const double sigma = layer.expected_move_bps / 10000.0;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const auto draw_adverse_boundary = [&](double multiplier, ImVec4 color,
                                           float fill_alpha,
                                           float line_alpha) {
        std::vector<ImVec2> boundary;
        std::vector<ImVec2> risky_boundary;
        std::vector<ImVec2> risky_anchor;
        boundary.reserve(segments + 1);
        risky_boundary.reserve(segments + 1);
        risky_anchor.reserve(segments + 1);
        for (int index = 0; index <= segments; ++index) {
            const double fraction = static_cast<double>(index) / segments;
            const double x = x0 + (x1 - x0) * fraction;
            const double forward_fraction =
                std::clamp((x - layer.start_t) / duration, 0.0, 1.0);
            const double move
                = sigma * multiplier * std::sqrt(forward_fraction);
            const double adverse_price
                = layer.current_price
                * std::exp(layer.direction_up ? -move : move);
            const ImVec2 point = ImPlot::PlotToPixels(x, adverse_price);
            boundary.push_back(point);
            const bool reaches_anchor
                = layer.direction_up ? adverse_price <= layer.anchor_price
                                     : adverse_price >= layer.anchor_price;
            if (reaches_anchor) {
                risky_boundary.push_back(point);
                risky_anchor.push_back(
                    ImPlot::PlotToPixels(x, layer.anchor_price));
            }
        }
        color.w = line_alpha;
        if (boundary.size() >= 2)
            dl->AddPolyline(boundary.data(), static_cast<int>(boundary.size()),
                ImGui::GetColorU32(color), ImDrawFlags_None, 1.25f);
        if (risky_boundary.size() >= 2) {
            std::vector<ImVec2> polygon;
            polygon.reserve(risky_boundary.size() + risky_anchor.size());
            polygon.insert(
                polygon.end(), risky_boundary.begin(), risky_boundary.end());
            polygon.insert(
                polygon.end(), risky_anchor.rbegin(), risky_anchor.rend());
            color.w = fill_alpha;
            dl->AddConvexPolyFilled(polygon.data(),
                static_cast<int>(polygon.size()), ImGui::GetColorU32(color));
        }
    };

    draw_adverse_boundary(
        2.0, readable_overlay_accent(
                 ImVec4(0.98f, 0.68f, 0.16f, 1.0f)),
        0.035f, 0.30f);
    draw_adverse_boundary(
        1.0, readable_overlay_accent(
                 ImVec4(0.95f, 0.27f, 0.35f, 1.0f)),
        0.085f, 0.72f);
}

void Chart::draw_expiry_risk_hud()
{
    auto& layer = expiry_risk;
    if (!layer.available)
        return;
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x < 260.0f || plot_size.y < 90.0f)
        return;

    const bool safe = layer.safety_distance >= 2.0;
    const bool watch = layer.safety_distance >= 1.0;
    const char* state = safe ? "SAFE" : (watch ? "WATCH" : "RISK");
    ImVec4 accent = readable_overlay_accent(
        safe ? theme.bull
             : (watch ? ImVec4(0.98f, 0.68f, 0.16f, 1.0f)
                      : theme.bear));
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    if (layer.minimized) {
        const std::string compact = "[+] " + layer.title;
        const ImVec2 text_size = ImGui::CalcTextSize(compact.c_str());
        const ImVec2 p0(plot_pos.x + 9.0f,
            plot_pos.y + plot_size.y - text_size.y - 27.0f);
        const ImVec2 p1(p0.x + text_size.x + 16.0f,
            p0.y + text_size.y + 9.0f);
        ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        background.w = 0.66f;
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(background), 4.0f);
        accent.w = 0.78f;
        dl->AddRect(p0, p1, ImGui::GetColorU32(accent), 4.0f);
        dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f),
            ImGui::GetColorU32(accent), compact.c_str());
        if (ImGui::IsMouseHoveringRect(p0, p1)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            layer.minimized = false;
        return;
    }
    char text[256];
    int used = std::snprintf(text, sizeof text,
        "%s %s %.2fσ  |  T-%.0fs", layer.title.c_str(), state,
        layer.safety_distance, std::max(0.0, layer.remaining_seconds));
    if (layer.motion_available && used > 0
        && used < static_cast<int>(sizeof text)) {
        used += std::snprintf(text + used, sizeof text - used,
            "  |  v5 %+.2fbp/s  a %+.2f", layer.velocity_5s_bps_per_second,
            layer.acceleration_bps_per_second);
    }
    if (layer.giveback_available && used > 0
        && used < static_cast<int>(sizeof text)) {
        std::snprintf(text + used, sizeof text - used, "  |  giveback %.0f%%",
            std::clamp(layer.giveback, 0.0, 9.99) * 100.0);
    }

    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 p0(plot_pos.x + 9.0f,
        plot_pos.y + plot_size.y - text_size.y - 27.0f);
    const ImVec2 p1(p0.x + text_size.x + 16.0f,
        p0.y + text_size.y + 9.0f);
    ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
    background.w = 0.90f;
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(background), 4.0f);
    accent.w = 0.90f;
    dl->AddRect(p0, p1, ImGui::GetColorU32(accent), 4.0f);
    dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f),
        ImGui::GetColorU32(accent), text);
    if (ImGui::IsMouseHoveringRect(p0, p1)
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        layer.minimized = true;

    if (layer.motion_available && layer.remaining_seconds > 0.0) {
        const double horizon = std::min(15.0, layer.remaining_seconds);
        const double projected = layer.current_price
            * std::exp(layer.velocity_5s_bps_per_second * horizon / 10000.0);
        const ImVec2 start
            = ImPlot::PlotToPixels(layer.start_t, layer.current_price);
        const ImVec2 end
            = ImPlot::PlotToPixels(layer.start_t + horizon, projected);
        const bool moving_away = layer.direction_up
            ? layer.velocity_5s_bps_per_second >= 0.0
            : layer.velocity_5s_bps_per_second <= 0.0;
        ImVec4 motion = readable_overlay_accent(
            moving_away ? theme.bull : theme.bear);
        motion.w = 0.88f;
        const ImU32 motion_color = ImGui::GetColorU32(motion);
        dl->AddLine(start, end, motion_color, 2.0f);
        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 8.0f) {
            const float ux = dx / length;
            const float uy = dy / length;
            const ImVec2 base(end.x - ux * 8.0f, end.y - uy * 8.0f);
            const ImVec2 left(base.x - uy * 3.5f, base.y + ux * 3.5f);
            const ImVec2 right(base.x + uy * 3.5f, base.y - ux * 3.5f);
            dl->AddTriangleFilled(end, left, right, motion_color);
        }
    }

    if (ImGui::IsMouseHoveringRect(p0, p1)) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f),
            ImVec2(520.0f, std::numeric_limits<float>::max()));
        ImGui::BeginTooltip();
        ImGui::TextColored(accent, "%s", text);
        if (!layer.help.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0f);
            ImGui::TextWrapped("%s", layer.help.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        ImGui::Text("Anchor %.2f  Current %.2f  adverse move +/-%.2fbp",
            layer.anchor_price, layer.current_price, layer.expected_move_bps);
        if (layer.motion_available)
            ImGui::Text("v15 %+.2fbp/s  acceleration %+.2fbp/s",
                layer.velocity_15s_bps_per_second,
                layer.acceleration_bps_per_second);
        ImGui::EndTooltip();
    }
}

void Chart::draw_rtds_price_layer(const ImPlotRect& limits)
{
    const auto& layer = rtds_price;
    if (!layer.available || !(layer.current_price > 0))
        return;
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x < 320.0f || plot_size.y < 100.0f)
        return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImVec4 current_color = readable_overlay_accent(
        layer.fresh ? ImVec4(0.22f, 0.74f, 0.96f, 1.0f)
                    : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool above_beat
        = layer.beat_price > 0 && layer.current_price >= layer.beat_price;
    ImVec4 direction_color = readable_overlay_accent(
        above_beat ? theme.bull : theme.bear);

    if (layer.beat_price > 0) {
        const ImVec2 beat_left
            = ImPlot::PlotToPixels(limits.X.Min, layer.beat_price);
        const ImVec2 beat_right
            = ImPlot::PlotToPixels(limits.X.Max, layer.beat_price);
        const ImVec2 current_left
            = ImPlot::PlotToPixels(limits.X.Min, layer.current_price);
        const ImVec2 current_right
            = ImPlot::PlotToPixels(limits.X.Max, layer.current_price);
        direction_color.w = layer.fresh ? 0.055f : 0.022f;
        dl->AddRectFilled(
            ImVec2(beat_left.x, std::min(beat_left.y, current_left.y)),
            ImVec2(beat_right.x, std::max(beat_right.y, current_right.y)),
            ImGui::GetColorU32(direction_color));

        ImVec4 beat_color = current_color;
        beat_color.w = layer.fresh ? 0.50f : 0.30f;
        ImPlotSpec beat_line;
        beat_line.LineColor = beat_color;
        beat_line.LineWeight = 1.25f;
        beat_line.Flags = static_cast<ImPlotItemFlags>(
            static_cast<int>(ImPlotInfLinesFlags_Horizontal)
            | static_cast<int>(ImPlotItemFlags_NoFit));
        ImPlot::PlotInfLines(
            "##rtds-beat", &layer.beat_price, 1, beat_line);
        ImPlot::TagY(layer.beat_price, beat_color,
            "CL BEAT %.2f", layer.beat_price);
    }

    ImVec4 current_line_color = current_color;
    current_line_color.w = layer.fresh ? 0.50f : 0.30f;
    const float current_y = ImPlot::PlotToPixels(
        limits.X.Max, layer.current_price).y;
    for (float x = plot_pos.x + 5.0f;
         x < plot_pos.x + plot_size.x - 5.0f; x += 10.0f) {
        dl->AddLine(ImVec2(x, current_y),
            ImVec2(std::min(x + 6.0f,
                       plot_pos.x + plot_size.x - 5.0f),
                current_y),
            ImGui::GetColorU32(current_line_color), 1.4f);
    }
    ImPlot::TagY(layer.current_price, current_color,
        "RTDS %.2f", layer.current_price);

    const double beat_delta = layer.beat_price > 0
        ? layer.current_price - layer.beat_price
        : 0.0;
    const double beat_gap_bps = layer.beat_price > 0
        ? std::log(layer.current_price / layer.beat_price) * 10000.0
        : 0.0;
    char text[320];
    if (layer.beat_price > 0) {
        std::snprintf(text, sizeof text,
            "%s %s %s  %.2f  |  CL BEAT %.2f  |  %+.2f USD / %+.2fbp  |  %.0fms",
            layer.title.c_str(), layer.fresh ? "LIVE" : "STALE",
            above_beat ? "UP" : "DOWN",
            layer.current_price, layer.beat_price, beat_delta, beat_gap_bps,
            std::max(0.0, layer.current_age_ms));
    } else {
        std::snprintf(text, sizeof text,
            "%s %s  NOW %.2f  CL BEAT N/A  %.0fms", layer.title.c_str(),
            layer.fresh ? "LIVE" : "STALE", layer.current_price,
            std::max(0.0, layer.current_age_ms));
    }
    // Keep the overlay geometry stable while prices, age and LIVE/STALE text
    // change. The slot follows a deliberate window resize, never the measured
    // width of a volatile value string.
    const float card_width = std::min(
        plot_size.x - 20.0f, std::max(360.0f, plot_size.x * 0.62f));
    ImVec2 text_size = ImGui::CalcTextSize(text);
    if (text_size.x > card_width - 16.0f && layer.beat_price > 0) {
        std::snprintf(text, sizeof text,
            "%s %s %s %.2f | CL B %.2f | %+.2fbp",
            layer.title.c_str(), layer.fresh ? "LIVE" : "STALE",
            above_beat ? "UP" : "DOWN",
            layer.current_price, layer.beat_price, beat_gap_bps);
        text_size = ImGui::CalcTextSize(text);
    }
    const ImVec2 p0(plot_pos.x + 10.0f,
        plot_pos.y + ImGui::GetTextLineHeightWithSpacing() * 2.0f + 10.0f);
    const ImVec2 p1(p0.x + card_width,
        p0.y + text_size.y + 9.0f);
    ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
    background.w = 0.50f;
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(background), 4.0f);
    current_color.w = layer.fresh ? 0.88f : 0.58f;
    dl->AddRect(p0, p1, ImGui::GetColorU32(current_color), 4.0f);
    dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f),
        ImGui::GetColorU32(current_color), text);

    if (ImGui::IsMouseHoveringRect(p0, p1)) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 0.0f),
            ImVec2(560.0f, std::numeric_limits<float>::max()));
        ImGui::BeginTooltip();
        ImGui::TextColored(current_color, "%s", text);
        if (!layer.help.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 500.0f);
            ImGui::TextWrapped("%s", layer.help.c_str());
            ImGui::PopTextWrapPos();
        }
        if (layer.beat_price > 0)
            ImGui::Text("Chainlink beat %.8g  official gap %+.3fbp",
                layer.beat_price, layer.official_gap_bps);
        if (layer.pmt_reference_price > 0 && layer.beat_price > 0)
            ImGui::Text("PMT reference %.8g  CL-vs-PMT basis %+.3fbp",
                layer.pmt_reference_price,
                std::log(layer.beat_price / layer.pmt_reference_price)
                    * 10000.0);
        if (layer.proxy_available)
            ImGui::Text("Proxy %+.3fbp  direction %s",
                layer.proxy_gap_bps,
                layer.direction_agrees ? "AGREE" : "DIVERGE");
        ImGui::Text("Chainlink reference %s  offset %+.0fms",
            layer.reference_exact ? "EXACT" : "APPROX",
            layer.reference_offset_ms);
        ImGui::EndTooltip();
    }
}

void Chart::draw_venue_race_layer()
{
    auto& layer = venue_race;
    if (!layer.available || layer.entries.empty())
        return;
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const auto& style = ImGui::GetStyle();
    const float text_height = ImGui::GetTextLineHeight();
    const float padding_x = std::max(9.0f, style.FramePadding.x + 3.0f);
    const float padding_y = std::max(5.0f, style.FramePadding.y + 1.0f);
    const float row_height = std::ceil(
        text_height + std::max(6.0f, style.ItemSpacing.y * 0.75f));
    const float header_height = std::ceil(text_height + padding_y * 2.0f);
    const float top_offset
        = (research.available ? text_height + 32.0f : 8.0f)
        + (rtds_price.available ? text_height + 34.0f : 0.0f);
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    if (layer.minimized) {
        const std::string compact = "[+] " + layer.title;
        const ImVec2 text_size = ImGui::CalcTextSize(compact.c_str());
        const ImVec2 p0(plot_pos.x + 10.0f, plot_pos.y + top_offset);
        const ImVec2 p1(p0.x + text_size.x + padding_x * 2.0f,
            p0.y + text_height + padding_y * 2.0f);
        ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        background.w = 0.62f;
        ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
        border.w = 0.52f;
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(background), 5.0f);
        dl->AddRect(p0, p1, ImGui::GetColorU32(border), 5.0f);
        dl->AddText(ImVec2(p0.x + padding_x, p0.y + padding_y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), compact.c_str());
        if (ImGui::IsMouseHoveringRect(p0, p1)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            layer.minimized = false;
        return;
    }
    const auto format_value = [](char* destination, std::size_t size,
                                  const auto& entry) {
        if (entry.lead_lag_5s_available)
            std::snprintf(destination, size, "%+.2fbp  L5 %+.2f",
                entry.deviation_bps, entry.lead_lag_5s);
        else
            std::snprintf(destination, size, "%+.2fbp  L5 --",
                entry.deviation_bps);
    };
    float label_width = 0.0f;
    float value_width = 0.0f;
    for (const auto& entry : layer.entries) {
        label_width
            = std::max(label_width, ImGui::CalcTextSize(entry.label.c_str()).x);
        char value[96];
        format_value(value, sizeof value, entry);
        value_width = std::max(value_width, ImGui::CalcTextSize(value).x);
    }
    const float track_width = std::max(132.0f, text_height * 5.8f);
    const float column_gap = std::max(10.0f, style.ItemSpacing.x);
    char header_stats[96];
    std::snprintf(header_stats, sizeof header_stats, "MAD %.2fbp  agree %.0f%%",
        layer.consensus_mad_bps,
        std::clamp(layer.agreement, 0.0, 1.0) * 100.0);
    const float width = std::ceil(std::max(
        padding_x * 2.0f + ImGui::CalcTextSize(layer.title.c_str()).x
            + column_gap + ImGui::CalcTextSize(header_stats).x,
        padding_x * 2.0f + label_width + column_gap + track_width
            + column_gap + value_width));
    const float card_height = header_height
        + row_height * static_cast<float>(layer.entries.size()) + padding_y;
    if (plot_size.x < width + 40.0f
        || plot_size.y < card_height + 30.0f)
        return;
    const ImVec2 p0(plot_pos.x + 10.0f, plot_pos.y + top_offset);
    const ImVec2 p1(p0.x + width, p0.y + card_height);
    ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
    background.w = 0.68f;
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(background), 6.0f);
    ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
    border.w = 0.58f;
    dl->AddRect(p0, p1, ImGui::GetColorU32(border), 6.0f);

    ImVec4 header_fill = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    header_fill.w = 0.34f;
    dl->AddRectFilled(
        p0, ImVec2(p1.x, p0.y + header_height),
        ImGui::GetColorU32(header_fill), 6.0f, ImDrawFlags_RoundCornersTop);
    const float header_text_y = p0.y + (header_height - text_height) * 0.5f;
    dl->AddText(ImVec2(p0.x + padding_x, header_text_y),
        ImGui::GetColorU32(ImGuiCol_Text), layer.title.c_str());
    const float header_stats_width = ImGui::CalcTextSize(header_stats).x;
    dl->AddText(
        ImVec2(p1.x - padding_x - header_stats_width, header_text_y),
        ImGui::GetColorU32(ImGuiCol_TextDisabled), header_stats);
    dl->AddLine(ImVec2(p0.x + padding_x, p0.y + header_height),
        ImVec2(p1.x - padding_x, p0.y + header_height),
        ImGui::GetColorU32(border), 1.0f);
    if (ImGui::IsMouseHoveringRect(p0, ImVec2(p1.x, p0.y + header_height))
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        layer.minimized = true;

    double scale = 1.0;
    for (const auto& entry : layer.entries)
        scale = std::max(scale, std::abs(entry.deviation_bps) * 1.15);
    const float label_x = p0.x + padding_x;
    const float track_min = label_x + label_width + column_gap;
    const float track_max = track_min + track_width;
    const float track_center = (track_min + track_max) * 0.5f;
    const float track_half = (track_max - track_min) * 0.5f;
    const float value_right = p1.x - padding_x;
    const ImVec2 mouse = ImGui::GetMousePos();
    for (std::size_t index = 0; index < layer.entries.size(); ++index) {
        const auto& entry = layer.entries[index];
        const float row_top
            = p0.y + header_height + row_height * static_cast<float>(index);
        const float y = row_top + row_height * 0.5f;
        const float text_y = std::floor(y - text_height * 0.5f);
        if (index % 2 == 1) {
            ImVec4 row_fill = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
            row_fill.w = 0.16f;
            dl->AddRectFilled(ImVec2(p0.x + 1.0f, row_top),
                ImVec2(p1.x - 1.0f, row_top + row_height),
                ImGui::GetColorU32(row_fill));
        }
        ImVec4 positive = readable_overlay_accent(theme.bull);
        ImVec4 negative = readable_overlay_accent(theme.bear);
        positive.w = negative.w = 0.14f;
        const float track_half_height
            = std::clamp(text_height * 0.28f, 5.0f, 8.0f);
        dl->AddRectFilled(ImVec2(track_center, y - track_half_height),
            ImVec2(track_max, y + track_half_height),
            ImGui::GetColorU32(positive), 3.0f);
        dl->AddRectFilled(ImVec2(track_min, y - track_half_height),
            ImVec2(track_center, y + track_half_height),
            ImGui::GetColorU32(negative), 3.0f);
        dl->AddLine(ImVec2(track_center, y - track_half_height - 1.0f),
            ImVec2(track_center, y + track_half_height + 1.0f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), 1.0f);
        const float normalized = static_cast<float>(
            std::clamp(entry.deviation_bps / scale, -1.0, 1.0));
        const float dot_x = track_center + normalized * track_half;
        ImVec4 dot = readable_overlay_accent(
            entry.deviation_bps >= 0.0 ? theme.bull : theme.bear);
        dot.w = 0.95f;
        dl->AddCircleFilled(ImVec2(dot_x, y), 4.0f, ImGui::GetColorU32(dot));
        dl->AddText(ImVec2(label_x, text_y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), entry.label.c_str());
        char value[96];
        format_value(value, sizeof value, entry);
        const float value_text_width = ImGui::CalcTextSize(value).x;
        dl->AddText(ImVec2(value_right - value_text_width, text_y),
            ImGui::GetColorU32(dot), value);

        const ImVec2 row0(p0.x, y - row_height * 0.5f);
        const ImVec2 row1(p1.x, y + row_height * 0.5f);
        if (mouse.x >= row0.x && mouse.x <= row1.x && mouse.y >= row0.y
            && mouse.y <= row1.y) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f),
                ImVec2(520.0f, std::numeric_limits<float>::max()));
            ImGui::BeginTooltip();
            ImGui::TextColored(dot, "%s  deviation %+.3fbp",
                entry.label.c_str(), entry.deviation_bps);
            ImGui::Text("Lead-lag correlation  1s %s  5s %s  15s %s",
                entry.lead_lag_1s_available ? "available" : "--",
                entry.lead_lag_5s_available ? "available" : "--",
                entry.lead_lag_15s_available ? "available" : "--");
            if (entry.lead_lag_1s_available)
                ImGui::Text("1s  %+.3f", entry.lead_lag_1s);
            if (entry.lead_lag_5s_available)
                ImGui::Text("5s  %+.3f", entry.lead_lag_5s);
            if (entry.lead_lag_15s_available)
                ImGui::Text("15s %+.3f", entry.lead_lag_15s);
            if (!layer.help.empty()) {
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0f);
                ImGui::TextWrapped("%s", layer.help.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTooltip();
        }
    }
}

void Chart::draw_leverage_regime_layer(const ImPlotRect& limits)
{
    auto& layer = leverage_regime;
    if (!layer.available || layer.points.empty())
        return;
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x < 160.0f || plot_size.y < 80.0f)
        return;
    const float strip_top = plot_pos.y + plot_size.y - 9.0f;
    const float strip_bottom = plot_pos.y + plot_size.y - 2.0f;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const auto color_for = [&](LeverageRegime regime) {
        switch (regime) {
        case LeverageRegime::LongBuild:
            return readable_overlay_accent(
                ImVec4(0.16f, 0.78f, 0.55f, 1.0f));
        case LeverageRegime::ShortBuild:
            return readable_overlay_accent(
                ImVec4(0.94f, 0.29f, 0.38f, 1.0f));
        case LeverageRegime::ShortCover:
            return readable_overlay_accent(
                ImVec4(0.23f, 0.63f, 0.96f, 1.0f));
        case LeverageRegime::LongUnwind:
            return readable_overlay_accent(
                ImVec4(0.98f, 0.64f, 0.15f, 1.0f));
        case LeverageRegime::Neutral:
            return readable_overlay_accent(
                ImVec4(0.55f, 0.58f, 0.66f, 1.0f));
        }
        return readable_overlay_accent(
            ImVec4(0.55f, 0.58f, 0.66f, 1.0f));
    };
    const auto label_for = [](LeverageRegime regime) {
        switch (regime) {
        case LeverageRegime::LongBuild:
            return "LONG BUILD";
        case LeverageRegime::ShortBuild:
            return "SHORT BUILD";
        case LeverageRegime::ShortCover:
            return "SHORT COVER";
        case LeverageRegime::LongUnwind:
            return "LONG UNWIND";
        case LeverageRegime::Neutral:
            return "NEUTRAL";
        }
        return "NEUTRAL";
    };

    const ImVec2 mouse = ImGui::GetMousePos();
    const LeverageRegimePoint* hovered = nullptr;
    for (std::size_t index = 0; index < layer.points.size(); ++index) {
        const auto& point = layer.points[index];
        const double end_t = index + 1 < layer.points.size()
            ? layer.points[index + 1].t
            : layer.through_t;
        const double x0 = std::max(limits.X.Min, point.t);
        const double x1 = std::min(limits.X.Max, end_t);
        if (!(x1 > x0))
            continue;
        const float px0 = ImPlot::PlotToPixels(x0, limits.Y.Min).x;
        const float px1 = ImPlot::PlotToPixels(x1, limits.Y.Min).x;
        ImVec4 color = color_for(point.regime);
        color.w = point.regime == LeverageRegime::Neutral
            ? 0.08f
            : static_cast<float>(
                  0.22 + 0.58 * std::clamp(point.intensity, 0.0, 1.0));
        dl->AddRectFilled(ImVec2(px0, strip_top),
            ImVec2(std::max(px0 + 1.0f, px1), strip_bottom),
            ImGui::GetColorU32(color));
        if (mouse.x >= px0 && mouse.x <= px1 && mouse.y >= strip_top - 2.0f
            && mouse.y <= strip_bottom + 2.0f)
            hovered = &point;
    }

    const auto& latest = layer.points.back();
    ImVec4 latest_color = color_for(latest.regime);
    latest_color.w = 0.95f;
    char badge[160];
    if (layer.minimized)
        std::snprintf(badge, sizeof badge, "[+] %s", layer.title.c_str());
    else
        std::snprintf(badge, sizeof badge, "%s %s  px %+.2fbp / OI %+.3f%%",
            layer.title.c_str(), label_for(latest.regime),
            latest.price_impulse_bps, latest.open_interest_delta_pct);
    const ImVec2 text_size = ImGui::CalcTextSize(badge);
    const ImVec2 badge0(plot_pos.x + plot_size.x - text_size.x - 25.0f,
        strip_top - text_size.y - 9.0f);
    const ImVec2 badge1(
        badge0.x + text_size.x + 15.0f, badge0.y + text_size.y + 7.0f);
    if (badge0.x > plot_pos.x + 280.0f) {
        ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        background.w = 0.86f;
        dl->AddRectFilled(
            badge0, badge1, ImGui::GetColorU32(background), 3.0f);
        dl->AddRect(
            badge0, badge1, ImGui::GetColorU32(latest_color), 3.0f);
        dl->AddText(ImVec2(badge0.x + 7.0f, badge0.y + 3.0f),
            ImGui::GetColorU32(latest_color), badge);
        if (ImGui::IsMouseHoveringRect(badge0, badge1)) {
            hovered = &latest;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                layer.minimized = !layer.minimized;
        }
    }

    if (hovered) {
        ImVec4 color = color_for(hovered->regime);
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f),
            ImVec2(520.0f, std::numeric_limits<float>::max()));
        ImGui::BeginTooltip();
        ImGui::TextColored(color, "%s %s", layer.title.c_str(),
            label_for(hovered->regime));
        ImGui::Text("Price impulse %+.3fbp", hovered->price_impulse_bps);
        ImGui::Text(
            "Open-interest change %+.4f%%", hovered->open_interest_delta_pct);
        if (!layer.help.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0f);
            ImGui::TextWrapped("%s", layer.help.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTooltip();
    }
}

void Chart::draw_main_pane(
    const Series& s, const IndicatorSet& ind, bool bottom)
{
    const auto& bars = s.bars();
    const double bar_s = s.bar_seconds();
    const WheelZoomIntent zoom = wheel_zoom_intent();
    const ImPlotAxisFlags xflags = interactive_x_flags(
        bottom ? ImPlotAxisFlags_NoLabel
               : (ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels),
        zoom);
    const ImPlotAxisFlags yflags = interactive_y_flags(
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite, zoom);
    ImPlot::SetupAxes(nullptr, nullptr, xflags, yflags);
    // The formatter rides every pane: NoTickLabels suppresses the tick
    // text but the crosshair readout still goes through the formatter
    // — hovering shows HH:MM:SS, not raw seconds.
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if ((follow_ || manual_view_frames_ > 0) && !bars.empty()) {
        // Y fits the visible candles plus visible indicators only (the
        // TV semantic), exponentially eased.
        double lo = 1e300, hi = -1e300;
        for (const Bar& b : bars) {
            if (b.t + bar_s < vx0_ || b.t > vx1_)
                continue;
            lo = std::min(lo, b.l);
            hi = std::max(hi, b.h);
        }
        if (ind.boll && !ind.boll_up.empty()) {
            minmax_visible(ind.x, ind.boll_up, vx0_, vx1_, lo, hi);
            minmax_visible(ind.x, ind.boll_dn, vx0_, vx1_, lo, hi);
        }
        if (lo < hi) {
            const float dt = ImGui::GetIO().DeltaTime;
            const double alpha
                = snap_frames_ > 0 ? 1.0 : 1.0 - std::exp(-double(dt) * 12.0);
            const double pad = (hi - lo) * 0.08 + 1e-12;
            const double ty0 = lo - pad, ty1 = hi + pad;
            if (vy1_ <= vy0_) {
                vy0_ = ty0;
                vy1_ = ty1;
            }
            vy0_ += (ty0 - vy0_) * alpha;
            vy1_ += (ty1 - vy1_) * alpha;
        }
        if (!zoom.active) {
            ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);
            if (follow_)
                ImPlot::SetupAxisLimits(
                    ImAxis_Y1, vy0_, vy1_, ImGuiCond_Always);
        }
    }

    // The Bollinger band is laid first; lines and candles go on top.
    if (ind.boll && !ind.boll_up.empty()) {
        ImPlotSpec fill;
        fill.FillColor = theme.boll;
        fill.FillAlpha = theme.boll_fill_alpha;
        ImPlot::PlotShaded("BOLL##band", ind.x.data(), ind.boll_up.data(),
            ind.boll_dn.data(), static_cast<int>(ind.x.size()), fill);
        plot_line("##boll-up", ind.x, ind.boll_up, theme.boll, 1.0f);
        plot_line("##boll-dn", ind.x, ind.boll_dn, theme.boll, 1.0f);
        plot_line("##boll-mid", ind.x, ind.boll_mid, theme.boll, 1.0f);
    }
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    if (research.available && research.anchor_price > 0
        && research.expected_move_bps > 0) {
        const double move
            = research.anchor_price * research.expected_move_bps / 10000.0;
        const double x0 = std::max(lim.X.Min, research.window_start_t);
        const double x1 = std::min(lim.X.Max, research.window_end_t);
        if (x1 > x0) {
            const ImVec2 p0
                = ImPlot::PlotToPixels(x0, research.anchor_price + move);
            const ImVec2 p1
                = ImPlot::PlotToPixels(x1, research.anchor_price - move);
            ImVec4 fill = ref_color;
            fill.w = 0.07f;
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(fill));
        }
    }
    if (research.dynamic_cone_enabled && research.cone_center_price > 0
        && research.cone_expected_move_bps > 0
        && research.cone_end_t > research.cone_start_t) {
        const double x0 = std::max(lim.X.Min, research.cone_start_t);
        const double x1 = std::min(lim.X.Max, research.cone_end_t);
        if (x1 > x0) {
            constexpr int segments = 48;
            const double duration
                = research.cone_end_t - research.cone_start_t;
            const double sigma
                = research.cone_expected_move_bps / 10000.0;
            const ImVec4 cone_color(0.28f, 0.64f, 0.98f, 1.0f);
            const auto draw_cone = [&](double multiplier, float alpha,
                                       float line_alpha) {
                std::vector<ImVec2> upper;
                std::vector<ImVec2> lower;
                std::vector<ImVec2> polygon;
                upper.reserve(segments + 1);
                lower.reserve(segments + 1);
                polygon.reserve((segments + 1) * 2);
                for (int index = 0; index <= segments; ++index) {
                    const double fraction
                        = static_cast<double>(index) / segments;
                    const double x = x0 + (x1 - x0) * fraction;
                    const double forward_fraction = std::clamp(
                        (x - research.cone_start_t) / duration, 0.0, 1.0);
                    const double move
                        = sigma * multiplier * std::sqrt(forward_fraction);
                    upper.push_back(ImPlot::PlotToPixels(
                        x, research.cone_center_price * std::exp(move)));
                    lower.push_back(ImPlot::PlotToPixels(
                        x, research.cone_center_price * std::exp(-move)));
                }
                polygon.insert(polygon.end(), upper.begin(), upper.end());
                polygon.insert(polygon.end(), lower.rbegin(), lower.rend());
                ImVec4 fill = cone_color;
                fill.w = alpha;
                dl->AddConvexPolyFilled(polygon.data(),
                    static_cast<int>(polygon.size()), ImGui::GetColorU32(fill));
                ImVec4 line = cone_color;
                line.w = line_alpha;
                dl->AddPolyline(upper.data(), static_cast<int>(upper.size()),
                    ImGui::GetColorU32(line), ImDrawFlags_None, 1.0f);
                dl->AddPolyline(lower.data(), static_cast<int>(lower.size()),
                    ImGui::GetColorU32(line), ImDrawFlags_None, 1.0f);
            };
            draw_cone(2.0, 0.025f, 0.28f);
            draw_cone(1.0, 0.065f, 0.58f);
        }
    }
    draw_expiry_risk_fan(lim);
    plot_candles("##candles", bars, bar_s, theme.bull, theme.bear);
    if (ind.ema_fast && !ind.ema_fast_v.empty())
        plot_line("##ema-fast", ind.x, ind.ema_fast_v, theme.ema_fast, 1.6f);
    if (ind.ema_slow && !ind.ema_slow_v.empty())
        plot_line("##ema-slow", ind.x, ind.ema_slow_v, theme.ema_slow, 1.6f);
    if (ind.sma && !ind.sma_v.empty())
        plot_line("##sma", ind.x, ind.sma_v, theme.sma, 1.4f);
    if (ind.vwap && !ind.vwap_v.empty())
        plot_line("VWAP", ind.x, ind.vwap_v,
            ImVec4(0.86f, 0.45f, 0.93f, 1.0f), 1.5f);

    // Draw the last-price dashed line now. The right-edge tag is deferred
    // until after EndPlot(), when ImPlot has merged its item channels; drawing
    // it here lets the newest candle paint over the price text.
    if (!bars.empty()) {
        const Bar& lb = bars.back();
        const bool up = lb.c >= lb.o;
        const ImU32 col = ImGui::GetColorU32(up ? theme.bull : theme.bear);
        const ImVec2 p0 = ImPlot::PlotToPixels(lim.X.Min, lb.c);
        const ImVec2 p1 = ImPlot::PlotToPixels(lim.X.Max, lb.c);
        for (float px = p0.x; px < p1.x; px += 10.0f)
            dl->AddLine(ImVec2(px, p0.y),
                ImVec2(std::min(px + 5.0f, p1.x), p0.y), col, 1.0f);
        std::snprintf(last_price_tag_text_, sizeof last_price_tag_text_,
            "%.2f", lb.c);
        const ImVec2 plot_pos = ImPlot::GetPlotPos();
        const ImVec2 plot_size = ImPlot::GetPlotSize();
        last_price_tag_visible_ = true;
        last_price_tag_right_ = p1.x;
        last_price_tag_y_ = p0.y;
        last_price_tag_color_ = col;
        last_price_tag_clip_min_ = plot_pos;
        last_price_tag_clip_max_
            = ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y);
    }

    // The top-left readout: the hovered bar wins, else the latest.
    {
        const Bar* rb = bars.empty() ? nullptr : &bars.back();
        std::size_t ri = bars.empty() ? 0 : bars.size() - 1;
        if (ImPlot::IsPlotHovered()) {
            const ImPlotPoint mp = ImPlot::GetPlotMousePos();
            for (std::size_t i = 0; i < bars.size(); ++i)
                if (mp.x >= bars[i].t && mp.x < bars[i].t + bar_s) {
                    rb = &bars[i];
                    ri = i;
                    break;
                }
        }
        if (rb) {
            const ImVec2 pos = ImPlot::GetPlotPos();
            float ty = pos.y + 8.0f;
            const bool up = rb->c >= rb->o;
            char line1[160];
            std::snprintf(line1, sizeof line1,
                "O %.2f  H %.2f  L %.2f  C %.2f  %+.2f (%+.2f%%)", rb->o, rb->h,
                rb->l, rb->c, rb->c - rb->o,
                rb->o != 0 ? (rb->c - rb->o) / rb->o * 100 : 0);
            dl->AddText(ImVec2(pos.x + 10.0f, ty),
                ImGui::GetColorU32(up ? theme.bull : theme.bear), line1);
            ty += ImGui::GetTextLineHeight() + 2.0f;
            char line2[160];
            int off = 0;
            if (ind.ema_fast && ri < ind.ema_fast_v.size()
                && !std::isnan(ind.ema_fast_v[ri]))
                off += std::snprintf(line2 + off, sizeof line2 - off,
                    "EMA%d %.2f  ", ind.ema_fast_n, ind.ema_fast_v[ri]);
            if (ind.ema_slow && ri < ind.ema_slow_v.size()
                && !std::isnan(ind.ema_slow_v[ri]))
                off += std::snprintf(line2 + off, sizeof line2 - off,
                    "EMA%d %.2f  ", ind.ema_slow_n, ind.ema_slow_v[ri]);
            if (ind.boll && ri < ind.boll_mid.size()
                && !std::isnan(ind.boll_mid[ri]))
                off += std::snprintf(line2 + off, sizeof line2 - off,
                    "BOLL %.2f/%.2f/%.2f", ind.boll_up[ri], ind.boll_mid[ri],
                    ind.boll_dn[ri]);
            if (ind.vwap && ri < ind.vwap_v.size()
                && !std::isnan(ind.vwap_v[ri]))
                off += std::snprintf(line2 + off, sizeof line2 - off,
                    "  VWAP %.2f", ind.vwap_v[ri]);
            if (off > 0)
                dl->AddText(ImVec2(pos.x + 10.0f, ty),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), line2);
        }
    }

    if (research.available) {
        char buf[256];
        if (research_minimized_) {
            std::snprintf(buf, sizeof buf, "[+] RESEARCH %s",
                research.horizon.c_str());
        }
        else {
            std::snprintf(buf, sizeof buf,
                "[-] RESEARCH %s  Fair UP %.1f%%  Market %.1f%%  Edge %+.1fpp  "
                "Conf %.0f%%  EM +/-%.1fbp",
                research.horizon.c_str(), research.fair_probability_up * 100.0,
                research.market_probability_up * 100.0, research.edge_up * 100.0,
                research.confidence * 100.0, research.expected_move_bps);
        }
        const ImVec2 pos = ImPlot::GetPlotPos();
        const ImVec2 text_size = ImGui::CalcTextSize(buf);
        const ImVec2 plot_size = ImPlot::GetPlotSize();
        const ImVec2 p0(pos.x + plot_size.x - text_size.x - 24.0f,
            pos.y + 8.0f);
        const ImVec2 p1(
            p0.x + text_size.x + 16.0f, p0.y + text_size.y + 10.0f);
        ImVec4 overlay_background =
            ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        overlay_background.w = 0.94f;
        dl->AddRectFilled(
            p0, p1, ImGui::GetColorU32(overlay_background), 4.0f);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ref_color), 4.0f);
        dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 5.0f),
            ImGui::GetColorU32(ImGuiCol_Text), buf);
        if (ImGui::IsMouseHoveringRect(p0, p1)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            research_minimized_ = !research_minimized_;
    }

    // The marker layer — the caller refills `markers` every frame.
    if (!markers.empty()) {
        const Marker* hover = nullptr;
        const bool hovered = ImPlot::IsPlotHovered();
        const ImVec2 mouse = ImGui::GetMousePos();
        for (const Marker& m : markers) {
            if (m.t < lim.X.Min || m.t > lim.X.Max)
                continue;
            const ImVec2 p = ImPlot::PlotToPixels(m.t, m.y);
            const ImU32 col = ImGui::GetColorU32(m.color);
            const float r = m.size;
            switch (m.shape) {
            case ImPlotMarker_Up:
                dl->AddTriangleFilled(ImVec2(p.x, p.y - r),
                    ImVec2(p.x - r, p.y + r), ImVec2(p.x + r, p.y + r), col);
                break;
            case ImPlotMarker_Down:
                dl->AddTriangleFilled(ImVec2(p.x, p.y + r),
                    ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y - r), col);
                break;
            case ImPlotMarker_Square:
                dl->AddRectFilled(ImVec2(p.x - r * 0.8f, p.y - r * 0.8f),
                    ImVec2(p.x + r * 0.8f, p.y + r * 0.8f), col);
                break;
            case ImPlotMarker_Diamond:
                dl->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y),
                    ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), col);
                break;
            default:
                dl->AddCircleFilled(p, r * 0.8f, col);
            }
            if (hovered && std::fabs(mouse.x - p.x) < r + 3.0f
                && std::fabs(mouse.y - p.y) < r + 3.0f)
                hover = &m;
        }
        if (hover && hover->legend[0] != '\0') {
            ImGui::BeginTooltip();
            ImGui::TextColored(hover->color, "%s", hover->legend);
            char b[64];
            fmt_hms(hover->t, b, sizeof b, nullptr);
            ImGui::Text("%s  @ %.2f", b, hover->y);
            ImGui::EndTooltip();
        }
    }

    // The reference line: hairline across the pane, tagged on the
    // price axis.
    if (ref_price > 0) {
        ImPlotSpec rs;
        rs.LineColor = ref_color;
        rs.LineWeight = 1.0f;
        rs.Flags = static_cast<ImPlotItemFlags>(
            static_cast<int>(ImPlotInfLinesFlags_Horizontal)
            | static_cast<int>(ImPlotItemFlags_NoFit));
        ImPlot::PlotInfLines("##refpx", &ref_price, 1, rs);
        ImPlot::TagY(ref_price, ref_color, "PMT REF %.2f", ref_price);
    }
    draw_rtds_price_layer(lim);
    draw_leverage_regime_layer(lim);
    draw_expiry_risk_hud();
    draw_venue_race_layer();

    takeover_check(zoom.active);
    if (!follow_) {
        vx0_ = lim.X.Min;
        vx1_ = lim.X.Max;
        span_ = lim.X.Max - lim.X.Min;
    }
}

void Chart::draw_last_price_tag()
{
    if (!last_price_tag_visible_)
        return;
    const ImVec2 ts = ImGui::CalcTextSize(last_price_tag_text_);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(
        last_price_tag_clip_min_, last_price_tag_clip_max_, true);
    dl->AddRectFilled(
        ImVec2(last_price_tag_right_ - ts.x - 12.0f,
            last_price_tag_y_ - ts.y * 0.5f - 3.0f),
        ImVec2(last_price_tag_right_,
            last_price_tag_y_ + ts.y * 0.5f + 3.0f),
        last_price_tag_color_, 3.0f);
    dl->AddText(
        ImVec2(last_price_tag_right_ - ts.x - 6.0f,
            last_price_tag_y_ - ts.y * 0.5f),
        IM_COL32(255, 255, 255, 255), last_price_tag_text_);
    dl->PopClipRect();
}

void Chart::draw_volume_pane(
    const Series& s, const IndicatorSet&, bool bottom, bool switched)
{
    const WheelZoomIntent zoom = wheel_zoom_intent();
    const ImPlotAxisFlags xflags = interactive_x_flags(
        bottom ? ImPlotAxisFlags_NoLabel
               : (ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels),
        zoom);
    ImPlotAxisFlags yflags =
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite;
    if (follow_ && !zoom.active)
        yflags |= ImPlotAxisFlags_AutoFit;
    yflags = interactive_y_flags(yflags, zoom);
    ImPlot::SetupAxes(nullptr, nullptr, xflags, yflags);
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if (!zoom.active && (switched || snap_frames_ > 0)) {
        // A source switch can move the volume scale by orders of
        // magnitude, and AutoFit commits this frame but applies NEXT
        // frame — the switch frame would paint against the old scale.
        // Pinning to the visible extrema still left one residual jump,
        // because AutoFit's steady state is the FULL-data [0, vmax];
        // pin to exactly that and there is no jump at all, pixel for
        // pixel. The seamless-switch window gets the same treatment:
        // an in-place refill keeps the pointer, `switched` cannot see
        // it, and backfill lands in batches — each batch would flash
        // one AutoFit frame, so the window pins throughout.
        double vmax = 0;
        for (const Bar& b : s.bars())
            vmax = std::max(vmax, b.v);
        if (vmax > 0)
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, vmax, ImGuiCond_Always);
    }
    if (!zoom.active && (follow_ || manual_view_frames_ > 0))
        ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);
    plot_volume("##vol", s.bars(), s.bar_seconds(), theme.bull, theme.bear);
    takeover_check(zoom.active);
}

void Chart::draw_macd_pane(const Series& s, const IndicatorSet& ind)
{
    const WheelZoomIntent zoom = wheel_zoom_intent();
    const ImPlotAxisFlags xflags =
        interactive_x_flags(ImPlotAxisFlags_NoLabel, zoom);
    ImPlotAxisFlags yflags =
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite;
    if (follow_ && !zoom.active)
        yflags |= ImPlotAxisFlags_AutoFit;
    yflags = interactive_y_flags(yflags, zoom);
    ImPlot::SetupAxes(nullptr, nullptr, xflags, yflags);
    if (super_macd.enabled) {
        constexpr ImPlotAxisFlags super_flags =
            ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels
            | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoGridLines
            | ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_LockMin
            | ImPlotAxisFlags_LockMax;
        ImPlot::SetupAxis(ImAxis_Y2, nullptr, super_flags);
        ImPlot::SetupAxisLimits(ImAxis_Y2, -105.0, 105.0,
            ImGuiCond_Always);
    }
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if (!zoom.active && (follow_ || manual_view_frames_ > 0))
        ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);

    ImPlot::SetupFinish();
    if (research.window_end_t > research.window_start_t) {
        const ImPlotRect limits = ImPlot::GetPlotLimits();
        const double x0 = std::max(limits.X.Min, research.window_start_t);
        const double x1 = std::min(limits.X.Max, research.window_end_t);
        if (x1 > x0) {
            const ImVec2 plot_pos = ImPlot::GetPlotPos();
            const ImVec2 plot_size = ImPlot::GetPlotSize();
            const float px0 = ImPlot::PlotToPixels(x0, limits.Y.Min).x;
            const float px1 = ImPlot::PlotToPixels(x1, limits.Y.Min).x;
            ImVec4 fill = ref_color;
            fill.w = 0.10f;
            ImVec4 edge = ref_color;
            edge.w = 0.62f;
            ImDrawList* draw_list = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            draw_list->AddRectFilled(ImVec2(px0, plot_pos.y),
                ImVec2(px1, plot_pos.y + plot_size.y),
                ImGui::GetColorU32(fill));
            draw_list->AddLine(ImVec2(px0, plot_pos.y),
                ImVec2(px0, plot_pos.y + plot_size.y),
                ImGui::GetColorU32(edge), 1.0f);
            draw_list->AddLine(ImVec2(px1, plot_pos.y),
                ImVec2(px1, plot_pos.y + plot_size.y),
                ImGui::GetColorU32(edge), 1.0f);
            const int minutes = std::max(1,
                static_cast<int>(std::lround(
                    (research.window_end_t - research.window_start_t) / 60.0)));
            char label[48];
            std::snprintf(label, sizeof label, "CURRENT %dm · 1m BARS", minutes);
            const ImVec2 label_size = ImGui::CalcTextSize(label);
            if (px1 - px0 > label_size.x + 14.0f)
                draw_list->AddText(ImVec2(px0 + 7.0f, plot_pos.y + 5.0f),
                    ImGui::GetColorU32(edge), label);
            ImPlot::PopPlotClipRect();
        }
    }
    draw_macd_outcome_layer();
    plot_hist("##macd-hist", ind.x, ind.macd_hist, s.bar_seconds(), theme.bull,
        theme.bear);
    plot_line("##macd-dif", ind.x, ind.macd_dif, theme.macd_dif, 1.5f);
    plot_line("##macd-dea", ind.x, ind.macd_dea, theme.macd_dea, 1.5f);
    draw_super_macd_layer();
    draw_macd_energy_layer(s, ind);

    // The readout: DIF / DEA / HIST.
    if (!ind.macd_dif.empty()) {
        std::size_t ri = ind.x.size() - 1;
        if (ImPlot::IsPlotHovered()) {
            const ImPlotPoint mp = ImPlot::GetPlotMousePos();
            for (std::size_t i = 0; i < ind.x.size(); ++i)
                if (mp.x >= ind.x[i] && mp.x < ind.x[i] + s.bar_seconds()) {
                    ri = i;
                    break;
                }
        }
        if (ri < ind.macd_dif.size() && !std::isnan(ind.macd_dif[ri])) {
            char buf[128];
            std::snprintf(buf, sizeof buf,
                "MACD(%d,%d,%d)  DIF %.3f  DEA %.3f  HIST %.3f", ind.macd_fast,
                ind.macd_slow, ind.macd_signal, ind.macd_dif[ri],
                ind.macd_dea[ri], ind.macd_hist[ri]);
            const ImVec2 pos = ImPlot::GetPlotPos();
            ImPlot::GetPlotDrawList()->AddText(
                ImVec2(pos.x + 10.0f, pos.y + 6.0f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), buf);
        }
    }
    takeover_check(zoom.active);
}

namespace {

const char* composite_regime_label(CompositeMomentumRegime regime)
{
    switch (regime) {
    case CompositeMomentumRegime::Warming:
        return "WARMING";
    case CompositeMomentumRegime::Weak:
        return "WEAK";
    case CompositeMomentumRegime::Building:
        return "BUILDING";
    case CompositeMomentumRegime::Persistent:
        return "PERSISTENT";
    case CompositeMomentumRegime::Fading:
        return "FADING";
    case CompositeMomentumRegime::Choppy:
        return "CHOPPY";
    case CompositeMomentumRegime::Transitional:
        return "TRANSITION";
    case CompositeMomentumRegime::Unavailable:
        break;
    }
    return "N/A";
}

ImVec4 composite_regime_color(CompositeMomentumRegime regime,
    const Theme& theme)
{
    switch (regime) {
    case CompositeMomentumRegime::Building:
        return ImVec4(0.16f, 0.78f, 0.62f, 0.78f);
    case CompositeMomentumRegime::Persistent:
        return theme.bull;
    case CompositeMomentumRegime::Fading:
        return ImVec4(0.96f, 0.64f, 0.18f, 0.78f);
    case CompositeMomentumRegime::Choppy:
        return theme.bear;
    case CompositeMomentumRegime::Transitional:
        return ImVec4(0.58f, 0.48f, 0.90f, 0.72f);
    case CompositeMomentumRegime::Warming:
    case CompositeMomentumRegime::Weak:
    case CompositeMomentumRegime::Unavailable:
        return ImVec4(0.48f, 0.52f, 0.62f, 0.58f);
    }
    return ImVec4(0.48f, 0.52f, 0.62f, 0.58f);
}

} // namespace

void Chart::draw_super_macd_layer()
{
    if (!super_macd.enabled || super_macd.points.size() < 2)
        return;

    std::vector<SuperMacdMomentumPoint> points;
    points.reserve(super_macd.points.size());
    for (const auto& point : super_macd.points) {
        if (!std::isfinite(point.t) || !std::isfinite(point.score)
            || (!points.empty() && point.t <= points.back().t))
            continue;
        points.push_back(point);
    }
    if (points.size() < 2)
        return;

    std::vector<double> x;
    std::vector<double> raw;
    std::vector<double> peak;
    x.reserve(points.size());
    raw.reserve(points.size());
    peak.reserve(points.size());
    for (const auto& point : points) {
        const double score = std::clamp(point.score, -100.0, 100.0);
        x.push_back(point.t);
        raw.push_back(score);
        peak.push_back(std::clamp(
            score + point.direction
                    * point.drawdown_from_segment_peak_points,
            -100.0, 100.0));
    }
    const auto smooth = super_macd_causal_smoothing(
        points, super_macd.smoothing_time_constant_seconds);

    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
    const ImVec4 raw_color(0.70f, 0.50f, 0.96f, 0.48f);
    const ImVec4 smooth_color(0.54f, 0.82f, 1.00f, 0.88f);
    ImVec4 drawdown_color(0.96f, 0.60f, 0.20f, 0.12f);
    ImPlot::PlotShaded("##super-macd-drawdown", x.data(), raw.data(),
        peak.data(), static_cast<int>(x.size()),
        { ImPlotProp_FillColor, drawdown_color,
            ImPlotProp_Flags, ImPlotItemFlags_NoLegend });
    ImPlot::PlotLine("##super-macd-raw", x.data(), raw.data(),
        static_cast<int>(x.size()),
        { ImPlotProp_LineColor, raw_color, ImPlotProp_LineWeight, 1.0f,
            ImPlotProp_Flags, ImPlotItemFlags_NoLegend });
    ImPlot::PlotLine("##super-macd-smooth", x.data(), smooth.data(),
        static_cast<int>(x.size()),
        { ImPlotProp_LineColor, smooth_color, ImPlotProp_LineWeight, 1.8f,
            ImPlotProp_Flags, ImPlotItemFlags_NoLegend });

    std::vector<double> crossing_x;
    std::vector<double> crossing_y;
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (points[index].effective_zero_crossings_60s
                > points[index - 1].effective_zero_crossings_60s
            || (points[index].direction != 0
                && points[index - 1].direction != 0
                && points[index].direction != points[index - 1].direction)) {
            crossing_x.push_back(points[index].t);
            crossing_y.push_back(raw[index]);
        }
    }
    if (!crossing_x.empty()) {
        ImPlot::PlotScatter("##super-macd-cross", crossing_x.data(),
            crossing_y.data(), static_cast<int>(crossing_x.size()),
            { ImPlotProp_LineColor, ImVec4(0.98f, 0.76f, 0.24f, 0.95f),
                ImPlotProp_Marker, ImPlotMarker_Diamond,
                ImPlotProp_MarkerSize, 4.5f,
                ImPlotProp_MarkerFillColor,
                ImVec4(0.98f, 0.76f, 0.24f, 0.90f),
                ImPlotProp_Flags, ImPlotItemFlags_NoLegend });
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    draw_list->Flags |= ImDrawListFlags_AntiAliasedLines
        | ImDrawListFlags_AntiAliasedLinesUseTex;
    const ImPlotRect limits = ImPlot::GetPlotLimits(ImAxis_X1, ImAxis_Y2);
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const float band_top = plot_pos.y + plot_size.y - 5.0f;
    ImPlot::PushPlotClipRect();
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (points[index].t < limits.X.Min || points[index].t > limits.X.Max)
            continue;
        const float px = ImPlot::PlotToPixels(
            points[index].t, raw[index], ImAxis_X1, ImAxis_Y2).x;
        float next_px = px + 2.0f;
        if (index + 1 < points.size()) {
            next_px = ImPlot::PlotToPixels(points[index + 1].t,
                raw[index], ImAxis_X1, ImAxis_Y2).x;
        }
        ImVec4 regime = composite_regime_color(points[index].regime, theme);
        regime.w *= 0.58f;
        draw_list->AddRectFilled(ImVec2(px, band_top),
            ImVec2(std::max(px + 1.0f, next_px), band_top + 4.0f),
            ImGui::GetColorU32(regime));

        if (index == 0 || !points[index].available)
            continue;
        const double directed_velocity = points[index].direction
            * points[index].velocity_5s_points_per_second;
        const float height = static_cast<float>(std::clamp(
            std::abs(points[index].velocity_5s_points_per_second) * 2.8,
            1.0, 13.0));
        ImVec4 momentum = directed_velocity >= 0.0
            ? ImVec4(0.20f, 0.76f, 0.64f, 0.28f)
            : ImVec4(0.96f, 0.48f, 0.34f, 0.30f);
        draw_list->AddRectFilled(ImVec2(px - 0.8f, band_top - height),
            ImVec2(px + 0.8f, band_top), ImGui::GetColorU32(momentum));
    }
    ImPlot::PopPlotClipRect();

    const auto& latest = points.back();
    if (latest.t >= limits.X.Min && latest.t <= limits.X.Max) {
        char badge[128];
        const char* direction = latest.direction > 0
            ? "UP"
            : latest.direction < 0 ? "DOWN" : "WEAK";
        std::snprintf(badge, sizeof badge, "CMP %s %+.0f  %s  %.0fs  R%.0f%%",
            direction, latest.score, composite_regime_label(latest.regime),
            latest.same_direction_duration_seconds,
            latest.same_direction_residence_30s * 100.0);
        const ImVec2 size = ImGui::CalcTextSize(badge);
        const ImVec2 minimum(plot_pos.x + plot_size.x - size.x - 18.0f,
            plot_pos.y + 6.0f);
        const ImVec2 maximum(minimum.x + size.x + 12.0f,
            minimum.y + size.y + 6.0f);
        ImVec4 badge_color = latest.direction > 0 ? theme.bull
            : latest.direction < 0 ? theme.bear
                                   : ImVec4(0.55f, 0.60f, 0.70f, 0.90f);
        draw_list->AddRectFilled(minimum, maximum, IM_COL32(12, 16, 24, 205),
            4.0f);
        draw_list->AddRect(minimum, maximum, ImGui::GetColorU32(badge_color),
            4.0f, 0, 1.0f);
        draw_list->AddText(ImVec2(minimum.x + 6.0f, minimum.y + 3.0f),
            ImGui::GetColorU32(badge_color), badge);
    }

    if (ImPlot::IsPlotHovered()) {
        const double mouse_t = ImPlot::GetPlotMousePos(
            ImAxis_X1, ImAxis_Y2).x;
        const auto nearest = std::min_element(points.begin(), points.end(),
            [mouse_t](const auto& left, const auto& right) {
                return std::abs(left.t - mouse_t)
                    < std::abs(right.t - mouse_t);
            });
        if (nearest != points.end()) {
            ImGui::BeginTooltip();
            ImGui::Text("SUPER MACD COMPOSITE  %+.1f", nearest->score);
            ImGui::Separator();
            ImGui::Text("v5 %+.2f pt/s   residence30 %.0f%%",
                nearest->velocity_5s_points_per_second,
                nearest->same_direction_residence_30s * 100.0);
            ImGui::Text("duration %.1fs   drawdown %.1fpt   cross60 %u",
                nearest->same_direction_duration_seconds,
                nearest->drawdown_from_segment_peak_points,
                nearest->effective_zero_crossings_60s);
            ImGui::Text("regime %s", composite_regime_label(nearest->regime));
            ImGui::EndTooltip();
        }
    }
    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
}

void Chart::draw_macd_outcome_layer()
{
    if (!macd_outcomes.enabled || macd_outcomes.windows.empty())
        return;

    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    draw_list->Flags |= ImDrawListFlags_AntiAliasedLines
        | ImDrawListFlags_AntiAliasedLinesUseTex;

    ImPlot::PushPlotClipRect();
    for (const MacdWindowOutcome& outcome : macd_outcomes.windows) {
        if (outcome.window_end_t <= outcome.window_start_t)
            continue;
        const double x0 = std::max(limits.X.Min, outcome.window_start_t);
        const double x1 = std::min(limits.X.Max, outcome.window_end_t);
        if (x1 <= x0)
            continue;

        const float px0 = ImPlot::PlotToPixels(x0, limits.Y.Min).x;
        const float px1 = ImPlot::PlotToPixels(x1, limits.Y.Min).x;
        ImVec4 color = outcome.direction_up ? theme.bull : theme.bear;
        const float confidence = static_cast<float>(
            std::clamp(outcome.confidence, 0.0, 1.0));
        ImVec4 fill = color;
        fill.w = 0.035f + 0.025f * confidence;
        ImVec4 edge = color;
        edge.w = 0.30f + 0.25f * confidence;
        ImVec4 strip = color;
        strip.w = 0.72f;

        draw_list->AddRectFilled(ImVec2(px0, plot_pos.y),
            ImVec2(px1, plot_pos.y + plot_size.y), ImGui::GetColorU32(fill));
        draw_list->AddLine(ImVec2(px0, plot_pos.y),
            ImVec2(px0, plot_pos.y + plot_size.y), ImGui::GetColorU32(edge),
            1.0f);
        draw_list->AddRectFilled(ImVec2(px0, plot_pos.y + plot_size.y - 3.0f),
            ImVec2(px1, plot_pos.y + plot_size.y), ImGui::GetColorU32(strip));

        constexpr const char* up_label = "UP WIN";
        constexpr const char* down_label = "DN WIN";
        const char* label = outcome.direction_up ? up_label : down_label;
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        if (px1 - px0 >= text_size.x + 12.0f) {
            const ImVec2 label_min(
                px0 + (px1 - px0 - text_size.x) * 0.5f - 4.0f,
                plot_pos.y + plot_size.y - text_size.y - 9.0f);
            const ImVec2 label_max(label_min.x + text_size.x + 8.0f,
                label_min.y + text_size.y + 4.0f);
            draw_list->AddRectFilled(label_min, label_max,
                IM_COL32(12, 15, 22, 190), 3.0f);
            draw_list->AddText(ImVec2(label_min.x + 4.0f, label_min.y + 2.0f),
                ImGui::GetColorU32(color), label);
        }
    }
    ImPlot::PopPlotClipRect();
}

void Chart::draw_macd_energy_layer(const Series& s, const IndicatorSet& ind)
{
    if (!macd_energy.enabled
        || macd_energy.window_end_t <= macd_energy.window_start_t
        || ind.x.size() < 2 || ind.macd_hist.size() != ind.x.size())
        return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    draw_list->Flags |= ImDrawListFlags_AntiAliasedLines
        | ImDrawListFlags_AntiAliasedLinesUseTex;
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const double half_bar = s.bar_seconds() * 0.5;
    std::size_t visible_transitions = 0;
    std::size_t latest_index = 0;
    MacdEnergyTransition latest;

    ImPlot::PushPlotClipRect();
    for (std::size_t i = 1; i < ind.x.size(); ++i) {
        const double center = ind.x[i] + half_bar;
        if (center < macd_energy.window_start_t
            || center >= macd_energy.window_end_t
            || center < limits.X.Min || center > limits.X.Max)
            continue;
        const MacdEnergyTransition transition =
            classify_macd_energy(ind.macd_hist[i - 1], ind.macd_hist[i]);
        if (!transition.available())
            continue;

        ++visible_transitions;
        latest = transition;
        latest_index = i;
        const bool bullish = ind.macd_hist[i] >= 0.0;
        const bool expanding =
            transition.state == MacdEnergyState::BullExpanding
            || transition.state == MacdEnergyState::BearExpanding;
        const bool crossing = transition.state == MacdEnergyState::CrossUp
            || transition.state == MacdEnergyState::CrossDown;
        ImVec4 color = bullish ? theme.bull : theme.bear;
        if (!expanding && !crossing)
            color = ImVec4(0.96f, 0.70f, 0.18f, 0.90f);
        const ImU32 packed = ImGui::GetColorU32(color);
        const ImVec2 point = ImPlot::PlotToPixels(center, ind.macd_hist[i]);
        constexpr float radius = 4.0f;

        if (crossing) {
            const ImVec2 diamond[] = {
                ImVec2(point.x, point.y - radius - 1.0f),
                ImVec2(point.x + radius + 1.0f, point.y),
                ImVec2(point.x, point.y + radius + 1.0f),
                ImVec2(point.x - radius - 1.0f, point.y),
            };
            draw_list->AddConvexPolyFilled(diamond, 4, packed);
        } else if (transition.state == MacdEnergyState::FlatBull
                   || transition.state == MacdEnergyState::FlatBear) {
            draw_list->AddCircleFilled(point, radius, packed, 12);
        } else {
            const bool points_up = bullish == expanding;
            const float direction = points_up ? -1.0f : 1.0f;
            const ImVec2 triangle[] = {
                ImVec2(point.x, point.y + direction * (radius + 2.0f)),
                ImVec2(point.x - radius, point.y - direction * radius),
                ImVec2(point.x + radius, point.y - direction * radius),
            };
            draw_list->AddConvexPolyFilled(triangle, 3, packed);
        }
    }
    ImPlot::PopPlotClipRect();

    if (!latest.available() || latest_index >= ind.macd_hist.size())
        return;
    const double magnitude_change_percent =
        std::clamp(latest.magnitude_change * 100.0, -999.0, 999.0);
    const MacdBarCountdown countdown =
        macd_bar_countdown(macd_energy.current_time_t, 60.0);
    const float ui_font_size = ImGui::GetFontSize();
    const float hud_top_margin = std::max(6.0f, ui_font_size * 0.24f);
    const float hud_right_margin = std::max(10.0f, ui_font_size * 0.40f);
    const ImVec4 color =
        ind.macd_hist[latest_index] >= 0.0 ? theme.bull : theme.bear;
    char prefix[48];
    char suffix[112];
    char countdown_text[24] {};
    const char* countdown_widest = "NEXT 60s";
    std::snprintf(prefix, sizeof prefix, "1m ENERGY %zu  ", visible_transitions);
    std::snprintf(suffix, sizeof suffix, "  %s  H %+.3f  |H| %+.1f%%",
        macd_energy_state_label(latest.state), ind.macd_hist[latest_index],
        magnitude_change_percent);
    if (countdown.available())
        std::snprintf(countdown_text, sizeof countdown_text, "NEXT %ds",
            countdown.display_seconds);

    ImVec2 prefix_size = ImGui::CalcTextSize(prefix);
    ImVec2 suffix_size = ImGui::CalcTextSize(suffix);
    float countdown_width = countdown.available()
        ? ImGui::CalcTextSize(countdown_widest).x : 0.0f;
    const float available_width = std::max(0.0f,
        plot_size.x - hud_right_margin - 20.0f);
    float total_width = prefix_size.x + countdown_width + suffix_size.x;

    // Keep the countdown in a fixed-width slot so changing seconds never
    // shifts the energy state. Narrow panes use a compact label and preserve
    // the state/value before secondary magnitude details.
    if (total_width > available_width) {
        std::snprintf(prefix, sizeof prefix, "E%zu  ", visible_transitions);
        std::snprintf(suffix, sizeof suffix, "  %s  H %+.2f",
            macd_energy_state_label(latest.state), ind.macd_hist[latest_index]);
        if (countdown.available())
            std::snprintf(countdown_text, sizeof countdown_text, "N %ds",
                countdown.display_seconds);
        countdown_widest = "N 60s";
        prefix_size = ImGui::CalcTextSize(prefix);
        suffix_size = ImGui::CalcTextSize(suffix);
        countdown_width = countdown.available()
            ? ImGui::CalcTextSize(countdown_widest).x : 0.0f;
        total_width = prefix_size.x + countdown_width + suffix_size.x;
    }

    const ImVec2 origin(
        std::max(plot_pos.x + 10.0f,
            plot_pos.x + plot_size.x - total_width - hud_right_margin),
        plot_pos.y + hud_top_margin);
    const ImU32 energy_color = ImGui::GetColorU32(color);
    draw_list->AddText(origin, energy_color, prefix);

    const float countdown_x = origin.x + prefix_size.x;
    if (countdown.available()) {
        ImVec4 countdown_color(0.55f, 0.64f, 0.76f, 0.94f);
        if (countdown.display_seconds <= 3)
            countdown_color = ImVec4(1.00f, 0.84f, 0.30f, 1.00f);
        else if (countdown.display_seconds <= 10)
            countdown_color = ImVec4(0.98f, 0.70f, 0.16f, 0.98f);
        const ImVec2 countdown_size = ImGui::CalcTextSize(countdown_text);
        const float text_x = countdown_x
            + std::max(0.0f, (countdown_width - countdown_size.x) * 0.5f);
        draw_list->AddText(ImVec2(text_x, origin.y),
            ImGui::GetColorU32(countdown_color), countdown_text);

        const ImVec4 axis_grid = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
        const float underline_y = origin.y + ui_font_size + 2.0f;
        const float progress_x = countdown_x + countdown_width
            * static_cast<float>(countdown.elapsed_fraction);
        draw_list->AddLine(ImVec2(countdown_x, underline_y),
            ImVec2(countdown_x + countdown_width, underline_y),
            ImGui::GetColorU32(
                ImVec4(axis_grid.x, axis_grid.y, axis_grid.z, 0.62f)), 1.0f);
        draw_list->AddLine(ImVec2(countdown_x, underline_y),
            ImVec2(progress_x, underline_y),
            ImGui::GetColorU32(countdown_color), 1.8f);
    }
    draw_list->AddText(ImVec2(countdown_x + countdown_width, origin.y),
        energy_color, suffix);
}

void Chart::draw_rsi_pane(const Series& s, const IndicatorSet& ind)
{
    const WheelZoomIntent zoom = wheel_zoom_intent();
    const ImPlotAxisFlags xflags =
        interactive_x_flags(ImPlotAxisFlags_NoLabel, zoom);
    const ImPlotAxisFlags yflags = interactive_y_flags(
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite, zoom);
    ImPlot::SetupAxes(nullptr, nullptr, xflags, yflags);
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if (follow_ && !zoom.active)
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImGuiCond_Always);
    if (!zoom.active && (follow_ || manual_view_frames_ > 0))
        ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);
    plot_line("RSI", ind.x, ind.rsi_v,
        ImVec4(0.73f, 0.41f, 0.78f, 1.0f), 1.5f);
    const double guides[] = { 30, 50, 70 };
    ImPlotSpec spec;
    spec.LineColor = ImVec4(0.55f, 0.58f, 0.66f, 0.45f);
    spec.LineWeight = 1.0f;
    spec.Flags = ImPlotInfLinesFlags_Horizontal;
    ImPlot::PlotInfLines("##rsi-guides", guides, 3, spec);
    takeover_check(zoom.active);
}

void Chart::draw_atr_pane(const Series& s, const IndicatorSet& ind)
{
    const WheelZoomIntent zoom = wheel_zoom_intent();
    const ImPlotAxisFlags xflags =
        interactive_x_flags(ImPlotAxisFlags_NoLabel, zoom);
    ImPlotAxisFlags yflags =
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite;
    if (follow_ && !zoom.active)
        yflags |= ImPlotAxisFlags_AutoFit;
    yflags = interactive_y_flags(yflags, zoom);
    ImPlot::SetupAxes(nullptr, nullptr, xflags, yflags);
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if (!zoom.active && (follow_ || manual_view_frames_ > 0))
        ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);
    plot_line("ATR", ind.x, ind.atr_v,
        ImVec4(0.95f, 0.65f, 0.20f, 1.0f), 1.5f);
    takeover_check(zoom.active);
}

void Chart::draw_auxiliary_pane(const Series&, const AuxiliaryPane& pane)
{
    const WheelZoomIntent zoom = wheel_zoom_intent();
    ImPlotAxisFlags yflags
        = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite;
    if (follow_ && !zoom.active && !(pane.y_max > pane.y_min))
        yflags |= ImPlotAxisFlags_AutoFit;
    yflags = interactive_y_flags(yflags, zoom);
    const ImPlotAxisFlags xflags =
        interactive_x_flags(ImPlotAxisFlags_NoLabel, zoom);
    ImPlot::SetupAxes(nullptr, nullptr, xflags, yflags);
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if (follow_ && !zoom.active && pane.y_max > pane.y_min)
        ImPlot::SetupAxisLimits(
            ImAxis_Y1, pane.y_min, pane.y_max, ImGuiCond_Always);
    if (!zoom.active && (follow_ || manual_view_frames_ > 0))
        ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);
    for (const auto& line : pane.lines)
        plot_line(line.label.c_str(), line.x, line.y, line.color, 1.5f);
    if (!pane.guides.empty()) {
        ImPlotSpec spec;
        spec.LineColor = ImVec4(0.55f, 0.58f, 0.66f, 0.45f);
        spec.LineWeight = 1.0f;
        spec.Flags = ImPlotInfLinesFlags_Horizontal;
        ImPlot::PlotInfLines("##guides", pane.guides.data(),
            static_cast<int>(pane.guides.size()), spec);
    }
    const ImVec2 pos = ImPlot::GetPlotPos();
    ImPlot::GetPlotDrawList()->AddText(
        ImVec2(pos.x + 10.0f, pos.y + 6.0f),
        ImGui::GetColorU32(ImGuiCol_TextDisabled), pane.title.c_str());
    takeover_check(zoom.active);
}

void Chart::draw(
    const char* id, const Series& series, IndicatorSet& ind, const ImVec2& size)
{
    const bool switched = last_series_ != nullptr && last_series_ != &series;
    last_series_ = &series;
    last_price_tag_visible_ = false;
    ind.compute(series.bars());
    update_view(series);

    if (!follow_) {
        if (ImGui::SmallButton(
                (std::string(follow_label) + " »##" + id).c_str())) {
            follow_ = true;
            vx1_ = vx0_; // snap next frame
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", paused_note);
    }

    // Pane count and ratios — the TV default layout.
    const auto pane_available = [&](FocusedPane pane) {
        switch (pane) {
        case FocusedPane::All:
        case FocusedPane::Main:
            return true;
        case FocusedPane::Volume:
            return ind.volume;
        case FocusedPane::Macd:
            return ind.macd;
        case FocusedPane::Rsi:
            return ind.rsi;
        case FocusedPane::Atr:
            return ind.atr;
        case FocusedPane::Auxiliary:
            return auxiliary && !auxiliary->lines.empty();
        }
        return false;
    };
    const auto is_indicator_pane = [](FocusedPane pane) {
        return pane == FocusedPane::Macd || pane == FocusedPane::Rsi
            || pane == FocusedPane::Atr || pane == FocusedPane::Auxiliary;
    };
    const FocusedPane active_indicator_pane = ind.macd   ? FocusedPane::Macd
        : ind.rsi                                          ? FocusedPane::Rsi
        : ind.atr                                          ? FocusedPane::Atr
        : auxiliary && !auxiliary->lines.empty()           ? FocusedPane::Auxiliary
                                                           : FocusedPane::All;
    const bool indicator_changed = active_indicator_pane != FocusedPane::All
        && last_indicator_pane_ != FocusedPane::All
        && active_indicator_pane != last_indicator_pane_;
    if (indicator_changed) {
        // MACD/RSI/ATR/auxiliary are different contents of one linked pane
        // slot. Replacing the content must not leave focus mode or discard the
        // shared manual X viewport.
        if (is_indicator_pane(focused_pane_))
            focused_pane_ = active_indicator_pane;
        manual_view_frames_ = std::max(manual_view_frames_, 3);
    }
    if (active_indicator_pane != FocusedPane::All)
        last_indicator_pane_ = active_indicator_pane;
    if (!pane_available(focused_pane_) && !is_indicator_pane(focused_pane_))
        focused_pane_ = FocusedPane::All;
    const FocusedPane frame_focus = focused_pane_;
    focused_pane_plot_bounds_valid_ = false;
    const auto visible = [&](FocusedPane pane) {
        return frame_focus == FocusedPane::All || frame_focus == pane;
    };
    const auto capture_focused_plot_bounds = [&](FocusedPane pane) {
        if (frame_focus != pane)
            return;
        const ImVec2 minimum = ImPlot::GetPlotPos();
        const ImVec2 size = ImPlot::GetPlotSize();
        if (size.x <= 0.0f || size.y <= 0.0f)
            return;
        focused_pane_plot_min_ = minimum;
        focused_pane_plot_max_ = ImVec2(minimum.x + size.x, minimum.y + size.y);
        focused_pane_plot_bounds_valid_ = true;
    };
    const auto focus_on_double_click = [&](FocusedPane pane) {
        // ImPlot reserves plain left double-click for fit/reset. Its default
        // OverrideMod is Ctrl, so Ctrl+double-click is ignored by ImPlot and
        // can safely toggle pane focus without also changing the plot range.
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl
            && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // Changing the subplot row count resets ImPlot's shared-link
            // storage to [0,1]. Preserve the real timestamp viewport and
            // explicitly restore it for the layout-change frames.
            const ImPlotRect limits = ImPlot::GetPlotLimits();
            vx0_ = limits.X.Min;
            vx1_ = limits.X.Max;
            span_ = std::max(bar_seconds_, vx1_ - vx0_);
            manual_view_frames_ = 3;
            focused_pane_ = frame_focus == pane ? FocusedPane::All : pane;
        }
    };

    const int indicator_rows = (ind.volume ? 1 : 0) + (ind.macd ? 1 : 0)
        + (ind.rsi ? 1 : 0) + (ind.atr ? 1 : 0)
        + (auxiliary && !auxiliary->lines.empty() ? 1 : 0);
    const int rows = frame_focus == FocusedPane::All ? 1 + indicator_rows : 1;
    std::vector<float> ratios_buf(static_cast<std::size_t>(rows),
        indicator_rows == 0 ? 1.0f : 0.18f);
    if (indicator_rows > 0)
        ratios_buf[0] = std::max(0.46f, 1.0f - indicator_rows * 0.18f);

    const ImPlotSubplotFlags sflags = ImPlotSubplotFlags_NoTitle
        | ImPlotSubplotFlags_NoMenus
        | (rows > 1 ? ImPlotSubplotFlags_LinkAllX : ImPlotSubplotFlags_None);
    if (ImPlot::BeginSubplots(
            id, rows, 1, size, sflags, ratios_buf.data())) {
        const ImPlotFlags pflags = ImPlotFlags_Crosshairs | ImPlotFlags_NoLegend
            | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMenus
            | ImPlotFlags_NoTitle;
        if (visible(FocusedPane::Main)
            && ImPlot::BeginPlot("##main", ImVec2(-1, 0), pflags)) {
            draw_main_pane(series, ind, rows == 1);
            capture_focused_plot_bounds(FocusedPane::Main);
            focus_on_double_click(FocusedPane::Main);
            ImPlot::EndPlot();
            draw_last_price_tag();
        }
        if (visible(FocusedPane::Volume) && ind.volume
            && ImPlot::BeginPlot("##volume", ImVec2(-1, 0), pflags)) {
            draw_volume_pane(series, ind,
                !ind.macd && !ind.rsi && !ind.atr
                    && (!auxiliary || auxiliary->lines.empty()),
                switched);
            capture_focused_plot_bounds(FocusedPane::Volume);
            focus_on_double_click(FocusedPane::Volume);
            ImPlot::EndPlot();
        }
        if (visible(FocusedPane::Macd) && ind.macd
            && ImPlot::BeginPlot("##macd", ImVec2(-1, 0), pflags)) {
            draw_macd_pane(series, ind);
            capture_focused_plot_bounds(FocusedPane::Macd);
            focus_on_double_click(FocusedPane::Macd);
            ImPlot::EndPlot();
        }
        if (visible(FocusedPane::Rsi) && ind.rsi
            && ImPlot::BeginPlot("##rsi", ImVec2(-1, 0), pflags)) {
            draw_rsi_pane(series, ind);
            capture_focused_plot_bounds(FocusedPane::Rsi);
            focus_on_double_click(FocusedPane::Rsi);
            ImPlot::EndPlot();
        }
        if (visible(FocusedPane::Atr) && ind.atr
            && ImPlot::BeginPlot("##atr", ImVec2(-1, 0), pflags)) {
            draw_atr_pane(series, ind);
            capture_focused_plot_bounds(FocusedPane::Atr);
            focus_on_double_click(FocusedPane::Atr);
            ImPlot::EndPlot();
        }
        if (visible(FocusedPane::Auxiliary) && auxiliary && !auxiliary->lines.empty()
            && ImPlot::BeginPlot("##auxiliary", ImVec2(-1, 0), pflags)) {
            draw_auxiliary_pane(series, *auxiliary);
            capture_focused_plot_bounds(FocusedPane::Auxiliary);
            focus_on_double_click(FocusedPane::Auxiliary);
            ImPlot::EndPlot();
        }
        ImPlot::EndSubplots();
    }
    if (manual_view_frames_ > 0)
        --manual_view_frames_;
}

}
