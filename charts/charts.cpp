#include "charts/charts.hpp"

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
        ImPlotSpec spec;
        spec.LineColor = col;
        spec.LineWeight = weight;
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

void Chart::takeover_check()
{
    if (ImPlot::IsPlotHovered()
        && (ImGui::IsMouseDragging(ImGuiMouseButton_Left)
            || ImGui::GetIO().MouseWheel != 0.0f)) {
        follow_ = false;
    }
}

void Chart::update_view(const Series& s)
{
    const auto& bars = s.bars();
    if (bars.empty())
        return;
    const double bar_s = s.bar_seconds();
    if (span_ <= 0)
        span_ = bar_s * 120; // 120 bars per screen by default
    const float dt = ImGui::GetIO().DeltaTime;
    // Inside the seamless-switch window every easing teleports; the
    // frame counter burns here, exactly once per frame.
    if (snap_frames_ > 0)
        --snap_frames_;
    const double alpha
        = snap_frames_ > 0 ? 1.0 : 1.0 - std::exp(-double(dt) * 12.0);
    const double tx1 = bars.back().t + bar_s * 6.0; // TV's right margin
    double tx0 = tx1 - span_;
    const double first = bars.front().t;
    if (tx0 < first)
        tx0 = first; // less than a screen of data leaves no dead space
    if (vx1_ <= vx0_) {
        vx0_ = tx0;
        vx1_ = tx1;
    }
    vx0_ += (tx0 - vx0_) * alpha;
    vx1_ += (tx1 - vx1_) * alpha;
}

void Chart::draw_main_pane(
    const Series& s, const IndicatorSet& ind, bool bottom)
{
    const auto& bars = s.bars();
    const double bar_s = s.bar_seconds();
    ImPlot::SetupAxes(nullptr, nullptr,
        bottom ? ImPlotAxisFlags_NoLabel
               : (ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels),
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite);
    // The formatter rides every pane: NoTickLabels suppresses the tick
    // text but the crosshair readout still goes through the formatter
    // — hovering shows HH:MM:SS, not raw seconds.
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if (follow_ && !bars.empty()) {
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
        ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, vy0_, vy1_, ImGuiCond_Always);
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
    plot_candles("##candles", bars, bar_s, theme.bull, theme.bear);
    if (ind.ema_fast && !ind.ema_fast_v.empty())
        plot_line("##ema-fast", ind.x, ind.ema_fast_v, theme.ema_fast, 1.6f);
    if (ind.ema_slow && !ind.ema_slow_v.empty())
        plot_line("##ema-slow", ind.x, ind.ema_slow_v, theme.ema_slow, 1.6f);
    if (ind.sma && !ind.sma_v.empty())
        plot_line("##sma", ind.x, ind.sma_v, theme.sma, 1.4f);

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImPlotRect lim = ImPlot::GetPlotLimits();

    // The last-price dashed line and its right-edge tag.
    if (!bars.empty()) {
        const Bar& lb = bars.back();
        const bool up = lb.c >= lb.o;
        const ImU32 col = ImGui::GetColorU32(up ? theme.bull : theme.bear);
        const ImVec2 p0 = ImPlot::PlotToPixels(lim.X.Min, lb.c);
        const ImVec2 p1 = ImPlot::PlotToPixels(lim.X.Max, lb.c);
        for (float px = p0.x; px < p1.x; px += 10.0f)
            dl->AddLine(ImVec2(px, p0.y),
                ImVec2(std::min(px + 5.0f, p1.x), p0.y), col, 1.0f);
        char tag[32];
        std::snprintf(tag, sizeof tag, "%.2f", lb.c);
        const ImVec2 ts = ImGui::CalcTextSize(tag);
        dl->AddRectFilled(
            ImVec2(p1.x - ts.x - 12.0f, p0.y - ts.y * 0.5f - 3.0f),
            ImVec2(p1.x, p0.y + ts.y * 0.5f + 3.0f), col, 3.0f);
        dl->AddText(ImVec2(p1.x - ts.x - 6.0f, p0.y - ts.y * 0.5f),
            IM_COL32(255, 255, 255, 255), tag);
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
            if (off > 0)
                dl->AddText(ImVec2(pos.x + 10.0f, ty),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), line2);
        }
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
        rs.Flags = ImPlotInfLinesFlags_Horizontal;
        ImPlot::PlotInfLines("##refpx", &ref_price, 1, rs);
        ImPlot::TagY(ref_price, ref_color, "%.2f", ref_price);
    }

    takeover_check();
    if (!follow_)
        span_ = lim.X.Max - lim.X.Min;
}

void Chart::draw_volume_pane(
    const Series& s, const IndicatorSet&, bool bottom, bool switched)
{
    ImPlot::SetupAxes(nullptr, nullptr,
        bottom ? ImPlotAxisFlags_NoLabel
               : (ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels),
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite
            | ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if (switched || snap_frames_ > 0) {
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
    if (follow_)
        ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);
    plot_volume("##vol", s.bars(), s.bar_seconds(), theme.bull, theme.bear);
    takeover_check();
}

void Chart::draw_macd_pane(const Series& s, const IndicatorSet& ind)
{
    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoLabel,
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite
            | ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisFormat(ImAxis_X1, fmt_hms, nullptr);
    if (follow_)
        ImPlot::SetupAxisLimits(ImAxis_X1, vx0_, vx1_, ImGuiCond_Always);
    plot_hist("##macd-hist", ind.x, ind.macd_hist, s.bar_seconds(), theme.bull,
        theme.bear);
    plot_line("##macd-dif", ind.x, ind.macd_dif, theme.macd_dif, 1.5f);
    plot_line("##macd-dea", ind.x, ind.macd_dea, theme.macd_dea, 1.5f);

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
    takeover_check();
}

void Chart::draw(
    const char* id, const Series& series, IndicatorSet& ind, const ImVec2& size)
{
    const bool switched = last_series_ != nullptr && last_series_ != &series;
    last_series_ = &series;
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
    int rows = 1;
    float ratios_buf[3] = { 1.0f, 0, 0 };
    if (ind.volume && ind.macd) {
        rows = 3;
        ratios_buf[0] = 0.60f;
        ratios_buf[1] = 0.14f;
        ratios_buf[2] = 0.26f;
    } else if (ind.volume || ind.macd) {
        rows = 2;
        ratios_buf[0] = 0.72f;
        ratios_buf[1] = 0.28f;
    }

    const ImPlotSubplotFlags sflags = ImPlotSubplotFlags_LinkAllX
        | ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_NoResize
        | ImPlotSubplotFlags_NoMenus;
    if (ImPlot::BeginSubplots(id, rows, 1, size, sflags, ratios_buf)) {
        const ImPlotFlags pflags = ImPlotFlags_Crosshairs | ImPlotFlags_NoLegend
            | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMenus
            | ImPlotFlags_NoTitle;
        if (ImPlot::BeginPlot("##main", ImVec2(-1, 0), pflags)) {
            draw_main_pane(series, ind, rows == 1);
            ImPlot::EndPlot();
        }
        if (ind.volume
            && ImPlot::BeginPlot("##volume", ImVec2(-1, 0), pflags)) {
            draw_volume_pane(series, ind, !ind.macd, switched);
            ImPlot::EndPlot();
        }
        if (ind.macd && ImPlot::BeginPlot("##macd", ImVec2(-1, 0), pflags)) {
            draw_macd_pane(series, ind);
            ImPlot::EndPlot();
        }
        ImPlot::EndSubplots();
    }
}

}
