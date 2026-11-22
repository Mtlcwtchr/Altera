// New screens of the Edge style (AL-67):
// - the name screen: the player names their person; the language's rules
//   (Loc::personNoun) fill the cases as the name is typed, each case can be
//   corrected by hand, and the forms go to the sim (setOwnName, AL-61);
// - talk topics without an agent (sim/dialogue_say.hpp dialogueTopics): what
//   the person could bring up with the wheel's target, one click says it;
// - the case card: a case opened against the person (sim/justice.hpp), who
//   accuses, the stage, the judge; the answers are shown, the sim chooses.

#include <algorithm>
#include <cmath>

#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"

namespace altera::game {
namespace {
using ui::Colour;
using ui::Rect;
using namespace hudroles;
const char* kCases[] = {"nom", "gen", "dat", "acc", "ins", "pre"};
} // namespace

void Hud::namePanel(const Snapshot& s, float W, float H) {
    const ActorSnap* a = actor(s, nameFor != sim::kNone ? nameFor : selected_);
    if (!a || a->animal) {
        panel = "none";
        return;
    }
    if (nameNom_.empty() && nameTyped_.empty()) nameNom_ = a->name;
    // the rules' forms for what is typed, over every case not corrected by hand
    if (nameNom_ != nameTyped_) {
        nameTyped_ = nameNom_;
        const sim::Json forms = host_->nameCases(nameNom_, a->sex);
        for (const char* c : kCases)
            if (!nameEdited_[c]) nameCases_[c] = forms.is_object() ? forms.value(c, nameNom_) : nameNom_;
    }
    dimmer(W, H, 0.6f);
    const float w = std::min(640.f, W - 80), h = 520;
    const Rect P{(W - w) * 0.5f, std::max(40.f, (H - h) * 0.5f), w, h};
    glass(P, true);
    seal(P.right() - 30, P.y + 30);
    const float d = 60, hh = d * 0.7071f;
    diamond(P.x + 30 + hh, P.y + 30 + hh, d, false, true);
    portrait(*a, P.x + 30 + hh, P.y + 30 + hh, hh - 6);
    const float tx = P.x + 30 + hh * 2 + 20;
    caps(tx, P.y + 30, host_->text("hud.name.title"), kGold, 24, 0.14f);
    label(tx, P.y + 66, ui_->fit(host_->text("hud.name.hint"), P.right() - 30 - tx, 13 / 15.f), kMute, 13);
    float y = P.y + 120;
    const float lx = P.x + 36, fx = P.x + 36 + 200, fw = P.right() - 36 - fx;
    for (int k = 0; k < 6; ++k) {
        const std::string c = kCases[k];
        label(lx, y + 9, host_->text("hud.name.case." + c), k == 0 ? kGold : kText, 15);
        label(lx, y + 28, host_->text("hud.name.case." + c + ".q"), kMute, 12);
        const Rect f{fx, y, fw, 36};
        if (k == 0) {
            ui_->textField(ui::widgetId("name.nom"), f, nameNom_, a->name, 48);
        } else {
            std::string& v = nameCases_[c];
            if (ui_->textField(ui::widgetId("name.case", k), f, v, nameNom_, 48)) nameEdited_[c] = true;
        }
        ui_->line(lx, y + 48, P.right() - 36, y + 48, kText.withAlpha(0.07f));
        y += 56;
    }
    const float bw = 180, by = P.bottom() - 58;
    const bool ok = !nameNom_.empty();
    if (edgeButton({P.x + w * 0.5f - bw - 10, by, bw, 38}, host_->text("hud.name.save"), true, ok) || (ok && in_->enter)) {
        sim::Json forms = sim::Json::object();
        forms["nom"] = nameNom_;
        for (int k = 1; k < 6; ++k) forms[kCases[k]] = nameCases_[kCases[k]].empty() ? nameNom_ : nameCases_[kCases[k]];
        host_->simOp("set_name", {{"id", a->id}, {"forms", forms}});
        panel = "none";
    }
    if (edgeButton({P.x + w * 0.5f + 10, by, bw, 38}, host_->text("hud.name.cancel"))) panel = "none";
    ui_->claim(P);
}

void Hud::talkPanel(const Snapshot& s, float W, float H) {
    const ActorSnap* t = actor(s, wheelTarget_);
    if (!t || t->animal) {
        panel = "none";
        return;
    }
    host_->wheelTarget(t->id);
    dimmer(W, H, 0.5f);
    const float w = std::min(620.f, W - 80);
    const std::size_t n = s.topicsFor == t->id ? s.topics.size() : 0;
    const float h = 170 + std::max(1.f, float(n)) * 52 + 70;
    const Rect P{(W - w) * 0.5f, std::max(40.f, (H - h) * 0.5f), w, h};
    glass(P, true);
    seal(P.right() - 30, P.y + 30);
    const float d = 60, hh = d * 0.7071f;
    diamond(P.x + 30 + hh, P.y + 30 + hh, d, false, true);
    portrait(*t, P.x + 30 + hh, P.y + 30 + hh, hh - 6);
    const float tx = P.x + 30 + hh * 2 + 20;
    caps(tx, P.y + 28, host_->text("hud.talk.title"), kGold, 22, 0.14f);
    caps(tx, P.y + 64, t->name, kText, 15, 0.12f);
    ui_->line(P.x + 30, P.y + 130, P.right() - 30, P.y + 130, kGold.withAlpha(0.35f));
    float y = P.y + 146;
    const bool canAct = s.controlled != sim::kNone && s.controlled != t->id;
    if (s.topicsFor != t->id) {
        label(P.x + 40, y + 10, host_->text("hud.talk.wait"), kMute, 14);
    } else if (s.topics.empty()) {
        label(P.x + 40, y + 10, ui_->fit(host_->text("hud.talk.none"), w - 80, 14 / 15.f), kMute, 14);
    } else {
        for (std::size_t i = 0; i < s.topics.size(); ++i) {
            const Rect row{P.x + 40, y, w - 80, 44};
            const bool hot = canAct && ui_->hovered(row);
            if (hot) selRow(row);
            diamond(row.x + 16, row.y + 22, 10, false, false);
            label(row.x + 34, row.y + 5, ui_->fit(s.topics[i].label, row.w - 44, 15 / 15.f), hot ? kGoldPale : kText, 15);
            label(row.x + 34, row.y + 25, host_->text("hud.talk.act." + s.topics[i].act), kMute, 12);
            if (canAct && clicked(row)) {
                host_->simOp("say_topic", {{"id", s.controlled}, {"target", t->id}, {"index", int(i)}});
                panel = "none";
                return;
            }
            y += 52;
        }
    }
    if (!canAct) label(P.x + 40, P.bottom() - 96, host_->text("hud.wheel.need_control"), kBad, 13);
    if (edgeButton({P.x + (w - 220) * 0.5f, P.bottom() - 56, 220, 38}, host_->text("hud.talk.back"))) openWheel(t->id);
    ui_->claim(P);
}

// A case opened against the person: who accuses, the stage, the judge; the
// answers the accused may give, shown, while the sim chooses one itself.
void Hud::caseCard(const Snapshot& s, float x, float y, float w) {
    if (s.cases.empty()) return;
    const CaseSnap& c = s.cases.front();
    const float h = 138 + (s.caseAnswers.empty() ? 0 : 40);
    const Rect r{x, y, w, h};
    glass(r, true);
    ui_->rect({r.x, r.y, 3, r.h}, kBad);
    caps(r.x + 20, r.y + 14, host_->text("hud.case.title"), kBad, 17, 0.14f, true, true);
    label(r.right() - 20 - textW(host_->text("hud.case.day", {{"n", c.day}}), 12), r.y + 18,
          host_->text("hud.case.day", {{"n", c.day}}), kMute, 12);
    float ly = r.y + 46;
    const auto line = [&](const std::string& t) {
        label(r.x + 20, ly, ui_->fit(t, w - 40, 14 / 15.f), kText, 14);
        ly += 22;
    };
    line(host_->text("hud.case.accuser", {{"who", c.accuserName}}));
    line(host_->text("hud.case.stage." + (c.stage.empty() ? std::string("answer") : c.stage)));
    if (!c.judgeName.empty()) line(host_->text("hud.case.judge", {{"who", c.judgeName}}));
    if (!s.caseAnswers.empty()) {
        float bx = r.x + 20;
        for (const auto& a : s.caseAnswers) {
            const std::string t = host_->text("hud.case.answer." + a);
            const float bw = textW(t, 13) + 20;
            if (bx + bw > r.right() - 20) break;
            const Rect b{bx, ly + 2, bw, 26};
            ui_->rect(b, kText.withAlpha(0.03f));
            ui_->border(b, kGold.withAlpha(0.25f), 1.0f, 0);
            label(b.x + 10, b.y + 5, t, kText.withAlpha(0.55f), 13);
            bx += bw + 8;
        }
        ly += 36;
    }
    label(r.x + 20, ly, ui_->fit(host_->text("hud.case.sim_decides"), w - 40, 12 / 15.f), kMute, 12);
    if (ui_->hovered(r)) tipAt(r.x + w * 0.5f, r.bottom() + 6, host_->text("hud.case.tip"));
    ui_->claim(r);
}

} // namespace altera::game
