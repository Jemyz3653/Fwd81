# SPDX-License-Identifier: MIT
"""Проверка лицензионной границы Fwd81.

В проекте две лицензии, и граница между ними проходит строго по каталогам:

    LGPL-2.1-or-later   src/core, src/libs, src/apisets
    MIT                 src/cli, src/diag, src/cfg, tools, tests, .github,
                        файлы сборки в корне

Скрипт требует, чтобы в шапке каждого исходного файла стояла строка
SPDX-License-Identifier с лицензией его каталога, и чтобы в файле НЕ упоминалась
чужая лицензия — так ловится случайный перенос кода между зонами копипастой.

Запуск (из корня репозитория):

    py -3 tools/check_licenses.py

Код возврата 0 — всё в порядке, 1 — есть нарушения, они перечислены поимённо.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# На некоторых машинах стандартный вывод Python по умолчанию не UTF-8 (у
# Windows-раннеров GitHub это cp1252), и печать кириллицы падает с
# UnicodeEncodeError. Переключаем вывод на UTF-8 сами, чтобы скрипт не зависел от
# окружения. Потоки, которые так не умеют, тихо пропускаем.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
    except (AttributeError, ValueError):
        pass

LGPL = "LGPL-2.1-or-later"
MIT = "MIT"

# Порядок важен: берётся первое совпадение по началу пути.
DIRECTORY_RULES: list[tuple[str, str]] = [
    ("src/core", LGPL),
    ("src/libs", LGPL),
    ("src/apisets", LGPL),
    ("src/cli", MIT),
    ("src/diag", MIT),
    ("src/cfg", MIT),
    ("tools", MIT),
    ("tests", MIT),
    (".github", MIT),
]

# Файлы сборки в корне репозитория. Они не относятся ни к ядру, ни к утилитам,
# поэтому перечислены поимённо: новый файл в корне придётся добавить сюда руками,
# и это намеренно — иначе он молча окажется без лицензии.
ROOT_FILE_RULES: dict[str, str] = {
    "CMakeLists.txt": MIT,
    "build.ps1": MIT,
    "build.bat": MIT,
}

CHECKED_SUFFIXES = {
    ".c", ".h", ".cpp", ".hpp", ".cc",
    ".def", ".rc",
    ".py", ".ps1", ".bat", ".cmd",
    ".cmake", ".yml", ".yaml",
}
CHECKED_NAMES = {"CMakeLists.txt"}

SKIPPED_DIRECTORIES = {
    ".git", "build", "out", "data", "__pycache__", ".vs",
    # Заготовки exportdiff — производное от данных 8.1, тысячи автогенерируемых
    # файлов, не рабочий исходник. У них своя SPDX-шапка, но проверять их незачем.
    "_generated_stubs",
}

# Проверка «нет упоминания чужой лицензии» ловит перенос кода между зонами. Но
# два файла обязаны называть обе лицензии по существу своей работы, и для них она
# отключена. Оба исключения названы поимённо; проверка правильной SPDX-шапки
# самого файла при этом остаётся в силе.
#   * check_licenses.py — этот сторож, он лицензии сравнивает;
#   * exportdiff.py — генератор: пишет LGPL-шапку в заготовки src/libs, которые
#     сам же создаёт, поэтому строка LGPL в нём — данные, а не заимствование.
FOREIGN_CHECK_EXEMPT = {
    "tools/check_licenses.py",
    "tools/exportdiff/exportdiff.py",
}

SPDX_PATTERN = re.compile(r"SPDX-License-Identifier:\s*(\S+)")
FOREIGN_PATTERNS = {
    MIT: re.compile(r"LGPL-2\.1-or-later"),
    LGPL: re.compile(r"(?<![\w-])MIT(?![\w-])"),
}


def expected_license(relative_path: str) -> str | None:
    """Какая лицензия положена файлу по его месту в дереве. None — место не описано."""
    if "/" not in relative_path:
        return ROOT_FILE_RULES.get(relative_path)
    for prefix, license_id in DIRECTORY_RULES:
        if relative_path == prefix or relative_path.startswith(prefix + "/"):
            return license_id
    return None


def collect_files(root: Path) -> list[Path]:
    result = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if any(part in SKIPPED_DIRECTORIES for part in path.relative_to(root).parts):
            continue
        if path.suffix.lower() in CHECKED_SUFFIXES or path.name in CHECKED_NAMES:
            result.append(path)
    return sorted(result)


def check_file(path: Path, relative_path: str) -> list[str]:
    """Вернуть список претензий к файлу. Пустой список — файл в порядке."""
    problems: list[str] = []

    expected = expected_license(relative_path)
    if expected is None:
        problems.append(
            "файл не отнесён ни к одной лицензионной зоне. "
            "Добавь его каталог в DIRECTORY_RULES или имя в ROOT_FILE_RULES "
            "в tools/check_licenses.py — и заодно опиши решение в docs/licensing.md"
        )
        return problems

    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        problems.append("файл не читается как UTF-8, а исходники проекта хранятся в UTF-8")
        return problems

    found = SPDX_PATTERN.search(text)
    if found is None:
        problems.append(
            f"нет строки SPDX-License-Identifier. Первой строкой файла должна идти "
            f"пометка {expected}"
        )
    elif found.group(1) != expected:
        problems.append(
            f"лицензия в файле — {found.group(1)}, а по месту в дереве положена {expected}"
        )

    if relative_path not in FOREIGN_CHECK_EXEMPT:
        foreign = FOREIGN_PATTERNS[expected].search(text)
        if foreign is not None:
            line_number = text.count("\n", 0, foreign.start()) + 1
            other = LGPL if expected == MIT else MIT
            problems.append(
                f"строка {line_number}: упоминается чужая лицензия ({other}), "
                f"хотя файл лежит в зоне {expected}. Похоже на перенос кода между "
                f"зонами — так делать нельзя без отдельного решения"
            )

    return problems


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    files = collect_files(root)

    if not files:
        print("ОШИБКА: не нашлось ни одного файла для проверки — скрипт запущен не оттуда?")
        return 1

    failures = 0
    for path in files:
        relative_path = path.relative_to(root).as_posix()
        for problem in check_file(path, relative_path):
            print(f"НАРУШЕНИЕ  {relative_path}: {problem}")
            failures += 1

    if failures:
        print()
        print(f"Проверка лицензионной границы провалена: нарушений — {failures}.")
        print("Правила и обоснование: docs/licensing.md")
        return 1

    print(f"Лицензионная граница в порядке: проверено файлов — {len(files)}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
