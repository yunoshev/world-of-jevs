"""Квантованные факты положения для prompt, не команды движения."""
import math

from .config import IN_WATER_YARDS, WATER_OFFSET
from .contract import Position


def distance_2d(a: Position, b: Position) -> float:
    """Расстояние по земле. Высота игнорируется намеренно: берег покатый."""
    return math.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2)


def water_point(home: Position) -> Position:
    """Точка мелководья, взятая с карты, а не вычисленная моделью."""
    return Position(x=home.x + WATER_OFFSET[0],
                    y=home.y + WATER_OFFSET[1],
                    z=home.z + WATER_OFFSET[2])


def is_in_water(position: Position, home: Position) -> bool:
    """Краткий факт для prompt: мурлок находится у точки мелководья."""
    return distance_2d(position, water_point(home)) <= IN_WATER_YARDS
