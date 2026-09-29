import re
import uuid

from fastapi import FastAPI, HTTPException, Request, status
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from . import config, jev
from .activity import Activity
from .budget import Budget
from .contract import DecideRequest, DecideResponse
from .journal import Journal
from .policy import store as policy_store
from .tiers import NoDecision, Tiers

app = FastAPI(title="World of Jevs gateway", version="1.0")
journal = Journal(config.JOURNAL_PATH)
activity = Activity(config.ACTIVITY_PATH, journal_path=config.JOURNAL_PATH,
                    rotations=config.ACTIVITY_ROTATIONS)
jev.set_activity(activity)
budget = Budget(config.JEV_BUDGET_USD)
tiers = Tiers(budget)


@app.get("/healthz")
def healthz() -> dict:
    policy = policy_store.status()
    jev_enabled = (jev.key_present() and not budget.exhausted()
                   and policy["available"])
    decision_available = (policy["available"]
                          and (jev_enabled or not config.JEV_ONLY))
    return {
        "status": "ok",
        # "Текущий" ярус — что попробует СЛЕДУЮЩИЙ свежий запрос, а не то,
        # чем реально ответил последний (тот мог быть из кэша).
        "tier": ("jev" if jev_enabled else
                 "heuristic" if policy["available"] and not config.JEV_ONLY
                 else "unavailable"),
        "jev_only": config.JEV_ONLY,
        "jev_enabled": jev_enabled,
        "decision_available": decision_available,
        "behavior_policy": policy,
        "budget_spent_usd": budget.spent(),
        "budget_limit_usd": budget.limit(),
        # Число цен провайдера, подменённых нашей оценкой из-за порчи
        # (не число, вне диапазона); «цену не прислал» сюда не входит.
        "budget_anomalies": budget.anomalies(),
        "counts": tiers.stats(),
        "activity": activity.status(),
        "provider_http": {
            "timeouts_seconds": {
                "connect": config.JEV_CONNECT_TIMEOUT_SECONDS,
                "read": config.JEV_TIMEOUT_SECONDS,
                "write": config.JEV_WRITE_TIMEOUT_SECONDS,
                "pool": config.JEV_POOL_TIMEOUT_SECONDS,
            },
            # Полезно для оператора, но не обещает deadline: httpx timeout
            # фазовый, а DNS getaddrinfo не имеет отдельного потолка.
            "nominal_phase_sum_seconds": round(
                config.JEV_CONNECT_TIMEOUT_SECONDS
                + config.JEV_TIMEOUT_SECONDS
                + config.JEV_WRITE_TIMEOUT_SECONDS
                + config.JEV_POOL_TIMEOUT_SECONDS,
                6,
            ),
            "hard_total_deadline_seconds": None,
            "dns_timeout_bounded": False,
            "max_connections": config.JEV_MAX_CONNECTIONS,
            "max_keepalive_connections": config.JEV_MAX_KEEPALIVE_CONNECTIONS,
            "keepalive_seconds": config.JEV_KEEPALIVE_SECONDS,
        },
    }


@app.exception_handler(RequestValidationError)
def on_invalid_request(request: Request, exc: RequestValidationError) -> JSONResponse:
    """Answer 422 without echoing the value that failed.

    FastAPI's default handler echoes the offending input; for a NaN that
    makes Starlette fail to serialise its own error response, so the client
    gets a 500 instead of a 422.
    """
    errors = [{"loc": list(e["loc"]), "msg": e["msg"], "type": e["type"]}
              for e in exc.errors()]
    request_id = None
    if isinstance(exc.body, dict):
        candidate = exc.body.get("request_id")
        if isinstance(candidate, str) and re.fullmatch(r"[0-9a-f]{16}", candidate):
            request_id = candidate
    # Never persist the rejected request body: it may contain malformed or
    # non-finite values.  The field path and validator reason are sufficient
    # to diagnose a bad world snapshot without leaking or re-serialising it.
    activity.write("request_invalid", {
        "path": request.url.path,
        "request_id": request_id,
        "errors": errors,
    })
    return JSONResponse(
        status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
        content={"detail": errors},
    )


# Annotated with the model, not a raw Response: that is what puts
# DecideResponse into /openapi.json, the contract C++ checks its parser against.
@app.post("/v1/decide")
def v1_decide(req: DecideRequest) -> DecideResponse:
    # Это единственный честный факт о принятом gateway запросе до результата.
    # Полный state остаётся только в decision journal, здесь — компактная
    # identity и состояние триггеров для терминального наблюдателя.
    common = {
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
    activity.write("server_request", {
        **common,
        "trigger_summary": [item.model_dump() for item in req.state.behavior.triggers],
    })
    try:
        outcome = tiers.decide(req)
    except NoDecision as exc:
        decision_id = uuid.uuid4().hex
        record = {
            "decision_id": decision_id,
            "request_id": req.request_id,
            "schema_version": req.schema_version,
            "server_boot_id": req.server_boot_id,
            "snapshot_seq": req.snapshot_seq,
            "transport": req.transport.model_dump(),
            "npc": req.npc.model_dump(),
            "tier": "unavailable",
            "state": req.state.model_dump(),
            # Journal-only sentinel: it is deliberately outside Action and is
            # never returned as a successful decision to the executor.
            "action": {"kind": "NO_DECISION"},
            "outcome": "rejected",
            "trainable": False,
            "degraded_reason": exc.reason,
            "from_cache": False,
            "provider_calls": exc.provider_calls,
            "prompt_mode": ("parallel" if len(exc.provider_calls) > 1
                            else "single" if exc.provider_calls else "none"),
        }
        if exc.charge is not None:
            record.update({
                "cost_usd": exc.charge.usd,
                "cost_is_estimate": exc.charge.is_estimate,
                "cost_estimate_reason": exc.charge.estimate_reason,
            })
        journal.write(record)
        activity.write("request_failed", {
            **common,
            "decision_id": decision_id,
            "tier": "unavailable",
            "failure": "unavailable",
            "reason": exc.reason,
        })
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail={"code": "NO_DECISION", "decision_id": decision_id,
                    "reason": exc.reason},
        )
    resp = DecideResponse(
        request_id=req.request_id,
        # id берётся у решения, а не сочиняется здесь: попадание в кэш обязано
        # вернуть id того решения, которое кэш отдал.
        decision_id=outcome.decision_id,
        tier=outcome.tier,
        action=outcome.action,
        thought=outcome.thought,
        marker=outcome.marker,
        valid_for_seconds=config.VALID_FOR_SECONDS,
    )
    # Решение из кэша в журнал не пишется: повтор раздул бы обучающую выборку
    # записями, которых в мире не было.
    if not outcome.from_cache:
        # `snapshot_seq` — счётчик per-NPC, обнуляется при рестарте
        # worldserver'а; без `server_boot_id` записи из разных прогонов
        # неразличимы.
        record = {
            "decision_id": resp.decision_id,
            "request_id": req.request_id,
            "schema_version": req.schema_version,
            "server_boot_id": req.server_boot_id,
            "snapshot_seq": req.snapshot_seq,
            # Transport — доказательство свежести wire, но не смысловой
            # факт для prompt/cache: от течения часов решение не меняется.
            "transport": req.transport.model_dump(),
            "npc": req.npc.model_dump(),
            "tier": resp.tier,
            "state": req.state.model_dump(),
            "action": resp.action.model_dump(),
            "thought": outcome.thought,
            "marker": outcome.marker,
            "valid_for_seconds": resp.valid_for_seconds,
            "outcome": "issued",
            "trainable": outcome.trainable,
            "degraded_reason": outcome.degraded_reason,
            "from_cache": outcome.from_cache,
            "provider_calls": outcome.provider_calls,
            "prompt_mode": ("parallel" if len(outcome.provider_calls) > 1
                            else "single" if outcome.provider_calls else "none"),
        }
        if outcome.jev is not None:
            j = outcome.jev
            record.update({
                "model": j.model,
                "answers": j.answers,
                "prompt_mode": j.prompt_mode,
                "latency_ms": j.latency_ms,
                "billed_input_tokens": j.billed_input_tokens,
                "billed_output_tokens": j.billed_output_tokens,
                "own_tokens_estimate": j.own_tokens_estimate,
            })
            if j.prompt_mode == "single":
                record.update({"questions_sent": j.questions_sent,
                               "state_sent": j.state_sent})
            else:
                # Exact wire payloads live in provider_calls. These two are
                # the merged logical decision view, not a claimed API request.
                record.update({"logical_questions": j.questions_sent,
                               "logical_state": j.state_sent})
        # Цена — из того, что списал сторож (Budget), не пересчитывается
        # здесь: сумма cost_usd по журналу обязана сходиться со spent().
        if outcome.charge is not None:
            c = outcome.charge
            record.update({
                "cost_usd": c.usd,
                # cost_is_estimate читает woj-soak — не переименовывать.
                "cost_is_estimate": c.is_estimate,
                "cost_estimate_reason": c.estimate_reason,
            })
        journal.write(record)
    response_activity = {
        **common,
        "decision_id": resp.decision_id,
        "tier": resp.tier,
        "action_kind": resp.action.kind,
        "from_cache": outcome.from_cache,
    }
    if outcome.from_cache:
        # Кэш повторяет исходное решение без нового provider-вызова. Это не
        # второй источник биллинга: цена остаётся только в decision journal.
        response_activity.update({"source_decision_id": resp.decision_id,
                                  "incremental_cost_usd": 0})
    activity.write("response_ready", response_activity)
    return resp
