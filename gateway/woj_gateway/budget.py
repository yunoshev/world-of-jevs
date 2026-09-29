"""Сторож трат.

Считает по фактическому usage.cost из ответа, а не по оценке токенов, —
иначе оценка разошлась бы с реальным расходом и это вскрылось бы, только
когда деньги уже кончились. Предохранитель от ошибки в коде (например,
цикла, спрашивающего модель слишком часто); от забывчивости человека
отвечает отдельный таймер, scripts/woj-session.
"""
import logging
import threading
import time
from dataclasses import dataclass

from .config import JEV_COST_FALLBACK_USD

# Без своего обработчика: logging.lastResort шлёт в stderr — тот же поток,
# куда пишет uvicorn (gateway.out).
_log = logging.getLogger("woj_gateway.budget")

# Это процессный ограничитель, а не свойство Budget: проба создаёт новые
# сторожа, а живой процесс всё равно не должен печатать шквал одной порчи.
_price_log_lock = threading.Lock()
_last_price_substitution_log = float("-inf")
_PRICE_SUBSTITUTION_LOG_SECONDS = 10.0

# Почему цена наша, а не провайдера — четыре разные причины (контракт
# провайдера изменился / порча числа / число неправдоподобно / не успели
# прочитать). Строки короткие и машинные — их читают woj-soak и загрузчик
# Ф6, не только человек. Не менять текст.
ESTIMATE_NOT_REPORTED = "not reported"
ESTIMATE_UNUSABLE_NUMBER = "unusable number"
ESTIMATE_OUT_OF_RANGE = "out of range"
ESTIMATE_NO_PRICE_READ = "no price read"

# Аномалия — только порча числа; отсутствие цены и недочитанный ответ —
# честные события, не аномалия.
_CORRUPT_PRICE = frozenset({ESTIMATE_UNUSABLE_NUMBER, ESTIMATE_OUT_OF_RANGE})

# Потолок цены одного решения — доля лимита сеанса, не абсолютное число,
# чтобы ехать вместе с лимитом при смене настройки. 1/100 — с большим
# запасом выше замеренной цены вызова (~$0.0000223), но достаточно тесно,
# чтобы не спутать порчу числа с законным подорожанием.
PRICE_CEILING_SHARE_OF_LIMIT = 1 / 100


@dataclass(frozen=True)
class Charge:
    """Что сторож списал на самом деле — и почему.

    Запись журнала строится из этого объекта, а не из ответа провайдера
    напрямую, чтобы не утверждать одно, пока сторож сделал другое.
    """
    usd: float
    estimate_reason: str | None

    @property
    def is_estimate(self) -> bool:
        return self.estimate_reason is not None


def _short(value) -> str:
    """repr для строки лога — без 4000-значных целых из кривого ответа."""
    text = repr(value)
    return text if len(text) <= 60 else f"{text[:40]}…({len(text)} знаков)"


def _usd(usd: float) -> str:
    """Доллары со значащими разрядами: 4 знака после запятой напечатали бы
    «$0.0000» для типичного вызова (~$0.0000223). Копия этих строк
    сознательно живёт в scripts/woj-status и scripts/woj-soak — те
    запускаются системным python3 и не импортируют этот пакет.
    """
    if usd == 0:
        return "$0.00"
    if abs(usd) >= 1:
        return f"${usd:.2f}"
    if abs(usd) >= 0.01:
        return f"${usd:.4f}"
    if abs(usd) >= 0.0001:
        return f"${usd:.6f}"
    return f"${usd:.8f}"


class Budget:
    def __init__(self, limit_usd: float,
                 estimate_usd: float = JEV_COST_FALLBACK_USD) -> None:
        self._limit = limit_usd
        self._ceiling = limit_usd * PRICE_CEILING_SHARE_OF_LIMIT
        self._estimate = estimate_usd
        self._spent = 0.0
        self._lock = threading.Lock()
        # Число подмен цены из-за порчи (см. _CORRUPT_PRICE). Без счётчика
        # подмена была бы не видна: spent() растёт как обычно.
        self._anomalies = 0
        if self._ceiling < estimate_usd:
            # Наша ошибка настройки, не провайдера: при таком лимите даже
            # настоящая цена всегда выше потолка — сказать один раз, а не
            # на каждом вызове.
            _log.warning("woj: лимит сеанса %s слишком мал — потолок цены одного "
                         "решения (%s, это доля лимита) ниже нашей же оценки "
                         "вызова %s; каждая настоящая цена будет засчитана "
                         "порчей. Это ошибка настройки, а не провайдера",
                         _usd(limit_usd), _usd(self._ceiling), _usd(estimate_usd))

    def allows(self) -> bool:
        with self._lock:
            # Строго "<": на spent == limit доступ уже закрыт, иначе
            # последнее списание пропустило бы ещё один запрос сверх лимита.
            return self._spent < self._limit

    def meter(self) -> "Meter":
        """Счётчик одного оплачиваемого вызова (см. Meter)."""
        return Meter(self)

    def _settle(self, reported, estimate_reason: str | None) -> Charge:
        """Единственное место, где решается, какую сумму списать.

        reported — цена провайдера (годное число double), либо то, что
        пришло вместо неё, если estimate_reason уже задан (нужно только для
        строки лога). Диапазон проверяется здесь, у владельца лимита, — не
        дублировать проверку в jev.
        """
        # not (0 <= x <= потолок), а не через or: отказ по умолчанию. Ноль
        # допустим — бесплатный ответ бывает.
        if estimate_reason is None and not (0.0 <= reported <= self._ceiling):
            estimate_reason = ESTIMATE_OUT_OF_RANGE
        # Вне диапазона списывается ПОТОЛКОМ, не оценкой: оценка
        # ($0.0000223) слишком мала, чтобы закрыть ярус при настоящем
        # массовом подорожании — сторож должен ошибаться в сторону
        # перестраховки.
        if estimate_reason is None:
            usd = reported
        elif estimate_reason == ESTIMATE_OUT_OF_RANGE:
            usd = max(self._estimate, self._ceiling)
        else:
            usd = self._estimate
        corrupt = estimate_reason in _CORRUPT_PRICE
        with self._lock:
            if corrupt:
                self._anomalies += 1
            # Логируем переход в закрытое состояние один раз, не на каждом
            # последующем отказе — иначе тонет в повторах. Лок делает
            # переход однократным для конкурентных потоков.
            was_open = self._spent < self._limit
            self._spent += usd
            crossed = was_open and self._spent >= self._limit
            spent, limit = self._spent, self._limit
        # Печать — вне замка: I/O незачем делать под ним.
        if corrupt and _should_log_price_substitution():
            # Счётчик выше растёт на каждую порчу; ограничивается только шум
            # строки в логе, а не видимость аномалии через /healthz.
            _log.warning("woj: цена провайдера %s негодна (%s, потолок %s) — "
                         "списано %s, засчитана аномалия",
                         _short(reported), estimate_reason, _usd(self._ceiling),
                         _usd(usd))
        if crossed:
            _log.warning("woj: бюджет сеанса исчерпан — потрачено %s из %s; "
                         "ярус Jev выключен до конца процесса гейтвея",
                         _usd(spent), _usd(limit))
        return Charge(usd=usd, estimate_reason=estimate_reason)

    def spent(self) -> float:
        with self._lock:
            return self._spent

    def limit(self) -> float:
        return self._limit

    def price_ceiling(self) -> float:
        return self._ceiling

    def exhausted(self) -> bool:
        return not self.allows()

    def anomalies(self) -> int:
        with self._lock:
            return self._anomalies


def _should_log_price_substitution() -> bool:
    global _last_price_substitution_log
    now = time.monotonic()
    with _price_log_lock:
        if now - _last_price_substitution_log < _PRICE_SUBSTITUTION_LOG_SECONDS:
            return False
        _last_price_substitution_log = now
        return True


class Meter:
    """Начисление одного оплаченного вызова — ровно один раз.

    Tiers заводит его перед jev.decide и после читает списанное на любом
    исходе, включая отказ. Однократность — здесь, а не флагом в decide:
    попытка отмечается до обращения к бюджету, так что сторож, успевший
    списать и потом упавший, не спишет дважды.
    """

    def __init__(self, budget: Budget) -> None:
        self._budget = budget
        self._attempted = False
        self.charge: Charge | None = None

    def price(self, usd: float) -> Charge:
        """Цена провайдера, уже годное число double; правдоподобие решит бюджет."""
        return self._once(usd, None)

    def estimate(self, reason: str, reported=None) -> Charge:
        """Цены нет или она негодна — списать оценку и назвать причину."""
        return self._once(reported, reason)

    def _once(self, reported, reason: str | None) -> Charge:
        if not self._attempted:
            self._attempted = True
            self.charge = self._budget._settle(reported, reason)
        # self.charge не None здесь: либо только что присвоен строкой выше,
        # либо это повторный вызов и он был выставлен при первом. assert —
        # для pyright; если инвариант нарушится, AssertionError уйдёт в
        # Tiers._fresh как наш баг, не как отказ провайдера.
        assert self.charge is not None
        return self.charge
