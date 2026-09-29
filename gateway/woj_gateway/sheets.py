"""Строгая загрузка постоянных листов способностей по creature entry.

Лист — часть промпта, а не вход от игрока. Всё же он читается как внешний
JSON: ранний и подробный отказ лучше, чем тихо изменить поведение NPC после
редактирования одного файла.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator


SHEETS_DIR = Path(__file__).with_name("sheets")
_MAX_SHEET_BYTES = 16 * 1024
_MAX_BRIEF_PROMPT_CHARS = 480


_BRIEF_PROMPT_REPLACEMENTS = (
    ("Knows only visible state, nearby kin, listed triggers, and abilities.",
     "state/kin/listed triggers/abilities only"),
    ("No authored voice style.", "unspecified"),
    ("No authored speech behavior.", "none authored"),
    ("Use only eligible actions and targets.",
     "eligible listed actions/targets only"),
    ("Do not invent unseen facts or abilities.",
     "no invented facts/abilities"),
    ("Remote-Controlled", "RC"),
    (" in the Deadmines ", " in "),
    ("the Deadmines ", ""),
    ("Nearby ", ""),
    ("nearby ", ""),
    ("Visible ", ""),
    ("visible ", ""),
    (", and ", "/"),
    (" and ", "/"),
    (" entering the ", " in "),
    (" threatening the ", " threatening "),
    ("below fifteen percent health", "under 15% HP"),
    ("When nearly dead, ", "near death, "),
    ("Is defended by ", "Defended by "),
    ("Fights as one of ", "One of "),
    ("Can summon a ", "Summons "),
    ("Uses only configured ", "configured "),
    ("No authored ", "no authored "),
    # The two longest shipped briefs use these equivalent short forms.  The
    # source card remains untouched; only the provider projection is compact.
    ("Goblin Engineer in the Foundry", "Goblin Engineer"),
    ("Defend Gilnid with ranged fire/configured machinery", "Defend Gilnid/Foundry"),
    ("Gilnid, Foundry defenders/summoned golems", "Gilnid/defenders/golems"),
    ("hostiles in Foundry", "Foundry hostiles"),
    ("Summons RC Golem beside the group", "Summons RC Golem"),
    ("Defensive/machinery-focused", "Defensive/technical"),
    ("Flees toward Foundry defenders under 15% HP", "Flee to defenders under 15% HP"),
    ("RC Golem summoned in the Foundry", "RC Golem"),
    ("Fight beside the Foundry defenders until destroyed", "Fight with defenders until destroyed"),
    ("Summoning engineer/Foundry defenders", "Summoner/defenders"),
    ("hostiles fighting the Foundry group", "group hostiles"),
    ("Is summoned by a Goblin Engineer", "Summoned by Engineer"),
    ("Fights until destroyed/does not flee", "Fight until destroyed; never flee"),
)


def _words(value: str) -> int:
    return len(value.split())


def _constant_text(value: str, *, field: str, maximum_words: int) -> str:
    """Отсеять пустой или разросшийся текст до сборки вопроса Jev."""
    if value != value.strip() or not value:
        raise ValueError(f"{field} must be non-empty without surrounding whitespace")
    if "{" in value or "}" in value:
        raise ValueError(f"{field} must be a constant string, not a template")
    if _words(value) > maximum_words:
        raise ValueError(f"{field} must contain at most {maximum_words} words")
    return value


def _brief_prompt_text(value: str) -> str:
    for source, replacement in _BRIEF_PROMPT_REPLACEMENTS:
        value = value.replace(source, replacement)
    return value.removesuffix(".")


class Ability(BaseModel):
    """Одна разрешённая для entry способность и её краткое знание."""

    model_config = ConfigDict(extra="forbid", strict=True)

    spell: int = Field(gt=0, le=4_294_967_295)
    slug: str = Field(pattern=r"^[a-z]+(?:_[a-z]+)*$")
    name: str = Field(min_length=1)
    target: Literal["enemy", "ally", "self"]
    does: str
    tendency: str
    ally_below_pct: int | None = Field(default=None, ge=1, le=100)

    @model_validator(mode="after")
    def _validate_text_and_target(self) -> "Ability":
        self.name = _constant_text(self.name, field="name", maximum_words=12)
        self.does = _constant_text(self.does, field="does", maximum_words=12)
        self.tendency = _constant_text(
            self.tendency, field="tendency", maximum_words=12)
        if self.target == "ally" and self.ally_below_pct is None:
            raise ValueError("ally abilities require ally_below_pct")
        if self.target != "ally" and self.ally_below_pct is not None:
            raise ValueError("non-ally abilities must not set ally_below_pct")
        return self


class CharacterBrief(BaseModel):
    """Bounded authored context which does not grant executable actions.

    Version one sheets predate this layer.  Version two makes every field
    explicit so an unknown original character never silently inherits lore
    from the provider model.
    """

    model_config = ConfigDict(extra="forbid", strict=True)

    identity: str
    local_goal: str
    allies: list[str] = Field(min_length=1, max_length=3)
    enemies: list[str] = Field(min_length=1, max_length=3)
    relationships: list[str] = Field(min_length=1, max_length=3)
    temperament: str
    risk: str
    knowledge_boundary: str
    voice: str
    speech: str
    constraints: list[str] = Field(min_length=1, max_length=4)

    @model_validator(mode="after")
    def _validate_brief(self) -> "CharacterBrief":
        scalar_limits = {
            "identity": 18,
            "local_goal": 18,
            "temperament": 14,
            "risk": 14,
            "knowledge_boundary": 20,
            "voice": 12,
            "speech": 14,
        }
        for field, maximum in scalar_limits.items():
            setattr(self, field, _constant_text(
                getattr(self, field), field=f"brief.{field}",
                maximum_words=maximum))
        list_limits = {
            "allies": 10,
            "enemies": 10,
            "relationships": 14,
            "constraints": 14,
        }
        for field, maximum in list_limits.items():
            values = getattr(self, field)
            checked = [_constant_text(value, field=f"brief.{field}",
                                      maximum_words=maximum)
                       for value in values]
            if len(checked) != len(set(checked)):
                raise ValueError(f"brief.{field} must not repeat values")
            setattr(self, field, checked)
        total_words = sum(_words(value) for value in (
            self.identity, self.local_goal, *self.allies, *self.enemies,
            *self.relationships, self.temperament, self.risk,
            self.knowledge_boundary, self.voice, self.speech,
            *self.constraints))
        if total_words > 100:
            raise ValueError("character brief must contain at most 100 words")
        prompt_chars = len(json.dumps(
            self.prompt_state(), ensure_ascii=False, separators=(",", ":")))
        if prompt_chars > _MAX_BRIEF_PROMPT_CHARS:
            raise ValueError(
                f"character brief prompt exceeds {_MAX_BRIEF_PROMPT_CHARS} characters")
        return self

    def prompt_state(self) -> dict[str, str]:
        """Compact stable projection used in provider payloads."""
        return {
            "id": _brief_prompt_text(self.identity),
            "goal": _brief_prompt_text(self.local_goal),
            "ally": "/".join(_brief_prompt_text(value) for value in self.allies),
            "enemy": "/".join(_brief_prompt_text(value) for value in self.enemies),
            "ties": "/".join(_brief_prompt_text(value) for value in self.relationships),
            "mood": _brief_prompt_text(self.temperament),
            "risk": _brief_prompt_text(self.risk),
            "known": _brief_prompt_text(self.knowledge_boundary),
            "voice": _brief_prompt_text(self.voice),
            "speech": _brief_prompt_text(self.speech),
            "rules": "/".join(_brief_prompt_text(value) for value in self.constraints),
        }

    def prompt_line(self) -> str:
        """Still-labelled projection with less JSON overhead for split P0."""
        return ";".join(f"{key}={value}"
                        for key, value in self.prompt_state().items())


class Sheet(BaseModel):
    """Самоописание конкретного creature_template для Jev."""

    model_config = ConfigDict(extra="forbid", strict=True)

    entry: int = Field(gt=0, le=4_294_967_295)
    schema_version: Literal[1, 2] = 1
    you: str
    character_brief: CharacterBrief | None = None
    abilities: list[Ability] = Field(default_factory=list)
    flee: str

    @model_validator(mode="after")
    def _validate_sheet(self) -> "Sheet":
        self.you = _constant_text(self.you, field="you", maximum_words=25)
        self.flee = _constant_text(self.flee, field="flee", maximum_words=12)
        if self.schema_version == 1 and self.character_brief is not None:
            raise ValueError("schema v1 must not contain character_brief")
        if self.schema_version == 2 and self.character_brief is None:
            raise ValueError("schema v2 requires character_brief")
        spells = [ability.spell for ability in self.abilities]
        slugs = [ability.slug for ability in self.abilities]
        if len(spells) != len(set(spells)):
            raise ValueError("abilities must not repeat a spell")
        if len(slugs) != len(set(slugs)):
            raise ValueError("abilities must not repeat a slug")
        return self


def _json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """JSON с повторённым ключом неоднозначен, поэтому не принимается."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"JSON constant {value!r} is not allowed")


def load_sheet(entry: int) -> Sheet:
    """Загрузить единственный допустимый лист для creature entry.

    `entry` не становится частью произвольного пути: только строго
    положительное десятичное имя файла выводится из validated policy/actor.
    """
    if type(entry) is not int or not 0 < entry <= 4_294_967_295:
        raise ValueError(f"no sheet for entry {entry!r}")

    path = SHEETS_DIR / f"{entry}.json"
    try:
        data = path.read_bytes()
        if len(data) > _MAX_SHEET_BYTES:
            raise ValueError(f"sheet {entry} exceeds {_MAX_SHEET_BYTES} bytes")
        raw = json.loads(
            data,
            object_pairs_hook=_json_object,
            parse_constant=_reject_json_constant,
        )
    except OSError as exc:
        raise RuntimeError(f"cannot read sheet {entry}: {exc}") from exc
    except (json.JSONDecodeError, ValueError) as exc:
        raise ValueError(f"invalid JSON in sheet {entry}: {exc}") from exc

    try:
        sheet = Sheet.model_validate(raw)
    except ValidationError as exc:
        raise ValueError(f"invalid sheet {entry}: {exc}") from exc
    if sheet.entry != entry:
        raise ValueError(f"sheet filename {entry} disagrees with entry {sheet.entry}")
    return sheet
