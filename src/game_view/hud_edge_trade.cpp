// The trade window of the Edge style (AL-75, sim/trade.hpp): the goods of both
// sides with what each is worth to the other person and whether they like it,
// the draft the player puts together (a click adds a thing, a right click takes
// it back), the other side's own weighing of the draft before it is offered
// (will agree / will haggle / will refuse and why), a counter-offer to take,
// and the last words of the deal. The offer itself goes as the sim command
// "trade": the person walks over, says it aloud, the other answers aloud.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"

namespace lr::game {
namespace {
using ui::Colour;
using ui::Rect;
using namespace hudroles;

std::string coinsText(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

std::string joinList(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}
} // namespace

void Hud::tradePanel(const Snapshot& s, float W, float H) {
    const ActorSnap* t = actor(s, wheelTarget_);
    if (!t || t->animal) {
        panel = "none";
        return;
    }
    host_->wheelTarget(t->id);
    const TradeSnap& ts = s.trade;
    const bool ready = ts.with == t->id;
    const bool canAct = s.controlled != sim::kNone && s.controlled != t->id;
    // the draft goes to the sim thread, which weighs it the other side's way
    {
        sim::Json give = sim::Json::object(), take = sim::Json::object();
        for (const auto& [k, n] : tradeGive_)
            if (n > 0) give[k] = n;
        for (const auto& [k, n] : tradeTake_)
            if (n > 0) take[k] = n;
        const sim::Json draft = {{"target", t->id}, {"give", give}, {"take", take}};
        const std::string sig = draft.dump();
        if (sig != tradeSig_) {
            tradeSig_ = sig;
            host_->simOp("trade_draft", draft);
        }
    }
    dimmer(W, H, 0.5f);
    const float w = std::min(920.f, W - 60), h = std::min(640.f, H - 60);
    const Rect P{(W - w) * 0.5f, std::max(30.f, (H - h) * 0.5f), w, h};
    glass(P, true);
    seal(P.right() - 30, P.y + 30);
    const float d = 56, hh = d * 0.7071f;
    diamond(P.x + 28 + hh, P.y + 26 + hh, d, false, true);
    portrait(*t, P.x + 28 + hh, P.y + 26 + hh, hh - 6);
    const float tx = P.x + 28 + hh * 2 + 18;
    caps(tx, P.y + 22, host_->text("hud.trade.title", {{"who", t->name}}), kGold, 20, 0.12f);
    if (ready && !ts.role.empty()) label(tx, P.y + 52, ts.role, kGoldPale, 14);
    float ly = P.y + 72;
    if (ready && !ts.likes.empty())
        label(tx, ly, ui_->fit(host_->text("hud.trade.likes", {{"list", joinList(ts.likes)}}), w - (tx - P.x) - 60, 13 / 15.f), kGood, 13), ly += 18;
    if (ready && !ts.dislikes.empty())
        label(tx, ly, ui_->fit(host_->text("hud.trade.dislikes", {{"list", joinList(ts.dislikes)}}), w - (tx - P.x) - 60, 13 / 15.f), kBad, 13);
    const float top = P.y + 118;
    ui_->line(P.x + 24, top, P.right() - 24, top, kGold.withAlpha(0.35f));
    // two columns of goods: a click adds one to the draft, a right click takes one back
    const float colW = (w - 72) * 0.5f, rowH = 30;
    const float listH = h - 118 - 230;
    auto column = [&](float x, const std::string& head, const std::vector<TradeGoodSnap>& goods,
                      std::map<std::string, int>& draft) {
        caps(x, top + 10, head, kGold, 14, 0.12f);
        float y = top + 36;
        const int maxRows = std::max(1, int(listH / rowH));
        int shown = 0;
        for (const auto& g : goods) {
            if (shown++ >= maxRows) break;
            const Rect row{x, y, colW, rowH - 2};
            const bool hot = canAct && ui_->hovered(row);
            if (hot) selRow(row);
            const int in = draft.count(g.item) ? draft[g.item] : 0;
            const std::string nm = g.name + " ×" + std::to_string(g.n) + (in > 0 ? "  [" + std::to_string(in) + "]" : "");
            label(row.x + 10, row.y + 5, ui_->fit(nm, colW - 120, 14 / 15.f), in > 0 ? kGoldPale : kText, 14);
            const std::string wv = "≈" + coinsText(g.worth);
            label(row.right() - 64 - textW(wv, 13), row.y + 6, wv, kMute, 13);
            if (g.taste != 0) label(row.right() - 40, row.y + 5, g.taste > 0 ? "♥" : "✗", g.taste > 0 ? kGood : kBad, 14);
            if (canAct && clicked(row) && in < g.n) draft[g.item] = in + 1;
            if (canAct && hot && in_->rightPressed && in > 0) {
                if (in - 1 <= 0) draft.erase(g.item);
                else draft[g.item] = in - 1;
                in_->rightPressed = false;
            }
            y += rowH;
        }
    };
    if (ready) {
        column(P.x + 24, host_->text("hud.trade.mine"), ts.mine, tradeGive_);
        column(P.x + 48 + colW, host_->text("hud.trade.theirs"), ts.theirs, tradeTake_);
    }
    // the draft as the other side weighs it
    float y = P.bottom() - 226;
    ui_->line(P.x + 24, y, P.right() - 24, y, kGold.withAlpha(0.25f));
    y += 10;
    auto goodsLine = [&](const std::map<std::string, int>& g, const std::vector<TradeGoodSnap>& names) {
        std::string out;
        for (const auto& [k, n] : g) {
            std::string nm = k;
            for (const auto& x : names)
                if (x.item == k) nm = x.name;
            out += (out.empty() ? "" : ", ") + nm + " ×" + std::to_string(n);
        }
        return out.empty() ? std::string("—") : out;
    };
    label(P.x + 28, y, ui_->fit(host_->text("hud.trade.you_give") + ": " + goodsLine(tradeGive_, ts.mine), w - 56, 14 / 15.f), kText, 14);
    y += 22;
    label(P.x + 28, y, ui_->fit(host_->text("hud.trade.you_get") + ": " + goodsLine(tradeTake_, ts.theirs), w - 56, 14 / 15.f), kText, 14);
    y += 24;
    if (ready && !ts.verdict.empty()) {
        std::string v = host_->text("hud.trade.verdict." + ts.verdict, {{"why", ts.reason}});
        if (ts.loss > 0)
            v = host_->text("hud.trade.worth", {{"gain", coinsText(ts.gain)}, {"loss", coinsText(ts.loss)}, {"keep", coinsText(ts.keep)}}) +
                " — " + v;
        label(P.x + 28, y, ui_->fit(v, w - 56, 14 / 15.f), ts.verdict == "accept" ? kGood : ts.verdict == "counter" ? kGoldPale : kBad, 14);
    }
    y += 24;
    if (ready && ts.counter) {
        label(P.x + 28, y + 6, ui_->fit(ts.counterText, w - 300, 14 / 15.f), kGoldPale, 14);
        if (edgeButton({P.right() - 250, y, 220, 32}, host_->text("hud.trade.accept"), true, canAct))
            host_->command("trade_accept", {{"id", s.controlled}, {"target", t->id}});
    }
    y += 40;
    // what came of it: the last lines of the deal
    if (ready)
        for (const auto& l : ts.last) {
            label(P.x + 28, y, ui_->fit(l, w - 56, 12 / 15.f), kMute, 12);
            y += 16;
        }
    if (!canAct) label(P.x + 28, P.bottom() - 84, host_->text("hud.trade.need_control"), kBad, 13);
    const float by = P.bottom() - 52, bw = 200;
    const bool any = !tradeGive_.empty() || !tradeTake_.empty();
    if (edgeButton({P.x + w * 0.5f - bw * 1.5f - 16, by, bw, 38}, host_->text("hud.trade.offer"), true, canAct && any && ready)) {
        sim::Json give = sim::Json::object(), take = sim::Json::object();
        for (const auto& [k, n] : tradeGive_) give[k] = n;
        for (const auto& [k, n] : tradeTake_) take[k] = n;
        host_->command("trade", {{"id", s.controlled}, {"target", t->id}, {"mode", ts.mode.empty() ? "barter" : ts.mode},
                                 {"give", give}, {"take", take}});
        tradeSent_ = true;
        tradeGive_.clear();
        tradeTake_.clear();
    }
    if (edgeButton({P.x + w * 0.5f - bw * 0.5f, by, bw, 38}, host_->text("hud.trade.clear"), false, any)) {
        tradeGive_.clear();
        tradeTake_.clear();
    }
    if (edgeButton({P.x + w * 0.5f + bw * 0.5f + 16, by, bw, 38}, host_->text("hud.trade.close"))) panel = "none";
    ui_->claim(P);
}

} // namespace lr::game
