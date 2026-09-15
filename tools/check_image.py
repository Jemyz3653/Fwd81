# SPDX-License-Identifier: MIT
"""Проверка собранного файла Fwd81: от чего он зависит и на какой Windows пойдёт.

Два правила проекта, которые нельзя оставлять на дисциплину программиста:

1. Ядро fwd81core.dll попадает в чужой процесс раньше, чем там заведётся
   библиотека языка C, поэтому зависеть оно имеет право только от ntdll.dll.
   Любая другая строка в его таблице импорта — будущее падение на живой машине.

2. Наши собственные утилиты обязаны запускаться на самой Windows 8.1.
   Компоновщик пишет в заголовок файла «мне нужна такая-то версия Windows»,
   и если там окажется 10.0, загрузчик 8.1 откажется запускать файл ещё до
   того, как дело дойдёт до функций. Для 8.1 в заголовке должно быть не больше 6.3.

Запуск:

    py -3 tools/check_image.py build/bin/Release/fwd81core.dll --imports ntdll.dll
    py -3 tools/check_image.py build/bin/Release/fwd81cli.exe

Код возврата 0 — файл в порядке, 1 — нарушение (названо поимённо), 2 — не смог
проверить (нет файла, нет pefile).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

EXIT_OK = 0
EXIT_VIOLATION = 1
EXIT_CANNOT_CHECK = 2

DEFAULT_MAX_SUBSYSTEM = "6.3"  # Windows 8.1


def parse_version(text: str) -> tuple[int, int]:
    major, _, minor = text.partition(".")
    return int(major), int(minor or 0)


def imported_libraries(pe) -> list[str]:
    """Все библиотеки, от которых файл зависит: обычный импорт, отложенный и связанный."""
    names: list[str] = []
    for attribute in ("DIRECTORY_ENTRY_IMPORT", "DIRECTORY_ENTRY_DELAY_IMPORT"):
        for entry in getattr(pe, attribute, None) or []:
            if entry.dll:
                names.append(entry.dll.decode("ascii", "replace"))
    for entry in getattr(pe, "DIRECTORY_ENTRY_BOUND_IMPORT", None) or []:
        name = getattr(entry.struct, "name", None)
        if name:
            names.append(name.decode("ascii", "replace") if isinstance(name, bytes) else str(name))
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description="Проверка собранного файла Fwd81")
    parser.add_argument("image", help="путь к .dll или .exe")
    parser.add_argument(
        "--imports",
        nargs="*",
        default=None,
        metavar="DLL",
        help="разрешённые зависимости; если не указано, состав зависимостей не проверяется",
    )
    parser.add_argument(
        "--max-subsystem",
        default=DEFAULT_MAX_SUBSYSTEM,
        help=f"максимальная требуемая версия Windows в заголовке (по умолчанию {DEFAULT_MAX_SUBSYSTEM})",
    )
    arguments = parser.parse_args()

    image_path = Path(arguments.image)
    if not image_path.is_file():
        print(f"НЕ МОГУ ПРОВЕРИТЬ: файла нет — {image_path}")
        print("Сначала собери проект: build.ps1")
        return EXIT_CANNOT_CHECK

    try:
        import pefile
    except ImportError:
        print("НЕ МОГУ ПРОВЕРИТЬ: не установлен модуль pefile.")
        print("Поставь его так:  py -3 -m pip install pefile")
        return EXIT_CANNOT_CHECK

    pe = pefile.PE(str(image_path), fast_load=True)
    pe.parse_data_directories(
        directories=[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT"],
        ]
    )

    problems: list[str] = []

    subsystem = (
        pe.OPTIONAL_HEADER.MajorSubsystemVersion,
        pe.OPTIONAL_HEADER.MinorSubsystemVersion,
    )
    os_version = (
        pe.OPTIONAL_HEADER.MajorOperatingSystemVersion,
        pe.OPTIONAL_HEADER.MinorOperatingSystemVersion,
    )
    limit = parse_version(arguments.max_subsystem)

    print(f"Файл:                   {image_path}")
    print(f"Требует подсистему:     {subsystem[0]}.{subsystem[1]}")
    print(f"Требует версию ОС:      {os_version[0]}.{os_version[1]}")

    dependencies = imported_libraries(pe)
    print(f"Зависимости ({len(dependencies)}):        {', '.join(dependencies) if dependencies else 'нет'}")

    if subsystem > limit:
        problems.append(
            f"в заголовке записано требование Windows {subsystem[0]}.{subsystem[1]}, "
            f"а нужно не выше {limit[0]}.{limit[1]} — на Windows 8.1 такой файл "
            f"не запустится вовсе. Лечится флагом компоновщика /SUBSYSTEM:<вид>,6.03"
        )
    if os_version > limit:
        problems.append(
            f"в заголовке записана требуемая версия ОС {os_version[0]}.{os_version[1]}, "
            f"а нужно не выше {limit[0]}.{limit[1]}"
        )

    if arguments.imports is not None:
        allowed = {name.lower() for name in arguments.imports}
        for dependency in dependencies:
            if dependency.lower() not in allowed:
                problems.append(
                    f"лишняя зависимость — {dependency}. "
                    f"Разрешено только: {', '.join(sorted(allowed))}"
                )

    pe.close()

    if problems:
        print()
        for problem in problems:
            print(f"НАРУШЕНИЕ: {problem}")
        return EXIT_VIOLATION

    print("Проверка пройдена.")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
