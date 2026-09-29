# pyright: strict
"""Форма ответа /api/alpha/decisions — типами pydantic, а не обходом словаря.

Эндпоинт альфовый: JSON неожиданной формы — обычное дело, и порча одного
поля метаданных не должна стоить решения. Поэтому чтение идёт по одному
листовому полю за раз, без модели на всё тело целиком; остальное дерево
остаётся непровалидированным `object`.

Каждый читатель возвращает одну из трёх форм: годное значение, `Bad`
(поле пришло, но негодно — с уликой для лога) или `Absent` (поля нет).
`pydantic.ValidationError` наружу из модуля не выходит.
"""
from dataclasses import dataclass
from typing import Annotated, cast

from pydantic import Field, TypeAdapter, ValidationError

# Единственное определение годного числа в пакете.
#   * strict=True не приводит типы: "0.5" не станет float, bool не
#     считается числом вовсе.
#   * allow_inf_nan=False отвергает голые NaN/Infinity.
#   * целое вне диапазона double (`1e309` и `10**309` — одно число, разного
#     вида на входе) pydantic отвергает так же, как любой другой
#     несовместимый JSON-тип, не бросая исключение вроде OverflowError.
UsableFloat = Annotated[float, Field(strict=True, allow_inf_nan=False)]

_FLOAT: TypeAdapter[float] = TypeAdapter(UsableFloat)
_STR: TypeAdapter[str] = TypeAdapter(Annotated[str, Field(strict=True)])


@dataclass(frozen=True)
class Absent:
    """Поля с точки зрения решения нет.

    Три причины схлопнуты в одну: поля не было, пришло null, или контейнер,
    где оно должно лежать, не той формы. Кривой контейнер равносилен
    отсутствующему значению, а не отказу — метаданные не должны стоить
    исполнимого решения.
    """


@dataclass(frozen=True)
class Bad:
    """Поле пришло, но пользоваться им нельзя.

    `evidence` — исходное кривое значение, не сокращённое: решение
    сокращать ли его (`_short` в budget.py) и логировать ли — за
    вызывающим кодом, не за этим модулем.
    """
    evidence: object


def dig(container: object, key: str) -> object:
    """container[key], или None, если container не словарь, ключа нет или
    значение — null. Обычный `.get` на не-словаре бросил бы AttributeError
    вместо того, чтобы просто сказать «значения нет».
    """
    if isinstance(container, dict):
        # cast, не просто isinstance: без параметров типа dict сужается до
        # dict[Unknown, Unknown], и pyright в strict режиме не выводит тип
        # .get(...) из него.
        return cast("dict[object, object]", container).get(key)
    return None


def is_usable(value: object) -> bool:
    """Тот же предикат годности числа, что в read_*-функциях, в форме bool —
    для `_unusable_numbers_marked` (jev.py), которой нужен именно предикат,
    не результат с уликой.
    """
    try:
        _FLOAT.validate_python(value)
    except ValidationError:
        return False
    return True


def _read_number(container: object, key: str) -> "float | Bad | Absent":
    raw = dig(container, key)
    if raw is None:
        return Absent()
    try:
        return _FLOAT.validate_python(raw)
    except ValidationError:
        return Bad(raw)


def _read_str(container: object, key: str) -> "str | Bad | Absent":
    raw = dig(container, key)
    if raw is None:
        return Absent()
    try:
        return _STR.validate_python(raw)
    except ValidationError:
        return Bad(raw)


def read_usage_field(usage: object, key: str) -> "float | Bad | Absent":
    """usage.cost / usage.input_tokens / usage.output_tokens.

    Все три ложатся в один и тот же биллинговый контейнер и проверяются
    одной и той же годностью числа — параметризация ключом, а не три
    одинаковые функции.
    """
    return _read_number(usage, key)


def read_choice(answers: object, head: str = "intent") -> "str | Bad | Absent":
    """``answers[head].choice`` for a typed choice head.

    Пустая строка приравнена к отсутствующей: пустой выбор так же
    неисполним, как отсутствующий. Вызывающему коду видна разница между
    Bad (форма не та) и Absent (нечего было выбрать). ``head`` остаётся
    явным параметром, а не вторым почти таким же reader: protocol 1.3
    добавляет выборы целей в тот же ответ провайдера.
    """
    result = _read_str(dig(answers, head), "choice")
    if isinstance(result, str) and result == "":
        return Absent()
    return result


def read_probability(answers: object, choice: str) -> "float | Bad | Absent":
    """answers.intent.probabilities[<выбранный вариант>].

    Читается уже ПОСЛЕ выбора и по нему же: вероятность приложена к уже
    принятому решению, а не наоборот, поэтому её порча не может стоить
    решения — только строки "p=0.NN" в мысли для лога.
    """
    intent = dig(answers, "intent")
    return _read_number(dig(intent, "probabilities"), choice)


def read_afraid(answers: object) -> "float | Bad | Absent":
    """answers.afraid.noul."""
    return _read_number(dig(answers, "afraid"), "noul")


def read_danger(answers: object) -> "float | Bad | Absent":
    """answers.danger.score."""
    return _read_number(dig(answers, "danger"), "score")


def read_model(body: object) -> "str | Bad | Absent":
    """body.model — сырое значение, без проверки "непустая после strip()":
    что считается подтверждённой моделью, решает jev.py (JevResult.model)."""
    return _read_str(body, "model")
