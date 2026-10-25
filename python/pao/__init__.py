"""PAO — система проактивного адміністративного нагляду за кібербезпекою
транспортних мереж і логістичних хабів (модель за Директивою (ЄС) 2022/2555, NIS2)."""

from .core import Engine, backend, load_registry

__all__ = ["Engine", "backend", "load_registry"]
__version__ = "1.0.0"
