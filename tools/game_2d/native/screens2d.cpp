// The windows over the 2D map (see ctx2d.hpp): the stack, the frame, Esc, the
// pause the menus hold, the column's screen buttons.
#include "native/ctx2d.hpp"

#include <algorithm>

#include "native/pixels2d.hpp"

namespace game2d {

namespace pal {
ui::Colour hex(const char* h, float a) {
    const Rgba c = parseHex(h);
    return ui::Colour::rgb(int(c >> 16 & 0xff), int(c >> 8 & 0xff), int(c & 0xff), a);
}
const ui::Colour walnut = hex("#3a2416"), walnutD = hex("#24160c"), bronze = hex("#b08a4a"), bronzeL = hex("#e0c080"),
                 leather = hex("#4a2e1c"), leatherD = hex("#2e1c10"), parch = hex("#e8d8b0"), parchD = hex("#c8b488"),
                 ink = hex("#3a2414"), gold = hex("#e8c060"), text = hex("#efe2c4"), dim = hex("#b8a07a"), bad = hex("#b83a28"),
                 mid = hex("#c89a30"), good = hex("#5a9a3a"), player = hex("#c03028"), out = hex("#1a1410");
ui::Colour tone(double v) { return v < 0.3 ? bad : v < 0.6 ? mid : good; }
} // namespace pal

std::string jstr(const Json& j, const char* k) {
    if (!j.is_object()) return "";
    const auto it = j.find(k);
    if (it == j.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return "";
}
double jnum(const Json& j, const char* k, double d) {
    if (!j.is_object()) return d;
    const auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : d;
}
bool jflag(const Json& j, const char* k) {
    if (!j.is_object()) return false;
    const auto it = j.find(k);
    return it != j.end() && ((it->is_boolean() && it->get<bool>()) || (it->is_number() && it->get<double>() != 0));
}
long long jid(const Json& j, const char* k) {
    if (!j.is_object()) return -1;
    const auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<long long>() : -1;
}
const Json& jarr(const Json& j, const char* k) {
    static const Json empty = Json::array();
    if (!j.is_object()) return empty;
    const auto it = j.find(k);
    return it != j.end() && it->is_array() ? *it : empty;
}

struct Screens::Views {};

Screens::Screens(Ctx& c) : c_(c), v_(std::make_unique<Views>()) {}
Screens::~Screens() = default;

namespace {
bool holdsPause(const std::string& id) { return id == "menu" || id == "new" || id == "pick" || id == "saves" || id == "create"; }
} // namespace

void Screens::hold() {
    if (holding_) return;
    holding_ = true;
    heldPause_ = c_.isPaused();
    if (!heldPause_) c_.menuPause(true);   // LR-160: not with others in the game
}

void Screens::releaseIfNone() {
    if (!holding_) return;
    for (const auto& w : stack_)
        if (holdsPause(w->id) && !w->closing) return;
    holding_ = false;
    if (!heldPause_) c_.menuPause(false);
}

Win& Screens::push(std::unique_ptr<Win> w) {
    for (auto it = stack_.begin(); it != stack_.end(); ++it)
        if ((*it)->id == w->id) {
            // the same window again: its new state, raised
            stack_.erase(it);
            break;
        }
    if (holdsPause(w->id)) hold();
    stack_.push_back(std::move(w));
    return *stack_.back();
}

bool Screens::isOpen(const std::string& id) const {
    return std::any_of(stack_.begin(), stack_.end(), [&](const auto& w) { return w->id == id && !w->closing; });
}

void Screens::close(const std::string& id) {
    for (auto& w : stack_)
        if (w->id == id) w->closing = true;
}

void Screens::closeTop() {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it)
        if (!(*it)->closing) {
            (*it)->closing = true;
            return;
        }
}

void Screens::closeAll() {
    for (auto& w : stack_) w->closing = true;
}

void Screens::open(const std::string& id, const Json& args) {
    if (id == "menu" || id == "new" || id == "pick" || id == "saves" || id == "net") openMenu(c_, *this, id, args);
    else if (id == "create") openCreate(c_, *this, args.is_string() ? args.get<std::string>() : std::string());
    else if (id == "chronicle") openChronicle(c_, *this);
    else if (id == "workshop") openWorkshop(c_, *this);
    else if (id == "build") openBuild(c_, *this);
    else if (id == "colony") openColony(c_, *this);
    else if (id == "dynasty") openDynasty(c_, *this);
    else if (id == "map") openWorldMap(c_, *this);
    else if (id == "culture") openCulture(c_, *this, args.is_number() ? args.get<long long>() : -1);
    else if (id == "inspect") openInspect(c_, *this, args.is_number() ? args.get<long long>() : c_.selected());
    else if (id == "sheet") openSheet(c_, *this, args.is_number() ? args.get<long long>() : -1);
    else if (id == "talk" || id == "trade") openTalk(c_, *this, args.is_number() ? args.get<long long>() : c_.selected(), id == "trade");
    else c_.toast(c_.L("ui2d.soon", {{"what", c_.L("ui2d.screen." + id)}}));
}

void Screens::openScreen(const std::string& id) {
    if (isOpen(id)) {
        close(id);
        return;
    }
    open(id);
}

std::vector<std::pair<std::string, std::string>> Screens::columnScreens() const {
    return {{"sheet", "C"}, {"dynasty", "Y"}, {"colony", "O"}, {"build", "B"}, {"map", "M"}, {"culture", "V"}, {"workshop", "K"}, {"chronicle", "J"}, {"menu", "Esc"}};
}

void Screens::start() { open("menu"); }

bool Screens::key(SDL_Scancode sc, SDL_Keymod mod) {
    if (mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) return false;
    if (sc == SDL_SCANCODE_ESCAPE) {
        if (any()) {
            closeTop();
            return true;
        }
        if (buildKey(c_, *this, sc)) return true;
        if (c_.selected() >= 0 && c_.selected() != c_.meId()) return false;   // the first Esc lets go of someone else
        open("menu");
        return true;
    }
    if (Win* t = top(); t && t->onKey && t->onKey(c_, *t, sc)) return true;
    if (!any() && buildKey(c_, *this, sc)) return true;
    if (sc == SDL_SCANCODE_T && !any()) {   // talk to the selected one
        open("talk");
        return true;
    }
    for (const auto& [id, k] : columnScreens())
        if (k.size() == 1 && sc == SDL_Scancode(SDL_SCANCODE_A + (k[0] - 'A'))) {   // by place, any layout
            openScreen(id);
            return true;
        }
    return any();   // a window up: the map's keys wait
}

void Screens::polled() {
    for (auto& w : stack_)
        if (w->onPoll && !w->closing) w->onPoll(c_, *w);
}

bool Screens::mapInput(double wx, double wy, const ui::Input& in, bool overUi) {
    return !any() && buildMapInput(c_, *this, wx, wy, in, overUi);
}
void Screens::drawWorld() { buildDrawWorld(c_, *this); }
void Screens::personButtons(long long, std::vector<std::pair<std::string, std::function<void()>>>&) {}

void Screens::draw() {
    if (!later_.empty() && ++laterFrames_ >= 2) {
        auto jobs = std::move(later_);
        later_.clear();
        laterFrames_ = 0;
        for (auto& f : jobs) f();
    }
    // closed windows leave, their onClose told
    for (std::size_t i = 0; i < stack_.size();) {
        if (stack_[i]->closing) {
            std::unique_ptr<Win> w = std::move(stack_[i]);
            stack_.erase(stack_.begin() + std::ptrdiff_t(i));
            if (w->onClose) w->onClose(c_, *w);
            releaseIfNone();
        } else ++i;
    }
    if (stack_.empty()) {
        buildPanel(c_, *this);
        return;
    }
    ui::Ui& g = c_.gui();
    ui::Input& in = c_.input();
    const float U = c_.u(), W = c_.screenW(), H = c_.screenH();
    Win& w = *stack_.back();
    // the darkened map; a click on it closes the window
    c_.fill(0, 0, W, H, ui::Colour(0, 0, 0, 0.45f));
    const float pad = 12 * U, headH = 40 * U;
    const float fw = std::min(W - 40 * U, w.wantW > 0 ? w.wantW : std::max(420 * U, W * w.width));
    const float fullH = std::min(H - 40 * U, std::max(300 * U, H * w.height));
    const float fh = std::min(H - 40 * U, w.fitH > 0 ? std::max(160 * U, w.fitH + headH + 10 * U + pad) : fullH);
    // a fitted window keeps its top where the full one has it: tabs do not jump as it hugs
    const float top = w.autoFit ? std::round((H - fullH) / 2) : std::round((H - fh) / 2);
    const ui::Rect box{std::round((W - fw) / 2), top, std::round(fw), std::round(fh)};
    const bool outside = in.pressed && !box.contains(in.mouseX, in.mouseY);
    g.claim({0, 0, W, H});
    c_.carved(box);
    // the head: title, tabs, close
    c_.text(box.x + pad, box.y + 10 * U, w.title, pal::gold, 20 * U, true);
    float tx = box.x + pad + c_.textW(w.title, 20 * U) + 20 * U;
    int ti = 0;
    for (const auto& [id, label] : w.tabs) {
        const float tw = c_.textW(label, 14 * U) + 24 * U;
        if (g.toggle(ui::widgetId("win.tab", ti++), {tx, box.y + 8 * U, tw, 28 * U}, label, w.tab == id)) w.tab = id;
        tx += tw + 4 * U;
    }
    if (g.button(ui::widgetId("win.close"), {box.right() - pad - 30 * U, box.y + 8 * U, 30 * U, 28 * U}, "X")) w.closing = true;
    c_.fill(box.x + pad, box.y + headH + 2 * U, box.w - 2 * pad, std::max(1.0f, U), pal::bronze);
    const ui::Rect body{box.x + pad, box.y + headH + 10 * U, box.w - 2 * pad, box.h - headH - 10 * U - pad};
    if (w.autoFit) {
        // laid out at the full height, measured by how far down it drew
        const ui::Rect full{body.x, body.y, body.w, fullH - headH - 10 * U - pad};
        float ink = body.y;
        g.inkTo(&ink);
        if (w.render) w.render(c_, w, full);
        g.inkTo(nullptr);
        w.fitH = std::ceil(ink - body.y + 8 * U);
    } else if (w.render) w.render(c_, w, body);
    if (outside && w.frames > 0) w.closing = true;   // not the click that opened it
    ++w.frames;
}

} // namespace game2d
