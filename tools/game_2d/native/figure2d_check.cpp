// LR-165: does native/figure2d.cpp draw what web/figures.js draws? Reads the dump of
// `node tools/game_2d/figure_sheet.js --dump dump.json` and draws each figure again.
//   figure2d_check content/sprites_2d/figures2d.json dump.json   (exit 0: all pixel-equal)
#include <chrono>
#include <cstdio>
#include <fstream>

#include "native/figure2d.hpp"

using namespace game2d;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: figure2d_check figures2d.json dump.json\n"); return 2; }
    nlohmann::json data, dump;
    std::ifstream(argv[1]) >> data;
    std::ifstream(argv[2]) >> dump;
    setFigureData(data);
    int bad = 0, n = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& d : dump) {
        const auto& s = d["spec"];
        FigureSpec f;
        f.people = s.value("people", ""), f.culture = s.value("culture", ""), f.outfit = s["outfit"].is_string() ? s["outfit"].get<std::string>() : "";
        f.fem = s.value("fem", false), f.child = s.value("child", false), f.hide = s.value("hide", false), f.kit = s.value("kit", false);
        f.v = s.value("v", 0), f.tool = s.value("tool", "");
        PawnPose p;
        const auto& ps = d["pose"];
        p.step = ps.value("step", -1), p.bob = ps.value("bob", 0), p.dip = ps.value("dip", 0), p.swing = ps.value("swing", 0), p.blink = ps.value("blink", false);
        const char side = d["side"].get<std::string>()[0];
        const Figure g = drawFigure(f, side, p);
        ++n;
        int diff = 0, first = -1;
        if (g.w != d["w"].get<int>() || g.h != d["h"].get<int>()) diff = 1 << 20;
        else
            for (std::size_t i = 0; i < g.px.size(); ++i) {
                const std::string want = d["px"][i].get<std::string>();
                char got[8] = "";
                if (g.px[i]) std::snprintf(got, sizeof got, "#%06x", unsigned(g.px[i] & 0xffffff));
                if (want != got) { if (first < 0) first = int(i); ++diff; }
            }
        if (diff) {
            ++bad;
            if (bad <= 10) std::printf("DIFF %s %s %c: %d px (first at %d,%d)\n", f.people.c_str(), f.outfit.c_str(), side, diff, first % std::max(1, g.w), first / std::max(1, g.w));
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d figures, %d differ, %.1f ms each\n", n, bad, ms / std::max(1, n));
    return bad ? 1 : 0;
}
