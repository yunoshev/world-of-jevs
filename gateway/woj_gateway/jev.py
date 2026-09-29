"""Ярус Jev: платная модель через OpenRouter /api/alpha/decisions.

Контракт выяснен запросами к живому API, не из документации: расходится с
черновиком docs/observation-contract-draft.json — `questions` объект, а не
массив, и обязательны оба поля `instructions` и `criteria`.

Эндпоинт не возвращает свободный текст (типы вопросов: choice/noul/score),
поэтому реплики мурлока — наши, а «мысль» в логе собирается из вероятностей.
"""
import json
import logging
import math
import random
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any, Callable, Literal

import httpx

from . import geometry, provider
from .activity import Activity
from .budget import (_short, ESTIMATE_NO_PRICE_READ, ESTIMATE_NOT_REPORTED,
                     ESTIMATE_UNUSABLE_NUMBER, Charge, Meter)
from .config import (JEV_CONNECT_TIMEOUT_SECONDS, JEV_KEEPALIVE_SECONDS,
                     JEV_KEY_PATH, JEV_MAX_OWN_TOKENS, JEV_MODEL,
                     JEV_MAX_CONNECTIONS, JEV_MAX_KEEPALIVE_CONNECTIONS,
                     JEV_POOL_TIMEOUT_SECONDS, JEV_TIMEOUT_SECONDS, JEV_URL,
                     JEV_WRITE_TIMEOUT_SECONDS)
from .contract import (Action, Ally, AnswerHelp, Attack, CallHelp, Cast, HealthEvent, AggroEvent,
                       DecideRequest, Flee, FleeForAssist, Idle, Marker,
                       ResumeRoutine, StartPhase, StockTalk, StopAttack)
from .policy import (Policy, SelectedPolicy, SpellPolicy, SpellPolicyV3,
                     select_policy, store as policy_store)
from .sheets import Ability, Sheet, load_sheet


_activity: Activity | None = None
_ACTIVITY_SUMMARY_LIMIT = 512


def set_activity(activity: Activity | None) -> None:
    """Подключается только app runtime, а не offline-import этого модуля."""
    global _activity
    _activity = activity


def _activity_common(req: DecideRequest) -> dict[str, Any]:
    """Identity, достаточная для склейки activity с world-журналом."""
    return {
        "server_boot_id": req.server_boot_id,
        "request_id": req.request_id,
        "snapshot_seq": req.snapshot_seq,
        "npc": {"id": req.npc.id, "guid": req.npc.guid,
                "entry": req.npc.entry, "name": req.npc.name,
                "epoch": req.npc.epoch, "map_id": req.npc.map_id,
                "instance_id": req.npc.instance_id, "raw_guid": req.npc.raw_guid},
        "epoch": req.npc.epoch,
        "policy_revision": req.state.behavior.policy_revision,
        "group_id": req.state.behavior.group_id,
        "binding_generation": req.state.behavior.binding_generation,
    }


def _activity_write(event: str, req: DecideRequest, **fields: Any) -> None:
    if _activity is not None:
        _activity.write(event, {**_activity_common(req), **fields})


def _activity_state_summary(state: dict) -> dict[str, str]:
    """Короткая фактическая часть отправленного batch-state без payload целиком."""
    summary: dict[str, str] = {}
    for name in ("self", "scene", "triggers", "trigger_events"):
        value = state.get(name, "")
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)
        summary[name] = (text if len(text) <= _ACTIVITY_SUMMARY_LIMIT else
                         text[:_ACTIVITY_SUMMARY_LIMIT - 1] + "…")
    return summary


class JevUnavailable(Exception):
    """Ярус не смог ответить — причина короткая и машинная, для журнала."""

    def __init__(self, reason: str, *, provider_calls: list[dict] | None = None,
                 charge: Charge | None = None) -> None:
        super().__init__(reason)
        self.reason = reason
        self.provider_calls = provider_calls or []
        self.charge = charge


class JevTargetUnavailable(JevUnavailable):
    """Intent valid, but its required typed target answer is unusable.

    Tiers treats this separately: an invalid target must become a safe IDLE,
    never a heuristic choice of some other combat target.
    """

@dataclass(frozen=True)
class JevResult:
    action: Action
    marker: Marker
    thought: str
    # Any, не dict: journal-копия answers ПОСЛЕ _unusable_numbers_marked
    # (числа заменены пометкой, форма дерева не тронута). answers —
    # альфовый ответ и вправе прислать вместо словаря список, строку, число
    # или null — dict был бы неверной аннотацией, не более строгой.
    answers: Any
    questions_sent: dict
    state_sent: dict
    model: str
    # Ложь, если провайдер не назвал модель строкой (тогда в model — наша
    # пометка). Выборка Ф6 не должна включать записи с неподтверждённой
    # моделью; это решает trainable, не разбор строки пометки.
    model_confirmed: bool
    latency_ms: int
    # Цены своей нет: списанное и причина — Charge от сторожа; своя копия
    # была бы вторым источником правды, расходящимся с первым.
    charge: Charge
    billed_input_tokens: int
    billed_output_tokens: int
    own_tokens_estimate: int
    provider_calls: list[dict]
    prompt_mode: Literal["single", "parallel"]


@dataclass(frozen=True)
class PromptBatch:
    name: str
    state: dict
    questions: dict
    own_tokens: int

    @property
    def payload(self) -> dict:
        return {"model": JEV_MODEL, "state": self.state,
                "questions": self.questions}


@dataclass(frozen=True)
class DecisionPlan:
    state: dict
    questions: dict
    catalog: "TargetCatalog"
    ability_spells: dict[str, int]
    health_phase_id: str | None
    batches: list[PromptBatch]
    policy_revision: int


@dataclass(frozen=True)
class BatchAnswer:
    answers: Any
    model: str
    model_confirmed: bool
    charge: Charge
    billed_input_tokens: int
    billed_output_tokens: int
    call: dict


Intent = str


def _is_intent(value: str) -> bool:
    """Common actions plus bounded sheet-derived legacy/v3 ability slugs."""
    if value in INTENT_CRITERIA:
        return True
    if value.startswith(("health_event_", "aggro_event_")):
        event_id = value.split("event_", 1)[1]
        return (1 <= len(event_id) <= 32 and event_id[0].islower()
                and event_id[0].isascii() and
                all(char.isascii() and (char.islower() or char.isdigit() or char == "_")
                    for char in event_id))
    for prefix in ("cast_", "use_"):
        if value.startswith(prefix):
            slug = value.removeprefix(prefix)
            return bool(slug) and all(part.isalpha() and part.islower()
                                      for part in slug.split("_"))
    return False


# Намерение -> маркер, который модуль запишет в last_actions. Маркеры те же,
# что у эвристики: два яруса должны быть неразличимы для модуля, иначе
# деградация будет видна ему как смена словаря.
INTENT_MARKERS: dict[str, Marker] = {
    "hold": "idle",
    "follow_routine": "resume_routine",
    "attack": "attack",
    "cast_frost_nova": "cast",
    "cast_poison": "cast",
    "cast_holy_smite": "cast",
    "cast_renew": "cast",
    "cast_volatile_infection": "cast",
    "cast_shoot": "cast",
    "cast_lightning_bolt": "cast",
    "cast_healing_wave": "cast",
    "flee_for_help": "flee",
    "call_help": "call_help",
    "answer_help": "answer_help",
    "start_health_phase": "start_phase",
    "flee": "flee",
    "stock_talk": "stock_talk",
    "stop_fighting": "stop",
}

# Описания вариантов. Постоянные строки: подстановка имени игрока или
# другого текста, выбранного человеком, сделала бы это каналом инъекции.
INTENT_CRITERIA: dict[str, str] = {
    "hold": "Continue current activity; no new order.",
    "follow_routine": "Follow your usual stand, roam, or patrol routine.",
    "attack": "Attack a chosen enemy.",
    "cast_frost_nova": "Use Frost Nova on an enemy you choose.",
    "cast_poison": "Use Poison on an enemy you choose.",
    "cast_holy_smite": "Use Holy Smite on an enemy you choose.",
    "cast_renew": "Use Renew on a wounded kinsman you choose.",
    "cast_volatile_infection": "Use Volatile Infection on an enemy you choose.",
    "cast_shoot": "Use Shoot on an enemy you choose.",
    "cast_lightning_bolt": "Use Lightning Bolt on an enemy you choose.",
    "cast_healing_wave": "Use Healing Wave on a wounded kinsman you choose.",
    "flee_for_help": "Ask chosen kin, then run to them.",
    "call_help": "Ask chosen kin; stay here.",
    "answer_help": "Answer one exact help request.",
    "start_health_phase": "Begin the due original encounter phase.",
    "flee": "Flee from a chosen enemy; ask nobody.",
    "stock_talk": "Use the eligible stock creature line.",
    "stop_fighting": "Stop fighting; stay here.",
}

assert set(INTENT_MARKERS) == set(INTENT_CRITERIA)


def _intent_marker(intent: str) -> Marker:
    if intent.startswith("aggro_event_"):
        return "aggro_event"
    if intent.startswith("health_event_"):
        return "health_event"
    if intent.startswith(("cast_", "use_")):
        return "cast"
    marker = INTENT_MARKERS.get(intent)
    if marker is None:
        raise RuntimeError(f"intent has no marker: {intent}")
    return marker

DANGER_SCALE = ["safe", "wary", "threatened", "fighting for your life"]

# Полосы «сколько прошло с последнего действия» (seconds_since_last_action),
# а не «сколько длилось прошлое намерение» — этой величины модуль не
# присылает; текст не пишет `for`, чтобы не выдавать одно за другое.
# Полосы, а не секунды: иначе _snapshot_key в tiers.py менялся бы на каждом
# тике, хотя обстановка не менялась.
LAST_ACTION_JUST_NOW_SECONDS = 5.0
LAST_ACTION_MOMENTS_AGO_SECONDS = 20.0
LAST_ACTION_A_WHILE_AGO_SECONDS = 60.0


def _last_action_band(seconds: float) -> str:
    if seconds < LAST_ACTION_JUST_NOW_SECONDS:
        return "just now"
    if seconds < LAST_ACTION_MOMENTS_AGO_SECONDS:
        return "moments ago"
    if seconds < LAST_ACTION_A_WHILE_AGO_SECONDS:
        return "a while ago"
    return "long ago"


def _estimate_tokens(payload: dict) -> int:
    """Грубая оценка: символы, делённые на 4, вверх.

    Точный токенизатор провайдера нам недоступен, а инвариант существует,
    чтобы поймать разбухание в разы, а не в проценты.
    """
    text = json.dumps(payload, ensure_ascii=False)
    return math.ceil(len(text) / 4)


def _hp_band(current: int, maximum: int) -> str:
    """Здоровье полосой, а не числом: полосы стабильнее между тиками и
    дешевле в токенах, а решение от одного процента не зависит."""
    if maximum <= 0:
        return "hp unknown"
    pct = 100 * current // maximum
    if pct >= 95:
        return "hp 100%"
    if pct >= 70:
        return "hp 70-95%"
    if pct >= 40:
        return "hp 40-70%"
    if pct >= 15:
        return "hp 15-40%"
    return "hp below 15%"


def _pct_band(pct: float) -> str:
    # C++ отдаёт GetHealthPct(), поэтому health_pct — дробное. Те же
    # границы остаются полосами, не округлёнными значениями снимка.
    if pct >= 95:
        return "hp 100%"
    if pct >= 70:
        return "hp 70-95%"
    if pct >= 40:
        return "hp 40-70%"
    if pct >= 15:
        return "hp 15-40%"
    return "hp below 15%"


def _distance_band(distance: float) -> str:
    """Границы 5/10/20/30 yd; ключ стабилен внутри полосы, не через неё."""
    if distance < 5.0:
        return "within 5 yd"
    if distance < 10.0:
        return "5-10 yd away"
    if distance < 20.0:
        return "10-20 yd away"
    if distance < 30.0:
        return "20-30 yd away"
    return "30+ yd away"


def _unit_role(unit) -> str:
    return "player" if unit.is_player else "enemy creature"


def _kin_role(entry: int, *, dynamic: bool = False) -> str:
    if dynamic:
        return "kin"
    return {117: "gnoll", 127: "tidehunter", 391: "leader", 500: "scout",
            517: "oracle", 1065: "shaman"}.get(entry, "kin")


def _compact_kin_role(entry: int, *, dynamic: bool = False) -> str:
    if dynamic:
        return "kin"
    return {117: "fighter", 127: "fighter", 391: "leader", 500: "scout",
            517: "healer", 1065: "shaman"}.get(entry, "kin")


def _intent_role(entry: int, *, dynamic: bool = False,
                 sheet: Sheet | None = None) -> str:
    if dynamic:
        return sheet.you if sheet is not None else "kin defending its group"
    return {127: "tidehunter defending kin",
            391: "leader defending kin",
            500: "scout defending the Riverpaw camp",
            517: "oracle defending and healing kin",
            1065: "shaman defending and healing kin",
            117: "gnoll defending Riverpaw kin"}.get(entry, "kin defending its camp")


def _all_kin(req: DecideRequest) -> list[Ally]:
    """Ambient kin first, then heal-only kin, with one stable local label."""
    result = list(req.state.allies)
    seen = {ally.id for ally in result}
    result.extend(ally for ally in req.state.heal_allies if ally.id not in seen)
    return result


def _spell_by_id(req: DecideRequest, spell_id: int):
    return next((spell for spell in req.state.spells if spell.id == spell_id), None)


def _trigger(req: DecideRequest, trigger_id: str):
    trigger = next((item for item in req.state.behavior.triggers
                    if item.id == trigger_id), None)
    if trigger is None:
        raise RuntimeError(f"missing validated trigger {trigger_id}")
    return trigger


def _trigger_eligible(req: DecideRequest, trigger_id: str) -> bool:
    return _trigger(req, trigger_id).status == "eligible"


def _eligible_stock_talk(req: DecideRequest,
                         policy: SelectedPolicy) -> tuple[str, int] | None:
    candidates: list[tuple[str, bool, int]] = [
        ("ambient_talk", policy.ambient_talk.enabled and policy.actor_ambient_talk,
         policy.ambient_talk.text_group),
        ("aggro_talk", policy.aggro_talk.enabled
         and (not policy.dynamic or policy.actor_aggro_talk),
         policy.aggro_talk.text_group),
    ]
    for trigger_id, configured, text_group in candidates:
        if configured and _trigger_eligible(req, trigger_id):
            return trigger_id, text_group
    return None


def _eligible_health_phase(req: DecideRequest, policy: SelectedPolicy):
    """Return the sole policy-owned phase which the executor made eligible."""
    eligible = [phase for phase in policy.health_phases
                if _trigger_eligible(req, f"health_phase_{phase.id}")]
    if len(eligible) > 1:
        raise JevUnavailable("multiple health phases are simultaneously eligible")
    if not eligible:
        return None
    phase = eligible[0]
    health = req.state.health
    if (not req.state.in_combat or health.max <= 0
            or health.current * 100 >= health.max * phase.trigger_below_health_pct):
        raise JevUnavailable("eligible health phase contradicts current health state")
    index = policy.health_phases.index(phase)
    for earlier in policy.health_phases[:index]:
        trigger = _trigger(req, f"health_phase_{earlier.id}")
        if trigger.status != "blocked" or trigger.blocked_by != "once":
            raise JevUnavailable("eligible health phase violates configured phase order")
    return phase


def _eligible_health_events(req: DecideRequest, policy: SelectedPolicy):
    """Each event remains a separate choice when health windows overlap."""
    eligible = [event for event in policy.health_events
                if _trigger_eligible(req, f"health_event_{event.id}")]
    if eligible:
        health = req.state.health
        if not req.state.in_combat or health.max <= 0:
            raise JevUnavailable("eligible health event contradicts combat state")
        # Stock SmartAI truncates GetHealthPct() to uint32 before comparing
        # its inclusive min/max window.
        pct = health.current * 100 // health.max
        for event in eligible:
            if not event.health_pct_min <= pct <= event.health_pct_max:
                raise JevUnavailable("eligible health event contradicts health window")
    return eligible


def _eligible_aggro_events(req: DecideRequest, policy: SelectedPolicy):
    eligible = [event for event in policy.aggro_events
                if _trigger_eligible(req, f"aggro_event_{event.id}")]
    if eligible and not req.state.in_combat:
        raise JevUnavailable("eligible aggro event contradicts combat state")
    return eligible


def _policy(req: DecideRequest,
            value: Policy | SelectedPolicy | None) -> SelectedPolicy:
    if isinstance(value, SelectedPolicy):
        return value
    policy = (policy_store.for_revision(req.state.behavior.policy_revision)
              if value is None else value)
    try:
        return select_policy(
            policy, schema_version=req.schema_version,
            scene_id=req.state.behavior.scene_id,
            group_id=req.state.behavior.group_id,
            spawn_id=req.npc.guid, entry=req.npc.entry)
    except ValueError as exc:
        raise JevUnavailable(str(exc)) from exc


def _sheet(req: DecideRequest, value: Sheet | None) -> Sheet:
    return load_sheet(req.npc.entry) if value is None else value


def _ability_rules(policy: SelectedPolicy, sheet: Sheet
                   ) -> tuple[tuple[Ability, SpellPolicy | SpellPolicyV3], ...]:
    rules = {item.spell: item for item in policy.spells_for_entry(sheet.entry)}
    described = {ability.spell for ability in sheet.abilities}
    if not set(rules) <= described:
        raise ValueError("behavior policy spell is missing from ability sheet")
    # A shared creature sheet can describe more spells than a particular
    # scene enables; only policy-enabled abilities may reach Jev.
    result = tuple((ability, rules[ability.spell]) for ability in sheet.abilities
                   if ability.spell in rules)
    if any(ability.target != rule.target for ability, rule in result):
        raise ValueError("behavior policy and ability sheet target kinds disagree")
    if any(ability.ally_below_pct != rule.max_target_health_pct
           for ability, rule in result if ability.target == "ally"):
        raise ValueError("behavior policy and ability sheet ally health limits disagree")
    return result


def _validate_policy_snapshot(req: DecideRequest, policy: SelectedPolicy,
                              sheet: Sheet) -> None:
    """Cross-check all v3 facts which cannot be validated by wire alone."""
    if not policy.dynamic:
        return
    common = {"aggro", "help_call", "help_answer", "flee", "routine", "return"}
    spells = policy.spells_for_entry(req.npc.entry)
    expected = common | {f"spell_{rule.spell}" for rule in spells}
    expected.update(f"health_phase_{phase.id}" for phase in policy.health_phases)
    expected.update(f"health_event_{event.id}" for event in policy.health_events)
    expected.update(f"aggro_event_{event.id}" for event in policy.aggro_events)
    if policy.actor_aggro_talk:
        expected.add("aggro_talk")
    if policy.actor_ambient_talk:
        expected.add("ambient_talk")
    actual = {trigger.id for trigger in req.state.behavior.triggers}
    if actual != expected:
        raise JevUnavailable("behavior trigger catalog does not match policy group")

    # C++ owns the primary observation filter, but a malformed/stale bridge
    # snapshot must not let a foreign group's spawn enter Jev's heal/help
    # candidates.  Raw object ids can change across lives; the durable group
    # identity here is the validated spawn+entry pair.
    ordinary = _all_kin(req)
    ordinary_spawns = [ally.guid_spawn for ally in ordinary]
    if len(ordinary_spawns) != len(set(ordinary_spawns)):
        raise JevUnavailable("snapshot kin repeat spawn identity")
    for ally in ordinary:
        actor = policy.actor_for_spawn(ally.guid_spawn)
        if (actor is None or actor.entry != ally.entry
                or ally.guid_spawn == req.npc.guid):
            raise JevUnavailable("snapshot kin do not belong to policy group")
    for request in req.state.help_requests:
        requester = request.requester
        actor = policy.actor_for_spawn(requester.guid_spawn)
        if (actor is None or actor.entry != requester.entry
                or requester.guid_spawn == req.npc.guid):
            raise JevUnavailable(
                "help requester does not belong to policy group")

    rules = {rule.spell: rule for rule in spells}
    enemies = {unit.id for unit in req.state.enemies}
    allies = {ally.id for ally in req.state.heal_allies}
    for spell in req.state.spells:
        rule = rules.get(spell.id)
        if rule is None:
            raise JevUnavailable("snapshot spell is not configured for actor")
        if rule.target == "enemy":
            domain = enemies
        elif rule.target == "self":
            domain = {req.npc.id}
        else:
            domain = set(allies)
        if (rule.target == "ally" and isinstance(rule, SpellPolicyV3)
                and rule.allow_self):
            domain.add(req.npc.id)
        if not set(spell.valid_targets) <= domain:
            raise JevUnavailable("spell valid_targets reference wrong policy domain")
        if not set(spell.aura_targets) <= domain:
            raise JevUnavailable("spell aura_targets reference wrong policy domain")
    _ability_rules(policy, sheet)


@dataclass(frozen=True)
class TargetCatalog:
    """Local labels and physical option sets for one immutable snapshot."""

    labels_by_raw: dict[str, str]
    raw_by_label: dict[str, str]
    options: dict[str, tuple[str, ...]]


def _catalog(req: DecideRequest, policy: Policy | SelectedPolicy | None = None,
             sheet: Sheet | None = None) -> TargetCatalog:
    """Build opaque labels without leaking or losing the raw-id mapping."""
    policy = _policy(req, policy)
    sheet = _sheet(req, sheet)
    st = req.state
    enemies = {unit.id: f"e{i}" for i, unit in enumerate(st.enemies, 1)}
    allies = {req.npc.id: "a0"}
    allies.update({unit.id: f"a{i}" for i, unit in enumerate(_all_kin(req), 1)})
    labels_by_raw = enemies | allies
    raw_by_label = {label: raw for raw, label in labels_by_raw.items()}
    help_labels = {request.request_id: f"h{i}"
                   for i, request in enumerate(st.help_requests, 1)}
    raw_by_label.update({label: raw for raw, label in help_labels.items()})
    enemies_by_id = {unit.id: unit for unit in st.enemies}
    self_pct = (0 if st.health.max <= 0 else
                100 * st.health.current / st.health.max)
    flee_valid = list(st.flee_targets)
    if (not policy.flee.enabled
            or not _trigger_eligible(req, "flee")
            or (policy.flee.require_in_combat and not st.in_combat)
            or self_pct > policy.flee.max_health_pct):
        flee_valid = []
    else:
        flee_valid = [raw for raw in flee_valid
                      if ((not policy.flee.require_engaged
                           or enemies_by_id[raw].engaged)
                          and enemies_by_id[raw].distance
                          <= policy.flee.max_distance_yards)]
    options = {
        "attack": tuple(enemies[raw] for raw in st.attack_targets),
        "engaged": tuple(enemies[unit.id] for unit in st.enemies if unit.engaged),
        "flee": tuple(enemies[raw] for raw in flee_valid),
        "call_recipient": tuple(allies[raw] for raw in st.call_help_targets),
        "flee_recipient": (tuple(allies[raw] for raw in st.flee_assist_targets)
                           if flee_valid else ()),
        "help": tuple(help_labels[request.request_id] for request in st.help_requests),
    }
    for ability, rule in _ability_rules(policy, sheet):
        spell = _spell_by_id(req, ability.spell)
        if (spell is not None and spell.ready
                and _trigger_eligible(req, f"spell_{ability.spell}")):
            labels = enemies if ability.target == "enemy" else allies
            valid = list(spell.valid_targets)
            if rule.target == "self":
                valid = [raw for raw in valid if raw == req.npc.id]
            if (isinstance(rule, SpellPolicyV3) and rule.target == "ally"
                    and not rule.allow_self):
                valid = [raw for raw in valid if raw != req.npc.id]
            aura = set(spell.aura_targets)
            allies_by_id = {ally.id: ally for ally in _all_kin(req)}

            def eligible(raw: str) -> bool:
                if rule.require_caster_combat and not st.in_combat:
                    return False
                if rule.require_caster_out_of_combat and st.in_combat:
                    return False
                if rule.require_aura_absent and raw in aura:
                    return False
                if raw == req.npc.id:
                    in_combat, engaged, distance, hp = (
                        st.in_combat, st.in_combat, 0.0, self_pct)
                elif rule.target == "ally":
                    unit = allies_by_id[raw]
                    in_combat, engaged, distance, hp = (
                        unit.in_combat, unit.in_combat, unit.distance,
                        unit.health_pct)
                else:
                    unit = enemies_by_id[raw]
                    in_combat, engaged, distance, hp = (
                        unit.engaged, unit.engaged, unit.distance,
                        unit.health_pct)
                if rule.require_target_in_combat and not in_combat:
                    return False
                if rule.require_engaged and not engaged:
                    return False
                if rule.max_distance_yards is not None and distance > rule.max_distance_yards:
                    return False
                if rule.max_target_health_pct is not None and hp > rule.max_target_health_pct:
                    return False
                return True

            valid = [raw for raw in valid if eligible(raw)]
            options[_intent_for_ability(ability, policy)] = tuple(
                labels[raw] for raw in valid)
    return TargetCatalog(labels_by_raw, raw_by_label, options)


def _ability_is_offered(ability: Ability, req: DecideRequest,
                        catalog: TargetCatalog, policy: SelectedPolicy) -> bool:
    spell = _spell_by_id(req, ability.spell)
    return (spell is not None and spell.ready
            and bool(catalog.options.get(_intent_for_ability(ability, policy))))


def _intent_for_ability(ability: Ability, policy: SelectedPolicy) -> Intent:
    value = f"{'use' if policy.dynamic else 'cast'}_{ability.slug}"
    if not _is_intent(value):
        raise RuntimeError(f"sheet has unsupported ability slug {ability.slug}")
    return value


def _short_hp(pct: float) -> str:
    if pct >= 95:
        return "full"
    if pct >= 70:
        return "high"
    if pct >= 40:
        return "mid"
    if pct >= 15:
        return "low"
    return "critical"


def _short_distance(distance: float) -> str:
    if distance < 5:
        return "close"
    if distance < 15:
        return "near"
    if distance < 30:
        return "mid-range"
    return "far"


def _compact_distance(distance: float) -> str:
    """Короткая, но однозначно числовая полоса для плотного P2."""
    if distance < 5:
        return "<5yd"
    if distance < 15:
        return "5-15yd"
    if distance < 30:
        return "15-30yd"
    return "30+yd"


def _milliseconds_band(milliseconds: int | None) -> str:
    if milliseconds is None:
        return "never"
    if milliseconds == 0:
        return "0"
    if milliseconds < 1_000:
        return "under 1s"
    if milliseconds < 5_000:
        return "1-5s"
    if milliseconds < 15_000:
        return "5-15s"
    return "15s+"


def _compact_milliseconds_band(milliseconds: int | None) -> str:
    band = _milliseconds_band(milliseconds)
    return "<1s" if band == "under 1s" else band


def _ttl_band(milliseconds: int) -> str:
    if milliseconds <= 2_000:
        return "expires soon"
    if milliseconds <= 5_000:
        return "expires in 2-5s"
    return "expires in 5-10s"


def _ability_codes(req: DecideRequest, sheet: Sheet | None = None) -> dict[int, str]:
    preferred = {"frost_nova": "F", "poison": "O", "holy_smite": "S",
                 "renew": "R", "volatile_infection": "V", "shoot": "H",
                 "lightning_bolt": "L", "healing_wave": "W"}
    abilities = _sheet(req, sheet).abilities
    if req.schema_version == "1.7":
        return {ability.spell: preferred[ability.slug] for ability in abilities}
    return {ability.spell: f"U{index}"
            for index, ability in enumerate(abilities, 1)}


def _routine_fact(req: DecideRequest) -> str:
    routine = req.state.routine
    if routine.profile == "idle":
        usual = "stand at your post"
    elif routine.profile == "random":
        radius = f"{routine.wander_radius:g}"
        usual = f"roam within {radius} yd of your post"
    else:
        usual = f"patrol {routine.point_count} stops"
    state = "active" if routine.active else "ready to resume"
    return f"usual routine: {usual}; {state}"


_TRIGGER_NAMES = {
    "aggro": "aggro", "help_call": "help call",
    "help_answer": "help answer", "flee": "flee",
    "routine": "routine", "return": "return home",
    "aggro_talk": "stock aggro talk",
    "ambient_talk": "stock ambient talk",
    "spell_11831": "Frost Nova", "spell_744": "Poison",
    "spell_3584": "Volatile Infection", "spell_9734": "Holy Smite",
    "spell_6074": "Renew", "spell_6660": "Shoot",
    "spell_9532": "Lightning Bolt", "spell_913": "Healing Wave",
}

def _seconds(milliseconds: int) -> str:
    return f"{milliseconds / 1000:g}s"


def _range_text(values: tuple[int, int]) -> str:
    return (_seconds(values[0]) if values[0] == values[1] else
            f"{_seconds(values[0])}-{_seconds(values[1])}")


def _spell_rule_text(rule: SpellPolicy) -> str:
    facts = [f"first{_range_text(rule.initial_ms)}/repeat{_range_text(rule.repeat_ms)}"]
    if rule.require_caster_combat:
        facts.append("combat")
    if rule.require_caster_out_of_combat:
        facts.append("out of combat")
    if rule.require_target_in_combat:
        facts.append("target combat")
    if rule.require_engaged:
        facts.append("engaged")
    if rule.max_distance_yards is not None:
        facts.append(f"target<={rule.max_distance_yards:g}yd")
    if rule.chance_pct < 100:
        facts.append(f"{rule.chance_pct}% chance")
    if rule.require_aura_absent:
        facts.append("no aura")
    if rule.max_target_health_pct is not None:
        facts.append(f"target hp<={rule.max_target_health_pct}%")
    return ",".join(facts)


def _trigger_state_text(trigger) -> str:
    if trigger.status == "waiting":
        return f"waiting {_milliseconds_band(trigger.wait_ms)}"
    if trigger.status == "blocked":
        return f"blocked by {trigger.blocked_by}"
    return trigger.status


def _trigger_name(trigger_id: str, sheet: Sheet) -> str:
    if trigger_id.startswith("spell_"):
        spell_id = int(trigger_id.removeprefix("spell_"))
        ability = next((item for item in sheet.abilities
                        if item.spell == spell_id), None)
        return ability.name if ability is not None else f"spell {spell_id}"
    if trigger_id.startswith("health_phase_"):
        return "health phase " + trigger_id.removeprefix("health_phase_").replace("_", " ")
    if trigger_id.startswith("health_event_"):
        return "health event " + trigger_id.removeprefix("health_event_").replace("_", " ")
    if trigger_id.startswith("aggro_event_"):
        return "aggro event " + trigger_id.removeprefix("aggro_event_").replace("_", " ")
    return _TRIGGER_NAMES.get(trigger_id, trigger_id.replace("_", " "))


def _behavior_facts(req: DecideRequest, cat: TargetCatalog,
                    policy: SelectedPolicy, sheet: Sheet) -> tuple[str, str]:
    """All current triggers plus a bounded ordered transition window for P0."""
    st = req.state
    spell_by_id = {spell.id: spell for spell in st.spells}
    lines: list[str] = []
    for trigger in st.behavior.triggers:
        name = _trigger_name(trigger.id, sheet)
        dynamic = ""
        if trigger.id == "aggro":
            dynamic = f"; attack{len(st.attack_targets)}"
        elif trigger.id == "aggro_talk":
            dynamic = f"; {policy.aggro_talk.chance_pct}% on aggro,once/combat"
        elif trigger.id == "ambient_talk":
            dynamic = (f"; {policy.ambient_talk.chance_pct}% out of combat,"
                       f"first{_range_text(policy.ambient_talk.initial_ms)}/"
                       f"repeat{_range_text(policy.ambient_talk.repeat_ms)}")
        elif trigger.id == "help_call":
            dynamic = (f"; kin{len(st.call_help_targets)}/"
                       f"engaged foes{len(cat.options['engaged'])}")
        elif trigger.id == "help_answer":
            dynamic = f"; requests{len(st.help_requests)}"
        elif trigger.id == "flee":
            flee = policy.flee
            if not flee.enabled:
                dynamic = "; disabled by policy"
                lines.append(f"{name}#{trigger.generation} {_trigger_state_text(trigger)}{dynamic}")
                continue
            conditions = []
            if flee.require_in_combat:
                conditions.append("combat")
            conditions.append(f"hp<={flee.max_health_pct}%")
            if flee.require_not_casting:
                conditions.append("not casting")
            if flee.require_engaged:
                conditions.append("engaged")
            if flee.max_distance_yards is not None:
                conditions.append(f"target<={flee.max_distance_yards:g}yd")
            if flee.once_per_epoch:
                conditions.append("once/epoch")
            dynamic = f"; {','.join(conditions)},targets{len(st.flee_targets)}"
        elif trigger.id == "routine":
            dynamic = "; peaceful"
        elif trigger.id == "return":
            dynamic = "; engine overlay"
        elif trigger.id.startswith("spell_"):
            spell_id = int(trigger.id.removeprefix("spell_"))
            spell = spell_by_id.get(spell_id)
            physical = len(spell.valid_targets) if spell is not None else 0
            ability = next(a for a in sheet.abilities if a.spell == spell_id)
            rule = policy.spell(req.npc.entry, spell_id)
            legal = len(cat.options.get(_intent_for_ability(ability, policy), ()))
            dynamic = (f"; {_spell_rule_text(rule)}; "
                       f"physical{physical}/eligible{legal}")
        elif trigger.id.startswith("health_phase_"):
            dynamic = "; exact encounter choreography"
        elif trigger.id.startswith("health_event_"):
            dynamic = "; stock health window"
        elif trigger.id.startswith("aggro_event_"):
            dynamic = "; exact combat entry,once/combat"
        lines.append(f"{name}#{trigger.generation} {_trigger_state_text(trigger)}{dynamic}")

    events = []
    visible_events = st.behavior.events[-3:]
    terminal = {"rejected", "failed", "cancelled"}
    latest_terminal_seq = max(
        (event.seq for event in visible_events if event.kind in terminal
         and event.reason), default=None)
    for event in visible_events:
        who = (_trigger_name(event.trigger_id, sheet) if event.trigger_id is not None
               else "system")
        generation = f"#{event.generation}" if event.generation else ""
        item = f"{event.seq} {who}{generation} {event.kind}"
        if event.seq == latest_terminal_seq and event.reason:
            reason = " ".join(event.reason.split())[:32]
            item += f" ({reason})"
        events.append(item)
    if events:
        clipped = (f"last {len(events)} of {len(st.behavior.events)}" if
                   len(visible_events) < len(st.behavior.events) else "all")
        window = (f"events{st.behavior.floor_seq}-{st.behavior.seq} {clipped}: "
                  + "; ".join(events))
    else:
        window = "events seq0 none"
    return "; ".join(lines), window


def _compact_behavior_facts(req: DecideRequest, cat: TargetCatalog,
                            policy: SelectedPolicy, sheet: Sheet
                            ) -> tuple[str, str]:
    """Lossless compact projection for the split intent head.

    Every current trigger keeps its wire id, generation, status and blocker.
    Physical/eligible target counts are retained. Static policy predicates and
    descriptive names are omitted here because the server already folds them
    into that current status and the typed action criteria: a spell is not an
    offered action before its trigger is eligible. This is deterministic
    prompt compaction, not removal of a trigger or an executable choice.
    """
    st = req.state
    spell_by_id = {spell.id: spell for spell in st.spells}

    def status(trigger) -> str:
        if trigger.status == "waiting":
            return f"waiting({_milliseconds_band(trigger.wait_ms)})"
        if trigger.status == "blocked":
            return f"blocked({trigger.blocked_by})"
        return trigger.status

    lines: list[str] = []
    for trigger in st.behavior.triggers:
        facts: list[str] = []
        if trigger.id == "aggro":
            facts.append(f"attack={len(st.attack_targets)}")
        elif trigger.id == "help_call":
            facts.extend((f"kin={len(st.call_help_targets)}",
                          f"engaged={len(cat.options['engaged'])}"))
        elif trigger.id == "help_answer":
            facts.append(f"requests={len(st.help_requests)}")
        elif trigger.id == "flee":
            flee = policy.flee
            if not flee.enabled:
                facts.append("policy=disabled")
            else:
                facts.append(f"targets={len(st.flee_targets)}")
        elif trigger.id.startswith("spell_"):
            spell_id = int(trigger.id.removeprefix("spell_"))
            ability = next(a for a in sheet.abilities if a.spell == spell_id)
            spell = spell_by_id.get(spell_id)
            physical = len(spell.valid_targets) if spell is not None else 0
            legal = len(cat.options.get(_intent_for_ability(ability, policy), ()))
            facts.append(f"physical={physical}/eligible={legal}")
        suffix = "," + ",".join(facts) if facts else ""
        lines.append(f"{trigger.id}#{trigger.generation}={status(trigger)}{suffix}")

    visible_events = st.behavior.events[-3:]
    terminal = {"rejected", "failed", "cancelled"}
    latest_terminal_seq = max(
        (event.seq for event in visible_events if event.kind in terminal
         and event.reason), default=None)
    events: list[str] = []
    for event in visible_events:
        trigger_id = event.trigger_id or "system"
        generation = f"#{event.generation}" if event.generation else ""
        item = f"{event.seq}:{trigger_id}{generation}={event.kind}"
        if event.seq == latest_terminal_seq and event.reason:
            reason = " ".join(event.reason.split())[:32]
            item += f"({reason})"
        events.append(item)
    if events:
        clipped = (f"last{len(events)}/{len(st.behavior.events)}" if
                   len(visible_events) < len(st.behavior.events) else "all")
        window = (f"events{st.behavior.floor_seq}-{st.behavior.seq},{clipped}:"
                  + ";".join(events))
    else:
        window = "events=none,seq=0"
    return ";".join(lines), window


def build_state(req: DecideRequest, *, policy: Policy | SelectedPolicy | None = None,
                sheet: Sheet | None = None,
                catalog: TargetCatalog | None = None) -> dict:
    """Плоские квантованные английские строки для ключа и вопроса Jev.

    Имя персонажа игрока сюда НЕ попадает: это строка, выбранную игроком,
    то есть вектор инъекции. Игрок описан ролью и расстоянием.
    """
    st = req.state
    policy = _policy(req, policy)
    sheet = _sheet(req, sheet)
    to_home = geometry.distance_2d(st.position, st.home)
    where = "at your post" if to_home <= 3.0 else f"{to_home:.0f} yd from your post"
    if geometry.is_in_water(st.position, st.home):
        where = "in the shallows, " + where
    self_line = f"{_hp_band(st.health.current, st.health.max)}; {where}"
    self_line += "; fleeing" if st.fleeing else "; fighting" if st.in_combat else "; free to act"
    cat = _catalog(req, policy, sheet) if catalog is None else catalog
    codes = _ability_codes(req, sheet)
    spell_by_id = {spell.id: spell for spell in st.spells}
    capabilities: dict[str, list[str]] = {label: []
                                          for label in cat.raw_by_label
                                          if label.startswith(("e", "a"))}
    for label in cat.options["attack"]:
        capabilities[label].append("A")
    for ability in sheet.abilities:
        for label in cat.options.get(_intent_for_ability(ability, policy), ()):
            capabilities[label].append(codes[ability.spell])

    enemies: list[str] = []
    for unit in st.enemies:
        label = cat.labels_by_raw[unit.id]
        flags = (["engaged"] if unit.engaged else ["seen"])
        if unit.attacking_ally:
            flags.append("attacks kin")
        flags.extend(capabilities[label])
        enemies.append(f"{label} {_unit_role(unit)} {_short_hp(unit.health_pct)} "
                       f"{_short_distance(unit.distance)} {' '.join(flags)}")
    self_pct = 0 if st.health.max <= 0 else 100 * st.health.current / st.health.max
    kin = [f"a0 self {_short_hp(self_pct)} "
           f"{' '.join(capabilities['a0'])}".rstrip()]
    for ally in _all_kin(req):
        label = cat.labels_by_raw[ally.id]
        flags = (["fighting"] if ally.in_combat else ["free"])
        flags.extend(capabilities[label])
        kin.append(f"{label} {_kin_role(ally.entry, dynamic=policy.dynamic)} {_short_hp(ally.health_pct)} "
                   f"{_short_distance(ally.distance)} {' '.join(flags)}")

    magic: list[str] = []
    for ability in sheet.abilities:
        spell = spell_by_id.get(ability.spell)
        if spell is None:
            continue
        aura = [cat.labels_by_raw[raw] for raw in spell.aura_targets]
        availability = ('physically ready' if spell.ready and spell.cooldown_ms == 0 else
                        f"physically {'ready' if spell.ready else 'unavailable'}, "
                        f"cooldown {_milliseconds_band(spell.cooldown_ms)}")
        magic.append(f"{codes[ability.spell]}={ability.name}: "
                     f"{availability}, aura {','.join(aura) if aura else 'none'}")

    triggers, events = _behavior_facts(req, cat, policy, sheet)

    legend_parts = ["A=can attack"]
    legend_parts.extend(f"{codes[a.spell]}={a.name}" for a in sheet.abilities)
    state: dict[str, Any] = {
        "self": self_line,
        "legend": "; ".join(legend_parts),
        "enemies": "; ".join(enemies) if enemies else "none",
        "kin": "; ".join(kin),
        "scene": f"{len(st.attackers)} attackers; {len(_all_kin(req))} observed kin",
        "magic": "; ".join(magic) if magic else "no spell facts",
        "triggers": triggers,
        "trigger_events": events,
    }
    if sheet.character_brief is None:
        state["you"] = sheet.you
    else:
        state["character"] = sheet.character_brief.prompt_state()
    if st.last_actions:
        # Не "<полоса> ago"/"for <полоса>" — не должно читаться как
        # длительность эпизода (см. LAST_ACTION_* выше); честно "сколько
        # прошло с тех пор".
        band = _last_action_band(st.seconds_since_last_action)
        state["last"] = f"{st.last_actions[-1]}, nothing since - {band}"
    if st.help_requests:
        help_lines = []
        for i, request in enumerate(st.help_requests, 1):
            requester = request.requester
            target = request.target
            requester_state = "fighting" if requester.in_combat else "free"
            target_state = "engaged" if target.engaged else "seen"
            if target.attacking_ally:
                target_state += ", attacks kin"
            # r/q aliases intentionally do not borrow an unrelated truncated
            # a/e label. Each inbox item remains self-contained.
            help_lines.append(
                f"h{i}: r{i} {_kin_role(requester.entry, dynamic=policy.dynamic)} {_short_hp(requester.health_pct)} "
                f"{_short_distance(requester.distance)} {requester_state} asks against "
                f"q{i} {_unit_role(target)} {_short_hp(target.health_pct)} "
                f"{_short_distance(target.distance)} {target_state}; "
                f"{_ttl_band(request.expires_in_ms)}")
        state["help"] = "; ".join(help_lines)
    if not st.in_combat and not st.attack_targets and (st.routine.active or st.routine.can_resume):
        state["routine"] = _routine_fact(req)
    return state


def _head_specs(req: DecideRequest, allowed: list[Intent], cat: TargetCatalog,
                sheet: Sheet, policy: SelectedPolicy
                ) -> dict[str, tuple[tuple[str, ...], str]]:
    """head -> (labels, meaningful instruction)."""
    specs: dict[str, tuple[tuple[str, ...], str]] = {}

    def add(name: str, values: tuple[str, ...], instruction: str) -> None:
        if len(values) > 1:
            specs[name] = (values, instruction)

    if "attack" in allowed:
        add("attack_target", cat.options["attack"],
            "Choose which attackable enemy to attack.")
    for ability in sheet.abilities:
        intent = _intent_for_ability(ability, policy)
        if intent not in allowed:
            continue
        values = cat.options[intent]
        add(f"{ability.slug}_target", values,
            f"Choose a valid target for {ability.name}.")
    call_values = cat.options["call_recipient"]
    flee_values = cat.options["flee_recipient"]
    if "call_help" in allowed:
        add("call_help_recipient", call_values,
            "Choose which eligible kinsman to ask for help.")
    if "flee_for_help" in allowed:
        add("flee_assist_recipient", flee_values,
            "Choose which reachable kinsman to ask and run toward.")
    if "call_help" in allowed:
        add("engaged_enemy", cat.options["engaged"],
            "Choose which engaged enemy the help action concerns.")
    if any(intent in allowed for intent in ("flee", "flee_for_help")):
        add("flee_enemy", cat.options["flee"],
            "Choose which enemy to flee from.")
    if "answer_help" in allowed:
        add("help_request", cat.options["help"],
            "Choose which exact help request to answer.")
    return specs


def build_questions(req: DecideRequest, *, policy: Policy | SelectedPolicy | None = None,
                    sheet: Sheet | None = None,
                    catalog: TargetCatalog | None = None) -> dict:
    """Варианты строит гейтвей, и в список попадает только исполнимое.

    Дело не в экономии токенов: у модели не должно быть физической
    возможности выбрать невозможное, иначе появляется ветка «выбрано
    неисполнимое», которую нечем закрыть.
    """
    st = req.state
    policy = _policy(req, policy)
    sheet = _sheet(req, sheet)
    # list[Intent], не list[str]: без аннотации pyright вывел бы list[str] из
    # литералов, и опечатка в .append() не поймалась бы, хотя дальше уронит
    # KeyError на INTENT_CRITERIA[k].
    peaceful_routine = (not st.in_combat and not st.attack_targets
                         and (st.routine.active or st.routine.can_resume))
    due_phase = _eligible_health_phase(req, policy)
    due_events = _eligible_health_events(req, policy)
    due_aggro = _eligible_aggro_events(req, policy)
    allowed: list[Intent] = ["hold"]
    if peaceful_routine:
        allowed.append("follow_routine")
    cat = _catalog(req, policy, sheet) if catalog is None else catalog
    if cat.options["attack"]:
        allowed.append("attack")
    for ability in sheet.abilities:
        if _ability_is_offered(ability, req, cat, policy):
            allowed.append(_intent_for_ability(ability, policy))
    if st.in_combat:
        allowed.append("stop_fighting")
    if cat.options["flee"]:
        allowed.append("flee")
    if cat.options["engaged"]:
        if cat.options["call_recipient"]:
            allowed.append("call_help")
        if cat.options["flee"] and cat.options["flee_recipient"]:
            allowed.append("flee_for_help")
    if cat.options["help"]:
        allowed.append("answer_help")
    stock_talk = _eligible_stock_talk(req, policy)
    if stock_talk is not None:
        allowed.append("stock_talk")
    if due_phase is not None:
        allowed.append("start_health_phase")
    for event in due_events:
        allowed.append(f"health_event_{event.id}")

    if due_aggro:
        allowed = [f"aggro_event_{event.id}" for event in due_aggro]
    elif (stock_talk is not None and stock_talk[0] == "aggro_talk"
          and policy.aggro_talk.chance_pct == 100 and due_phase is None
          and not due_events):
        # SmartAI's 100% one-time aggro line is due now. Treat it like the
        # other mandatory source events: ask Jev to execute that event before
        # another ATTACK choice can continually starve it. No local speech
        # fallback exists if Jev/provider cannot answer.
        allowed = ["stock_talk"]
    # A 100%-chance source timer that is already eligible is an action due
    # now, not another patrol preference.  Keep all such OOC moves as Jev
    # choices; a usable model answer is still required to execute any cast.
    due_ooc_abilities = [
        _intent_for_ability(ability, policy)
        for ability, rule in _ability_rules(policy, sheet)
        if (not st.in_combat and rule.timer_mode == "timer"
            and rule.ticks_out_of_combat and rule.chance_pct == 100
            and _intent_for_ability(ability, policy) in allowed)
    ]
    if (due_ooc_abilities and not (due_aggro or due_events or due_phase
                                   or stock_talk)):
        allowed = due_ooc_abilities
    criteria = {k: INTENT_CRITERIA[k] for k in allowed
                if k in INTENT_CRITERIA}
    due_abilities = False
    for ability in sheet.abilities:
        intent = _intent_for_ability(ability, policy)
        if intent in allowed:
            # An offered ability is not merely physically possible: its
            # pinned timer/predicate trigger is eligible now.  Keep the
            # decision with Jev, but make that original-behaviour meaning
            # explicit so a redundant generic ATTACK or routine resumption
            # does not hide a due SmartAI action (as happened for Gilnid and
            # his Engineer).  Jev still chooses from every offered intent.
            criteria[intent] = f"Due: {ability.does}"
            due_abilities = True
    if "flee_for_help" in criteria:
        criteria["flee_for_help"] = f"Ask eligible kin and run to them. {sheet.flee}"

    intent_instruction = "Choose eligible action."
    if sheet.character_brief is not None:
        intent_instruction = "Choose only listed eligible actions and targets."
    if due_aggro:
        for event in due_aggro:
            criteria[f"aggro_event_{event.id}"] = (
                f"Due original aggro event {event.id.replace('_', ' ')}; execute its configured sequence.")
        intent_instruction = "Choose the due original aggro event now."
    elif due_events:
        for event in due_events:
            effects = "cast then use the stock line" if len(event.actions) == 2 else "use the stock line"
            criteria[f"health_event_{event.id}"] = (
                f"Due stock health event {event.id.replace('_', ' ')}: {effects}.")
        intent_instruction = "Choose among due stock health events before ordinary combat actions."
    elif due_phase is not None:
        criteria["start_health_phase"] = (
            f"Due now: begin original phase {due_phase.id.replace('_', ' ')}.")
        intent_instruction = "Prefer the due original encounter phase to other actions."
    elif stock_talk is not None:
        criteria["stock_talk"] = (
            "Due now: use the original one-time creature line before other combat actions.")
        intent_instruction = "Prefer the due original creature line to attack/hold."
    elif due_ooc_abilities:
        intent_instruction = "Choose one due original out-of-combat move now."
    elif due_abilities:
        intent_instruction = (
            "Prefer due original moves to attack, hold, or follow routine."
            if "follow_routine" in allowed else
            "Prefer due original moves to attack/hold.")
    if ("hold" in criteria and "stop_fighting" in criteria
            and not st.enemies and not cat.options["attack"]
            and not cat.options["flee"]):
        # A creature can remain flagged in combat after its target flees or
        # disappears.  Explain the state effect; Jev still chooses the intent.
        criteria["hold"] = "Continue current activity; keep the combat state."
        criteria["stop_fighting"] = (
            "End the stale combat state and stay here; no visible or eligible enemy remains.")
        intent_instruction += (
            " No enemy is visible or attackable. Prefer stop_fighting to end"
            " stale combat; hold keeps you locked in combat without a target.")
    questions = {
        "intent": {
            "type": "choice",
            "instructions": intent_instruction,
            "criteria": criteria,
        },
        "afraid": {"type": "noul", "instructions": "Are you afraid right now?"},
        "danger": {
            "type": "score",
            "instructions": "How dangerous is your situation?",
            "criteria": DANGER_SCALE,
        },
    }
    for name, (values, instruction) in _head_specs(
            req, allowed, cat, sheet, policy).items():
        def description(value: str) -> str:
            if value.startswith("e"):
                return f"Enemy {value}."
            if value == "a0":
                return "Self a0."
            if value.startswith("a"):
                return f"Kin {value}."
            return f"Request {value}."
        questions[name] = {
            "type": "choice",
            "instructions": instruction,
            "criteria": {value: description(value) for value in values},
        }
    return questions


def build_prompt_batches(req: DecideRequest, *, state: dict | None = None,
                         questions: dict | None = None,
                         policy: Policy | SelectedPolicy | None = None,
                         sheet: Sheet | None = None,
                         catalog: TargetCatalog | None = None) -> list[PromptBatch]:
    """One-call fast path, otherwise three disjoint parallel head groups."""
    policy = _policy(req, policy)
    sheet = _sheet(req, sheet)
    cat = _catalog(req, policy, sheet) if catalog is None else catalog
    state = (build_state(req, policy=policy, sheet=sheet, catalog=cat)
             if state is None else state)
    questions = (build_questions(req, policy=policy, sheet=sheet, catalog=cat)
                 if questions is None else questions)

    def batch(name: str, selected_state: dict, selected_questions: dict
              ) -> PromptBatch:
        payload = {"model": JEV_MODEL, "state": selected_state,
                   "questions": selected_questions}
        return PromptBatch(name, selected_state, selected_questions,
                           _estimate_tokens(payload))

    full = batch("all", state, questions)
    if full.own_tokens <= JEV_MAX_OWN_TOKENS:
        return [full]

    intent_heads = {"intent", "afraid", "danger"}
    hostile_heads = {"attack_target", "engaged_enemy", "flee_enemy"}
    kin_heads = {"call_help_recipient", "flee_assist_recipient", "help_request"}
    for ability in sheet.abilities:
        head = f"{ability.slug}_target"
        if ability.target == "enemy":
            hostile_heads.add(head)
        elif ability.target == "ally":
            kin_heads.add(head)

    def questions_for(names: set[str]) -> dict:
        return {name: value for name, value in questions.items() if name in names}

    def band_counts(values: list[float]) -> str:
        counts: dict[str, int] = {}
        for value in values:
            band = _short_hp(value)
            counts[band] = counts.get(band, 0) + 1
        return ",".join(f"{count} {band}" for band, count in counts.items()) or "none"

    def range_counts(values: list[float]) -> str:
        counts: dict[str, int] = {}
        for value in values:
            band = _compact_distance(value)
            counts[band] = counts.get(band, 0) + 1
        return ",".join(f"{count} {band}" for band, count in counts.items()) or "none"

    intent_help = []
    for i, request in enumerate(req.state.help_requests, 1):
        intent_help.append(
            f"h{i}:{_compact_kin_role(request.requester.entry, dynamic=policy.dynamic)}/"
            f"{_short_hp(request.requester.health_pct)}>"
            f"{'p' if request.target.is_player else 'npc'}/"
            f"{_short_hp(request.target.health_pct)},"
            f"{_ttl_band(request.expires_in_ms).replace('expires in ', 'ttl')}")
    compact_triggers, compact_events = _compact_behavior_facts(
        req, cat, policy, sheet)
    intent_state: dict[str, Any] = {
        "self": state["self"],
        "enemy_summary": (
            f"hp {band_counts([unit.health_pct for unit in req.state.enemies])}; "
            f"range {range_counts([unit.distance for unit in req.state.enemies])}; "
            f"{len(req.state.attackers)} attackers, "
            f"{sum(unit.is_player for unit in req.state.enemies)} players, "
            f"{len(cat.options['engaged'])} engaged, "
            f"{sum(unit.attacking_ally for unit in req.state.enemies)} attack kin; "
            f"attack {len(cat.options['attack'])}, flee {len(cat.options['flee'])}"),
        "kin_summary": (
            f"kin health {band_counts([0 if req.state.health.max <= 0 else 100 * req.state.health.current / req.state.health.max]
                                           + [ally.health_pct for ally in _all_kin(req)])}; "
            f"call {len(cat.options['call_recipient'])}, "
            f"run-to {len(cat.options['flee_recipient'])}"),
        "triggers": compact_triggers,
        "trigger_events": compact_events,
    }
    if intent_help:
        intent_state["help"] = ";".join(intent_help)
    if sheet.character_brief is None:
        intent_state["role"] = _intent_role(
            req.npc.entry, dynamic=policy.dynamic, sheet=sheet)
    else:
        intent_state["character"] = sheet.character_brief.prompt_line()
    if "routine" in state:
        intent_state["routine"] = state["routine"]
    if "last" in state:
        intent_state["last"] = state["last"]

    # P0 sees the whole semantic situation but none of the bulky target
    # heads. P1/P2 project only facts needed to compare their candidates.
    p0_questions = questions_for(intent_heads)
    compact_intents = {
        "hold": "Continue current activity.",
        "follow_routine": "Resume usual routine.",
        "attack": "Attack chosen enemy.",
        "flee_for_help": "Ask kin; run to them.",
        "call_help": "Ask kin; stay here.",
        "answer_help": "Answer exact request.",
        "flee": "Flee chosen enemy alone.",
        "stop_fighting": "Stop fighting.",
    }
    intent_question = p0_questions.get("intent")
    if intent_question is not None:
        p0_questions["intent"] = {
            **intent_question,
            "criteria": {
                key: (value if key in {"hold", "stop_fighting"}
                      and value != INTENT_CRITERIA[key] else compact_intents.get(key, value))
                for key, value in intent_question["criteria"].items()},
        }
    p0 = batch("intent", intent_state, p0_questions)
    enemy_codes = {"A"} | {_ability_codes(req, sheet)[ability.spell]
                           for ability in sheet.abilities if ability.target == "enemy"}
    ally_codes = {_ability_codes(req, sheet)[ability.spell]
                  for ability in sheet.abilities if ability.target == "ally"}
    legend_parts = state["legend"].split("; ")
    magic_parts = state["magic"].split("; ")
    p1_state = {
        "role": f"{_kin_role(req.npc.entry, dynamic=policy.dynamic)}: choose hostile targets.",
        "self": state["self"], "scene": state["scene"],
        "legend": "; ".join(part for part in legend_parts
                             if part.split("=", 1)[0] in enemy_codes),
        "enemies": state["enemies"],
        "magic": "; ".join(part for part in magic_parts
                            if part.split("=", 1)[0] in enemy_codes - {"A"}),
    }
    self_pct = (0 if req.state.health.max <= 0 else
                100 * req.state.health.current / req.state.health.max)
    to_home = geometry.distance_2d(req.state.position, req.state.home)
    compact_where = f"{_compact_distance(to_home)} from post"
    if geometry.is_in_water(req.state.position, req.state.home):
        compact_where = "shallows; " + compact_where
    compact_self = f"hp {_short_hp(self_pct)}; {compact_where}"
    compact_self += ("; fleeing" if req.state.fleeing else
                     "; fighting" if req.state.in_combat else "; free")
    kin_capabilities: dict[str, list[str]] = {
        label: [] for label in cat.raw_by_label if label.startswith("a")}
    for ability in sheet.abilities:
        if ability.target != "ally":
            continue
        code = _ability_codes(req, sheet)[ability.spell]
        for label in cat.options.get(_intent_for_ability(ability, policy), ()):
            kin_capabilities[label].append(code)
    compact_kin = [f"a0 self {_short_hp(self_pct)} "
                   f"{' '.join(kin_capabilities['a0'])}".rstrip()]
    for ally in _all_kin(req):
        label = cat.labels_by_raw[ally.id]
        flags = ["fighting" if ally.in_combat else "free"]
        flags.extend(kin_capabilities[label])
        compact_kin.append(
            f"{label} {_compact_kin_role(ally.entry, dynamic=policy.dynamic)} {_short_hp(ally.health_pct)} "
            f"{_compact_distance(ally.distance)} {' '.join(flags)}")
    p2_state = {
        "role": f"{_kin_role(req.npc.entry, dynamic=policy.dynamic)}: kin/heal/help",
        "self": compact_self,
        "kin": "; ".join(compact_kin),
        "help": state.get("help", "none").replace(
            " asks against ", " asks vs ").replace("enemy creature", "creature"),
    }
    if ally_codes:
        ally_magic: list[str] = []
        for ability in sheet.abilities:
            if ability.target != "ally":
                continue
            spell = _spell_by_id(req, ability.spell)
            if spell is None:
                continue
            code = _ability_codes(req, sheet)[ability.spell]
            aura = [cat.labels_by_raw[raw] for raw in spell.aura_targets]
            ally_magic.append(
                f"{code}={ability.name} "
                f"{'ready' if spell.ready else 'unavailable'}; "
                f"cd {_compact_milliseconds_band(spell.cooldown_ms)}; "
                f"aura {','.join(aura) if aura else 'none'}")
        p2_state["magic"] = "; ".join(ally_magic)
    result = [p0]
    hostile = questions_for(hostile_heads)
    kin = questions_for(kin_heads)
    # В P2 каждая метка уже раскрыта полной строкой kin/help. Повтор
    # ``aN: Kin aN.`` на 13 целях не добавляет смысла, но съедает лимит.
    kin = {
        name: ({**question,
                "criteria": {label: label for label in question["criteria"]}}
               if question.get("type") == "choice" else question)
        for name, question in kin.items()
    }
    if hostile:
        result.append(batch("hostile_targets", p1_state, hostile))
    if kin:
        result.append(batch("kin_help_targets", p2_state, kin))
    oversized = [item for item in result
                 if item.own_tokens > JEV_MAX_OWN_TOKENS]
    if oversized:
        detail = ",".join(f"{item.name}={item.own_tokens}" for item in oversized)
        raise JevUnavailable(
            f"parallel own tokens over {JEV_MAX_OWN_TOKENS}: {detail}")
    return result


def build_decision_plan(req: DecideRequest,
                        policy: Policy | None = None) -> DecisionPlan:
    """Freeze every interpretation needed after the provider answers."""
    selected = _policy(req, policy)
    sheet = load_sheet(req.npc.entry)
    _validate_policy_snapshot(req, selected, sheet)
    catalog = _catalog(req, selected, sheet)
    state = build_state(req, policy=selected, sheet=sheet, catalog=catalog)
    questions = build_questions(req, policy=selected, sheet=sheet, catalog=catalog)
    ability_spells: dict[str, int] = {
        _intent_for_ability(ability, selected): ability.spell
        for ability in sheet.abilities}
    due_phase = _eligible_health_phase(req, selected)
    stock_talk = _eligible_stock_talk(req, selected)
    if stock_talk is not None:
        trigger_id, text_group = stock_talk
        ability_spells["__stock_talk_group"] = text_group
        ability_spells["__stock_talk_ambient"] = int(trigger_id == "ambient_talk")
    batches = build_prompt_batches(
        req, state=state, questions=questions, policy=selected, sheet=sheet,
        catalog=catalog)
    return DecisionPlan(state, questions, catalog, ability_spells,
                        due_phase.id if due_phase is not None else None, batches,
                        selected.revision)


def _read_key() -> str:
    try:
        key = JEV_KEY_PATH.read_text(encoding="utf-8").strip()
    except OSError:
        raise JevUnavailable("no key file")
    if not key:
        raise JevUnavailable("empty key file")
    return key


def key_present() -> bool:
    """Ключ на месте и не пуст — не раскрывая его значение.

    Публичная обёртка над `_read_key()` для вызывающих снаружи модуля
    (`/healthz`): правило "что считается наличием ключа" живёт только здесь.
    """
    try:
        _read_key()
    except JevUnavailable:
        return False
    return True


def _target_answer(answers: Any, cat: TargetCatalog, values: tuple[str, ...],
                   head: str) -> str:
    """Resolve one physical option, requiring Jev only for a real choice."""
    if not values:
        raise RuntimeError(f"offered target action has empty {head}")
    if len(values) == 1:
        return cat.raw_by_label[values[0]]
    result = provider.read_choice(answers, head)
    if isinstance(result, provider.Absent):
        raise JevTargetUnavailable(f"no target choice: {head}")
    if isinstance(result, provider.Bad):
        raise JevTargetUnavailable(f"bad target answer shape: {head}")
    if result not in values:
        raise JevTargetUnavailable(f"target not offered: {head}={result}")
    return cat.raw_by_label[result]


def _selected_targets(intent: Intent, cat: TargetCatalog, answers: Any
                      ) -> dict[str, str]:
    """Validate only the heads required by the chosen intent."""
    if intent == "attack":
        return {"target": _target_answer(
            answers, cat, cat.options["attack"], "attack_target")}
    if intent.startswith(("cast_", "use_")):
        slug = intent.split("_", 1)[1]
        return {"target": _target_answer(
            answers, cat, cat.options[intent], f"{slug}_target")}
    if intent == "call_help":
        return {
            "recipient": _target_answer(
                answers, cat, cat.options["call_recipient"], "call_help_recipient"),
            "target": _target_answer(
                answers, cat, cat.options["engaged"], "engaged_enemy"),
        }
    if intent == "flee_for_help":
        return {
            "recipient": _target_answer(
                answers, cat, cat.options["flee_recipient"], "flee_assist_recipient"),
            "target": _target_answer(
                answers, cat, cat.options["flee"], "flee_enemy"),
        }
    if intent == "flee":
        return {"target": _target_answer(
            answers, cat, cat.options["flee"], "flee_enemy")}
    if intent == "answer_help":
        return {"help_request_id": _target_answer(
            answers, cat, cat.options["help"], "help_request")}
    return {}


def _to_action(intent: Intent, req: DecideRequest, rng: random.Random,
               selected: dict[str, str], ability_spells: dict[str, int],
               health_phase_id: str | None = None) -> Action:
    """Перевести типизированный выбор, не принимая координаты от модели."""
    st = req.state
    if intent == "hold":
        return Idle()
    if intent == "follow_routine":
        if st.routine.can_resume:
            return ResumeRoutine()
        if st.routine.active:
            return Idle()
        raise RuntimeError("offered routine is neither active nor resumable")
    if intent == "attack":
        return Attack(target=selected["target"])
    if intent == "start_health_phase":
        if health_phase_id is None:
            raise RuntimeError("offered health phase has no frozen policy identity")
        trigger_id = f"health_phase_{health_phase_id}"
        return StartPhase(
            phase_id=health_phase_id,
            trigger_generation=_trigger(req, trigger_id).generation)
    if intent.startswith("health_event_"):
        event_id = intent.removeprefix("health_event_")
        trigger_id = f"health_event_{event_id}"
        return HealthEvent(event_id=event_id,
                           trigger_generation=_trigger(req, trigger_id).generation)
    if intent.startswith("aggro_event_"):
        event_id = intent.removeprefix("aggro_event_")
        return AggroEvent(event_id=event_id,
                          trigger_generation=_trigger(req, f"aggro_event_{event_id}").generation)
    if intent in ability_spells:
        spell = ability_spells[intent]
        return Cast(spell=spell, target=selected["target"],
                    trigger_generation=_trigger(req, f"spell_{spell}").generation)
    if intent == "call_help":
        return CallHelp(recipient=selected["recipient"], target=selected["target"])
    if intent == "flee_for_help":
        return FleeForAssist(
            recipient=selected["recipient"], target=selected["target"],
            trigger_generation=_trigger(req, "flee").generation)
    if intent == "flee":
        return Flee(target=selected["target"],
                    trigger_generation=_trigger(req, "flee").generation)
    if intent == "answer_help":
        return AnswerHelp(help_request_id=selected["help_request_id"])
    if intent == "stock_talk":
        trigger_id = ("ambient_talk" if ability_spells.get("__stock_talk_ambient")
                      else "aggro_talk")
        return StockTalk(text_group=ability_spells.get("__stock_talk_group", 0),
                         trigger_id=trigger_id,
                         trigger_generation=_trigger(req, trigger_id).generation)
    if intent == "stop_fighting":
        return StopAttack()
    # По типам недостижимо: шесть if выше исчерпывают Intent, pyright сужает
    # сюда до Never. Строка остаётся на случай, если добавят седьмой Intent
    # и забудут для него ветку — лучше JevUnavailable с уликой, чем None.
    raise JevUnavailable(f"unknown intent {intent}")


def _plain(result):
    """Схлопнуть Bad и Absent (provider.py) в одно и то же «значения нет».

    Годится для метаданных мысли (probability/afraid/danger) и биллинга
    (_usage_number) — они не влияют на выбор действия, поэтому разница
    между «не пришло» и «пришло, но негодно» здесь не нужна.
    """
    return None if isinstance(result, (provider.Absent, provider.Bad)) else result


def _usage_number(usage, key: str):
    """0, если билленное поле отсутствует, null, не числом, неконечным
    числом или сам usage — не словарь.

    `usage.get(key, 0)` не спасает от null: `get` вернёт None для явного
    null, и float(None) уронит decide() необработанным исключением. Негодное
    число (inf/NaN/вне диапазона double) обрабатывается так же: `int(inf)`
    бросает OverflowError, что иначе подписалось бы как "unexpected:
    OverflowError" — наш баг, хотя данные кривые у провайдера. Проверку
    делает provider.read_usage_field; здесь остаётся заменить нечитаемое
    нулём.
    """
    value = _plain(provider.read_usage_field(usage, key))
    return 0 if value is None else value


# Чем заменяется негодное число, где выбросить нельзя, а оставить — значит
# отравить журнал. Строка, не число — это главное. Накрывает и неконечный
# float (inf/NaN), и целое вне диапазона double (оно как раз конечно).
UNUSABLE_NUMBER_MARK = "unusable number: "

# Та же семья пометок, но не про число — закрывает значение не того типа
# по контракту JevResult. Общий префикс — чтобы загрузчик Ф6 отличал наши
# подстановки от данных провайдера одним сравнением.
UNUSABLE_VALUE_MARK = "unusable value: "

# Реальная глубина ответа — 4 уровня (answers.intent.probabilities.hold).
# Предел — не про опрятность записи, а чтобы обход не мог уйти в рекурсию
# глубже, чем выдержит стек интерпретатора: json.loads разбирает куда
# глубже, чем обычный рекурсивный обход.
MAX_ANSWER_DEPTH = 32


def _unusable_numbers_marked(value, depth: int = 0):
    """Заменить негодные числа видимой пометкой, не глубже MAX_ANSWER_DEPTH.

    Проверка — provider.is_usable, та же, что у читателей чисел: "не уронит
    json.dumps(allow_nan=False)" — не то же самое, что "годится". Целое вне
    диапазона double проходит dumps дословно и тихо отравляет запись (его
    молча проглотит и `jq`, и загрузчик выборки Ф6).

    `answers` ложится в журнал дословно и становится обучающей выборкой,
    поэтому замена — не косметика:
      * удалить ключ нельзя — «поля не было» и «поле пришло как NaN» разные
        события;
      * null нельзя по той же причине — читается как «не прислал»;
      * число (0.0 и т.п.) нельзя категорически — неотличимо от настоящего
        измерения и тихо портит выборку;
      * строка — единственный вариант, который и человек, и загрузчик
        заметят как аномалию, а не примут за данные; исходное значение
        остаётся внутри как улика.

    Предел глубины устроен по той же логике: слишком глубокое поддерево
    получает пометку, а не роняет обход.
    """
    if depth > MAX_ANSWER_DEPTH:
        return f"{UNUSABLE_VALUE_MARK}nested deeper than {MAX_ANSWER_DEPTH}"
    if (isinstance(value, (int, float)) and not isinstance(value, bool)
            and not provider.is_usable(value)):
        return f"{UNUSABLE_NUMBER_MARK}{value}"
    if isinstance(value, dict):
        return {k: _unusable_numbers_marked(v, depth + 1) for k, v in value.items()}
    if isinstance(value, list):
        return [_unusable_numbers_marked(v, depth + 1) for v in value]
    return value


def _bill_price(usage, meter: Meter) -> Charge:
    """Прочитать usage.cost и списать — цену провайдера или оценку с причиной.

    Ноль как дефолт был бы хуже отсутствующего предохранителя: если провайдер
    перестанет присылать cost, ноль сделает бюджет неисчерпаемым, и это будет
    выглядеть рабочим, а не сломанным. Поэтому нечитаемая цена заменяется
    оценкой по самому дорогому замеренному запросу.

    Здесь различается только то, что видно из ответа: цены нет, или пришло
    не число double (`{"cost": 1e309}` — валидный JSON, resp.json() отдаёт
    inf; UsableFloat в provider.py отвергает и это, и Python-int вне
    диапазона). «Число годное, но правдоподобна ли цена» решает Budget —
    у порога один владелец, тот, у кого лимит.
    """
    result = provider.read_usage_field(usage, "cost")
    if isinstance(result, provider.Absent):
        return meter.estimate(ESTIMATE_NOT_REPORTED)
    if isinstance(result, provider.Bad):
        return meter.estimate(ESTIMATE_UNUSABLE_NUMBER, reported=result.evidence)
    return meter.price(result)


# Один клиент на процесс, не httpx.post() на каждый вызов: httpx.post
# открывает новый Client и платит полное TCP+TLS рукопожатие на каждое
# решение (замерено: 30-46 мс). httpx.Client потокобезопасен, что здесь
# обязательно: v1_decide синхронный, FastAPI гоняет его в пуле потоков, и
# несколько мурлоков приходят сюда одновременно.
_client_lock = threading.Lock()
_client: httpx.Client | None = None


def _client_for_calls():
    """Общий клиент с раздельными таймаутами фаз (разбор — в config.py)."""
    global _client
    with _client_lock:
        if _client is None:
            _client = httpx.Client(
                timeout=httpx.Timeout(connect=JEV_CONNECT_TIMEOUT_SECONDS,
                                      read=JEV_TIMEOUT_SECONDS,
                                      write=JEV_WRITE_TIMEOUT_SECONDS,
                                      pool=JEV_POOL_TIMEOUT_SECONDS),
                # Четыре bridge worker могут одновременно дать по три
                # части одного решения. Пул ограничен тем же максимумом:
                # ожидание его слота не должно выглядеть как provider lag.
                limits=httpx.Limits(max_connections=JEV_MAX_CONNECTIONS,
                                    max_keepalive_connections=(
                                        JEV_MAX_KEEPALIVE_CONNECTIONS),
                                    keepalive_expiry=JEV_KEEPALIVE_SECONDS),
            )
        return _client


def _model_of(body) -> tuple[str, bool]:
    """(строка для записи, подтвердил ли её провайдер).

    Строка и только строка: поле кладётся в журнал дословно, и {"model":
    NaN} уронило бы Journal.write тем же необработанным ValueError, что и
    NaN в answers.

    Подтверждена только непустая строка ОТ ПРОВАЙДЕРА — подстановка нашего
    JEV_MODEL при отсутствии поля выдала бы её за подтверждённую, а выборке
    Ф6 нужна ровно одна подтверждённая версия. Отсутствие и форма не той
    получают ту же семью пометок "unusable value", запись — не trainable.
    """
    result = provider.read_model(body)
    if isinstance(result, provider.Absent):
        return f"{UNUSABLE_VALUE_MARK}not reported, requested {JEV_MODEL}", False
    if isinstance(result, provider.Bad):
        # _short, как у сторожа: вложенный на тысячи уровней model иначе
        # ложится в запись целиком; для улики хватает начала и длины.
        return f"{UNUSABLE_VALUE_MARK}{_short(result.evidence)}", False
    # strip(): строка из одних пробелов — не имя модели; пропустить её
    # подтверждённой значило бы положить в выборку Ф6 запись без версии.
    if result.strip():
        return result, True
    return f"{UNUSABLE_VALUE_MARK}{_short(result)}", False


# Как у сторожа: без своего обработчика logging.lastResort шлёт в stderr,
# в gateway.out рядом с uvicorn.
_log = logging.getLogger("woj_gateway.jev")


def _provider_did_the_work(status_code: int) -> bool:
    """Выставлен ли счёт: весь класс 2xx, а не литерал 200.

    `!= 200` — список из одного элемента, который уже не раз оказывался
    неполным: 202/204 тоже значат «принято и отработано», просто без
    начисления ни на одном таком вызове. Граница — по смыслу класса: 2xx
    «сделано», 3xx/4xx/5xx «не сделано» (httpx за редиректами не ходит).
    """
    return 200 <= status_code < 300


def _bill_estimate_keeping_cause(meter: Meter, cause: BaseException) -> None:
    """Страховочное начисление оценкой, не затирающее исходную причину.

    Если сторож упадёт здесь, наружу обязана уйти ИСХОДНАЯ причина отказа, а
    не "unexpected: <класс сторожа>" — иначе настоящая причина исчезнет без
    следа. Но и молчать нельзя: оплаченный вызов остался бы неначисленным.
    Поэтому падение сторожа — в лог и в пометку к исходному исключению, а
    исходное исключение уходит дальше как было.
    """
    try:
        meter.estimate(ESTIMATE_NO_PRICE_READ)
    except Exception as billing_exc:
        _log.error("woj: оплаченный вызов Jev НЕ начислен — сторож бюджета "
                   "упал на начислении (исходная причина отказа: %r)", cause,
                   exc_info=billing_exc)
        cause.add_note(f"начисление после этого отказа тоже упало: {billing_exc!r}")


def _aggregate_charge(meters: list[Meter]) -> Charge | None:
    charges = [meter.charge for meter in meters if meter.charge is not None]
    if not charges:
        return None
    if len(charges) == 1:
        return charges[0]
    reasons = [charge.estimate_reason for charge in charges
               if charge.estimate_reason is not None]
    reason = None if not reasons else "parallel: " + ",".join(reasons)
    return Charge(sum(charge.usd for charge in charges), reason)


def _call_batch(batch: PromptBatch, key: str, meter: Meter, req: DecideRequest,
                call_index: int, call_count: int
                ) -> tuple[BatchAnswer | None, dict, str | None]:
    """Execute and account one independent provider call without escaping."""
    call: dict[str, Any] = {
        "name": batch.name,
        "payload": batch.payload,
        "own_tokens_estimate": batch.own_tokens,
    }
    provider_call = {"name": batch.name, "index": call_index,
                     "count": call_count}
    started = time.monotonic()
    provider_sent = False
    response_headers_at: float | None = None
    body_read_started_at: float | None = None
    body_read_finished_at: float | None = None
    failure_stage = "client_setup"

    def elapsed_ms(begin: float, end: float) -> int:
        # monotonic() должен не убывать, но max сохраняет простой числовой
        # контракт телеметрии и при необычных тестовых часах.
        return max(0, int((end - begin) * 1000))

    def stage_timings(now: float) -> dict[str, int | None]:
        """Публичные HTTP-границы, без заголовков, тела и transport internals.

        headers_elapsed_ms — время до получения response headers, а не
        «чистое время модели»: сюда входят pool/connect/write и ожидание
        ответа. body_read_elapsed_ms при ошибке означает время, проведённое
        в незавершённом чтении. post_read_elapsed_ms отделяет закрытие
        response context и локальный JSON, проверку ответа и биллинг после
        полностью прочитанного тела.
        """
        headers_ms = (None if response_headers_at is None else
                      elapsed_ms(started, response_headers_at))
        body_ms = None
        if body_read_started_at is not None:
            body_end = (body_read_finished_at
                        if body_read_finished_at is not None else now)
            body_ms = elapsed_ms(body_read_started_at, body_end)
        post_ms = (None if body_read_finished_at is None else
                   elapsed_ms(body_read_finished_at, now))
        return {
            "headers_elapsed_ms": headers_ms,
            "body_read_elapsed_ms": body_ms,
            "post_read_elapsed_ms": post_ms,
        }

    def cost_observation() -> dict[str, Any] | None:
        if meter.charge is None:
            return None
        return {**_charge_fields(meter.charge), "not_additive": True}

    def emit_result(result: str, *, error: str | None = None,
                    http_status: int | None = None,
                    timeout_phase: str | None = None,
                    timings: dict[str, int | None] | None = None,
                    failed_at: str | None = None) -> None:
        now = time.monotonic()
        fields: dict[str, Any] = {
            "provider_call": provider_call,
            "status": result,
            # Ошибка до готового client не является попыткой HTTP и не может
            # выглядеть в monitor как ответ провайдера. post_send означает
            # только вход в HTTP-вызов после provider_send; connect/pool
            # timeout не доказывает, что байты ушли в сеть.
            "phase": "post_send" if provider_sent else "pre_send",
            "latency_ms": elapsed_ms(started, now),
            **(stage_timings(now) if timings is None else timings),
        }
        if error is not None:
            fields["error"] = error
        if http_status is not None:
            fields["http_status"] = http_status
        if timeout_phase is not None:
            fields["timeout_phase"] = timeout_phase
        if failed_at is not None:
            fields["failure_stage"] = failed_at
        observed = cost_observation()
        if observed is not None:
            fields["cost_observation"] = observed
        _activity_write("provider_result", req, **fields)

    def failed(reason: str, *, timeout_phase: str | None = None
               ) -> tuple[None, dict, str]:
        now = time.monotonic()
        timings = stage_timings(now)
        call["latency_ms"] = elapsed_ms(started, now)
        call["error"] = reason
        call["failure_stage"] = failure_stage
        call.update(timings)
        if timeout_phase is not None:
            call["timeout_phase"] = timeout_phase
        if meter.charge is not None:
            call.update(_charge_fields(meter.charge))
        emit_result("error", error=reason,
                    http_status=call.get("http_status"),
                    timeout_phase=timeout_phase, timings=timings,
                    failed_at=failure_stage)
        return None, call, reason

    try:
        client = _client_for_calls()
        assert client is not None
        _activity_write(
            "provider_send", req,
            provider_call=provider_call,
            question_heads=sorted(batch.questions),
            state_summary=_activity_state_summary(batch.state),
            own_tokens_estimate=batch.own_tokens,
        )
        provider_sent = True
        # Не включаем append activity в исходно измерявшуюся длительность
        # обращения к провайдеру.
        started = time.monotonic()
        failure_stage = "awaiting_headers"
        # stream — публичный API httpx. Вход в context заканчивается после
        # response headers, а явный read() даёт отдельную границу полного
        # тела. Это та же одна HTTP-попытка с теми же phase timeouts.
        with client.stream(
                "POST", JEV_URL, json=batch.payload,
                headers={"Authorization": f"Bearer {key}"}) as resp:
            response_headers_at = time.monotonic()
            # Статус уже доказан response headers и не должен исчезнуть,
            # если чтение тела затем оборвётся или превысит read timeout.
            call["http_status"] = resp.status_code
            body_read_started_at = response_headers_at
            failure_stage = "reading_body"
            resp.read()
            body_read_finished_at = time.monotonic()
            failure_stage = "processing_response"
    except httpx.ConnectTimeout:
        # Соединение не установлено: провайдеру нечего консервативно
        # начислять даже оценкой.
        return failed("timeout: connect", timeout_phase="connect")
    except httpx.PoolTimeout:
        # Свободного локального соединения не было, HTTP не начался.
        return failed("timeout: pool", timeout_phase="pool")
    except httpx.WriteTimeout:
        # Часть запроса могла уйти; цену уже нельзя честно считать нулевой.
        cause = JevUnavailable("timeout: write")
        _bill_estimate_keeping_cause(meter, cause)
        return failed(cause.reason, timeout_phase="write")
    except httpx.ReadTimeout:
        # Запрос ушёл, но ответ не пришёл вовремя: сохраняем прежнюю
        # консервативную оценку стоимости.
        cause = JevUnavailable("timeout: read")
        _bill_estimate_keeping_cause(meter, cause)
        return failed(cause.reason, timeout_phase="read")
    except httpx.TimeoutException:
        cause = JevUnavailable("timeout: other")
        _bill_estimate_keeping_cause(meter, cause)
        return failed(cause.reason, timeout_phase="other")
    except (httpx.ReadError, httpx.RemoteProtocolError,
            httpx.DecodingError, httpx.CloseError) as exc:
        cause = JevUnavailable(f"transport: {type(exc).__name__}")
        _bill_estimate_keeping_cause(meter, cause)
        return failed(cause.reason)
    except httpx.HTTPError as exc:
        return failed(f"transport: {type(exc).__name__}")
    except Exception as exc:
        return failed(f"unexpected: {type(exc).__name__}")
    latency_ms = elapsed_ms(started, body_read_finished_at or time.monotonic())
    call.update({"latency_ms": latency_ms, "http_status": resp.status_code})

    if not _provider_did_the_work(resp.status_code):
        failure_stage = "response_status"
        return failed(f"http {resp.status_code}")

    try:
        try:
            body = resp.json()
        except Exception as exc:
            raise JevUnavailable("not json") from exc
        usage = provider.dig(body, "usage")
        call["usage"] = _unusable_numbers_marked(usage)
        received_answers = _unusable_numbers_marked(provider.dig(body, "answers"))
        call["answers_received"] = received_answers
        answers = received_answers
        if isinstance(answers, dict):
            # Parallel namespaces are disjoint. Ignore unsolicited foreign
            # heads so one provider branch cannot inject into another.
            answers = {head: value for head, value in answers.items()
                       if head in batch.questions}
        model, model_confirmed = _model_of(body)
        call.update({"model": model, "model_confirmed": model_confirmed,
                     "answers_used": answers})
        charge = _bill_price(usage, meter)
        billed_input = int(_usage_number(usage, "input_tokens"))
        billed_output = int(_usage_number(usage, "output_tokens"))
        call.update({
            "billed_input_tokens": billed_input,
            "billed_output_tokens": billed_output,
            **_charge_fields(charge),
        })
        answer = BatchAnswer(answers, model, model_confirmed, charge,
                             billed_input, billed_output, call)
        timings = stage_timings(time.monotonic())
        call.update(timings)
        emit_result("success", http_status=resp.status_code, timings=timings)
        return answer, call, None
    except BaseException as cause:
        _bill_estimate_keeping_cause(meter, cause)
        reason = cause.reason if isinstance(cause, JevUnavailable) else (
            f"unexpected: {type(cause).__name__}")
        if not isinstance(cause, Exception):
            raise
        return failed(reason)


def _charge_fields(charge: Charge) -> dict:
    return {"cost_usd": charge.usd, "cost_is_estimate": charge.is_estimate,
            "cost_estimate_reason": charge.estimate_reason}


def _merge_answers(results: list[BatchAnswer]) -> Any:
    merged: dict[str, Any] = {}
    for result in results:
        if not isinstance(result.answers, dict):
            return result.answers
        overlap = set(merged) & set(result.answers)
        if overlap:
            raise RuntimeError(f"parallel answers repeat heads {sorted(overlap)}")
        merged.update(result.answers)
    return merged


def _finish_answer(req: DecideRequest, plan: DecisionPlan, rng: random.Random,
                   answers: Any,
                   results: list[BatchAnswer], calls: list[dict],
                   meters: list[Meter], logical_latency_ms: int) -> JevResult:
    questions = plan.questions
    state = plan.state
    choice_result = provider.read_choice(answers)
    if isinstance(choice_result, provider.Absent):
        raise JevUnavailable("no intent in answers")
    if isinstance(choice_result, provider.Bad):
        # {"value": "hold"}, ["hold"], 123, true: поле есть, но форма не та.
        raise JevUnavailable("bad answer shape")
    # Гарантированно str здесь: Absent/Bad уже обработаны выше, разницу
    # провёл тип результата чтения, не isinstance-проверка на этой строке.
    choice = choice_result
    if choice not in questions["intent"]["criteria"]:
        # Модель выбрала то, чего ей не предлагали. Применять нельзя: список
        # вариантов и есть проверка исполнимости.
        raise JevUnavailable(f"intent not offered: {choice}")
    if not _is_intent(choice):
        # Недостижимо на практике: "choice предложен" уже влечёт "choice —
        # Intent" (см. INTENT_CRITERIA), но questions — обычный dict, и
        # pyright между строками этого не выведет. Перевод уже доказанного
        # факта в форму, понятную TypeGuard, не новый деловой случай —
        # отсюда та же машинная строка.
        raise JevUnavailable(f"intent not offered: {choice}")

    selected = _selected_targets(choice, plan.catalog, answers)

    # Если _to_action упадёт — баг в нашем коде, не завёрнут в отказ Jev
    # нарочно: Tiers._fresh сведёт его к "unexpected: <класс>", отправляя
    # искать баг у нас, а не в OpenRouter. Запись сохраняет вход целиком,
    # так что падение воспроизводимо и без стека.
    # Keep the long-standing five-argument call shape for every ordinary
    # decision.  Besides making the new phase context explicit only where it
    # is meaningful, this preserves the probe/plugin seam used to prove that
    # unexpected executor failures remain visible as our bugs.
    if choice == "start_health_phase":
        action = _to_action(choice, req, rng, selected, plan.ability_spells,
                            plan.health_phase_id)
    else:
        action = _to_action(choice, req, rng, selected, plan.ability_spells)

    # Метаданные ниже (вероятность, испуг, опасность) не влияют на выбор
    # действия — choice уже провалидирован. Кривой контейнер здесь не стоит
    # нам исполнимого решения: read_* читают защищённо, _plain схлопывает
    # Bad/Absent в одно «значения нет».
    probability = _plain(provider.read_probability(answers, choice))
    afraid = _plain(provider.read_afraid(answers))
    danger = _plain(provider.read_danger(answers))
    # Мысль собирается здесь, потому что эндпоинт не возвращает текста вовсе.
    thought = (f"{choice} p={probability:.2f}" if probability is not None else choice)
    if afraid is not None:
        thought += f" | afraid {afraid:.2f}"
    if danger is not None:
        thought += f" | danger {danger:.1f}/{len(DANGER_SCALE) - 1}"

    models = {result.model for result in results}
    model = next(iter(models)) if len(models) == 1 else "parallel model mismatch"
    model_confirmed = (len(models) == 1
                       and all(result.model_confirmed for result in results))
    charge = _aggregate_charge(meters)
    if charge is None:
        raise RuntimeError("successful provider calls were not charged")
    marker: Marker = ("idle" if choice == "follow_routine"
                      and isinstance(action, Idle) else _intent_marker(choice))
    return JevResult(
        action=action,
        marker=marker,
        thought=thought,
        answers=answers,
        questions_sent=questions,
        state_sent=state,
        model=model,
        model_confirmed=model_confirmed,
        latency_ms=logical_latency_ms,
        charge=charge,
        billed_input_tokens=sum(result.billed_input_tokens for result in results),
        billed_output_tokens=sum(result.billed_output_tokens for result in results),
        own_tokens_estimate=sum(call["own_tokens_estimate"] for call in calls),
        provider_calls=calls,
        prompt_mode="single" if len(calls) == 1 else "parallel",
    )


def decide(req: DecideRequest, rng: random.Random, *,
           meter_factory: Callable[[], Meter],
           plan: DecisionPlan | None = None) -> JevResult:
    """Issue one request when it fits, otherwise up to three parallel calls."""
    key = _read_key()
    plan = build_decision_plan(req) if plan is None else plan
    batches = plan.batches
    meters = [meter_factory() for _ in batches]
    logical_started = time.monotonic()
    if len(batches) == 1:
        outcomes = [_call_batch(batches[0], key, meters[0], req, 0, 1)]
    else:
        # All futures are submitted before any result is read. Leaving the
        # context waits for every started call even when one branch fails.
        with ThreadPoolExecutor(max_workers=len(batches),
                                thread_name_prefix="jev-head") as executor:
            futures = [executor.submit(_call_batch, batch, key, meter, req,
                                       index, len(batches))
                       for index, (batch, meter) in enumerate(
                           zip(batches, meters, strict=True))]
            outcomes = [future.result() for future in futures]

    calls = [call for _, call, _ in outcomes]
    failures = [f"{batch.name}: {reason}"
                for batch, (_, _, reason) in zip(batches, outcomes, strict=True)
                if reason is not None]
    charge = _aggregate_charge(meters)
    if failures:
        reason = (outcomes[0][2] if len(batches) == 1
                  else "; ".join(failures))
        assert reason is not None
        exc_type = JevUnavailable if len(batches) == 1 else JevTargetUnavailable
        raise exc_type(reason, provider_calls=calls, charge=charge)
    results = [answer for answer, _, _ in outcomes if answer is not None]
    try:
        answers = _merge_answers(results)
        logical_latency_ms = int((time.monotonic() - logical_started) * 1000)
        return _finish_answer(req, plan, rng, answers, results, calls, meters,
                              logical_latency_ms)
    except JevTargetUnavailable as exc:
        raise JevTargetUnavailable(exc.reason, provider_calls=calls,
                                   charge=charge) from exc
    except JevUnavailable as exc:
        # In a parallel plan even a malformed intent branch is conservative:
        # all other paid target calls have already completed and are recorded.
        if len(batches) > 1:
            raise JevTargetUnavailable(exc.reason, provider_calls=calls,
                                       charge=charge) from exc
        raise JevUnavailable(exc.reason, provider_calls=calls,
                             charge=charge) from exc
    except Exception as exc:
        exc_type = JevUnavailable if len(batches) == 1 else JevTargetUnavailable
        raise exc_type(f"unexpected: {type(exc).__name__}",
                       provider_calls=calls, charge=charge) from exc
