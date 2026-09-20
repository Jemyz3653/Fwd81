# SPDX-License-Identifier: MIT
"""Сторож: одноразовый измеритель загрузчика НЕ должен попасть в боевую сборку.

Файл src/core/fwd81probe_ldr.c содержит зашитые под конкретную сборку ntdll
адреса и компилируется только под опцией FWD81_LDR_EXPERIMENT. Этот сторож
проверяет собранный fwd81core.dll: если в нём есть уникальная метка измерителя —
значит эксперимент просочился в обычную сборку, и это ошибка (код 1).

Одноразовый код имеет привычку переживать свой один раз — поэтому проверка
механическая, а не на доверии.

Запуск:  py -3 tools/check_no_experiment.py <путь-к-fwd81core.dll>
"""

from __future__ import annotations

import sys

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
    except (AttributeError, ValueError):
        pass

MARKER = b"FWD81_LDR_EXPERIMENT_MARKER_DO_NOT_SHIP"

EXIT_OK = 0
EXIT_LEAK = 1
EXIT_CANNOT = 2


def main() -> int:
    if len(sys.argv) < 2:
        print("Использование: check_no_experiment.py <путь-к-fwd81core.dll>")
        return EXIT_CANNOT

    path = sys.argv[1]
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"НЕ МОГУ ПРОВЕРИТЬ: не читается {path}: {e}")
        return EXIT_CANNOT

    if MARKER in data:
        print(f"НАРУШЕНИЕ: одноразовый измеритель загрузчика попал в сборку — {path}")
        print("Экспериментальный код с зашитыми под конкретную ntdll адресами не")
        print("должен быть в боевом ядре. Собирай без -Experiment (FWD81_LDR_EXPERIMENT=OFF).")
        return EXIT_LEAK

    print(f"Экспериментального измерителя в сборке нет: {path}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
