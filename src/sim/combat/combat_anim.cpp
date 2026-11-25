#include "sim/combat/combat_anim.hpp"

#include <algorithm>

namespace lr::combat {

namespace {

// "arm_l" -> "arm": hit reactions are keyed by body part, zones by side.
std::string part(const std::string& zone) {
    const auto p = zone.find('_');
    return p == std::string::npos ? zone : zone.substr(0, p);
}

std::string strAt(const OJson& j, std::initializer_list<const char*> path) {
    const OJson* cur = &j;
    for (const char* k : path) {
        if (!cur->is_object() || !cur->contains(k)) return {};
        cur = &(*cur)[k];
    }
    return cur->is_string() ? cur->get<std::string>() : std::string();
}

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return x;
}

} // namespace

AnimBinder::AnimBinder(const CombatData& d, const nlohmann::json& manifest) : d_(d) {
    if (manifest.contains("keys") && manifest["keys"].contains("combat"))
        for (auto it = manifest["keys"]["combat"].begin(); it != manifest["keys"]["combat"].end(); ++it) {
            const auto& v = it.value();
            if (v.contains("clips") && v["clips"].is_array() && !v["clips"].empty() && v["clips"][0].is_string())
                clips_[it.key()] = v["clips"][0].get<std::string>();
        }
    const OJson& t = d_.animations.contains("timing") ? d_.animations["timing"] : OJson::object();
    roundMs_ = t.value("round_ms", d_.roundSeconds * 1000.0);
    hitFrameMs_ = t.value("hit_frame_ms", roundMs_ / 3);
    if (t.contains("windup_ms") && t["windup_ms"].is_array() && !t["windup_ms"].empty())
        windupMs_ = t["windup_ms"][0].get<double>();
}

std::string AnimBinder::clipFor(const std::string& key) {
    if (key.empty()) return {};
    auto it = clips_.find(key);
    if (it == clips_.end()) {
        missingClips_.insert(key);
        return {};
    }
    return it->second;
}

std::string AnimBinder::variant(const OJson& list, std::uint64_t salt) const {
    if (list.is_string()) return list.get<std::string>();
    if (!list.is_array() || list.empty()) return {};
    return list[mix(salt) % list.size()].get<std::string>();
}

std::string AnimBinder::tierOf(double skill) const {
    const OJson& a = d_.animations;
    if (!a.contains("skill_tiers")) return {};
    for (auto it = a["skill_tiers"].begin(); it != a["skill_tiers"].end(); ++it)
        if (skill >= it.value()[0].get<double>() && skill <= it.value()[1].get<double>()) return it.key();
    return {};
}

std::vector<AnimCue> AnimBinder::bind(const std::vector<Event>& batch) {
    const OJson& A = d_.animations;
    std::vector<AnimCue> out;
    int exchanges = 0;
    for (const auto& e : batch) exchanges += e.kind == EventKind::Attack || e.kind == EventKind::Clinch ||
                                             e.kind == EventKind::Shoot;
    const double slot = roundMs_ / std::max(1, exchanges);
    int ex = -1;
    double base = 0.0;
    std::map<std::int64_t, int> attackDir; // target -> direction of the strike being answered
    for (std::size_t i = 0; i < batch.size(); ++i) {
        const Event& e = batch[i];
        const bool opens = e.kind == EventKind::Attack || e.kind == EventKind::Clinch || e.kind == EventKind::Shoot;
        if (opens) base = e.round * roundMs_ + slot * ++ex;
        else if (ex < 0) base = e.round * roundMs_;
        AnimCue c;
        c.round = e.round;
        c.actor = e.actor;
        c.source = e.kind;
        c.startMs = base;
        const std::uint64_t salt = (std::uint64_t(e.round) << 32) ^ std::uint64_t(e.actor) ^ (i << 16);
        switch (e.kind) {
        case EventKind::Attack: {
            const std::string cls = strAt(A, {"weapon_class", d_.weapons[e.weapon].id.c_str()});
            c.tier = tierOf(e.skill);
            if (A.contains("attack") && A["attack"].contains(cls))
                c.key = variant(A["attack"][cls].value(d_.directions[e.direction], OJson()), salt);
            const OJson& t = A.contains("timing") ? A["timing"] : OJson::object();
            if (t.contains("speed_mult_by")) {
                const OJson& s = t["speed_mult_by"];
                if (s.value("weapon_speed", false)) c.speedMult /= d_.weapons[e.weapon].speed;
                c.speedMult *= 1.0 + s.value("fatigue", 0.0) * (1.0 - e.stamina / d_.staminaMax);
                if (s.contains("skill_tiers") && s["skill_tiers"].contains(c.tier))
                    c.speedMult *= s["skill_tiers"][c.tier].get<double>();
            }
            attackDir[e.target] = e.direction;
            break;
        }
        case EventKind::Guard: continue; // the pose shows on the result (parry / block / dodge)
        case EventKind::Result: {
            c.actor = e.target;
            c.startMs = base + hitFrameMs_;
            const OJson& D = A.contains("defense") ? A["defense"] : OJson::object();
            switch (e.outcome) {
            case Outcome::Hit: continue; // the wound event carries the reaction
            case Outcome::Parry: {
                // guard of the defender: the Guard event just before this result
                for (std::size_t k = i; k-- > 0;)
                    if (batch[k].kind == EventKind::Guard && batch[k].actor == e.target) {
                        if (batch[k].guard >= 0)
                            c.key = strAt(D, {"parry", "by_guard", d_.guards[batch[k].guard].c_str()});
                        break;
                    }
                break;
            }
            case Outcome::BlockShield: c.key = strAt(D, {"block_shield", "anim"}); break;
            case Outcome::Block: c.key = strAt(D, {"block", "anim"}); break;
            case Outcome::Dodge: c.key = strAt(D, {"dodge", "by_direction", d_.directions[e.direction].c_str()}); break;
            }
            break;
        }
        case EventKind::Wound: {
            c.startMs = base + hitFrameMs_;
            c.additive = true;
            lastZone_[e.actor] = e.zone;
            const std::string sev = d_.severity[e.severity].id, zp = part(d_.zones[e.zone]);
            bool armored = false;
            for (std::size_t k = i; k-- > 0;)
                if (batch[k].kind == EventKind::Result) {
                    armored = batch[k].armored;
                    break;
                }
            c.key = armored && e.severity == 0 ? strAt(A, {"hit_reaction", "armored_bruise"})
                                               : strAt(A, {"hit_reaction", sev.c_str(), zp.c_str()});
            break;
        }
        case EventKind::Riposte: c.key = strAt(A, {"riposte", "anim"}); break;
        case EventKind::CloseIn: c.key = strAt(A, {"locomotion", "close_in"}); break;
        case EventKind::Exhausted: c.key = strAt(A, {"locomotion", "exhausted"}); break;
        case EventKind::Wavering: continue; // shows through stance and breathing, no clip of its own
        case EventKind::LimpOrCradle: {
            c.additive = true;
            for (std::size_t z = 0; z < d_.zones.size(); ++z) {
                if (!(e.zones & (1u << z))) continue;
                const std::string zn = d_.zones[z], zp = part(zn);
                std::string k = strAt(A, {"persistent_pose", (zn + "_disabled").c_str()});
                if (k.empty()) k = strAt(A, {"persistent_pose", (zp + "_disabled").c_str()});
                if (k.empty()) k = strAt(A, {"persistent_pose", (zp + "_wounded").c_str()});
                if (k.empty()) continue;
                AnimCue p = c;
                p.key = k;
                p.clip = clipFor(k);
                out.push_back(p);
            }
            continue;
        }
        case EventKind::State: {
            const OJson& es = A.contains("end_states") ? A["end_states"] : OJson::object();
            const std::string st = stateId(e.state);
            auto lz = lastZone_.find(e.actor);
            const std::string zp = lz == lastZone_.end() ? "" : part(d_.zones[lz->second]);
            if (es.contains(st) && es[st].is_object()) {
                const OJson& v = es[st];
                if (e.state == State::Dead && e.blood <= 0) c.key = strAt(v, {"bleed"});
                if (c.key.empty() && !zp.empty()) c.key = strAt(v, {zp.c_str()});
                if (c.key.empty()) c.key = strAt(v, {"other"});
                if (c.key.empty()) c.key = strAt(v, {"torso"});
            } else {
                c.key = strAt(A, {"end_states", st.c_str()});
            }
            break;
        }
        case EventKind::Clinch: {
            const OJson& cl = A.contains("core_events") && A["core_events"].contains("clinch") ? A["core_events"]["clinch"] : OJson::object();
            c.key = strAt(cl, {"actor"});
            c.clip = clipFor(c.key);
            out.push_back(c);
            AnimCue t = c;
            t.actor = e.target;
            t.startMs = base + hitFrameMs_;
            t.key = e.success ? strAt(cl, {"target_by_move", d_.moves[e.move].id.c_str()}) : strAt(cl, {"fail"});
            t.additive = e.success;
            t.clip = clipFor(t.key);
            if (!t.key.empty()) out.push_back(t);
            continue;
        }
        case EventKind::GetUp:
        case EventKind::Shoot:
        case EventKind::ProjectileHit:
        case EventKind::ProjectileMiss:
        case EventKind::WeaponBreak: {
            const char* k = eventId(e.kind);
            if (e.kind == EventKind::ProjectileHit) {
                c.actor = e.target;
                c.additive = true;
            }
            c.key = strAt(A, {"core_events", k});
            if (c.key.empty()) {
                unbound_.insert(k);
                continue;
            }
            break;
        }
        case EventKind::End: continue;
        }
        if (c.key.empty()) {
            unbound_.insert(eventId(e.kind));
            continue;
        }
        c.clip = clipFor(c.key);
        out.push_back(c);
    }
    return out;
}

} // namespace lr::combat
