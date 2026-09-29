"""Порядок ярусов и правило деградации — в одном месте.

Размазанная по app.py деградация означала бы, что при добавлении яруса
Laya (Ф6) её придётся искать по всему файлу. Здесь же живёт кэш снимка:
он не оптимизация, а статья бюджета и одновременно лекарство от
однообразия обучающей выборки — снимок, который не изменился, не порождает
ни платного вызова, ни новой записи.
"""
import dataclasses
import hashlib
import json
import random
import threading
import time
import uuid
from dataclasses import dataclass
from typing import Literal

from . import config, jev
from .budget import Budget, Charge
from .config import VALID_FOR_SECONDS
from .contract import Action, DecideRequest, Marker
from .jev import JevResult, JevTargetUnavailable, JevUnavailable
from .policy import PolicyUnavailable, store as policy_store


class NoDecision(Exception):
    """Jev produced no usable decision and fail-closed mode forbids fallback."""

    def __init__(self, reason: str, charge: Charge | None = None,
                 provider_calls: list[dict] | None = None) -> None:
        super().__init__(reason)
        self.reason = reason
        self.charge = charge
        self.provider_calls = provider_calls or []


@dataclass(frozen=True)
class TierOutcome:
    action: Action
    marker: Marker
    thought: str
    # Тот же словарь значений, что у DecideResponse.tier — не вынесено в
    # общий алиас, чтобы не расширять contract.py.
    tier: Literal["heuristic", "jev"]
    trainable: bool
    degraded_reason: str
    jev: JevResult | None
    from_cache: bool
    # Идентификатор РЕШЕНИЯ, а не запроса. Живёт здесь, а не в app.py, потому
    # что здесь кэш: попадание в кэш обязано вернуть тот же id, что и решение.
    decision_id: str
    # Что сторож списал — на любом ярусе, не только jev: эвристика, в которую
    # ушёл оплаченный вызов, деньги тоже потратила. None — вызова к
    # провайдеру не было.
    charge: Charge | None = None
    provider_calls: list[dict] = dataclasses.field(default_factory=list)


def _snapshot_key(req: DecideRequest, plan: jev.DecisionPlan) -> str:
    """Хэш того, что влияет на решение.

    Берём state после квантования в строки, а не сырой снимок: дрожание
    координат в сантиметрах не должно считаться изменением обстановки и
    оплачиваться как новое решение.
    """
    # Строки state намеренно не содержат raw GUID. Их добавляем отдельно:
    # одинаковые полосы hp/distance нового врага не вправе повторить ATTACK
    # или CAST, выданный старому. Порядок attackers/allies значим для выбора.
    st = req.state
    targets = {
        "self": req.npc.id,
        "victim": st.victim.id if st.victim else None,
        "attackers": [unit.id for unit in st.attackers],
        "nearest_enemy": st.nearest_enemy.id if st.nearest_enemy else None,
        "enemies": [unit.id for unit in st.enemies],
        "allies": [(ally.id, ally.guid_spawn) for ally in st.allies],
        "heal_allies": [(ally.id, ally.guid_spawn) for ally in st.heal_allies],
        "attack_targets": st.attack_targets,
        "flee_targets": st.flee_targets,
        "call_help_targets": st.call_help_targets,
        "flee_assist_targets": st.flee_assist_targets,
        "spells": [(spell.id, spell.ready,
                    jev._milliseconds_band(spell.cooldown_ms),
                    tuple(spell.valid_targets), tuple(spell.aura_targets))
                   for spell in st.spells],
        "help": [(request.request_id, request.requester.id,
                  request.requester.guid_spawn, request.requester_epoch,
                  request.target.id, jev._ttl_band(request.expires_in_ms))
                 for request in st.help_requests],
        "routine": st.routine.model_dump(),
        # wait_ms тикает и намеренно квантован только в prompt-state. seq,
        # поколения, статусы, причины блокировки и outcomes меняют ключ.
        "behavior": {
            "policy_revision": plan.policy_revision,
            "scene_id": st.behavior.scene_id,
            "group_id": st.behavior.group_id,
            "binding_generation": st.behavior.binding_generation,
            "seq": st.behavior.seq,
            "floor_seq": st.behavior.floor_seq,
            "triggers": [(item.id, item.generation, item.status,
                          item.blocked_by)
                         for item in st.behavior.triggers],
            "events": [(item.seq, item.trigger_id, item.generation, item.kind,
                        item.decision_id, item.reason)
                       for item in st.behavior.events],
        },
    }
    payload = {"server_boot_id": req.server_boot_id,
               "actor": {"raw_id": req.npc.id, "raw_guid": req.npc.raw_guid,
                         "map_id": req.npc.map_id, "instance_id": req.npc.instance_id,
                         "spawn_id": req.npc.guid, "entry": req.npc.entry, "epoch": req.npc.epoch},
               "state": plan.state,
               "questions": plan.questions,
               "targets": targets}
    blob = json.dumps(payload, ensure_ascii=False, sort_keys=True)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


class Tiers:
    def __init__(self, budget: Budget) -> None:
        self._budget = budget
        self._counts = {"jev": 0, "heuristic": 0, "cached": 0,
                        "unavailable": 0}
        # (ключ снимка, решение, время по monotonic()) — единственные часы,
        # которым можно доверять при переводе времени на машине.
        # Один слот на live actor (group/map/instance/raw GUID). boot/epoch
        # живут в хэше, поэтому старая жизнь не попадёт в кэш, а две copies
        # одного static spawn не могут обменяться решением.
        self._cache: dict[tuple[str, str, int, int, str, int, int],
                          tuple[str, TierOutcome, float]] = {}
        # v1_decide выполняется в пуле потоков FastAPI: _cache/_counts трогают
        # из разных потоков одновременно, `+= 1` и запись в словарь не
        # атомарны. Замок — только вокруг этого состояния, не вокруг
        # jev.decide() в _fresh: тот сетевой вызов может занять больше
        # секунды, и держать его под замком сериализовало бы всех мурлоков
        # в очередь на один запрос.
        self._lock = threading.Lock()

    def stats(self) -> dict:
        with self._lock:
            return dict(self._counts)

    def decide(self, req: DecideRequest) -> TierOutcome:
        binding = req.state.behavior.binding_generation or 0
        group = req.state.behavior.group_id or req.state.behavior.scene_id or ""
        rng = random.Random(
            f"{group}:{binding}:{req.npc.guid}:{req.npc.id}:{req.snapshot_seq}")
        try:
            policy = policy_store.for_revision(req.state.behavior.policy_revision)
            plan = jev.build_decision_plan(req, policy)
        except PolicyUnavailable as exc:
            self._unavailable()
            raise NoDecision(str(exc)) from exc
        except JevUnavailable as exc:
            self._unavailable()
            raise NoDecision(exc.reason) from exc
        except Exception as exc:
            self._unavailable()
            raise NoDecision(f"decision plan unavailable: {type(exc).__name__}") from exc
        key = _snapshot_key(req, plan)
        slot = (req.server_boot_id, group, req.npc.map_id or 0,
                req.npc.instance_id or 0, req.npc.raw_guid or req.npc.id,
                req.npc.epoch, binding)
        with self._lock:
            now = time.monotonic()
            for expired in [item for item, value in self._cache.items()
                            if now - value[2] >= VALID_FOR_SECONDS]:
                self._cache.pop(expired, None)
            cached = self._cache.get(slot)
            # Срок годности — valid_for_seconds того же решения (сейчас общий
            # VALID_FOR_SECONDS). Без него решение Jev залипло бы в кэше
            # навсегда вместо свежего мнения яруса.
            if (cached is not None and cached[0] == key
                    and (now - cached[2]) < VALID_FOR_SECONDS):
                self._counts["cached"] += 1
                # replace сохраняет decision_id исходного решения: в мир
                # должен уйти id записи, которая это решение и описывает.
                return dataclasses.replace(cached[1], from_cache=True)

        outcome = self._fresh(req, rng, plan)
        if outcome.tier == "jev":
            with self._lock:
                self._cache[slot] = (key, outcome, time.monotonic())
        return outcome

    def _fresh(self, req: DecideRequest, rng: random.Random,
               plan: jev.DecisionPlan) -> TierOutcome:
        # allows() и начисление ниже не атомарны: между ними лежит сетевой
        # вызов jev.decide(), который может занять больше секунды. При
        # нескольких одновременных запросах бюджет может чуть превысится —
        # окно нельзя закрыть, цена известна только из ответа провайдера.
        # Один decision protocol 1.7 может запустить до трёх параллельных
        # вызовов; четыре worker мира дают до 12 одновременно. Это граница
        # рабочего контура, не hard reservation бюджета в gateway: мягкий
        # лимит всё ещё может быть немного превышен уже начатыми вызовами.
        # JEV_COST_FALLBACK_USD — честная аварийная оценка одного вызова по
        # старому замеру, не заявленный верх цены нового payload; каждый
        # реально начатый вызов всё равно имеет отдельный Meter.
        if not self._budget.allows():
            return self._heuristic(req, "budget exhausted")
        # Начисление — внутри jev.decide, в момент, когда счёт провайдера уже
        # выставлен: до всякого raise по форме ответа и до нашего падения
        # ниже. Иначе любой путь через исключение уходил бы в эвристику мимо
        # сторожа — провайдер отработал и выставил счёт, а учёта не было.
        #
        try:
            result = jev.decide(req, rng, meter_factory=self._budget.meter,
                                plan=plan)
        except JevTargetUnavailable as exc:
            return self._safe_idle(exc.reason, exc.charge, exc.provider_calls)
        except JevUnavailable as exc:
            return self._heuristic(req, exc.reason, exc.charge,
                                   exc.provider_calls)
        except Exception as exc:  # адаптер не должен ронять мир ничем
            return self._heuristic(req, f"unexpected: {type(exc).__name__}")

        # Повторного начисления здесь нет и быть не должно: на успешном пути
        # meter уже отработал ровно один раз, в том же decide().
        with self._lock:
            self._counts["jev"] += 1
        # trainable — не «ответил Jev», а «ответил Jev и провайдер подтвердил
        # модель». Выборка Ф6 не должна включать записи, где модель
        # подставлена нами.
        return TierOutcome(action=result.action, marker=result.marker,
                           thought=result.thought, tier="jev",
                           trainable=result.model_confirmed,
                           degraded_reason="", jev=result, from_cache=False,
                           decision_id=uuid.uuid4().hex, charge=result.charge,
                           provider_calls=result.provider_calls)

    def _safe_idle(self, reason: str, charge: Charge | None,
                   provider_calls: list[dict]) -> TierOutcome:
        self._unavailable()
        raise NoDecision(reason, charge, provider_calls)

    def _heuristic(self, req: DecideRequest, reason: str,
                   charge: Charge | None = None,
                   provider_calls: list[dict] | None = None) -> TierOutcome:
        self._unavailable()
        raise NoDecision(reason, charge, provider_calls)

    def _unavailable(self) -> None:
        with self._lock:
            self._counts["unavailable"] += 1
