"""Strict, last-good hot loader for the shared NPC behavior policy."""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from .config import BEHAVIOR_POLICY_PATH

_STRICT_FROZEN = ConfigDict(extra="forbid", strict=True, frozen=True,
                            allow_inf_nan=False)
# Deadmines has over 200 distinct static spawns. Keep the hot-reload document
# bounded, while allowing the complete pinned map-36 roster in one revision.
# The native WojConfig behavior-policy loader uses the same 256 KiB bound.
_MAX_POLICY_BYTES = 256 * 1024
_MURLOC_CATALOG = {(127, 11831), (127, 744), (391, 3584),
                   (517, 9734), (517, 6074)}
_RIVERPAW_CATALOG = {(500, 6660), (1065, 9532), (1065, 913)}
_SCENE_CATALOGS = {"murlocs": _MURLOC_CATALOG, "riverpaw": _RIVERPAW_CATALOG}
_log = logging.getLogger("woj_gateway.policy")


class SpellPolicy(BaseModel):
    model_config = _STRICT_FROZEN
    entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    spell: int = Field(strict=True, gt=0, le=4_294_967_295)
    initial_ms: tuple[int, int]
    repeat_ms: tuple[int, int]
    ticks_out_of_combat: bool
    chance_pct: int = Field(strict=True, ge=1, le=100)
    target: Literal["enemy", "ally", "self"]
    require_caster_combat: bool
    require_caster_out_of_combat: bool = False
    require_target_in_combat: bool
    require_engaged: bool
    max_distance_yards: float | None = Field(strict=True, gt=0, le=100)
    min_snapshot_distance_yards: float | None = Field(
        default=None, strict=True, gt=0, le=100)
    require_aura_absent: bool
    max_target_health_pct: int | None = Field(strict=True, ge=1, le=100)
    max_caster_health_pct: int | None = Field(
        default=None, strict=True, ge=1, le=100)
    timer_mode: Literal["timer", "event"] = "timer"

    @field_validator("initial_ms", "repeat_ms", mode="before")
    @classmethod
    def _freeze_range(cls, value):
        if type(value) is not list:
            raise ValueError("timer range must be a JSON array")
        return tuple(value)

    @field_validator("min_snapshot_distance_yards", mode="before")
    @classmethod
    def _reject_explicit_null_snapshot_floor(cls, value):
        if value is None:
            raise ValueError("min_snapshot_distance_yards must be omitted or a positive number")
        return value

    @model_validator(mode="after")
    def _validate_ranges(self) -> "SpellPolicy":
        if self.require_caster_combat and self.require_caster_out_of_combat:
            raise ValueError("spell cannot require caster both in and out of combat")
        for name, values in (("initial_ms", self.initial_ms),
                             ("repeat_ms", self.repeat_ms)):
            if len(values) != 2:
                raise ValueError(f"{name} must contain exactly two timers")
            if any(type(value) is not int or not 0 <= value <= 3_600_000
                   for value in values):
                raise ValueError(f"{name} must contain uint timers <= 3600000")
            if values[0] > values[1]:
                raise ValueError(f"{name} minimum exceeds maximum")
        if self.target == "enemy" and self.require_target_in_combat:
            raise ValueError("enemy target_in_combat is not observable on protocol 1.7")
        if self.target == "ally" and self.require_engaged:
            raise ValueError("ally engaged is not observable on protocol 1.7")
        if self.target == "self" and (self.require_target_in_combat
                                      or self.require_engaged):
            raise ValueError("self target cannot require target combat or engagement")
        if self.timer_mode == "event" and (self.initial_ms != (0, 0)
                                           or self.repeat_ms != (0, 0)
                                           or self.chance_pct != 100
                                           or self.ticks_out_of_combat):
            raise ValueError("event spell requires zero timers, in-combat check, and 100 percent chance")
        return self


class SpellPolicyV3(SpellPolicy):
    """Policy-owned spell capability; no Python spell catalog is involved."""

    # The native v3 parser requires this key even though legacy SpellPolicy
    # supplies a default. Keep the gateway preflight in lockstep with native.
    timer_mode: Literal["timer", "event"]
    allow_self: bool
    once_per_epoch: bool
    completion: Literal["cast", "positive_heal"]
    target_selector: Literal["any", "victim", "hostile_random"] = "any"
    target_radius_yards: float | None = Field(default=None, strict=True, gt=0, le=100)

    @model_validator(mode="after")
    def _validate_completion(self) -> "SpellPolicyV3":
        if self.completion == "positive_heal" and self.target != "ally":
            raise ValueError("positive_heal completion requires an ally target")
        if self.target == "enemy" and self.allow_self:
            raise ValueError("enemy spells cannot allow self")
        if self.target == "self" and not self.allow_self:
            raise ValueError("self spells must allow self")
        if self.target_selector != "any" and self.target != "enemy":
            raise ValueError("target_selector requires enemy target")
        if ((self.target_selector == "hostile_random")
                != (self.target_radius_yards is not None)):
            raise ValueError("hostile_random requires target_radius_yards exclusively")
        return self


class FleePolicy(BaseModel):
    model_config = _STRICT_FROZEN
    enabled: bool = True
    max_health_pct: int = Field(strict=True, ge=1, le=100)
    once_per_epoch: bool
    require_in_combat: bool
    require_not_casting: bool
    require_engaged: bool
    max_distance_yards: float = Field(strict=True, gt=0, le=100)


class AggroTalkPolicy(BaseModel):
    model_config = _STRICT_FROZEN
    enabled: bool
    chance_pct: int = Field(strict=True, ge=0, le=100)
    text_group: int = Field(strict=True, ge=0, le=255)

    @field_validator("text_group", mode="before")
    @classmethod
    def _strict_text_group(cls, value):
        if type(value) is not int:
            raise ValueError("text_group must be an integer")
        return value


class AmbientTalkPolicy(BaseModel):
    model_config = _STRICT_FROZEN
    enabled: bool
    initial_ms: tuple[int, int]
    repeat_ms: tuple[int, int]
    chance_pct: int = Field(strict=True, ge=0, le=100)
    text_group: int = Field(strict=True, ge=0, le=255)

    @field_validator("initial_ms", "repeat_ms", mode="before")
    @classmethod
    def _freeze_range(cls, value):
        if type(value) is not list:
            raise ValueError("ambient talk timer range must be a JSON array")
        return tuple(value)

    @model_validator(mode="after")
    def _validate_policy(self) -> "AmbientTalkPolicy":
        for name, values in (("initial_ms", self.initial_ms),
                             ("repeat_ms", self.repeat_ms)):
            if (len(values) != 2
                    or any(type(value) is not int or not 0 <= value <= 3_600_000
                           for value in values)
                    or values[0] > values[1]):
                raise ValueError(f"{name} must be an ordered pair of uint timers")
        if not self.enabled and (self.chance_pct != 0
                                 or self.initial_ms != (0, 0)
                                 or self.repeat_ms != (0, 0)):
            raise ValueError("disabled ambient_talk must have zero timers and chance")
        if self.enabled and (self.chance_pct == 0 or self.repeat_ms == (0, 0)):
            raise ValueError("enabled ambient_talk requires positive chance and repeat timer")
        return self


class ArenaAnchor(BaseModel):
    model_config = _STRICT_FROZEN
    x: float = Field(strict=True)
    y: float = Field(strict=True)
    z: float = Field(strict=True)


class ActivationPolicy(BaseModel):
    model_config = _STRICT_FROZEN
    mode: Literal["always", "near_player"]
    radius_yards: float | None = Field(strict=True, gt=0, le=500)

    @model_validator(mode="after")
    def _validate_mode(self) -> "ActivationPolicy":
        if self.mode == "always" and self.radius_yards is not None:
            raise ValueError("always activation requires null radius_yards")
        if self.mode == "near_player" and self.radius_yards is None:
            raise ValueError("near_player activation requires radius_yards")
        return self


class AiInitDespawnPolicy(BaseModel):
    """A static creature's source AI_INIT despawn roll.

    This is deliberately not a generic timed action: the native executor owns
    the physical roll and lifecycle journal, while policy merely declares the
    one source-derived contract that it may execute.
    """

    model_config = _STRICT_FROZEN
    chance_pct: int = Field(strict=True, ge=0, le=100)
    delay_ms: int = Field(strict=True, ge=1, le=3_600_000)


class ActorPolicy(BaseModel):
    model_config = _STRICT_FROZEN
    spawn_id: int = Field(strict=True, gt=0, le=4_294_967_295)
    entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    control: Literal["inherit", "jev", "original"]
    aggro_talk: bool
    ambient_talk: bool = False
    preserve_stock_routine: bool = False
    preferred_combat_range_yards: float | None = Field(
        default=None, strict=True, gt=0, le=100)
    executor_repeat_continuation_spell: int | None = Field(
        default=None, strict=True, gt=0, le=4_294_967_295)
    flee_enabled: bool | None = None
    ai_init_despawn: AiInitDespawnPolicy | None = None

    @field_validator("preferred_combat_range_yards", mode="before")
    @classmethod
    def _reject_null_preferred_combat_range(cls, value):
        if value is None:
            raise ValueError("preferred_combat_range_yards must be omitted or a positive number")
        return value

    @model_validator(mode="after")
    def _only_taskmaster_shoot_continuation(self) -> "ActorPolicy":
        if self.executor_repeat_continuation_spell is not None and (
                self.spawn_id != 79230 or self.entry != 4417
                or self.executor_repeat_continuation_spell != 6660):
            raise ValueError(
                "executor_repeat_continuation_spell is reserved for Taskmaster 79230 Shoot 6660")
        return self


class RuntimeSummonPolicy(BaseModel):
    model_config = _STRICT_FROZEN
    actor_id: int = Field(strict=True, ge=2_147_483_648, le=4_294_967_295)
    entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    source_actor_id: int = Field(strict=True, gt=0, le=4_294_967_295)
    created_by_spell: int = Field(strict=True, gt=0, le=4_294_967_295)
    summon_index: int = Field(default=1, strict=True, ge=1, le=8)
    control: Literal["inherit", "jev", "original"]
    aggro_talk: bool
    ambient_talk: bool = False
    preserve_stock_routine: bool = False
    flee_enabled: bool | None = None

    @property
    def spawn_id(self) -> int:
        # Compatibility name on protocol 1.8: this is a policy actor id, not
        # a claim that a runtime summon owns a DB spawn row.
        return self.actor_id


class GoSummonPosition(BaseModel):
    model_config = _STRICT_FROZEN
    x: float = Field(strict=True, ge=-100_000, le=100_000)
    y: float = Field(strict=True, ge=-100_000, le=100_000)
    z: float = Field(strict=True, ge=-100_000, le=100_000)


class GoRuntimeSummonPolicy(BaseModel):
    """Exact GO-origin child, admitted only by a scoped physical trigger."""

    model_config = _STRICT_FROZEN
    actor_id: int = Field(strict=True, ge=2_147_483_648, le=4_294_967_295)
    entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    source_go_spawn_id: int = Field(strict=True, gt=0, le=4_294_967_295)
    source_go_entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    trigger: Literal["item_cast", "report_use"]
    item_spell: int | None = Field(default=None, strict=True, gt=0, le=4_294_967_295)
    summon_index: int = Field(strict=True, ge=1, le=8)
    position: GoSummonPosition
    control: Literal["inherit", "jev", "original"]
    aggro_talk: bool
    flee_enabled: bool | None = None
    preferred_combat_range_yards: float | None = Field(default=None, strict=True, gt=0, le=100)

    @field_validator("flee_enabled", "preferred_combat_range_yards", mode="before")
    @classmethod
    def _reject_explicit_null_overrides(cls, value):
        if value is None:
            raise ValueError("GO runtime summon overrides must be omitted or concrete")
        return value

    @model_validator(mode="after")
    def _exact_trigger(self) -> "GoRuntimeSummonPolicy":
        if ((self.trigger == "item_cast") != ("item_spell" in self.model_fields_set) or
                (self.trigger == "item_cast" and self.item_spell is None)):
            raise ValueError("GO runtime summon item_spell must exist only for item_cast")
        return self

    @property
    def spawn_id(self) -> int:
        return self.actor_id

    @property
    def ambient_talk(self) -> bool:
        return False



class HealthPhaseDestination(BaseModel):
    """One bounded encounter-choreography destination."""

    model_config = _STRICT_FROZEN
    x: float = Field(strict=True, ge=-100_000, le=100_000)
    y: float = Field(strict=True, ge=-100_000, le=100_000)
    z: float = Field(strict=True, ge=-100_000, le=100_000)
    o: float = Field(strict=True, ge=-6.283_186, le=6.283_186)


class HealthPhasePolicy(BaseModel):
    """A fixed health transition; tactical choices remain in Jev's catalog."""

    model_config = _STRICT_FROZEN
    id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,31}$")
    actor_id: int = Field(strict=True, gt=0, le=4_294_967_295)
    entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    trigger_below_health_pct: int = Field(strict=True, ge=1, le=100)
    transition_spell: int = Field(strict=True, gt=0, le=4_294_967_295)
    talk_group: int = Field(strict=True, ge=0, le=255)
    destination: HealthPhaseDestination
    equipment_id: int = Field(strict=True, ge=1, le=127)
    dual_wield: bool
    equipment_delay_ms: int = Field(strict=True, ge=0, le=3_600_000)
    restore_combat_ms: int = Field(strict=True, ge=0, le=3_600_000)
    tactical_delay_ms: int = Field(strict=True, ge=0, le=3_600_000)

    @model_validator(mode="after")
    def _validate_timing(self) -> "HealthPhasePolicy":
        if self.restore_combat_ms < self.equipment_delay_ms:
            raise ValueError("health phase restore_combat_ms must not precede equipment_delay_ms")
        return self


class HealthEventCast(BaseModel):
    model_config = _STRICT_FROZEN
    kind: Literal["cast"]
    spell: int = Field(strict=True, gt=0, le=4_294_967_295)
    target: Literal["self"]


class HealthEventTalk(BaseModel):
    model_config = _STRICT_FROZEN
    kind: Literal["talk"]
    text_group: int = Field(strict=True, ge=0, le=255)


class HealthEventPolicy(BaseModel):
    """One stock health window and its fixed physical action sequence."""
    model_config = _STRICT_FROZEN
    id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,31}$")
    actor_id: int = Field(strict=True, gt=0, le=4_294_967_295)
    entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    health_pct_min: int = Field(strict=True, ge=0, le=100)
    health_pct_max: int = Field(strict=True, ge=0, le=100)
    actions: tuple[HealthEventCast | HealthEventTalk, ...]

    @field_validator("actions", mode="before")
    @classmethod
    def _freeze_actions(cls, value):
        if type(value) is not list or not 1 <= len(value) <= 2:
            raise ValueError("health event actions must be a JSON array of 1..2 records")
        return tuple(value)

    @model_validator(mode="after")
    def _validate_event(self) -> "HealthEventPolicy":
        if self.health_pct_min > self.health_pct_max:
            raise ValueError("health event minimum exceeds maximum")
        if (len(self.actions) not in (1, 2) or
                not isinstance(self.actions[-1], HealthEventTalk) or
                (len(self.actions) == 2 and
                 not isinstance(self.actions[0], HealthEventCast))):
            raise ValueError("health event actions must be talk or cast then talk")
        return self


class AggroEventCast(BaseModel):
    model_config = _STRICT_FROZEN
    kind: Literal["cast"]
    spell: int = Field(strict=True, gt=0, le=4_294_967_295)
    target: Literal["aggro_invoker"]


class AggroEventRemoveAura(BaseModel):
    model_config = _STRICT_FROZEN
    kind: Literal["remove_aura"]
    spell: int = Field(strict=True, gt=0, le=4_294_967_295)
    target: Literal["self"]


class AggroEventTalk(BaseModel):
    model_config = _STRICT_FROZEN
    kind: Literal["talk"]
    text_group: int = Field(strict=True, ge=0, le=255)


class AggroEventPolicy(BaseModel):
    model_config = _STRICT_FROZEN
    id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,31}$")
    actor_id: int = Field(strict=True, gt=0, le=4_294_967_295)
    entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    actions: tuple[AggroEventCast | AggroEventRemoveAura | AggroEventTalk, ...]

    @field_validator("actions", mode="before")
    @classmethod
    def _freeze_actions(cls, value):
        if type(value) is not list or not 1 <= len(value) <= 3:
            raise ValueError("aggro event actions must be a JSON array of 1..3 records")
        return tuple(value)

    @model_validator(mode="after")
    def _validate_order(self) -> "AggroEventPolicy":
        kinds = tuple(action.kind for action in self.actions)
        if kinds not in (("talk",), ("cast", "remove_aura", "talk")):
            raise ValueError("aggro event actions must be talk or cast/remove_aura/talk")
        return self


class DeathEffectPolicy(BaseModel):
    model_config = _STRICT_FROZEN
    id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,31}$")
    type: Literal["cast_spell", "activate_gameobject"]
    actor_id: int = Field(strict=True, gt=0, le=4_294_967_295)
    entry: int = Field(strict=True, gt=0, le=4_294_967_295)
    spell: int | None = Field(default=None, strict=True, gt=0, le=4_294_967_295)
    expected_summon_actor_id: int | None = Field(
        default=None, strict=True, ge=2_147_483_648, le=4_294_967_295)
    expected_summon_count: int | None = Field(default=None, strict=True, ge=1, le=1)
    gameobject_spawn_id: int | None = Field(default=None, strict=True, gt=0, le=4_294_967_295)
    gameobject_entry: int | None = Field(default=None, strict=True, gt=0, le=4_294_967_295)
    initial_object_state: int | None = Field(default=None, strict=True, ge=0, le=3)
    final_object_state: int | None = Field(default=None, strict=True, ge=0, le=3)
    instance_data_index: int | None = Field(default=None, strict=True, ge=0, le=255)
    initial_instance_data_value: int | None = Field(default=None, strict=True, ge=0, le=4_294_967_295)
    instance_data_value: int | None = Field(default=None, strict=True, ge=0, le=4_294_967_295)
    instance_save_token_index: int | None = Field(default=None, strict=True, ge=0, le=255)

    @model_validator(mode="after")
    def _validate_shape(self) -> "DeathEffectPolicy":
        instance = (self.instance_data_index, self.initial_instance_data_value,
                    self.instance_data_value,
                    self.instance_save_token_index)
        if self.type == "cast_spell":
            if (self.spell is None or self.expected_summon_actor_id is None
                    or self.expected_summon_count is None
                    or any(value is not None for value in (
                    self.gameobject_spawn_id, self.gameobject_entry,
                    self.initial_object_state, self.final_object_state, *instance))):
                raise ValueError("cast_spell requires only spell")
        elif (self.spell is not None or self.expected_summon_actor_id is not None
              or self.expected_summon_count is not None
              or self.gameobject_spawn_id is None
              or self.gameobject_entry is None
              or self.initial_object_state is None
              or self.final_object_state is None
              or self.initial_object_state == self.final_object_state
              or (any(value is not None for value in instance)
                  and not all(value is not None for value in instance))):
            raise ValueError("activate_gameobject requires object identity and complete optional instance fields")
        return self


class GroupPolicy(BaseModel):
    """One simultaneous ownership group in schema v3."""

    model_config = _STRICT_FROZEN
    id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,31}$")
    enabled: bool
    map_id: int = Field(strict=True, ge=0, le=65_535)
    control: Literal["jev", "original"]
    activation: ActivationPolicy = ActivationPolicy(mode="always", radius_yards=None)
    arena_anchor: ArenaAnchor | None = None
    actors: tuple[ActorPolicy, ...]
    runtime_summons: tuple[RuntimeSummonPolicy, ...] = ()
    go_runtime_summons: tuple[GoRuntimeSummonPolicy, ...] = ()
    spells: tuple[SpellPolicyV3, ...]
    health_phases: tuple[HealthPhasePolicy, ...] = ()
    health_events: tuple[HealthEventPolicy, ...] = ()
    aggro_events: tuple[AggroEventPolicy, ...] = ()
    death_effects: tuple[DeathEffectPolicy, ...] = ()
    flee: FleePolicy
    aggro_talk: AggroTalkPolicy
    ambient_talk: AmbientTalkPolicy

    @field_validator("actors", mode="before")
    @classmethod
    def _freeze_actors(cls, value):
        if type(value) is not list or not 1 <= len(value) <= 128:
            raise ValueError("actors must be a JSON array of 1..128 records")
        return tuple(value)

    @field_validator("runtime_summons", mode="before")
    @classmethod
    def _freeze_runtime_summons(cls, value):
        if type(value) is not list or len(value) > 32:
            raise ValueError("runtime_summons must be a JSON array of 0..32 records")
        return tuple(value)

    @field_validator("go_runtime_summons", mode="before")
    @classmethod
    def _freeze_go_runtime_summons(cls, value):
        if type(value) is not list or len(value) > 16:
            raise ValueError("go_runtime_summons must be a JSON array of 0..16 records")
        return tuple(value)

    @field_validator("spells", mode="before")
    @classmethod
    def _freeze_v3_spells(cls, value):
        if type(value) is not list or len(value) > 128:
            raise ValueError("spells must be a JSON array of 0..128 records")
        return tuple(value)

    @field_validator("health_phases", mode="before")
    @classmethod
    def _freeze_health_phases(cls, value):
        if type(value) is not list or len(value) > 32:
            raise ValueError("health_phases must be a JSON array of 0..32 records")
        return tuple(value)

    @field_validator("health_events", mode="before")
    @classmethod
    def _freeze_health_events(cls, value):
        if type(value) is not list or len(value) > 32:
            raise ValueError("health_events must be a JSON array of 0..32 records")
        return tuple(value)

    @field_validator("aggro_events", mode="before")
    @classmethod
    def _freeze_aggro_events(cls, value):
        if type(value) is not list or len(value) > 32:
            raise ValueError("aggro_events must be a JSON array of 0..32 records")
        return tuple(value)

    @field_validator("death_effects", mode="before")
    @classmethod
    def _freeze_death_effects(cls, value):
        if type(value) is not list or len(value) > 16:
            raise ValueError("death_effects must be a JSON array of 0..16 records")
        return tuple(value)

    @model_validator(mode="after")
    def _validate_group(self) -> "GroupPolicy":
        spawn_ids = [actor.spawn_id for actor in self.actors]
        if len(spawn_ids) != len(set(spawn_ids)):
            raise ValueError("group actors must not repeat spawn_id")
        ai_init_actors = [actor for actor in self.actors
                          if actor.ai_init_despawn is not None]
        if ai_init_actors:
            if (self.id != "deadmines_post_rhahk" or len(ai_init_actors) != 1
                    or ai_init_actors[0].spawn_id != 84079
                    or ai_init_actors[0].entry != 3586
                    or ai_init_actors[0].ai_init_despawn != AiInitDespawnPolicy(
                        chance_pct=60, delay_ms=500)):
                raise ValueError(
                    "ai_init_despawn is reserved for static Miner Johnson 84079 in deadmines_post_rhahk")
        runtime_ids = [actor.actor_id for actor in self.runtime_summons]
        go_runtime_ids = [actor.actor_id for actor in self.go_runtime_summons]
        all_ids = spawn_ids + runtime_ids + go_runtime_ids
        if len(all_ids) != len(set(all_ids)):
            raise ValueError("group actor identities must not repeat")
        static_ids = set(spawn_ids)
        if any(actor.source_actor_id not in static_ids
               for actor in self.runtime_summons):
            raise ValueError("runtime summon source must be a static actor in the same group")
        summon_groups: dict[tuple[int, int, int], list[int]] = {}
        summon_source_entries: dict[tuple[int, int], int] = {}
        summon_source_spells: dict[tuple[int, int], int] = {}
        for actor in self.runtime_summons:
            source_entry = (actor.source_actor_id, actor.entry)
            source_spell = (actor.source_actor_id, actor.created_by_spell)
            if (source_entry in summon_source_entries and
                    summon_source_entries[source_entry] != actor.created_by_spell):
                raise ValueError("runtime summon source/entry cannot have multiple spells")
            if (source_spell in summon_source_spells and
                    summon_source_spells[source_spell] != actor.entry):
                raise ValueError("runtime summon source/spell cannot have multiple entries")
            summon_source_entries[source_entry] = actor.created_by_spell
            summon_source_spells[source_spell] = actor.entry
            summon_groups.setdefault((actor.source_actor_id, actor.entry,
                                      actor.created_by_spell), []).append(actor.summon_index)
        for indices in summon_groups.values():
            if len(indices) > 8 or sorted(indices) != list(range(1, len(indices) + 1)):
                raise ValueError("runtime summon indices must be contiguous 1..N, at most 8")
        go_sources: dict[int, tuple[int, int, int]] = {}
        go_indices: dict[int, list[int]] = {}
        go_positions: dict[int, list[GoSummonPosition]] = {}
        for actor in self.go_runtime_summons:
            source = actor.source_go_spawn_id
            selector = (actor.source_go_entry, actor.item_spell or 0, actor.entry)
            if source in go_sources and go_sources[source] != selector:
                raise ValueError("GO runtime summon source selector is ambiguous")
            go_sources[source] = selector
            go_indices.setdefault(source, []).append(actor.summon_index)
            for other in go_positions.setdefault(source, []):
                squared = sum((getattr(actor.position, axis) - getattr(other, axis)) ** 2
                              for axis in ("x", "y", "z"))
                if squared <= 4.0:
                    raise ValueError("GO runtime summon positions overlap")
            go_positions[source].append(actor.position)
        for indices in go_indices.values():
            if sorted(indices) != list(range(1, len(indices) + 1)):
                raise ValueError("GO runtime summon indices must be contiguous 1..N")
        spell_keys = [(spell.entry, spell.spell) for spell in self.spells]
        if len(spell_keys) != len(set(spell_keys)):
            raise ValueError("group spells must not repeat entry/spell")
        per_entry: dict[int, int] = {}
        for spell in self.spells:
            per_entry[spell.entry] = per_entry.get(spell.entry, 0) + 1
        if any(count > 16 for count in per_entry.values()):
            raise ValueError("an actor entry cannot expose more than 16 spells")
        actor_entries = {actor.entry for actor in (*self.actors, *self.runtime_summons,
                                                  *self.go_runtime_summons)}
        if any(spell.entry not in actor_entries for spell in self.spells):
            raise ValueError("group spell entry has no actor")
        buffered_spells = [spell for spell in self.spells
                           if spell.min_snapshot_distance_yards is not None]
        if buffered_spells and (self.id != "deadmines_mast_workshop_fringe"
                                or len(buffered_spells) != 1
                                or buffered_spells[0].entry != 4417
                                or buffered_spells[0].spell != 6660
                                or buffered_spells[0].min_snapshot_distance_yards != 12.0):
            raise ValueError(
                "min_snapshot_distance_yards is reserved for workshop Taskmaster Shoot 6660 at 12 yd")
        phase_ids = [phase.id for phase in self.health_phases]
        if len(phase_ids) != len(set(phase_ids)):
            raise ValueError("group health_phases must not repeat id")
        phase_keys = [(phase.actor_id, phase.trigger_below_health_pct)
                      for phase in self.health_phases]
        if len(phase_keys) != len(set(phase_keys)):
            raise ValueError("group health_phases must not repeat actor/threshold")
        actors_by_id = {actor.spawn_id: actor
                        for actor in (*self.actors, *self.runtime_summons,
                                      *self.go_runtime_summons)}
        prior_threshold: dict[int, int] = {}
        for phase in self.health_phases:
            owner = actors_by_id.get(phase.actor_id)
            if owner is None or owner.entry != phase.entry:
                raise ValueError("health_phase actor_id/entry must identify one exact actor")
            previous = prior_threshold.get(phase.actor_id)
            if previous is not None and phase.trigger_below_health_pct >= previous:
                raise ValueError("health phases must use descending thresholds per actor")
            prior_threshold[phase.actor_id] = phase.trigger_below_health_pct
        event_ids = [event.id for event in self.health_events]
        if len(event_ids) != len(set(event_ids)):
            raise ValueError("group health_events must not repeat id")
        for event in self.health_events:
            owner = actors_by_id.get(event.actor_id)
            if owner is None or owner.entry != event.entry:
                raise ValueError("health_event actor_id/entry must identify one exact actor")
        aggro_ids = [event.id for event in self.aggro_events]
        if len(aggro_ids) != len(set(aggro_ids)):
            raise ValueError("group aggro_events must not repeat id")
        for event in self.aggro_events:
            owner = actors_by_id.get(event.actor_id)
            if owner is None or owner.entry != event.entry:
                raise ValueError("aggro_event actor_id/entry must identify one exact actor")
        for actor in (*self.actors, *self.runtime_summons, *self.go_runtime_summons):
            trigger_count = (6 + int(actor.aggro_talk) + int(actor.ambient_talk)
                             + per_entry.get(actor.entry, 0)
                             + sum(phase.actor_id == actor.spawn_id
                                   for phase in self.health_phases)
                             + sum(event.actor_id == actor.spawn_id
                                   for event in self.health_events)
                             + sum(event.actor_id == actor.spawn_id
                                   for event in self.aggro_events))
            if trigger_count > 24:
                raise ValueError("an actor cannot expose more than 24 behavior triggers")
        effect_ids = [effect.id for effect in self.death_effects]
        if len(effect_ids) != len(set(effect_ids)):
            raise ValueError("group death_effects must not repeat id")
        if any(effect.entry not in actor_entries for effect in self.death_effects):
            raise ValueError("group death_effect entry has no actor")
        for effect in self.death_effects:
            owner = actors_by_id.get(effect.actor_id)
            if owner is None or owner.entry != effect.entry:
                raise ValueError("death_effect actor_id/entry must identify one exact actor")
            if effect.type == "cast_spell":
                expected = (actors_by_id.get(effect.expected_summon_actor_id)
                            if effect.expected_summon_actor_id is not None else None)
                if (not isinstance(expected, RuntimeSummonPolicy)
                        or expected.source_actor_id != effect.actor_id
                        or expected.created_by_spell != effect.spell):
                    raise ValueError("cast_spell expected summon lineage is inconsistent")
        if any(actor.aggro_talk for actor in (*self.actors, *self.runtime_summons,
                                             *self.go_runtime_summons)):
            if not self.aggro_talk.enabled:
                raise ValueError("actor aggro_talk requires enabled group policy")
        if not self.aggro_talk.enabled and self.aggro_talk.chance_pct != 0:
            raise ValueError("disabled aggro_talk must have zero chance")
        if self.aggro_talk.enabled and self.aggro_talk.chance_pct == 0:
            raise ValueError("enabled aggro_talk requires positive chance")
        if any(actor.ambient_talk for actor in (*self.actors, *self.runtime_summons,
                                               *self.go_runtime_summons)):
            if not self.ambient_talk.enabled:
                raise ValueError("actor ambient_talk requires enabled group policy")
        return self

    def actor(self, spawn_id: int) -> ActorPolicy | RuntimeSummonPolicy | GoRuntimeSummonPolicy | None:
        return next((actor for actor in (*self.actors, *self.runtime_summons,
                                         *self.go_runtime_summons)
                     if actor.spawn_id == spawn_id), None)

    def spells_for_entry(self, entry: int) -> tuple[SpellPolicyV3, ...]:
        return tuple(item for item in self.spells if item.entry == entry)

    def spell(self, entry: int, spell: int) -> SpellPolicyV3:
        found = next((item for item in self.spells
                      if item.entry == entry and item.spell == spell), None)
        if found is None:
            raise ValueError(f"group has no spell {spell} for entry {entry}")
        return found


class BehaviorPolicyV1(BaseModel):
    model_config = _STRICT_FROZEN
    schema_version: Literal[1]
    revision: int = Field(strict=True, ge=1, le=4_294_967_295)
    spells: tuple[SpellPolicy, SpellPolicy, SpellPolicy, SpellPolicy, SpellPolicy]
    flee: FleePolicy

    @field_validator("schema_version", mode="before")
    @classmethod
    def _strict_schema_version(cls, value):
        if type(value) is not int or value != 1:
            raise ValueError("schema_version must be integer 1")
        return value

    @field_validator("spells", mode="before")
    @classmethod
    def _freeze_spells(cls, value):
        if type(value) is not list:
            raise ValueError("spells must be a JSON array")
        return tuple(value)

    @model_validator(mode="after")
    def _validate_catalog(self) -> "BehaviorPolicyV1":
        actual = {(item.entry, item.spell) for item in self.spells}
        if len(actual) != len(self.spells) or actual != _MURLOC_CATALOG:
            raise ValueError("policy must contain each supported entry/spell exactly once")
        targets = {(item.entry, item.spell): item.target for item in self.spells}
        expected = {(127, 11831): "enemy", (127, 744): "enemy",
                    (391, 3584): "enemy", (517, 9734): "enemy",
                    (517, 6074): "ally"}
        if targets != expected:
            raise ValueError("policy target kinds disagree with the fixed spell catalog")
        return self

    def spells_for_entry(self, entry: int) -> tuple[SpellPolicy, ...]:
        return tuple(item for item in self.spells if item.entry == entry)

    def spell(self, entry: int, spell: int) -> SpellPolicy:
        found = next((item for item in self.spells
                      if item.entry == entry and item.spell == spell), None)
        if found is None:
            raise ValueError(f"policy has no spell {spell} for entry {entry}")
        return found

    @property
    def scene_id(self) -> str:
        return "murlocs"


class ScenePolicy(BaseModel):
    """Одна заранее известная сцена; все записи проверяются до выбора active."""

    model_config = _STRICT_FROZEN
    id: Literal["murlocs", "riverpaw"]
    map_id: int = Field(strict=True, ge=0, le=65_535)
    owned_guids: tuple[int, ...]
    spells: tuple[SpellPolicy, ...]
    flee: FleePolicy
    aggro_talk: AggroTalkPolicy

    @field_validator("owned_guids", mode="before")
    @classmethod
    def _freeze_owned(cls, value):
        if type(value) is not list or not 1 <= len(value) <= 14:
            raise ValueError("owned_guids must be a JSON array of 1..14 ids")
        return tuple(value)

    @field_validator("spells", mode="before")
    @classmethod
    def _freeze_spells(cls, value):
        if type(value) is not list or not 1 <= len(value) <= 16:
            raise ValueError("spells must be a JSON array of 1..16 records")
        return tuple(value)

    @model_validator(mode="after")
    def _validate_catalog(self) -> "ScenePolicy":
        if any(type(guid) is not int or guid <= 0 for guid in self.owned_guids):
            raise ValueError("owned_guids must contain positive integer ids")
        if len(set(self.owned_guids)) != len(self.owned_guids):
            raise ValueError("owned_guids must not repeat")
        actual = {(item.entry, item.spell) for item in self.spells}
        if len(actual) != len(self.spells) or actual != _SCENE_CATALOGS[self.id]:
            raise ValueError("scene spell catalog disagrees with curated capabilities")
        expected_targets = {
            (127, 11831): ("enemy", "timer"), (127, 744): ("enemy", "timer"),
            (391, 3584): ("enemy", "timer"), (517, 9734): ("enemy", "timer"),
            (517, 6074): ("ally", "timer"), (500, 6660): ("enemy", "timer"),
            (1065, 9532): ("enemy", "timer"), (1065, 913): ("ally", "event"),
        }
        for spell in self.spells:
            if (spell.target, spell.timer_mode) != expected_targets[(spell.entry, spell.spell)]:
                raise ValueError("spell target or timer mode disagrees with curated capability")
        valid_talk = ((not self.aggro_talk.enabled and self.aggro_talk.chance_pct == 0
                       and self.aggro_talk.text_group == 0) if self.id == "murlocs" else
                      (self.aggro_talk.enabled and 1 <= self.aggro_talk.chance_pct <= 100
                       and self.aggro_talk.text_group == 0))
        if not valid_talk:
            raise ValueError("aggro_talk disagrees with curated scene policy")
        return self


class BehaviorPolicyV2(BaseModel):
    model_config = _STRICT_FROZEN
    schema_version: Literal[2]
    revision: int = Field(strict=True, ge=1, le=4_294_967_295)
    active_scene: Literal["murlocs", "riverpaw"]
    scenes: tuple[ScenePolicy, ...]

    @field_validator("scenes", mode="before")
    @classmethod
    def _freeze_scenes(cls, value):
        if type(value) is not list or not 1 <= len(value) <= 8:
            raise ValueError("scenes must be a JSON array of 1..8 records")
        return tuple(value)

    @model_validator(mode="after")
    def _validate_scenes(self) -> "BehaviorPolicyV2":
        ids = [scene.id for scene in self.scenes]
        if len(ids) != len(set(ids)) or self.active_scene not in ids:
            raise ValueError("scenes must have unique ids including active_scene")
        spells = [spell.spell for scene in self.scenes for spell in scene.spells]
        if len(spells) != len(set(spells)):
            raise ValueError("spell ids must be unique across curated scenes")
        return self

    @property
    def selected(self) -> ScenePolicy:
        return next(scene for scene in self.scenes if scene.id == self.active_scene)

    @property
    def scene_id(self) -> str:
        return self.active_scene

    @property
    def flee(self) -> FleePolicy:
        return self.selected.flee

    def spells_for_entry(self, entry: int) -> tuple[SpellPolicy, ...]:
        return tuple(item for item in self.selected.spells if item.entry == entry)

    def spell(self, entry: int, spell: int) -> SpellPolicy:
        found = next((item for item in self.selected.spells
                      if item.entry == entry and item.spell == spell), None)
        if found is None:
            raise ValueError(f"policy has no spell {spell} for entry {entry}")
        return found


class BehaviorPolicyV3(BaseModel):
    model_config = _STRICT_FROZEN
    schema_version: Literal[3]
    revision: int = Field(strict=True, ge=1, le=4_294_967_295)
    groups: tuple[GroupPolicy, ...]

    @field_validator("schema_version", mode="before")
    @classmethod
    def _strict_schema_version(cls, value):
        if type(value) is not int or value != 3:
            raise ValueError("schema_version must be integer 3")
        return value

    @field_validator("groups", mode="before")
    @classmethod
    def _freeze_groups(cls, value):
        if type(value) is not list or not 1 <= len(value) <= 64:
            raise ValueError("groups must be a JSON array of 1..64 records")
        return tuple(value)

    @model_validator(mode="after")
    def _validate_groups(self) -> "BehaviorPolicyV3":
        ids = [group.id for group in self.groups]
        if len(ids) != len(set(ids)):
            raise ValueError("groups must have unique ids")
        # The first world-map implementation's native registry is keyed by
        # database spawn id. Instance-aware/map-scoped reuse stays reserved
        # until that registry identity changes end to end.
        ownership = [actor.spawn_id for group in self.groups
                     for actor in (*group.actors, *group.runtime_summons,
                                   *group.go_runtime_summons)]
        if len(ownership) != len(set(ownership)):
            raise ValueError("actor spawn ownership must be globally unique")
        return self

    def group(self, group_id: str) -> GroupPolicy | None:
        return next((group for group in self.groups if group.id == group_id), None)


@dataclass(frozen=True)
class SelectedPolicy:
    """One request's immutable view of a legacy scene or v3 group."""

    revision: int
    group_id: str
    flee: FleePolicy
    spells: tuple[SpellPolicy | SpellPolicyV3, ...]
    aggro_talk: AggroTalkPolicy
    ambient_talk: AmbientTalkPolicy
    health_phases: tuple[HealthPhasePolicy, ...]
    health_events: tuple[HealthEventPolicy, ...]
    aggro_events: tuple[AggroEventPolicy, ...]
    actor: ActorPolicy | RuntimeSummonPolicy | GoRuntimeSummonPolicy | None
    actors: tuple[ActorPolicy | RuntimeSummonPolicy | GoRuntimeSummonPolicy, ...]
    dynamic: bool

    def spells_for_entry(self, entry: int) -> tuple[SpellPolicy | SpellPolicyV3, ...]:
        return tuple(item for item in self.spells if item.entry == entry)

    def spell(self, entry: int, spell: int) -> SpellPolicy | SpellPolicyV3:
        found = next((item for item in self.spells
                      if item.entry == entry and item.spell == spell), None)
        if found is None:
            raise ValueError(f"policy has no spell {spell} for entry {entry}")
        return found

    @property
    def actor_aggro_talk(self) -> bool:
        return self.actor is not None and self.actor.aggro_talk

    @property
    def actor_ambient_talk(self) -> bool:
        return self.actor is not None and self.actor.ambient_talk

    def actor_for_spawn(self, spawn_id: int) -> ActorPolicy | RuntimeSummonPolicy | GoRuntimeSummonPolicy | None:
        return next((actor for actor in self.actors
                     if actor.spawn_id == spawn_id), None)


def select_policy(policy: "Policy", *, schema_version: str,
                  scene_id: str | None, group_id: str | None,
                  spawn_id: int, entry: int) -> SelectedPolicy:
    """Bind a request identity to exactly one validated policy group."""
    if schema_version == "1.8":
        if not isinstance(policy, BehaviorPolicyV3) or group_id is None:
            raise ValueError("protocol 1.8 requires policy schema v3 and group_id")
        group = policy.group(group_id)
        if group is None or not group.enabled:
            raise ValueError("request group is absent or disabled")
        actor = group.actor(spawn_id)
        if actor is None or actor.entry != entry:
            raise ValueError("request actor does not match group roster")
        # Process-local group/NPC overlays are authoritative for the live AI
        # owner and intentionally are not mirrored into this durable policy.
        # binding_generation fences a response across every such handoff;
        # here we only prove that C++ named an enabled configured roster.
        flee = group.flee
        if actor.flee_enabled is not None and actor.flee_enabled != flee.enabled:
            flee = flee.model_copy(update={"enabled": actor.flee_enabled})
        return SelectedPolicy(
            policy.revision, group.id, flee, tuple(group.spells),
            group.aggro_talk, group.ambient_talk,
            tuple(phase for phase in group.health_phases
                  if phase.actor_id == spawn_id),
            tuple(event for event in group.health_events
                  if event.actor_id == spawn_id),
            tuple(event for event in group.aggro_events
                  if event.actor_id == spawn_id), actor,
            tuple((*group.actors, *group.runtime_summons,
                   *group.go_runtime_summons)), True)

    if isinstance(policy, BehaviorPolicyV3):
        raise ValueError("policy schema v3 requires protocol 1.8")
    if scene_id is None or policy.scene_id != scene_id:
        raise ValueError("policy scene mismatch")
    if isinstance(policy, BehaviorPolicyV2):
        selected = policy.selected
        spells: tuple[SpellPolicy | SpellPolicyV3, ...] = tuple(selected.spells)
        talk = selected.aggro_talk
    else:
        spells = tuple(policy.spells)
        talk = AggroTalkPolicy(enabled=False, chance_pct=0, text_group=0)
    ambient = AmbientTalkPolicy.model_validate({
        "enabled": False, "initial_ms": [0, 0], "repeat_ms": [0, 0],
        "chance_pct": 0, "text_group": 0})
    return SelectedPolicy(policy.revision, scene_id, policy.flee, spells,
                          talk, ambient, (), (), (), None, (), False)


# Compatibility name used by the old offline probe. Runtime code uses Policy
# because v2 may select either immutable shape.
BehaviorPolicy = BehaviorPolicyV1
Policy = BehaviorPolicyV1 | BehaviorPolicyV2 | BehaviorPolicyV3


class PolicyUnavailable(Exception):
    """No last-good policy, or the request names a different revision."""


def _object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _parse(raw: bytes) -> Policy:
    if len(raw) > _MAX_POLICY_BYTES:
        raise ValueError(f"policy exceeds {_MAX_POLICY_BYTES} bytes")
    try:
        value = json.loads(
            raw, object_pairs_hook=_object,
            parse_constant=lambda value: (_ for _ in ()).throw(
                ValueError(f"JSON constant {value!r} is not allowed")))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise ValueError(f"invalid policy JSON: {exc}") from exc
    if not isinstance(value, dict) or type(value.get("schema_version")) is not int:
        raise ValueError("schema_version must be an integer")
    if value["schema_version"] == 1:
        return BehaviorPolicyV1.model_validate(value)
    if value["schema_version"] == 2:
        return BehaviorPolicyV2.model_validate(value)
    if value["schema_version"] == 3:
        return BehaviorPolicyV3.model_validate(value)
    raise ValueError("unsupported schema_version")


def validate_policy_file(path: Path) -> Policy:
    """CLI/deployer validation uses the exact same parser as runtime."""
    with path.open("rb") as stream:
        raw = stream.read(_MAX_POLICY_BYTES + 1)
    return _parse(raw)


def _validate_installed_transition_policy(path: Path) -> tuple[Policy, bool]:
    """Read an installed policy, including narrowly pinned historical shapes.

    Revision 15 made ``ambient_talk`` explicit and required for every group.
    A revision-14 file can still be installed while the deployer is replacing
    it, so the transition fence must be able to compare that exact historical
    shape.  This compatibility path is deliberately unavailable to runtime
    loading and candidate validation: only the installed side of a
    higher-revision deployment may synthesize the old implicit disabled value.
    Revision 35 also reached the installed path with 18 missing ``timer_mode``
    keys in six new groups. The gateway had defaulted those keys to ``timer``
    while native rejected the entire policy. Only this exact installed shape
    may migrate; candidates and runtime still require the explicit key.
    """
    with path.open("rb") as stream:
        raw = stream.read(_MAX_POLICY_BYTES + 1)
    try:
        return _parse(raw), False
    except ValueError as strict_error:
        if len(raw) > _MAX_POLICY_BYTES:
            raise strict_error
        try:
            value = json.loads(
                raw, object_pairs_hook=_object,
                parse_constant=lambda item: (_ for _ in ()).throw(
                    ValueError(f"JSON constant {item!r} is not allowed")))
        except (json.JSONDecodeError, UnicodeDecodeError, ValueError):
            raise strict_error
        legacy_timer_groups = {
            "ship_lower_approach", "ship_lower_corridor", "ship_mid_lower_deck",
            "ship_mid_upper_deck", "cookie_room_rim", "aft_lower_ship",
        }
        legacy_missing = {
            (group_id, 1732, spell)
            for group_id in legacy_timer_groups
            for spell in (12544, 2138, 4979)
        }
        if (isinstance(value, dict) and value.get("schema_version") == 3
                and value.get("revision") == 35
                and isinstance(value.get("groups"), list)):
            missing = set()
            for group in value["groups"]:
                if not isinstance(group, dict) or not isinstance(group.get("spells"), list):
                    raise strict_error
                for spell in group["spells"]:
                    if not isinstance(spell, dict):
                        raise strict_error
                    if "timer_mode" not in spell:
                        missing.add((group.get("id"), spell.get("entry"), spell.get("spell")))
            if missing == legacy_missing:
                for group in value["groups"]:
                    if group["id"] in legacy_timer_groups:
                        for spell in group["spells"]:
                            if "timer_mode" not in spell:
                                spell["timer_mode"] = "timer"
                return BehaviorPolicyV3.model_validate(value), True
        if (not isinstance(value, dict)
                or value.get("schema_version") != 3
                or value.get("revision") != 14
                or not isinstance(value.get("groups"), list)):
            raise strict_error
        if (not value["groups"]
                or any(not isinstance(group, dict) for group in value["groups"])
                or any("ambient_talk" in group for group in value["groups"])):
            # Revision 14's historical shape omitted the descriptor from
            # every group.  A mixed file was never published and must not be
            # normalized into a shape that appears legitimate.
            raise strict_error
        for group in value["groups"]:
            group["ambient_talk"] = {
                "enabled": False,
                "initial_ms": [0, 0],
                "repeat_ms": [0, 0],
                "chance_pct": 0,
                "text_group": 0,
            }
        return BehaviorPolicyV3.model_validate(value), True


def _semantic_digest(policy: Policy) -> str:
    canonical = json.dumps(policy.model_dump(), sort_keys=True,
                           separators=(",", ":")).encode()
    return hashlib.sha256(canonical).hexdigest()


def validate_policy_transition(path: Path, previous: Path) -> Policy:
    """Validate a deploy candidate against the installed last-good revision."""
    candidate = validate_policy_file(path)
    installed, migrated = _validate_installed_transition_policy(previous)
    if migrated and candidate.revision <= installed.revision:
        raise ValueError(
            f"legacy behavior policy migration requires a higher revision "
            f"{installed.revision}->{candidate.revision}")
    if candidate.revision < installed.revision:
        raise ValueError(
            f"behavior policy revision rollback {installed.revision}->{candidate.revision}")
    if (candidate.revision == installed.revision
            and _semantic_digest(candidate) != _semantic_digest(installed)):
        raise ValueError(
            f"behavior policy revision {candidate.revision} changed content")
    if (not isinstance(candidate, BehaviorPolicyV3)
            and not isinstance(installed, BehaviorPolicyV3)
            and candidate.scene_id != installed.scene_id):
        raise ValueError(
            f"active_scene change requires a world restart: {installed.scene_id}->{candidate.scene_id}")
    return candidate


def validate_restart_scene_transition(path: Path, previous: Path) -> BehaviorPolicyV2:
    """Validate the one deployment-only transition between owned scenes.

    This deliberately does not relax :func:`validate_policy_transition`, which
    is also the hot-reload fence.  A caller must separately prove that the
    worldserver is stopped before using this validator to atomically replace
    the installed policy for its next start.
    """
    candidate = validate_policy_file(path)
    installed = validate_policy_file(previous)
    if not isinstance(candidate, BehaviorPolicyV2):
        raise ValueError("restart scene transition requires schema_version 2 for the candidate policy")
    if not isinstance(installed, (BehaviorPolicyV1, BehaviorPolicyV2)):
        raise ValueError("restart scene transition has an unsupported installed policy schema")
    if candidate.revision <= installed.revision:
        raise ValueError(
            f"restart scene transition requires a higher revision "
            f"{installed.revision}->{candidate.revision}")
    if candidate.scene_id == installed.scene_id:
        raise ValueError(
            f"restart scene transition requires a changed active_scene "
            f"({installed.scene_id})")
    return candidate


class PolicyStore:
    def __init__(self, path: Path = BEHAVIOR_POLICY_PATH) -> None:
        self._path = path
        self._lock = threading.Lock()
        self._signature: tuple[int, int, int, int] | None = None
        self._current: Policy | None = None
        self._previous: Policy | None = None
        self._digest: str | None = None
        self._error = "policy has not been loaded"

    def for_revision(self, revision: int) -> Policy:
        self._refresh()
        with self._lock:
            current = self._current
            previous = self._previous
            error = self._error
        if current is None:
            raise PolicyUnavailable(error)
        if current.revision == revision:
            return current
        if previous is not None and previous.revision == revision:
            return previous
        if current.revision != revision:
            raise PolicyUnavailable(
                f"policy revision mismatch: request {revision}, gateway {current.revision}")
        return current

    def status(self) -> dict:
        self._refresh()
        with self._lock:
            return {"revision": self._current.revision if self._current else None,
                    "previous_revision": (self._previous.revision
                                          if self._previous else None),
                    "available": self._current is not None,
                    # A rejected reload is operationally important even when
                    # the last-good revision remains available to requests.
                    "error": self._error}

    def _refresh(self) -> None:
        try:
            with self._path.open("rb") as stream:
                stat = os.fstat(stream.fileno())
                signature = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
                with self._lock:
                    if signature == self._signature:
                        return
                raw = stream.read(_MAX_POLICY_BYTES + 1)
        except OSError as exc:
            with self._lock:
                self._error = f"behavior policy unavailable: {type(exc).__name__}"
            return

        try:
            candidate = _parse(raw)
        except Exception as exc:
            with self._lock:
                self._signature = signature
                self._error = f"invalid behavior policy: {type(exc).__name__}"
            _log.warning("woj: rejected behavior policy reload: %s", self._error)
            return

        digest = _semantic_digest(candidate)

        with self._lock:
            current = self._current
            if current is not None and candidate.revision < current.revision:
                self._signature = signature
                self._error = (f"behavior policy revision rollback "
                               f"{current.revision}->{candidate.revision} rejected")
                _log.warning("woj: %s", self._error)
                return
            if (current is not None and candidate.revision == current.revision
                    and digest != self._digest):
                self._signature = signature
                self._error = (f"behavior policy revision {candidate.revision} "
                               "changed content")
                _log.warning("woj: %s", self._error)
                return
            if (current is not None
                    and not isinstance(candidate, BehaviorPolicyV3)
                    and not isinstance(current, BehaviorPolicyV3)
                    and candidate.scene_id != current.scene_id):
                self._signature = signature
                self._error = ("active_scene change requires a world restart "
                               f"({current.scene_id}->{candidate.scene_id})")
                _log.warning("woj: %s", self._error)
                return
            self._signature = signature
            if current is not None and candidate.revision > current.revision:
                self._previous = current
            self._current = candidate
            self._digest = digest
            self._error = ""


store = PolicyStore()


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate World of Jevs behavior policy")
    parser.add_argument("path", nargs="?", type=Path, default=BEHAVIOR_POLICY_PATH)
    transition = parser.add_mutually_exclusive_group()
    transition.add_argument("--previous", type=Path,
                            help="also reject rollback or changed same revision")
    transition.add_argument("--restart-scene-previous", type=Path,
                            help="allow only a higher-revision switch into v2 for a stopped-world deployment")
    parser.add_argument("--print-active-scene", action="store_true",
                        help="print only the validated selected scene id")
    args = parser.parse_args()
    try:
        if args.previous is not None:
            policy = validate_policy_transition(args.path, args.previous)
        elif args.restart_scene_previous is not None:
            policy = validate_restart_scene_transition(args.path, args.restart_scene_previous)
        else:
            policy = validate_policy_file(args.path)
    except (OSError, ValueError) as exc:
        parser.exit(2, f"invalid behavior policy: {exc}\n")
    if args.print_active_scene:
        if isinstance(policy, BehaviorPolicyV3):
            print(",".join(group.id for group in policy.groups if group.enabled))
        else:
            print(policy.scene_id)
    else:
        print(f"valid behavior policy revision {policy.revision}: {args.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
