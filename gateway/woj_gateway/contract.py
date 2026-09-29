"""Строгие контракты 1.7/1.8 между исполнителем мира и гейтвеем."""
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


_STRICT = ConfigDict(extra="forbid", allow_inf_nan=False)

# Маркер — наше краткое имя результата, не название kind на проводе. C++
# добавляет only ``unknown`` для старого/непонятного записанного действия.
Marker = Literal["resume_routine", "idle", "attack", "cast", "flee", "stock_talk",
                 "call_help", "answer_help", "start_phase", "health_event", "aggro_event", "stop"]
RecordedMarker = Marker | Literal["unknown"]


class Position(BaseModel):
    model_config = _STRICT
    x: float
    y: float
    z: float
    o: float = 0.0


class Health(BaseModel):
    model_config = _STRICT
    current: int
    max: int


class Unit(BaseModel):
    """Враждебная цель; id — raw ObjectGuid, а не guid спавна NPC."""
    model_config = _STRICT
    id: str = Field(pattern=r"^[1-9][0-9]*$")
    entry: int
    name: str
    level: int
    is_player: bool
    health_pct: float = Field(ge=0, le=100)
    distance: float = Field(ge=0)
    engaged: bool
    attacking_ally: bool


class Ally(BaseModel):
    model_config = _STRICT
    id: str = Field(pattern=r"^[1-9][0-9]*$")
    guid_spawn: int = Field(gt=0)
    entry: int
    name: str
    health_pct: float = Field(ge=0, le=100)
    distance: float = Field(ge=0)
    in_combat: bool


class Spell(BaseModel):
    model_config = _STRICT
    id: int = Field(gt=0)
    ready: bool
    cooldown_ms: int = Field(ge=0)
    valid_targets: list[str] = Field(default_factory=list, max_length=14)
    aura_targets: list[str] = Field(default_factory=list, max_length=14)
    last_cast_age_ms: int | None = Field(default=None, ge=0)


class HelpRequest(BaseModel):
    """Точная доставленная просьба; raw IDs остаются только в gateway map."""
    model_config = _STRICT
    request_id: str = Field(pattern=r"^[1-9][0-9]*$")
    # Это отдельные фактические snapshots: requester/target не обязаны
    # одновременно попасть в обычные allies[:8]/enemies[:5].
    requester: Ally
    requester_epoch: int = Field(ge=0)
    target: Unit
    expires_in_ms: int = Field(ge=0, le=10_000)


class NpcIdentity(BaseModel):
    # guid в envelope — числовой spawn id, не ObjectGuid цели.
    model_config = _STRICT
    guid: int
    id: str = Field(pattern=r"^[1-9][0-9]*$")
    entry: int
    name: str
    epoch: int
    # 1.8 runtime identity.  A static spawn is reusable in concurrent map
    # instances; the raw GUID is recorded twice only for legacy `id` wire
    # compatibility and must agree when the composite fields are present.
    map_id: int | None = Field(default=None, strict=True, ge=0, le=65_535)
    instance_id: int | None = Field(default=None, strict=True, ge=0, le=4_294_967_295)
    raw_guid: str | None = Field(default=None, pattern=r"^[1-9][0-9]*$")


class Transport(BaseModel):
    """Монотонный возраст снимка до отправки; не является фактом мира."""
    model_config = _STRICT
    capture_to_enqueue_ms: int = Field(strict=True, ge=0, le=4_294_967_295)
    age_at_send_ms: int = Field(strict=True, ge=0, le=4_294_967_295)
    freshness_limit_ms: int = Field(strict=True, gt=0, le=4_294_967_295)

    @model_validator(mode="after")
    def _validate_order(self) -> "Transport":
        if self.age_at_send_ms < self.capture_to_enqueue_ms:
            raise ValueError("age_at_send_ms must include capture_to_enqueue_ms")
        return self


class Routine(BaseModel):
    """Родное мирное движение spawn и возможность вернуть его генератор."""
    model_config = _STRICT
    profile: Literal["idle", "random", "waypoint"]
    wander_radius: float = Field(strict=True, ge=0)
    path_id: int | None = Field(strict=True, ge=0, le=4_294_967_295)
    point_count: int = Field(strict=True, ge=0, le=4_294_967_295)
    active: bool = Field(strict=True)
    can_resume: bool = Field(strict=True)

    @model_validator(mode="after")
    def _validate_profile(self) -> "Routine":
        if self.profile == "idle":
            valid = (self.wander_radius == 0 and self.path_id is None
                     and self.point_count == 0)
        elif self.profile == "random":
            valid = (self.wander_radius > 0 and self.path_id is None
                     and self.point_count == 0)
        else:
            valid = (self.wander_radius == 0 and self.path_id is not None
                     and self.path_id > 0 and self.point_count > 0)
        if not valid:
            raise ValueError(f"fields do not match {self.profile} routine")
        if self.active and self.can_resume:
            raise ValueError("active routine cannot also be resumable")
        return self


# spell trigger выводится из curated policy, поэтому новый одобренный spell
# не требует ослаблять wire до произвольной строки. Его принадлежность entry
# проверяется ниже после разбора всего snapshot.
TriggerId = str
TriggerStatus = Literal["waiting", "eligible", "active", "blocked"]
TriggerBlocker = Literal["timer", "combat", "target", "health", "chance",
                         "physical", "control", "once", "home", "policy"]
TriggerEventKind = Literal["reset", "eligible", "blocked", "reserved",
                           "succeeded", "rejected", "failed", "cancelled", "rearmed",
                           "chance_skipped", "combat_enter", "combat_exit",
                           "died"]


class Trigger(BaseModel):
    model_config = _STRICT
    id: TriggerId = Field(pattern=r"^(?:aggro|aggro_talk|ambient_talk|help_call|help_answer|flee|routine|return|spell_[1-9][0-9]{0,9}|health_phase_[a-z][a-z0-9_]{0,31}|health_event_[a-z][a-z0-9_]{0,31}|aggro_event_[a-z][a-z0-9_]{0,31})$")
    generation: int = Field(strict=True, ge=1)
    status: TriggerStatus
    wait_ms: int | None = Field(strict=True, ge=0, le=4_294_967_295)
    blocked_by: TriggerBlocker | None

    @model_validator(mode="after")
    def _validate_status(self) -> "Trigger":
        if self.status in {"eligible", "active"} and self.blocked_by is not None:
            raise ValueError("eligible/active trigger cannot be blocked")
        if self.status in {"eligible", "active"} and self.wait_ms not in {None, 0}:
            raise ValueError("eligible/active trigger cannot have a positive wait")
        if self.status == "waiting" and not (
                self.blocked_by == "timer" and self.wait_ms is not None
                and self.wait_ms > 0):
            raise ValueError("waiting trigger requires a positive timer")
        if self.status == "blocked" and self.blocked_by is None:
            raise ValueError("blocked trigger requires blocked_by")
        return self


class TriggerEvent(BaseModel):
    model_config = _STRICT
    seq: int = Field(strict=True, ge=1)
    trigger_id: TriggerId | None = Field(default=None, pattern=r"^(?:aggro|aggro_talk|ambient_talk|help_call|help_answer|flee|routine|return|spell_[1-9][0-9]{0,9}|health_phase_[a-z][a-z0-9_]{0,31}|health_event_[a-z][a-z0-9_]{0,31}|aggro_event_[a-z][a-z0-9_]{0,31})$")
    generation: int = Field(strict=True, ge=0)
    kind: TriggerEventKind
    decision_id: str | None
    reason: str | None = Field(max_length=80)

    @model_validator(mode="after")
    def _validate_identity(self) -> "TriggerEvent":
        system = self.kind in {"reset", "combat_enter", "combat_exit", "died"}
        if system != (self.trigger_id is None and self.generation == 0):
            raise ValueError("system event kind requires null trigger_id and generation 0")
        if not system and (self.trigger_id is None or self.generation < 1):
            raise ValueError("trigger event requires trigger_id and positive generation")
        return self


class Behavior(BaseModel):
    model_config = _STRICT
    policy_revision: int = Field(strict=True, ge=1, le=4_294_967_295)
    # 1.7 uses scene_id. In 1.8 group_id replaces it and the binding
    # generation fences a live original<->Jev handoff independently of life.
    scene_id: str | None = Field(default=None, pattern=r"^[a-z][a-z0-9_]{0,31}$")
    group_id: str | None = Field(default=None, pattern=r"^[a-z][a-z0-9_]{0,31}$")
    binding_generation: int | None = Field(
        default=None, strict=True, ge=1, le=4_294_967_295)
    seq: int = Field(strict=True, ge=0)
    floor_seq: int = Field(strict=True, ge=1)
    triggers: list[Trigger] = Field(max_length=24)
    events: list[TriggerEvent] = Field(max_length=8)

    @model_validator(mode="after")
    def _validate_history(self) -> "Behavior":
        ids = [trigger.id for trigger in self.triggers]
        if len(ids) != len(set(ids)):
            raise ValueError("behavior triggers must not repeat id")
        event_seqs = [event.seq for event in self.events]
        if any(right != left + 1 for left, right in zip(event_seqs, event_seqs[1:])):
            raise ValueError("behavior events must be contiguous and ordered")
        if any(seq > self.seq for seq in event_seqs):
            raise ValueError("behavior event seq exceeds current seq")
        expected_floor = event_seqs[0] if event_seqs else self.seq + 1
        if self.floor_seq != expected_floor:
            raise ValueError("behavior floor_seq does not match retained events")
        if self.events and event_seqs[-1] != self.seq:
            raise ValueError("behavior events must end at current seq")
        if not self.events and self.seq != 0:
            raise ValueError("only initial behavior seq 0 may have no events")
        known = set(ids)
        if any(event.trigger_id is not None and event.trigger_id not in known
               for event in self.events):
            raise ValueError("behavior event references unknown trigger")
        return self


class State(BaseModel):
    model_config = _STRICT
    position: Position
    home: Position
    health: Health
    mana: Health
    routine: Routine
    behavior: Behavior
    in_combat: bool
    fleeing: bool
    victim: Unit | None = None
    attackers: list[Unit] = Field(default_factory=list, max_length=5)
    nearest_enemy: Unit | None = None
    enemies: list[Unit] = Field(default_factory=list, max_length=5)
    attack_targets: list[str] = Field(default_factory=list, max_length=5)
    flee_targets: list[str] = Field(default_factory=list, max_length=5)
    allies: list[Ally] = Field(default_factory=list, max_length=8)
    heal_allies: list[Ally] = Field(default_factory=list, max_length=13)
    call_help_targets: list[str] = Field(default_factory=list, max_length=8)
    flee_assist_targets: list[str] = Field(default_factory=list, max_length=8)
    spells: list[Spell] = Field(default_factory=list, max_length=16)
    help_requests: list[HelpRequest] = Field(default_factory=list, max_length=3)
    last_actions: list[RecordedMarker] = Field(default_factory=list)
    seconds_since_last_decision: float = Field(ge=0)
    seconds_since_last_action: float = Field(ge=0)

    @model_validator(mode="after")
    def _validate_references(self) -> "State":
        if self.routine.can_resume and (self.in_combat or self.health.current <= 0):
            raise ValueError("routine can resume only while alive and out of combat")
        if self.routine.can_resume and self.attack_targets:
            raise ValueError("routine cannot resume while a stock attack target is available")
        enemy_ids = [unit.id for unit in self.enemies]
        ally_ids = [unit.id for unit in self.allies]
        heal_ally_ids = [unit.id for unit in self.heal_allies]
        if len(enemy_ids) != len(set(enemy_ids)):
            raise ValueError("enemies must not repeat id")
        if len(ally_ids) != len(set(ally_ids)):
            raise ValueError("allies must not repeat id")
        if len(heal_ally_ids) != len(set(heal_ally_ids)):
            raise ValueError("heal_allies must not repeat id")
        if len(set(ally_ids) | set(heal_ally_ids)) > 13:
            raise ValueError("allies and heal_allies union exceeds owned group")
        ambient_by_id = {ally.id: ally for ally in self.allies}
        for ally in self.heal_allies:
            ambient = ambient_by_id.get(ally.id)
            if ambient is not None and ambient != ally:
                raise ValueError("allies and heal_allies overlap must agree")
        if set(enemy_ids) & (set(ally_ids) | set(heal_ally_ids)):
            raise ValueError("enemy and ally ids must be disjoint")
        for field, values in (("attack_targets", self.attack_targets),
                              ("flee_targets", self.flee_targets)):
            if len(values) != len(set(values)):
                raise ValueError(f"{field} must not repeat id")
            if not set(values) <= set(enemy_ids):
                raise ValueError(f"{field} must reference enemies")
        for field, values in (("call_help_targets", self.call_help_targets),
                              ("flee_assist_targets", self.flee_assist_targets)):
            if len(values) != len(set(values)):
                raise ValueError(f"{field} must not repeat id")
            if not set(values) <= set(ally_ids):
                raise ValueError(f"{field} must reference allies")
        spell_ids = [spell.id for spell in self.spells]
        if len(spell_ids) != len(set(spell_ids)):
            raise ValueError("spells must not repeat id")
        request_ids = [request.request_id for request in self.help_requests]
        if len(request_ids) != len(set(request_ids)):
            raise ValueError("help_requests must not repeat request_id")
        for spell in self.spells:
            if len(spell.valid_targets) != len(set(spell.valid_targets)):
                raise ValueError("spell valid_targets must not repeat id")
            if len(spell.aura_targets) != len(set(spell.aura_targets)):
                raise ValueError("spell aura_targets must not repeat id")
        return self


class DecideRequest(BaseModel):
    model_config = _STRICT
    schema_version: Literal["1.7", "1.8"]
    request_id: str
    server_boot_id: str
    npc: NpcIdentity
    snapshot_seq: int
    transport: Transport
    state: State

    @model_validator(mode="after")
    def _validate_snapshot_identity(self) -> "DecideRequest":
        st = self.state
        behavior = st.behavior
        if self.schema_version == "1.7":
            if behavior.scene_id not in {"murlocs", "riverpaw"}:
                raise ValueError("protocol 1.7 requires a curated scene_id")
            if behavior.group_id is not None or behavior.binding_generation is not None:
                raise ValueError("protocol 1.7 must not carry group binding fields")
            if len(st.spells) > 2 or len(behavior.triggers) > 9:
                raise ValueError("protocol 1.7 catalog exceeds legacy bounds")
        else:
            if behavior.scene_id is not None:
                raise ValueError("protocol 1.8 replaces scene_id with group_id")
            if behavior.group_id is None or behavior.binding_generation is None:
                raise ValueError("protocol 1.8 requires group_id and binding_generation")
            if (self.npc.map_id is None or self.npc.instance_id is None
                    or self.npc.raw_guid is None):
                raise ValueError("protocol 1.8 requires npc map_id, instance_id, and raw_guid")
            if self.npc.raw_guid != self.npc.id:
                raise ValueError("npc raw_guid must match npc id")
        enemy_ids = {unit.id for unit in st.enemies}
        ally_ids = {ally.id for ally in st.allies}
        heal_ally_ids = {ally.id for ally in st.heal_allies}
        ordinary_ids = enemy_ids | ally_ids | heal_ally_ids
        if self.npc.id in ordinary_ids:
            raise ValueError("npc id must not occur in enemies or allies")
        if self.schema_version == "1.8":
            # Group membership, trigger catalog and spell semantics are
            # policy facts. They are checked from one immutable policy in
            # build_decision_plan, not duplicated in this wire parser.
            return self
        allowed_spells = {117: {}, 127: {11831: "enemy", 744: "enemy"},
                          391: {3584: "enemy"}, 500: {6660: "enemy"},
                          517: {9734: "enemy", 6074: "ally"},
                          1065: {9532: "enemy", 913: "ally"}}
        sheet_spells = allowed_spells.get(self.npc.entry, {})
        common_triggers = {"aggro", "help_call", "help_answer", "flee",
                           "routine", "return"}
        expected_triggers = common_triggers | {
            127: {"spell_11831", "spell_744"},
            391: {"spell_3584"},
            500: {"spell_6660"},
            517: {"spell_9734", "spell_6074"},
            1065: {"spell_9532", "spell_913"},
        }.get(self.npc.entry, set())
        if st.behavior.scene_id == "riverpaw" and self.npc.entry in {117, 500, 1065}:
            expected_triggers.add("aggro_talk")
        actual_triggers = {trigger.id for trigger in st.behavior.triggers}
        if actual_triggers != expected_triggers:
            raise ValueError("behavior trigger catalog does not match npc entry")
        for spell in st.spells:
            target_kind = sheet_spells.get(spell.id)
            if target_kind is None:
                raise ValueError("spell id is not allowed for npc entry")
            domain = (enemy_ids if target_kind == "enemy"
                      else (heal_ally_ids if spell.id in {6074, 913}
                            else heal_ally_ids | {self.npc.id}))
            if not set(spell.valid_targets) <= domain:
                raise ValueError("spell valid_targets reference wrong target kind")
            if not set(spell.aura_targets) <= domain:
                raise ValueError("spell aura_targets reference wrong target kind")
        return self


class MoveTo(BaseModel):
    model_config = _STRICT
    kind: Literal["MOVE_TO"] = "MOVE_TO"
    x: float
    y: float
    z: float


class Say(BaseModel):
    model_config = _STRICT
    kind: Literal["SAY"] = "SAY"
    text: str


class StockTalk(BaseModel):
    """Native creature_text group selected by the validated group policy."""
    model_config = _STRICT
    kind: Literal["STOCK_TALK"] = "STOCK_TALK"
    text_group: int = Field(strict=True, ge=0, le=255)
    trigger_id: Literal["aggro_talk", "ambient_talk"] = "aggro_talk"
    trigger_generation: int = Field(strict=True, ge=1)


class Attack(BaseModel):
    model_config = _STRICT
    kind: Literal["ATTACK"] = "ATTACK"
    target: str = Field(pattern=r"^[1-9][0-9]*$")


class Cast(BaseModel):
    model_config = _STRICT
    kind: Literal["CAST"] = "CAST"
    spell: int = Field(gt=0)
    target: str = Field(pattern=r"^[1-9][0-9]*$")
    trigger_generation: int = Field(strict=True, ge=1)


class StartPhase(BaseModel):
    """Start one exact policy-owned encounter choreography."""
    model_config = _STRICT
    kind: Literal["START_PHASE"] = "START_PHASE"
    phase_id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,31}$")
    trigger_generation: int = Field(strict=True, ge=1)


class HealthEvent(BaseModel):
    """Execute one policy-owned stock health event selected by Jev."""
    model_config = _STRICT
    kind: Literal["HEALTH_EVENT"] = "HEALTH_EVENT"
    event_id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,31}$")
    trigger_generation: int = Field(strict=True, ge=1)


class AggroEvent(BaseModel):
    model_config = _STRICT
    kind: Literal["AGGRO_EVENT"] = "AGGRO_EVENT"
    event_id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,31}$")
    trigger_generation: int = Field(strict=True, ge=1)


class FleeForAssist(BaseModel):
    model_config = _STRICT
    kind: Literal["FLEE_FOR_ASSIST"] = "FLEE_FOR_ASSIST"
    recipient: str = Field(pattern=r"^[1-9][0-9]*$")
    target: str = Field(pattern=r"^[1-9][0-9]*$")
    trigger_generation: int = Field(strict=True, ge=1)


class CallHelp(BaseModel):
    model_config = _STRICT
    kind: Literal["CALL_HELP"] = "CALL_HELP"
    recipient: str = Field(pattern=r"^[1-9][0-9]*$")
    target: str = Field(pattern=r"^[1-9][0-9]*$")


class Flee(BaseModel):
    model_config = _STRICT
    kind: Literal["FLEE"] = "FLEE"
    target: str = Field(pattern=r"^[1-9][0-9]*$")
    trigger_generation: int = Field(strict=True, ge=1)


class AnswerHelp(BaseModel):
    model_config = _STRICT
    kind: Literal["ANSWER_HELP"] = "ANSWER_HELP"
    help_request_id: str = Field(pattern=r"^[1-9][0-9]*$")


class StopAttack(BaseModel):
    model_config = _STRICT
    kind: Literal["STOP_ATTACK"] = "STOP_ATTACK"


class Evade(BaseModel):
    model_config = _STRICT
    kind: Literal["EVADE"] = "EVADE"


class Idle(BaseModel):
    model_config = _STRICT
    kind: Literal["IDLE"] = "IDLE"


class ResumeRoutine(BaseModel):
    model_config = _STRICT
    kind: Literal["RESUME_ROUTINE"] = "RESUME_ROUTINE"


Action = (MoveTo | Say | StockTalk | Attack | Cast | StartPhase | HealthEvent | AggroEvent | CallHelp |
          FleeForAssist | Flee | AnswerHelp | StopAttack | Evade | Idle |
          ResumeRoutine)


class DecideResponse(BaseModel):
    model_config = _STRICT
    request_id: str
    decision_id: str
    tier: Literal["heuristic", "jev"]
    action: Action = Field(discriminator="kind")
    thought: str
    marker: Marker
    valid_for_seconds: int
