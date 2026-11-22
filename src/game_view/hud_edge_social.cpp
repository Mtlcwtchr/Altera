// Social chains in the HUD (AL-65, sim/social_chain.hpp): an NPC asks the
// player's person something (to follow, to dance, to share a bed): a glass
// card with the ask, its time running out and the answers Yes / No (keys Y,
// N); whoever follows the person shows under the family list with "let go"
// (key X), as the "dismiss" chip of the mockup.

#include <algorithm>
#include <cmath>

#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"

namespace altera::game {
namespace {
using ui::Colour;
using ui::Rect;
using namespace hudroles;
} // namespace

void Hud::socialAsk(const Snapshot& s, float W, float H) {
    const SocialSnap& q = s.social;
    const int key = socialKey;
    socialKey = 0;
    if (key == 2 && !q.followers.empty() && s.controlled != sim::kNone)
        host_->command("dismiss_follower", {{"id", s.controlled}});
    if (q.askFrom == sim::kNone || s.controlled == sim::kNone) return;
    const float w = std::min(520.f, W - 80), h = 150;
    const Rect r{(W - w) * 0.5f, std::max(120.f, H * 0.2f), w, h};
    glass(r, true);
    seal(r.right() - 22, r.y + 22);
    const ActorSnap* from = actor(s, q.askFrom);
    if (from) {
        const float d = 46, hh = d * 0.7071f;
        diamond(r.x + 18 + hh, r.y + 18 + hh, d, false, true);
        portrait(*from, r.x + 18 + hh, r.y + 18 + hh, hh - 5);
    }
    const float tx = r.x + 92;
    caps(tx, r.y + 16, ui_->fit(q.askName, r.right() - 40 - tx, 18 / 15.f, true), kGold, 18, 0.14f, true);
    label(tx, r.y + 44, ui_->fit(host_->text("hud.social.asks", {{"act", q.askActName}}), r.right() - 30 - tx, 15 / 15.f),
          kText, 15);
    // the time to answer
    const float left = float(q.askTicksLeft) / float(std::max(1, q.askTicks));
    frameBar({tx, r.y + 74, r.right() - 30 - tx, 4}, left, left < 0.3f ? kBad : kGold);
    // the answers
    const float bw = 150, by = r.bottom() - 50;
    const Rect yes{r.x + w * 0.5f - bw - 10, by, bw, 34}, no{r.x + w * 0.5f + 10, by, bw, 34};
    const bool accept = edgeButton(yes, host_->text("hud.social.yes"), true) || key == 1;
    const bool refuse = edgeButton(no, host_->text("hud.social.no"), false) || key == -1;
    keyCap(yes.x - 30, yes.y + 6, "Y", 12);
    keyCap(no.right() + 8, no.y + 6, "N", 12);
    if (accept || refuse) host_->command("answer_social", {{"id", s.controlled}, {"accept", accept}});
    ui_->claim(r);
}

// Under the family list: "[X] let <name> go" for whoever follows the person.
void Hud::followerChip(const Snapshot& s, float right, float y) {
    const auto& f = s.social.followers;
    if (f.empty() || s.controlled == sim::kNone) return;
    for (std::size_t i = 0; i < f.size() && i < 3; ++i) {
        const std::string t = host_->text("hud.social.dismiss", {{"who", f[i].second}});
        const float tw = textW(t, 14), kw = 13 * 1.6f;
        const float x = right - tw - 8 - kw - 12;
        const Rect hit{x - 8, y - 4, tw + kw + 28, 30};
        const bool hot = ui_->hovered(hit);
        ui_->rect(hit, kRim.withAlpha(0.55f));
        ui_->border(hit, kGold.withAlpha(hot ? 0.8f : 0.35f), 1.0f, 0);
        if (i == 0) keyCap(x, y, "X", 13);
        else icon("follow", {x + 2, y + 1, 18, 18}, kText);
        label(x + kw + 8, y + 2, t, hot ? kGoldPale : kText, 14, true);
        if (clicked(hit)) host_->command("dismiss_follower", {{"id", s.controlled}, {"follower", f[i].first}});
        y += 36;
    }
}

} // namespace altera::game
