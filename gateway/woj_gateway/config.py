import math
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _bool_env(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    normalized = raw.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"{name} must be a boolean")


def _positive_seconds_env(name: str, default: float) -> float:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        value = float(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be a finite positive number") from exc
    if not math.isfinite(value) or value <= 0 or value > 10.0:
        raise ValueError(f"{name} must be > 0 and <= 10 seconds")
    return value


def _bounded_int_env(name: str, default: int, maximum: int) -> int:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        value = int(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer from 1 to {maximum}") from exc
    if not 1 <= value <= maximum:
        raise ValueError(f"{name} must be an integer from 1 to {maximum}")
    return value

# Deliberately no HOST knob: the bind address is loopback and is not
# configurable — a 0.0.0.0 option would be a footgun, not a feature.
JOURNAL_PATH = Path(os.environ.get("WOJ_JOURNAL", ROOT / "private" / "logs" / "decisions.jsonl"))
# Поток терминального наблюдателя отделён от обучающего journal: его ротация
# никогда не удаляет исходные решения и не участвует в учёте бюджета.
ACTIVITY_PATH = Path(os.environ.get(
    "WOJ_ACTIVITY", JOURNAL_PATH.parent / f"activity-{JOURNAL_PATH.name}"))
# Keep bounded activity history independently from the append-only journal.
# Keep enough bounded history for the whole run (up to ~250 MiB), independently
# from the append-only decision journal. This is not Docker's build cache.
ACTIVITY_ROTATIONS = _bounded_int_env("WOJ_ACTIVITY_ROTATIONS", 24, 64)
BEHAVIOR_POLICY_PATH = Path(os.environ.get(
    "WOJ_BEHAVIOR_POLICY", ROOT / "private" / "etc" / "modules" / "woj_behavior.json"))

# Точка мелководья из карты нужна только для краткого факта окружения.
WATER_OFFSET = (-6.0, 6.0, -5.0)
# How long the module may keep applying this decision before asking again.
VALID_FOR_SECONDS = 15
# Это расстояние используется только как квантованный факт окружения в prompt.
IN_WATER_YARDS = 3.0
# Ярус Jev. Ключ читается из файла, а не из переменной окружения: так он
# не попадает в argv и в вывод `env` соседнего процесса.
JEV_KEY_PATH = Path(os.environ.get("WOJ_JEV_KEY", ROOT / "private" / "secrets" / "openrouter.runtime.key"))
JEV_URL = "https://openrouter.ai/api/alpha/decisions"
JEV_MODEL = "typesafe/jev-1.13"
# Production is fail-closed: only an actual Jev decision may become a world
# action. False exists solely for explicit historical/offline comparisons.
JEV_ONLY = True
# httpx применяет эти пределы к отдельным фазам, не ко всему запросу.
# Их сумма (3 с по умолчанию) — только ориентир для согласования с мостом,
# НЕ hard deadline: фазы могут повторяться, а DNS getaddrinfo не ограничен.
# Верхняя граница env защищает от опечатки, но тоже не превращает сумму фаз
# в deadline. Имена env явные, чтобы стартовые настройки было видно в health.
# The 2026-09-26 Greenskin run had four upstream header waits hit the 2.5 s
# read fence out of 1,128 provider calls.  Probe a bounded extra 250 ms while
# retaining margin below the native bridge's 3 s deadline on a pooled
# connection (observed failure-side bridge overhead was 9-15 ms).  This is a
# failure fence, not normal decision cadence; the benefit remains unverified.
JEV_TIMEOUT_SECONDS = _positive_seconds_env("WOJ_JEV_READ_TIMEOUT_SECONDS", 2.75)
JEV_CONNECT_TIMEOUT_SECONDS = _positive_seconds_env(
    "WOJ_JEV_CONNECT_TIMEOUT_SECONDS", 0.4)
JEV_WRITE_TIMEOUT_SECONDS = _positive_seconds_env(
    "WOJ_JEV_WRITE_TIMEOUT_SECONDS", 0.25)
JEV_POOL_TIMEOUT_SECONDS = _positive_seconds_env(
    "WOJ_JEV_POOL_TIMEOUT_SECONDS", 0.15)
# Сколько держать соединение открытым между решениями. Больше такта решений
# (2 с) и больше срока годности кэша снимка (VALID_FOR_SECONDS = 15 с),
# чтобы рукопожатие не повторялось на каждом платном вызове.
JEV_KEEPALIVE_SECONDS = 30.0
JEV_MAX_CONNECTIONS = 12
JEV_MAX_KEEPALIVE_CONNECTIONS = 12
# Инвариант: максимум токенов в том, что отправляем МЫ. Билленные
# input_tokens больше — в них служебный конверт провайдера (~304 токена),
# которым мы не управляем.
JEV_MAX_OWN_TOKENS = 512
# Чем заряжать сторожа бюджета, когда провайдер не прислал цену; замерено
# на боевом запросе (530 вход/85 выход токенов). Ноль был бы хуже
# отсутствующего сторожа: бюджет не исчерпался бы никогда.
JEV_COST_FALLBACK_USD = 0.0000223

# Порог трат за жизнь процесса гейтвея. Ниже лимита ключа ($5) намеренно:
# лимит ключа — последний рубеж, и упираться в него штатно нельзя, иначе он
# перестаёт быть рубежом.
JEV_BUDGET_USD = float(os.environ.get("WOJ_BUDGET_SESSION_USD", "1.0"))
