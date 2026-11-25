#!/usr/bin/env python3
"""Эталонный валидатор content/ai по AI_DATA_SCHEMA.md v0.2 и LOC_FORMAT.md v0.1 + эталонный GOAP-планировщик.

Запуск:  python validate_ai.py <папка с json> [--rates out.json] [--plans]
Проверяет правила схемы §12 и дополнительно прогоняет каждый метод HTN из «пустого старта»
(пустой инвентарь, все природные объекты известны, все знания из pre метода есть),
планируя каждый achieve-шаг A*-поиском с лимитом config.goap_max_depth (goto считается шагом).
Это не код кора, а образец поведения для сверки с lr::sim::validate.
"""
import heapq, json, re, sys
from pathlib import Path

# Мини-язык AI_DATA_SCHEMA v0.2 §1: префикс t. (цель-персонаж), параметры $name
COND = re.compile(r'^(!)?(?:(t)\.)?([a-z_]+):([a-z0-9_$]+)(?:(>=|<=|==|!=|>|<)(-?\d+(?:\.\d+)?))?$')
EFF = re.compile(r'^(?:(t)\.)?([a-z_]+):([a-z0-9_$]+)(?:([+\-=])(-?\d+(?:\.\d+)?))?$')
COND_SUBJ = {'has', 'at', 'need', 'knows', 'skill', 'trait', 'age', 'body', 'time', 'known', 'home',
             'target_stock', 'target_owner', 'rel', 'moodlet', 'reputation'}
EFF_SUBJ = {'has', 'need', 'target_stock', 'home', 'skill', 'body', 'moodlet', 'memory', 'rel', 'rel_back', 'spawn',
            'reputation', 'status_pair', 'household_join', 'household_adopt', 'household_leave', 'household_found',
            'commitment', 'death', 'target_need'}
T_COND = {'need', 'skill', 'body', 'age', 'moodlet', 'has', 'trait', 'knows', 'reputation'}
T_EFF = {'need', 'skill', 'body', 'moodlet', 'has', 'reputation', 'death'}
NEED_CHAR = {'rel', 'rel_back', 'status_pair', 'household_join', 'household_adopt', 'commitment', 'target_need'}
FLAG_EFF = {'moodlet', 'memory', 'spawn', 'household_join', 'household_adopt', 'household_leave', 'household_found', 'commitment', 'death'}
REL_AXES = ('affinity', 'trust', 'fear', 'familiarity', 'debt', 'enmity', 'respect', 'attraction')
PARAM_TYPES = {'item', 'skill', 'place', 'need', 'trait'}
PARAM_SUBJ = {'has': 'item', 'home': 'item', 'target_stock': 'item', 'skill': 'skill', 'knows': 'skill',
              'at': 'place', 'known': 'place', 'spawn': 'place', 'need': 'need', 'trait': 'trait'}
CFG_DEFAULTS = {'reputation_axes': ["honesty", "valor", "piety", "generosity", "notoriety"],
                'status_pairs': ["betrothed", "married", "master_apprentice", "lord_vassal", "sworn_kin"],
                'household_roles': ["head", "spouse", "child", "kin", "servant", "ward"],
                'death_causes': ["starvation", "combat", "illness", "old_age", "execution"]}

INPUTS = {'need', 'time', 'body', 'has', 'home', 'mood', 'const'}
CATEG = {'household', 'work', 'social', 'economy', 'violence', 'family', 'knowledge', 'faith', 'medicine', 'travel', 'politics'}
GOTO_TICKS = 2       # средний переход для оценки длительности (≈1.6 км при 800 м/тик)
GOTO_COST = 2

errors, warns = [], []
def err(m): errors.append(m)
def warn(m): warns.append(m)


def load(dirp):
    C = {}
    for name in ['items', 'objects', 'needs', 'traits', 'goals', 'htn', 'interactions', 'fsm', 'config', 'knowledge']:
        p = dirp / f'{name}.json'
        if p.exists():
            C[name] = json.loads(p.read_text(encoding='utf-8'))
        else:
            (warn if name in ('knowledge',) else err)(f'нет файла {name}.json')
            C[name] = {}
    for extra in sorted(dirp.glob('interactions.part_*.json')):
        part = json.loads(extra.read_text(encoding='utf-8'))
        C.setdefault('interactions', {}).setdefault('interactions', []).extend(part.get('interactions', []))
        warn(f'подмешан {extra.name}: {len(part.get("interactions", []))} записей')
    return C


def ids(lst, what):
    out = {}
    for x in lst:
        i = x.get('id')
        pat = r'^[A-Za-z][A-Za-z0-9_]*$' if what == 'fsm' else r'^[a-z0-9_]+$'
        if not i or not re.match(pat, i):
            err(f'{what}: плохой id {i!r}')
        if i in out:
            err(f'{what}: дубль id {i}')
        out[i] = x
    return out


def parse_cond(s, where):
    """-> (neg, subj, key, op, val, t) | None"""
    m = COND.match(s)
    if not m:
        err(f'{where}: синтаксис условия {s!r}'); return None
    neg, t, subj, key, op, val = m.groups()
    if subj not in COND_SUBJ:
        err(f'{where}: неизвестный subject {subj!r} в {s!r}')
    if t and subj not in T_COND: err(f'{where}: t.{subj} не поддерживает цель: {s!r}')
    return (bool(neg), subj, key, op, float(val) if val is not None else None, bool(t))


def parse_eff(s, where):
    """-> (subj, key, op, val, t) | None"""
    m = EFF.match(s)
    if not m:
        err(f'{where}: синтаксис эффекта {s!r}'); return None
    t, subj, key, op, val = m.groups()
    if subj not in EFF_SUBJ:
        err(f'{where}: неизвестный subject эффекта {subj!r}')
    if t and subj not in T_EFF: err(f'{where}: t.{subj} не поддерживает цель: {s!r}')
    if subj == 'status_pair' and op is not None and not (op == '=' and val in ('0', '1')):
        err(f'{where}: status_pair — флаг или =0/=1: {s!r}')
    elif subj in FLAG_EFF and op is not None:
        err(f'{where}: {subj} — флаговый эффект без значения: {s!r}')
    return (subj, key, op, float(val) if val is not None else None, bool(t))


def param_decl(x, items, objs, needs, traits, know, where):
    """params v0.2 §1.5 -> {name: (type, [of])}"""
    out = {}
    for k, v in (x.get('params') or {}).items():
        if isinstance(v, list):
            warn(f'{where}: параметр {k} списком без типа — не параметр схемы §1.5 (перенести в отдельное поле)'); continue
        typ, of = (v, []) if isinstance(v, str) else (v.get('type'), v.get('of', []))
        if typ not in PARAM_TYPES: err(f'{where}: параметр {k}: неизвестный тип {typ!r}'); continue
        pool = {'item': items, 'place': objs, 'need': needs, 'trait': traits, 'skill': know or {}}[typ]
        for o in of:
            if pool and o not in pool: err(f'{where}: параметр {k}: значение {o!r} — нет такого {typ}')
        out[k] = (typ, of)
    return out


def check_params_use(strings, decl, where, parametric):
    used = set()
    for s in strings:
        for m in re.finditer(r'(?:^|[!.])([a-z_]+):\$(\w+)', s):
            subj, name = m.groups(); used.add(name)
            if not parametric: err(f'{where}: параметр ${name} вне параметрического взаимодействия'); continue
            if name not in decl: err(f'{where}: необъявленный параметр ${name}'); continue
            need_t = PARAM_SUBJ.get(subj)
            if need_t and decl[name][0] != need_t: err(f'{where}: ${name} типа {decl[name][0]} в {subj}: (нужен {need_t})')
        for m in re.finditer(r'"\$(\w+)"|^\$(\w+)$', s):
            used.add(m.group(1) or m.group(2))
    return used


def bind(s, env):
    for k, v in sorted(env.items(), key=lambda kv: -len(kv[0])):
        s = s.replace('$' + k, v)
    return s


def cmp(a, op, b):
    return {'>=': a >= b, '<=': a <= b, '>': a > b, '<': a < b, '==': a == b, '!=': a != b}[op]


def main():
    args = sys.argv[1:]
    dirp = Path(args[0]) if args else Path('.')
    C = load(dirp)
    items = ids(C['items'].get('items', []), 'items')
    objs = ids(C['objects'].get('objects', []), 'objects')
    needs = ids(C['needs'].get('needs', []), 'needs')
    moods = ids(C['needs'].get('moodlets', []), 'moodlets')
    traits = ids(C['traits'].get('traits', []), 'traits')
    goals = ids(C['goals'].get('goals', []), 'goals')
    tasks = ids(C['htn'].get('tasks', []), 'tasks')
    inter = ids(C['interactions'].get('interactions', []), 'interactions')
    states = ids(C['fsm'].get('states', []), 'fsm')
    know = ids(C['knowledge'].get('knowledge', []), 'knowledge') if C['knowledge'] else None
    cfg = C['config']
    memtypes = set(cfg.get('memory_types', {}))
    dlg_p = dirp / 'dialogue.json'
    dialogue = json.loads(dlg_p.read_text(encoding='utf-8')) if dlg_p.exists() else {}
    speech_acts = set(dialogue.get('speech_acts', []))
    methods = {}
    for t in tasks.values():
        for m in t.get('methods', []):
            if m['id'] in methods:
                err(f'дубль id метода {m["id"]}')
            methods[m['id']] = (t['id'], m)

    # ---------- ссылки в условиях/эффектах ----------
    rep_axes = set(cfg.get('reputation_axes', CFG_DEFAULTS['reputation_axes']))
    pairs = set(cfg.get('status_pairs', CFG_DEFAULTS['status_pairs']))
    roles = set(cfg.get('household_roles', CFG_DEFAULTS['household_roles']))
    causes = set(cfg.get('death_causes', CFG_DEFAULTS['death_causes']))
    commits = cfg.get('commitments', {})

    def check_ref_cond(c, where, char=True):
        if not c: return
        _, subj, key, _, _, t = c
        if t and not char: err(f'{where}: t.{subj} без цели-персонажа')
        if subj == 'rel' and not char: err(f'{where}: rel без цели-персонажа')
        if key.startswith('$'): return
        if subj in ('has', 'home', 'target_stock') and key not in items: err(f'{where}: нет предмета {key}')
        if subj in ('at', 'known') and key not in objs: err(f'{where}: нет объекта {key}')
        if subj == 'need' and key not in needs: err(f'{where}: нет нужды {key}')
        if subj == 'trait' and key not in traits: err(f'{where}: нет черты {key}')
        if subj == 'moodlet' and key not in moods: err(f'{where}: нет moodlet {key}')
        if subj in ('knows', 'skill') and know is not None and key not in know: err(f'{where}: нет знания {key}')
        if subj == 'target_owner' and key not in ('self', 'family', 'other', 'none'): err(f'{where}: target_owner:{key}')
        if subj == 'rel' and key not in REL_AXES: err(f'{where}: rel:{key}')
        if subj == 'reputation' and key not in rep_axes: err(f'{where}: reputation:{key}')
        if subj == 'age' and key not in ('child', 'adult', 'elder'): err(f'{where}: age:{key}')
        if subj == 'body' and key not in ('health', 'fatigue'): err(f'{where}: body:{key}')
        if subj == 'time' and key not in ('night', 'day'): err(f'{where}: time:{key}')

    def check_ref_eff(e, where, char=True):
        if not e: return
        subj, key, op, val, t = e
        if (t or subj in NEED_CHAR) and not char: err(f'{where}: {"t." if t else ""}{subj} без цели-персонажа')
        if key.startswith('$'): return
        if subj in ('rel', 'rel_back') and key not in REL_AXES: err(f'{where}: rel:{key}')
        if subj == 'reputation' and key not in rep_axes: err(f'{where}: reputation:{key}')
        if subj == 'status_pair' and key not in pairs: err(f'{where}: status_pair:{key} нет в config.status_pairs')
        if subj in ('household_join', 'household_adopt') and key not in roles: err(f'{where}: роль {key} нет в config.household_roles')
        if subj == 'commitment' and key not in commits: err(f'{where}: commitment:{key} нет в config.commitments')
        if subj == 'death' and key not in causes: err(f'{where}: death:{key} нет в config.death_causes')
        if subj in ('has', 'home', 'target_stock') and key not in items: err(f'{where}: нет предмета {key}')
        if subj in ('need', 'target_need') and key not in needs: err(f'{where}: нет нужды {key}')
        if subj == 'moodlet' and key not in moods: err(f'{where}: нет moodlet {key}')
        if subj == 'memory' and key not in memtypes: err(f'{where}: нет memory_types.{key} в config')
        if subj in ('spawn', 'household_found') and key not in objs: err(f'{where}: объект {key} неизвестен')
        if subj == 'skill' and know is not None and key not in know: err(f'{where}: нет знания {key}')
        if subj == 'body' and key not in ('health', 'fatigue'): err(f'{where}: body:{key}')
        if subj in ('has', 'need', 'target_stock', 'home', 'skill', 'body', 'rel', 'rel_back', 'reputation', 'target_need') and op is None:
            err(f'{where}: эффекту {subj} нужен op/value')

    for k, v in commits.items():
        if not isinstance(v, dict) or v.get('due', 0) <= 0: err(f'config.commitments.{k}: due ≤ 0')
        for e in v.get('on_breach', []): check_ref_eff(parse_eff(e, f'commitment {k}'), f'commitment {k}', True)

    # ---------- interactions (v0.2 §8) ----------
    P = {}  # id -> parsed (для GOAP: без t., без параметров)
    risk_ids = set()
    for i, x in inter.items():
        w = f'interaction {i}'
        for f in ('category', 'target', 'duration', 'cost', 'fsm'):
            if f not in x: err(f'{w}: нет поля {f}')
        if x.get('category') not in CATEG: err(f'{w}: category {x.get("category")}')
        tgt = x.get('target', '')
        char = tgt == 'character'
        if tgt.startswith('object:'):
            if tgt[7:] not in objs: err(f'{w}: target {tgt} — нет объекта')
        elif tgt not in ('self', 'character'):
            err(f'{w}: target {tgt}')
        if x.get('duration', 0) < 1: err(f'{w}: duration < 1')
        if x.get('fsm') not in states: err(f'{w}: fsm {x.get("fsm")} нет в fsm.json')
        decl = param_decl(x, items, objs, needs, traits, know, w)
        parametric = bool(decl)
        risks = x.get('risk', [])
        combat, terms = x.get('combat'), x.get('terms')
        eff_lists = [('eff', x.get('eff', [])), ('on_fail', x.get('on_fail', []))] + \
            [(f'risk.{r.get("id")}', r.get('eff', [])) for r in risks if isinstance(r, dict)] + \
            [(f'combat.{k}', v) for k, v in ((combat or {}).get('outcomes') or {}).items()] + \
            [('terms.on_breach', (terms or {}).get('on_breach', []))]
        all_str = list(x.get('pre', [])) + [e for _, l in eff_lists for e in l]
        if terms: all_str += [json.dumps(terms.get('give', {})), json.dumps(terms.get('take', {}))]
        if isinstance(x.get('chance'), dict) and str(x['chance'].get('skill', '')).startswith('$'): all_str.append('skill:' + x['chance']['skill'])
        used = check_params_use(all_str, decl, w, parametric)
        for k in decl:
            if k not in used: warn(f'{w}: неиспользуемый параметр {k}')
        if parametric and x.get('goap'): warn(f'{w}: параметрическое взаимодействие с goap: true (GOAP их пока не планирует)')
        pre = [parse_cond(c, w) for c in x.get('pre', [])]
        for c in pre: check_ref_cond(c, w, char)
        eff = []
        for nm, l in eff_lists:
            parsed = [parse_eff(e, f'{w}.{nm}') for e in l]
            for e in parsed: check_ref_eff(e, f'{w}.{nm}', char)
            if nm == 'eff': eff = parsed
        if x.get('continuous') and 'until' in x:
            check_ref_cond(parse_cond(x['until'], w + '.until'), w, char)
        # chance / on_fail
        ch = x.get('chance')
        if ch is not None:
            if x.get('continuous'): err(f'{w}: chance у continuous')
            if isinstance(ch, (int, float)):
                if not 0 <= ch <= 1: err(f'{w}: chance вне [0,1]')
            elif isinstance(ch, dict):
                for f in ('base', 'min', 'max'):
                    if f in ch and not 0 <= ch[f] <= 1: err(f'{w}: chance.{f} вне [0,1]')
                sk = ch.get('skill')
                if sk and not str(sk).startswith('$') and know is not None and sk not in know: err(f'{w}: chance.skill {sk} — нет знания')
            else: err(f'{w}: chance — число или объект')
        elif x.get('on_fail'): warn(f'{w}: on_fail без chance')
        # risk
        if risks and not isinstance(risks, list): err(f'{w}: risk — список'); risks = []
        seen = set()
        for r in risks:
            if not r.get('id') or not r.get('eff'): err(f'{w}: риск без id или eff')
            if not 0 <= r.get('p', -1) <= 1: err(f'{w}: risk.p вне [0,1]')
            if r.get('id') in seen: err(f'{w}: дубль риска {r.get("id")}')
            seen.add(r.get('id')); risk_ids.add(r.get('id'))
        # combat
        if combat:
            if not char: err(f'{w}: combat не у target: character')
            d = combat.get('damage', [0, 0])
            if not (isinstance(d, list) and len(d) == 2 and 0 <= d[0] <= d[1] <= 1): err(f'{w}: combat.damage не [lo ≤ hi] в [0,1]')
            for k in (combat.get('outcomes') or {}):
                if k not in ('win', 'lose', 'draw', 'flee'): err(f'{w}: неизвестный исход боя {k}')
            if combat.get('lethal') and 'combat' not in causes: err(f'{w}: lethal без combat в death_causes')
            for f in ('attack_skill', 'defense_skill'):
                if combat.get(f) and know is not None and combat[f] not in know: err(f'{w}: combat.{f} {combat[f]} — нет знания')
        # terms
        if terms:
            if not char: err(f'{w}: terms не у target: character')
            give, take = terms.get('give', {}), terms.get('take', {})
            if not give and not take: err(f'{w}: terms пустые')
            for k, n in list(give.items()) + list(take.items()):
                if not k.startswith('$') and k not in items: err(f'{w}: terms: нет предмета {k}')
                if not isinstance(n, (int, float)) or n <= 0: err(f'{w}: terms: количество {k} ≤ 0')
            if terms.get('due', 0) > 0:
                if not terms.get('commitment'): err(f'{w}: terms.due > 0 без commitment')
                elif terms['commitment'] not in commits: err(f'{w}: terms.commitment {terms["commitment"]} нет в config.commitments')
        for a_ in x.get('speech', []):
            if speech_acts and a_ not in speech_acts: err(f'{w}: речевой акт {a_} нет в dialogue.json/speech_acts')
        goap_default = not char and not parametric
        P[i] = dict(pre=[c[:5] for c in pre if c and not c[5]], eff=[e[:4] for e in eff if e and not e[4]], target=tgt,
                    goap=x.get('goap', goap_default) and not parametric, legal=x.get('legal', []),
                    dur=x.get('duration', 1), cost=x.get('cost', 1))

    # ---------- dialogue.json (v0.2 §9a) ----------
    tmpl = {}
    for t in dialogue.get('templates', []):
        w = f'dialogue {t.get("id")}'
        if t.get('id') in tmpl: err(f'{w}: дубль шаблона')
        tmpl[t.get('id')] = t
        if t.get('speech') not in speech_acts: err(f'{w}: неизвестный речевой акт {t.get("speech")}')
        for c in t.get('pre', []): check_ref_cond(parse_cond(c, w), w, True)
        for e in t.get('eff', []): check_ref_eff(parse_eff(e, w), w, True)
        vs = t.get('variants', [])
        if not vs: err(f'{w}: нет вариантов')
        vids = set()
        for v in vs:
            v = {'id': v} if isinstance(v, str) else v
            if v['id'] in vids: err(f'{w}: дубль варианта {v["id"]}')
            vids.add(v['id'])
            if v.get('weight', 1) < 0: err(f'{w}.{v["id"]}: отрицательный вес')
            for c in v.get('pre', []): check_ref_cond(parse_cond(c, w), w, True)
    for t in tmpl.values():
        for r in t.get('replies', []):
            if r not in tmpl: err(f'dialogue {t["id"]}: ответ на неизвестный шаблон {r}')

    # ---------- fsm ----------
    for s, x in states.items():
        if 'parent' in x and x['parent'] not in states: err(f'fsm {s}: parent {x["parent"]}')

    # ---------- needs / traits ----------
    for t, x in traits.items():
        for g in x.get('goal_mult', {}):
            if g not in goals: err(f'trait {t}: goal_mult {g} — нет цели')
        for n in x.get('need_mult', {}):
            if n not in needs: err(f'trait {t}: need_mult {n} — нет нужды')
        for m in x.get('method_bias', {}):
            if m not in methods: err(f'trait {t}: method_bias {m} — нет метода')
    for a, b in C['traits'].get('exclusive', []):
        if a not in traits or b not in traits: err(f'exclusive {a},{b}: нет черты')

    # ---------- goals ----------
    def curve_ok(cv, w):
        t = cv.get('type')
        if t not in ('linear', 'inverse', 'quadratic', 'logistic', 'inverse_logistic', 'step', 'const'):
            err(f'{w}: тип кривой {t}'); return
        if t in ('logistic', 'inverse_logistic'):
            if not (0.1 <= cv.get('k', 0) <= 50): err(f'{w}: k вне [0.1,50]')
            if not (0 <= cv.get('x0', -1) <= 1): err(f'{w}: x0 вне [0,1]')
        if t == 'quadratic' and not (0.1 <= cv.get('p', 0) <= 8): err(f'{w}: p вне [0.1,8]')
        if t == 'step' and not (0 <= cv.get('x0', -1) <= 1): err(f'{w}: x0 вне [0,1]')

    def curve_eval(cv, x):
        import math
        t = cv['type']
        if t == 'linear': y = cv.get('m', 1) * x + cv.get('b', 0)
        elif t == 'inverse': y = 1 - x
        elif t == 'quadratic': y = x ** cv.get('p', 2)
        elif t == 'logistic': y = 1 / (1 + math.exp(-cv['k'] * (x - cv['x0'])))
        elif t == 'inverse_logistic': y = 1 - 1 / (1 + math.exp(-cv['k'] * (x - cv['x0'])))
        elif t == 'step': y = cv.get('hi', 1) if x >= cv['x0'] else cv.get('lo', 0)
        else: y = cv.get('v', 1)
        return max(0.0, min(1.0, y))

    for g, x in goals.items():
        w = f'goal {g}'
        if x.get('task') not in tasks: err(f'{w}: task {x.get("task")} нет в htn')
        elif not tasks[x['task']].get('methods'): err(f'{w}: у задачи нет методов')
        if x.get('aggregate', 'product') not in ('product', 'sum'): err(f'{w}: aggregate')
        if 'interrupt' in x: check_ref_cond(parse_cond(x['interrupt'], w), w)
        mx = 0.0
        for c in x.get('considerations', []):
            inp = c.get('input', '')
            subj, _, key = inp.partition(':')
            if subj not in INPUTS: err(f'{w}: input {inp}')
            if subj == 'need' and key not in needs: err(f'{w}: нужда {key}')
            if subj in ('has', 'home') and key not in items: err(f'{w}: предмет {key}')
            curve_ok(c.get('curve', {}), w)
        # оценка максимума: перебор x по сетке для каждого соображения отдельно
        try:
            prod = 1.0
            for c in x.get('considerations', []):
                prod *= max(curve_eval(c['curve'], i / 20) for i in range(21))
            if prod <= 1e-6: warn(f'{w}: балл ≡ 0')
        except Exception as e:
            err(f'{w}: кривая не считается ({e})')

    # ---------- htn ----------
    for t, x in tasks.items():
        for m in x.get('methods', []):
            w = f'method {t}.{m["id"]}'
            for s in m.get('pre', []): check_ref_cond(parse_cond(s, w), w)
            for st in m.get('steps', []):
                if 'do' in st and st['do'] not in inter: err(f'{w}: do {st["do"]} нет в interactions')
                if 'task' in st and st['task'] not in tasks: err(f'{w}: task {st["task"]}')
                for s in st.get('achieve', []):
                    c = parse_cond(s, w); check_ref_cond(c, w)
                    if c and c[1] not in ('has', 'at'): err(f'{w}: achieve только has/at: {s}')
                for a in st.get('allow', []):
                    if a not in inter: err(f'{w}: allow {a} нет в interactions')
    # цикл вложенных задач
    def dfs(t, stack):
        if t in stack: err(f'цикл HTN: {" → ".join(stack + [t])}'); return
        for m in tasks[t].get('methods', []):
            for st in m.get('steps', []):
                if 'task' in st and st['task'] in tasks: dfs(st['task'], stack + [t])
    for t in tasks: dfs(t, [])

    # ---------- физическая цепочка: что достижимо из пустого старта ----------
    start_stock = set()
    for o in objs.values():
        start_stock |= set(o.get('stock', {}))
    reach = set()
    changed = True
    while changed:
        changed = False
        for i, a in P.items():
            if a['legal']: continue
            ok = all((c[1] != 'has') or c[0] or (c[2] in reach) or (c[4] is not None and c[3] in ('<', '<=') ) for c in a['pre'])
            if a['target'] == 'character': ok = False
            if not ok: continue
            for e in a['eff']:
                if e[0] == 'has' and e[2] == '+' and e[1] not in reach:
                    reach.add(e[1]); changed = True
    needed = set()
    for i, a in P.items():
        for c in a['pre']:
            if c[1] == 'has' and not c[0] and not c[2].startswith('$'): needed.add((c[2], i))
    for it, i in sorted(needed):
        if it not in reach and it not in start_stock:
            err(f'предмет {it} нужен в {i}, но нигде не производится и не лежит в стартовом запасе')
    for it in items:
        if it not in reach and it not in start_stock:
            warn(f'предмет {it} недостижим из пустого старта (только обмен/дары/спец.)')

    # ---------- эталонный GOAP по методам ----------
    maxd = cfg.get('goap_max_depth', 10)
    maxn = cfg.get('goap_max_nodes', 2000)

    home_ok = set()  # home:X, разрешённые pre текущего метода

    def sat(inv, at, c):
        neg, subj, key, op, val = c[:5]
        if subj == 'home':
            r = key in home_ok
            return (not r) if neg else r
        if subj == 'has':
            v = inv.get(key, 0)
            r = cmp(v, op, val) if op else v > 0
        elif subj == 'at':
            r = (at == key)
        else:
            return True  # статические: считаем выполненными
        return (not r) if neg else r

    def apply(inv, a):
        inv = dict(inv)
        for subj, key, op, val in a['eff']:
            if subj != 'has': continue
            if op == '+': inv[key] = inv.get(key, 0) + val
            elif op == '-': inv[key] = max(0, inv.get(key, 0) - val)
            elif op == '=': inv[key] = val
        return {k: v for k, v in inv.items() if v > 0}

    def h(inv, at, goal):
        return sum(0 if sat(inv, at, c) else 1 for c in goal)

    def plan(inv, at, goal, allow):
        pool = [i for i, a in P.items() if a['target'] != 'character' and
                ((i in allow) if allow is not None else (a['goap'] and not a['legal']))]
        # отсечение по релевантности (обратное замыкание от подцели): берём только действия,
        # которые производят нужный предмет; нужное = предметы подцели + предметы в pre их производителей
        rel = {c[2] for c in goal if c[1] == 'has'}
        prods = {}
        for i in pool:
            for e in P[i]['eff']:
                if e[0] == 'has' and e[2] == '+': prods.setdefault(e[1], set()).add(i)
        grow = True
        while grow:
            grow = False
            for itm in list(rel):
                for i in prods.get(itm, ()):
                    for c in P[i]['pre']:
                        if c[1] == 'has' and not c[0] and c[2] not in rel:
                            rel.add(c[2]); grow = True
        pool = [i for i in pool if any(e[0] == 'has' and e[2] == '+' and e[1] in rel for e in P[i]['eff'])]
        key0 = (tuple(sorted(inv.items())), at)
        openq = [(h(inv, at, goal), 0, 0, key0, inv, at, [])]
        seen = {key0: 0}
        nodes = 0
        while openq:
            f, g, _, k, cinv, cat, path = heapq.heappop(openq)
            if all(sat(cinv, cat, c) for c in goal):
                return path, cinv, cat, nodes
            nodes += 1
            if nodes > 50000: break
            if len(path) >= maxd: continue
            for i in pool:
                a = P[i]
                tgt = a['target'][7:] if a['target'].startswith('object:') else None
                steps, nat, extra = [], cat, 0
                if tgt and cat != tgt:
                    steps.append(f'goto:{tgt}'); nat = tgt; extra = GOTO_COST
                if len(path) + len(steps) + 1 > maxd: continue
                if not all(sat(cinv, nat, c) for c in a['pre']): continue
                ninv = apply(cinv, a)
                if ninv == cinv: continue
                nk = (tuple(sorted(ninv.items())), nat)
                ng = g + a['cost'] + extra
                if seen.get(nk, 1e18) <= ng: continue
                seen[nk] = ng
                heapq.heappush(openq, (ng + h(ninv, nat, goal), ng, len(seen), nk, ninv, nat, path + steps + [i]))
        return None, None, None, nodes

    def ticks(path):
        return sum(GOTO_TICKS if s.startswith('goto:') else P[s]['dur'] for s in path)

    show = '--plans' in args
    report = {}
    for tid, t in tasks.items():
        for m in t.get('methods', []):
            w = f'{tid}.{m["id"]}'
            inv, at, full = {}, None, []
            pre = [parse_cond(s, w) for s in m.get('pre', [])]
            home_ok.clear()
            home_ok.update(c[2] for c in pre if c and c[1] == 'home' and not c[0] and c[3] in (None, '>=', '>'))
            # если метод требует has:X в pre — стартуем с этим (метод выбран только при наличии)
            for c in pre:
                if c and c[1] == 'has' and not c[0] and c[3] in (None, '>=', '>', '=='):
                    inv[c[2]] = int(c[4] or 1) + (1 if c[3] == '>' else 0)
            ok = True
            for st in m.get('steps', []):
                if 'achieve' in st:
                    goal = [parse_cond(s, w) for s in st['achieve']]
                    path, ninv, nat, nodes = plan(inv, at, goal, set(st['allow']) if 'allow' in st else None)
                    if path is None:
                        err(f'{w}: achieve {st["achieve"]} недостижим из пустого старта за ≤{maxd} шагов (узлов {nodes})')
                        ok = False; break
                    if nodes > maxn: warn(f'{w}: achieve {st["achieve"]} потребовал {nodes} узлов > goap_max_nodes {maxn}')
                    full += path; inv, at = ninv, nat
                elif 'do' in st:
                    a = P.get(st['do'])
                    if not a: ok = False; break
                    tgt = a['target'][7:] if a['target'].startswith('object:') else None
                    if tgt and at != tgt: full.append(f'goto:{tgt}'); at = tgt
                    miss = [c for c in a['pre'] if not sat(inv, at, c)]
                    if miss and a['target'] != 'character':
                        err(f'{w}: do {st["do"]} — не выполнено {miss} после предыдущих шагов'); ok = False; break
                    inv = apply(inv, a); full.append(st['do'])
                elif 'task' in st:
                    full.append(f'task:{st["task"]}')
            if ok:
                report[w] = {'steps': len(full), 'ticks': ticks([s for s in full if not s.startswith('task:')]), 'plan': full, 'left': inv}
                if show:
                    print(f'  {w}: {len(full)} шагов, ~{report[w]["ticks"]} тиков → {" › ".join(full)}')

    # неиспользуемые взаимодействия
    used = set()
    for _, m in methods.values():
        for st in m.get('steps', []):
            used |= {st.get('do')} | set(st.get('allow', []))
    for r in report.values():
        used |= {s for s in r['plan']}
    for i in inter:
        if i not in used and inter[i].get('target') != 'character': warn(f'interaction {i}: никто не ссылается из HTN/планов')

    # ---------- локализация (LOC_FORMAT v0.1) ----------
    check_loc(dirp, dict(items=items, objs=objs, needs=needs, moods=moods, states=states, traits=traits, goals=goals,
                         tasks=tasks, inter=inter, risks=risk_ids, speech=speech_acts | {a for x in inter.values() for a in x.get('speech', [])},
                         tmpl=tmpl, rep=rep_axes, pairs=pairs, roles=roles, causes=causes, commits=set(commits)))

    if '--rates' in args:
        out = Path(args[args.index('--rates') + 1])
        out.write_text(json.dumps({'_doc': 'Сгенерировано tools/validate_ai.py: длительность методов из пустого старта (тиков по 10 мин, goto ≈ 2 тика). Для L2/L3 (E): «эффект за период».', 'methods': report}, ensure_ascii=False, indent=1), encoding='utf-8')

    for m in warns: print('WARN ', m)
    for m in errors: print('ERROR', m)
    print(f'итог: {len(errors)} ошибок, {len(warns)} предупреждений; goals {len(goals)}, tasks {len(tasks)}, methods {len(methods)}, interactions {len(inter)}, items {len(items)}, objects {len(objs)}, traits {len(traits)}')
    sys.exit(1 if errors else 0)


CASES = ('nom', 'gen', 'dat', 'acc', 'ins', 'pre')
PH = re.compile(r'\{([^{}]*)\}')


def flatten(d, pre=''):
    out = {}
    for k, v in d.items():
        if k == '_doc': continue
        kk = f'{pre}.{k}' if pre else k
        if isinstance(v, dict) and 'nom' not in v: out.update(flatten(v, kk))
        else: out[kk] = v
    return out


def check_template(s, lang, where):
    """Синтаксис шаблона LOC_FORMAT §3; -> множество имён аргументов"""
    names = set()
    t = s.replace('{{', '').replace('}}', '')
    depth = 0
    for ch in t:
        depth += ch == '{'; depth -= ch == '}'
        if depth < 0 or depth > 1: err(f'{where}: скобки в {s!r}'); return names
    if depth: err(f'{where}: незакрытая скобка в {s!r}'); return names
    for m in PH.finditer(t):
        body = m.group(1)
        name, _, mod = body.partition(':')
        if not re.match(r'^[A-Za-z0-9_]+$', name): err(f'{where}: недопустимое имя {name!r} в {s!r}'); continue
        names.add(name)
        if not mod: continue
        parts = mod.split('|')
        if parts[0] in CASES:
            if len(parts) > 1: err(f'{where}: падеж с формами в {s!r}')
        elif parts[0] == 'g':
            if not 2 <= len(parts) - 1 <= 4: err(f'{where}: g нужно 2–4 формы в {s!r}')
        elif parts[0] == 'plural':
            need = 3 if lang == 'ru' else 2
            if len(parts) - 1 != need: err(f'{where}: plural нужно {need} формы ({lang}) в {s!r}')
        else: err(f'{where}: неизвестный модификатор {parts[0]!r} в {s!r}')
    return names


def check_loc(dirp, R):
    root = next((r for r in (dirp / 'loc', dirp.parent / 'loc') if (r / 'loc.json').exists()), None)
    if not root: err('нет loc/loc.json'); return
    meta = json.loads((root / 'loc.json').read_text(encoding='utf-8'))
    base, fb = meta.get('base', 'ru'), meta.get('fallback', 'ru')
    langs = {}
    for lang in meta.get('languages', []):
        d, src = {}, {}
        ld = root / lang
        if not ld.is_dir(): err(f'loc: нет каталога {lang}'); continue
        for f in sorted(ld.glob('*.json')):
            for k, v in flatten(json.loads(f.read_text(encoding='utf-8'))).items():
                if k in d: warn(f'loc/{lang}: ключ {k} и в {src[k]}, и в {f.name}'); continue
                d[k], src[k] = v, f.name
        langs[lang] = d
    for need in (base, fb):
        if need not in langs: err(f'loc: нет таблицы языка {need}')
    if base not in langs: return
    req = set()
    for coll, dom in ((R['items'], 'item'), (R['objs'], 'object'), (R['needs'], 'need'), (R['moods'], 'moodlet'), (R['states'], 'fsm'),
                      (R['goals'], 'goal'), (R['tasks'], 'task'), (R['inter'], 'interaction'), (R['risks'], 'risk'), (R['speech'], 'speech'),
                      (R['rep'], 'reputation'), (R['pairs'], 'status_pair'), (R['roles'], 'household_role'), (R['causes'], 'cause'),
                      (R['commits'], 'commitment'), (REL_AXES, 'rel')):
        req |= {f'{dom}.{i}.name' for i in coll}
    req |= {f'trait.{i}.{f}' for i in R['traits'] for f in ('name', 'desc')}
    req |= {'goal.queued.name', 'goal.player_action.name'}
    for t in R['tmpl'].values():
        for v in t.get('variants', []):
            req.add(f'dialogue.{t["id"]}.{v if isinstance(v, str) else v["id"]}')
    B = langs[base]
    for k in sorted(req):
        if k not in B: err(f'loc/{base}: нет ключа {k}')
        for lang, d in langs.items():
            if lang != base and k not in d: warn(f'loc/{lang}: нет перевода {k}')
    for lang, d in langs.items():
        for k, v in d.items():
            w = f'loc/{lang} {k}'
            if lang != base and k not in B: warn(f'{w}: сирота (нет в {base})')
            vals = v if isinstance(v, list) else [v]
            for x in vals:
                if isinstance(x, dict):
                    for f, fv in x.items():
                        if f == 'g':
                            if fv not in ('m', 'f', 'n', 'pl'): err(f'{w}: род {fv!r}')
                        elif f not in CASES: err(f'{w}: неизвестная форма {f!r}')
                        elif not isinstance(fv, str): err(f'{w}: форма {f} не строка')
                elif isinstance(x, str):
                    names = check_template(x, lang, w)
                    if lang != base and k in B and isinstance(B[k], str):
                        extra = names - check_template(B[k], base, f'loc/{base} {k}')
                        if extra: warn(f'{w}: плейсхолдеры {sorted(extra)} нет в базовом шаблоне')
                else: warn(f'{w}: не-строковое значение')
            if isinstance(v, list): warn(f'{w}: массив вариантов — в LOC_FORMAT варианты задаются в dialogue.json') if k.startswith('dialogue.') else None


if __name__ == '__main__':
    main()
