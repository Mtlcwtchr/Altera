# ai — 4-слойный ИИ NPC (Utility → HTN → GOAP → FSM)
Назначение: решения персонажа без нейросетей: цели по нуждам/чертам (Utility), общий план (HTN), физические шаги (GOAP), исполнение (FSM); трасса «почему так».
Где: код в `src/sim/` (ai, controllers) **[сверить]**; данные `content/ai/`: needs, traits, goals, htn, fsm, interactions, items, objects, knowledge, config, l3_rates.
Запуск/проверка: `python tools/validate_content.py ai` (схемы, ссылки, достижимость); дальше `altera_sim_runner` со сценариями `content/ai/scenarios/`; `utility_probe.py` смотрит кривые Utility.
Расширения: `gdd/content_expansion` (черты, умения, интересы, характеры, характеристики; слияние делает validate_expansion), мотивация/мораль `gdd/motivation`.
Подводные камни:
- Предупреждения «никто не ссылается из HTN» массовые и ожидаемы (задел под контент); ошибки — только реальные ссылки в пустоту.
- Новые id добавлять в данные и сразу ключи loc ru (en можно позже, будет WARN).
- LOD слоёв: дальние NPC на L3 (годовые ставки `l3_rates.json`), не считать в полном стеке.
- Agent API (LLM) трогает только диалоги: веса и ttl задач, не управляет персонажем.
Спеки: `gdd/ALTERA_AI_ARCHITECTURE_v0.1.md`, `gdd/content_ai/README_B.md`, `gdd/AI_DATA_SCHEMA*.md`, `gdd/agent_api/AGENT_API_v0.1.md`.
