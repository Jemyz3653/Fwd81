# SPDX-License-Identifier: MIT
"""exportdiff — сравнение экспортов System32 Windows 10 и Windows 8.1.

Отвечает на главный вопрос проекта: каких функций нет в 8.1. Всё остальное —
покрытие, заготовки, вердикт fwd81diag — выводится из результата его работы.

Что делает (одна команда `all` запускает всё по порядку):

  1. export   — читает таблицы экспорта всех DLL из двух каталогов System32
                и складывает в data/exports-win10.json и data/exports-win81.json
  2. diff     — считает data/missing.json: что есть в 10, но нет в 8.1
  3. coverage — собирает data/coverage.json из пометок в исходниках src/libs
                и генерирует docs/coverage.md
  4. stubs    — генерирует заготовки .def и .c для непокрытых функций
  5. baseline — генерирует src/diag/fwd81diag_baseline.h для fwd81diag.exe

Пути по умолчанию:
  Windows 10  → C:\\Windows\\System32
  Windows 8.1 → C:\\Fwd81\\data\\system32-win81   (пока пусто — см. ниже)

Про отсутствие данных 8.1. Виртуальной машины с 8.1 пока нет, каталог
system32-win81 пуст. Это НЕ ошибка и НЕ повод для трассировки. Инструмент в этом
случае:
  * печатает понятное сообщение «положи сюда System32 из 8.1»;
  * помечает результат явным признаком «данных нет» (available=false), а НЕ
    пустым списком: пустой missing.json означал бы «ничего не отсутствует» —
    это была бы ложь.

Запуск:
  py -3 tools/exportdiff/exportdiff.py all
  py -3 tools/exportdiff/exportdiff.py export --win81 D:\\some\\other\\system32
  py -3 tools/exportdiff/exportdiff.py --check      (для CI, см. команду check)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

# На некоторых машинах стандартный вывод Python по умолчанию не UTF-8 (у
# Windows-раннеров GitHub это cp1252), и печать кириллицы падает с
# UnicodeEncodeError. Переключаем вывод на UTF-8 сами. Потоки, которые так не
# умеют, тихо пропускаем.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
    except (AttributeError, ValueError):
        pass

# --- Пути проекта -------------------------------------------------------------

TOOLS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TOOLS_DIR.parent.parent
DATA_DIR = PROJECT_ROOT / "data"
DOCS_DIR = PROJECT_ROOT / "docs"
LIBS_DIR = PROJECT_ROOT / "src" / "libs"

DEFAULT_WIN10 = Path(r"C:\Windows\System32")
DEFAULT_WIN81 = DATA_DIR / "system32-win81"

EXPORTS_WIN10 = DATA_DIR / "exports-win10.json"
EXPORTS_WIN81 = DATA_DIR / "exports-win81.json"
MISSING_JSON = DATA_DIR / "missing.json"
COVERAGE_JSON = DATA_DIR / "coverage.json"
COVERAGE_MD = DOCS_DIR / "coverage.md"
STUBS_DIR = PROJECT_ROOT / "src" / "libs" / "_generated_stubs"
BASELINE_HEADER = PROJECT_ROOT / "src" / "diag" / "fwd81diag_baseline.h"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_CHECK_FAILED = 2

# Категории покрытия. Порядок — от «дешевле» к «дороже/невозможнее».
CATEGORIES = {
    "F": "forward — обёртка над функцией, которая в 8.1 уже есть",
    "I": "implement — реализация поверх более низкоуровневых функций",
    "S": "stub — заглушка: корректный код ошибки и запись в журнал",
    "N": "not possible — на ядре 8.1 невозможно (см. docs/limitations.md)",
}

# Пометка покрытия в исходниках src/libs. Пример строки:
#   // FWD81-COVER: kernel32!SetThreadDescription F 1607 обёртка через SetThreadName
# Поля: dll!функция, категория (F/I/S/N), версия появления, свободный комментарий.
COVER_PATTERN = re.compile(
    r"FWD81-COVER:\s*"
    r"(?P<dll>[A-Za-z0-9_.\-]+)!(?P<func>[A-Za-z0-9_@?$]+)\s+"
    r"(?P<category>[FISN])\b"
    r"(?:\s+(?P<since>[0-9A-Za-z.]+))?"
    r"(?:\s+(?P<note>.*))?"
)


def now_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def log(message: str = "") -> None:
    print(message, flush=True)


# =============================================================================
#  Чтение экспортов
# =============================================================================

def read_dll_exports(dll_path: Path):
    """Имена, экспортируемые одной DLL. None — файл не разобрать как PE.

    Возвращаем только именованные экспорты: сопоставление функций между версиями
    идёт по имени, экспорт по одному ординалу для нас неразличим.
    """
    import pefile

    try:
        pe = pefile.PE(str(dll_path), fast_load=True)
    except Exception:
        return None  # не PE, битый, занят — не наша забота, просто пропускаем

    names: list[str] = []
    try:
        pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]]
        )
        export_dir = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
        if export_dir is not None:
            for symbol in export_dir.symbols:
                if symbol.name:
                    names.append(symbol.name.decode("ascii", "replace"))
    except Exception:
        return None
    finally:
        pe.close()

    return sorted(set(names))


def export_folder(folder: Path, label: str):
    """Собрать экспорты всех *.dll каталога. Возвращает структуру для JSON.

    Признак отсутствия данных — поле available=false, а не пустой словарь.
    """
    result = {
        "label": label,
        "source": str(folder),
        "generated": now_stamp(),
        "available": False,
        "reason": None,
        "dll_count": 0,
        "function_count": 0,
        "exports": {},  # имя_dll(в нижнем регистре) -> [функции]
    }

    if not folder.exists():
        result["reason"] = (
            f"каталог не существует: {folder}. "
            "Для 8.1 сюда нужно скопировать System32 из виртуальной машины."
        )
        log(f"  [{label}] нет каталога: {folder}")
        return result

    dll_files = sorted(folder.glob("*.dll"))
    if not dll_files:
        result["reason"] = (
            f"в каталоге нет ни одной .dll: {folder}. "
            "Для 8.1 сюда нужно скопировать System32 из виртуальной машины "
            "(достаточно всех *.dll)."
        )
        log(f"  [{label}] каталог пуст: {folder}")
        return result

    exports: dict[str, list[str]] = {}
    total_functions = 0
    unreadable = 0

    for index, dll_path in enumerate(dll_files, start=1):
        names = read_dll_exports(dll_path)
        if names is None:
            unreadable += 1
            continue
        if names:
            exports[dll_path.name.lower()] = names
            total_functions += len(names)
        if index % 250 == 0:
            log(f"  [{label}] обработано {index}/{len(dll_files)} DLL…")

    result["available"] = True
    result["dll_count"] = len(exports)
    result["function_count"] = total_functions
    result["exports"] = exports
    log(
        f"  [{label}] готово: {len(exports)} DLL с экспортами, "
        f"{total_functions} функций"
        + (f", пропущено нечитаемых: {unreadable}" if unreadable else "")
    )
    return result


def union_of_functions(exported: dict) -> set[str]:
    """Множество всех имён функций во всех DLL набора (без учёта, где именно)."""
    result: set[str] = set()
    for functions in exported.get("exports", {}).values():
        result.update(functions)
    return result


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=False)
    path.write_text(text + "\n", encoding="utf-8")


def load_json(path: Path):
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


# =============================================================================
#  Команды
# =============================================================================

def cmd_export(win10: Path, win81: Path) -> int:
    log("Читаю экспорты Windows 10…")
    data10 = export_folder(win10, "win10")
    write_json(EXPORTS_WIN10, data10)
    log(f"  записано: {EXPORTS_WIN10.relative_to(PROJECT_ROOT)}")

    log("Читаю экспорты Windows 8.1…")
    data81 = export_folder(win81, "win81")
    write_json(EXPORTS_WIN81, data81)
    log(f"  записано: {EXPORTS_WIN81.relative_to(PROJECT_ROOT)}")

    if not data81["available"]:
        log("")
        log("Данных из Windows 8.1 пока нет — это ожидаемо, пока нет виртуальной машины.")
        log("Дальнейшие шаги (diff, stubs, baseline) будут помечены «данных нет».")
    return EXIT_OK


def compute_missing():
    """Вернуть структуру missing. available=false, если нет данных 8.1."""
    data10 = load_json(EXPORTS_WIN10)
    data81 = load_json(EXPORTS_WIN81)

    result = {
        "generated": now_stamp(),
        "available": False,
        "reason": None,
        "missing_count": 0,
        "by_dll": {},   # dll -> [функции, которых нет в 8.1]
    }

    if data10 is None or not data10.get("available"):
        result["reason"] = "нет данных экспортов Windows 10 — сначала запусти команду export"
        return result
    if data81 is None or not data81.get("available"):
        result["reason"] = (
            "нет данных экспортов Windows 8.1. Скопируй System32 из 8.1 в "
            f"{DEFAULT_WIN81} и запусти export заново. "
            "Пустой список здесь был бы ложью — поэтому признак «данных нет» явный."
        )
        return result

    present_in_81 = union_of_functions(data81)

    # Функция «отсутствует», если её имени нет НИГДЕ в 8.1 (учитываем переезды
    # функций между библиотеками через apiset — важно членство в объединении,
    # а не в конкретном файле). Группируем результат по DLL из Windows 10.
    by_dll: dict[str, list[str]] = {}
    missing_total = 0
    for dll_name, functions in data10["exports"].items():
        absent = sorted(fn for fn in functions if fn not in present_in_81)
        if absent:
            by_dll[dll_name] = absent
            missing_total += len(absent)

    result["available"] = True
    result["reason"] = None
    result["missing_count"] = missing_total
    result["by_dll"] = dict(sorted(by_dll.items(), key=lambda kv: -len(kv[1])))
    return result


def cmd_diff() -> int:
    log("Считаю, чего нет в Windows 8.1…")
    missing = compute_missing()
    write_json(MISSING_JSON, missing)
    log(f"  записано: {MISSING_JSON.relative_to(PROJECT_ROOT)}")
    if missing["available"]:
        log(f"  отсутствует функций: {missing['missing_count']} "
            f"в {len(missing['by_dll'])} библиотеках")
    else:
        log(f"  данных нет: {missing['reason']}")
    return EXIT_OK


def scan_coverage() -> dict:
    """Собрать покрытие из пометок FWD81-COVER в исходниках src/libs.

    Единственный источник правды о покрытии — сами исходники реализаций.
    Пока src/libs пуст, покрытие пустое — и это не ложь, а факт: мы ещё
    ничего не реализовали.
    """
    entries: list[dict] = []
    problems: list[str] = []

    if LIBS_DIR.exists():
        for source in sorted(LIBS_DIR.rglob("*.c")):
            if STUBS_DIR in source.parents:
                continue  # заготовки не считаем реализацией
            try:
                text = source.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                problems.append(f"{source.relative_to(PROJECT_ROOT)}: не UTF-8")
                continue
            for line_no, line in enumerate(text.splitlines(), start=1):
                match = COVER_PATTERN.search(line)
                if not match:
                    continue
                entries.append({
                    "dll": match.group("dll").lower(),
                    "function": match.group("func"),
                    "category": match.group("category"),
                    "since": match.group("since"),
                    "note": (match.group("note") or "").strip() or None,
                    "source": f"{source.relative_to(PROJECT_ROOT).as_posix()}:{line_no}",
                })

    entries.sort(key=lambda e: (e["dll"], e["function"]))

    by_category = {key: 0 for key in CATEGORIES}
    for entry in entries:
        by_category[entry["category"]] += 1

    return {
        "generated": now_stamp(),
        "total": len(entries),
        "by_category": by_category,
        "entries": entries,
        "problems": problems,
    }


def render_coverage_md(coverage: dict, missing: dict) -> str:
    lines: list[str] = []
    lines.append("<!-- Этот файл генерируется. Не правь руками: -->")
    lines.append("<!-- py -3 tools/exportdiff/exportdiff.py coverage -->")
    lines.append("")
    lines.append("# Таблица покрытия Fwd81")
    lines.append("")
    lines.append(f"Сгенерировано: {coverage['generated']}")
    lines.append("")
    lines.append("Категории:")
    lines.append("")
    for key, description in CATEGORIES.items():
        lines.append(f"- **{key}** — {description}")
    lines.append("")

    lines.append("## Сколько всего")
    lines.append("")
    if missing.get("available"):
        lines.append(f"- Отсутствует в Windows 8.1: **{missing['missing_count']}** функций "
                     f"в {len(missing['by_dll'])} библиотеках")
    else:
        lines.append("- Отсутствует в Windows 8.1: **данных пока нет** "
                     "(нет копии System32 из 8.1)")
    lines.append(f"- Покрыто нами: **{coverage['total']}** функций")
    for key in CATEGORIES:
        lines.append(f"  - {key}: {coverage['by_category'][key]}")
    lines.append("")

    lines.append("## Что реализовано")
    lines.append("")
    if not coverage["entries"]:
        lines.append("Пока ничего. Реализация функций начинается в вехе M4.")
        lines.append("")
        lines.append("Это не заглушка-обещание, а честный ноль: строка появится здесь "
                     "автоматически, как только в исходнике `src/libs` возникнет пометка "
                     "`FWD81-COVER`.")
    else:
        lines.append("| DLL | Функция | Категория | Появилась | Источник | Заметка |")
        lines.append("|-----|---------|:---------:|:---------:|----------|---------|")
        for entry in coverage["entries"]:
            lines.append(
                f"| {entry['dll']} | `{entry['function']}` | {entry['category']} "
                f"| {entry['since'] or '—'} | {entry['source']} | {entry['note'] or ''} |"
            )
    lines.append("")

    if coverage["problems"]:
        lines.append("## Проблемы при сборе покрытия")
        lines.append("")
        for problem in coverage["problems"]:
            lines.append(f"- {problem}")
        lines.append("")

    return "\n".join(lines) + "\n"


def cmd_coverage(write: bool = True) -> dict:
    log("Собираю покрытие из пометок в src/libs…")
    coverage = scan_coverage()
    missing = load_json(MISSING_JSON) or compute_missing()
    if write:
        write_json(COVERAGE_JSON, coverage)
        COVERAGE_MD.write_text(render_coverage_md(coverage, missing), encoding="utf-8")
        log(f"  записано: {COVERAGE_JSON.relative_to(PROJECT_ROOT)}")
        log(f"  записано: {COVERAGE_MD.relative_to(PROJECT_ROOT)}")
    log(f"  покрыто функций: {coverage['total']}")
    return coverage


# =============================================================================
#  Заготовки .def / .c
# =============================================================================

def cmd_stubs() -> int:
    log("Генерирую заготовки для непокрытых функций…")
    missing = load_json(MISSING_JSON)
    if missing is None or not missing.get("available"):
        log("  данных о недостающих функциях нет (нужна копия System32 из 8.1).")
        log("  Заготовки не создаются — генерировать нечего.")
        return EXIT_OK

    coverage = scan_coverage()
    covered = {(e["dll"], e["function"]) for e in coverage["entries"]}

    STUBS_DIR.mkdir(parents=True, exist_ok=True)
    generated = 0
    for dll_name, functions in missing["by_dll"].items():
        uncovered = [fn for fn in functions if (dll_name, fn) not in covered]
        if not uncovered:
            continue
        stem = dll_name.replace(".dll", "")
        write_stub_def(stem, uncovered)
        write_stub_c(stem, uncovered)
        generated += len(uncovered)

    log(f"  сгенерировано заготовок для {generated} функций в {STUBS_DIR.relative_to(PROJECT_ROOT)}")
    return EXIT_OK


def write_stub_def(stem: str, functions: list[str]) -> None:
    path = STUBS_DIR / f"fwd81_{stem}.def"
    lines = [
        "; SPDX-License-Identifier: LGPL-2.1-or-later",
        "; ЗАГОТОВКА, сгенерирована exportdiff. Правится вручную по мере реализации.",
        f"LIBRARY fwd81_{stem}",
        "EXPORTS",
    ]
    lines += [f"    {fn}" for fn in functions]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_stub_c(stem: str, functions: list[str]) -> None:
    path = STUBS_DIR / f"fwd81_{stem}.c"
    lines = [
        "// SPDX-License-Identifier: LGPL-2.1-or-later",
        "//",
        "// ЗАГОТОВКА, сгенерирована exportdiff. Ни одна функция здесь ещё НЕ реализована:",
        "// это список того, что предстоит сделать, а не рабочий код.",
        "//",
        "// Для каждой функции при реализации обязательно:",
        "//   * категория F/I/S/N в пометке FWD81-COVER (её читает exportdiff);",
        "//   * источник сигнатуры (документация Microsoft, заголовок SDK) и версия;",
        "//   * заглушка (S) возвращает честный код ошибки и пишет в журнал, а не врёт.",
        "",
        "#include <windows.h>",
        "",
    ]
    for fn in functions:
        lines.append(f"// TODO(fwd81): {fn} — категория не определена, реализация не написана.")
        lines.append(f"// FWD81-COVER: {stem}!{fn} S 0 автозаготовка, реализации нет")
        lines.append("")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# =============================================================================
#  Заголовок базы для fwd81diag.exe
# =============================================================================

def build_baseline() -> str:
    data81 = load_json(EXPORTS_WIN81)
    coverage = scan_coverage()

    available = bool(data81 and data81.get("available"))
    present = sorted(union_of_functions(data81)) if available else []

    lines: list[str] = []
    lines.append("// SPDX-License-Identifier: MIT")
    lines.append("//")
    lines.append("// ГЕНЕРИРУЕТСЯ. Не правь руками:")
    lines.append("//   py -3 tools/exportdiff/exportdiff.py baseline")
    lines.append("//")
    lines.append("// База для fwd81diag.exe: какие функции есть в Windows 8.1 и что покрыто нами.")
    lines.append(f"// Сгенерировано: {now_stamp()}")
    lines.append("")
    lines.append("#ifndef FWD81DIAG_BASELINE_H")
    lines.append("#define FWD81DIAG_BASELINE_H")
    lines.append("")
    lines.append(f"#define FWD81_BASELINE_AVAILABLE {1 if available else 0}")
    lines.append("")

    lines.append("// Имена функций, экспортируемых Windows 8.1 (объединение по всем DLL).")
    lines.append("static const char *const FWD81_PRESENT_IN_81[] = {")
    for name in present:
        lines.append(f'    "{name}",')
    lines.append("    0")
    lines.append("};")
    lines.append(f"#define FWD81_PRESENT_IN_81_COUNT {len(present)}")
    lines.append("")

    lines.append("// Функции, которые реализованы у нас, и их категория (F/I/S/N).")
    lines.append("struct Fwd81Coverage { const char *function; char category; };")
    lines.append("static const struct Fwd81Coverage FWD81_COVERAGE[] = {")
    for entry in coverage["entries"]:
        lines.append(f'    {{ "{entry["function"]}", \'{entry["category"]}\' }},')
    lines.append("    { 0, 0 }")
    lines.append("};")
    lines.append(f"#define FWD81_COVERAGE_COUNT {coverage['total']}")
    lines.append("")
    lines.append("#endif // FWD81DIAG_BASELINE_H")
    return "\n".join(lines) + "\n"


def cmd_baseline() -> int:
    log("Генерирую базу для fwd81diag…")
    text = build_baseline()
    BASELINE_HEADER.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_HEADER.write_text(text, encoding="utf-8")
    available = "есть данные 8.1" if "AVAILABLE 1" in text else "данных 8.1 нет (пустая база)"
    log(f"  записано: {BASELINE_HEADER.relative_to(PROJECT_ROOT)} ({available})")
    return EXIT_OK


# =============================================================================
#  Проверка для CI
# =============================================================================

def cmd_check() -> int:
    """CI: проверить, что docs/coverage.md соответствует пометкам в src/libs.

    Проверяем ТОЛЬКО coverage.md, потому что он выводится из src/libs — данных,
    которые есть и на раннере CI. Так ловится ситуация «поправили пометку
    FWD81-COVER в коде, а таблицу забыли перегенерировать».

    Файлы, зависящие от данных Windows 8.1 (missing.json, fwd81diag_baseline.h),
    здесь НЕ проверяются: на CI копии System32 из 8.1 нет, и правильный ответ для
    них — «данных нет». Проверять их против пустого состояния значило бы запрещать
    коммитить настоящую базу, снятую с 8.1. Их актуальность — на совести того, кто
    снял данные и перегенерировал (порядок описан в docs/ЖУРНАЛ.md).

    Сравниваем без строки времени генерации — она меняется каждый запуск.
    """
    log("Проверяю актуальность docs/coverage.md…")

    coverage = scan_coverage()
    missing = load_json(MISSING_JSON) or compute_missing()

    expected_md = render_coverage_md(coverage, missing)
    actual_md = COVERAGE_MD.read_text(encoding="utf-8") if COVERAGE_MD.exists() else ""
    if strip_timestamp(expected_md) != strip_timestamp(actual_md):
        log(f"  УСТАРЕЛ: {COVERAGE_MD.relative_to(PROJECT_ROOT)}")
        log("    Перегенерируй: py -3 tools/exportdiff/exportdiff.py coverage")
        log("")
        log("docs/coverage.md не соответствует пометкам в src/libs.")
        return EXIT_CHECK_FAILED

    log("  docs/coverage.md актуален.")
    return EXIT_OK


def strip_timestamp(text: str) -> str:
    return "\n".join(
        line for line in text.splitlines()
        if "Сгенерировано:" not in line and "generated" not in line
    )


def cmd_all(win10: Path, win81: Path) -> int:
    cmd_export(win10, win81)
    cmd_diff()
    cmd_coverage()
    cmd_stubs()
    cmd_baseline()
    log("")
    log("Готово. Результаты — в data/ и docs/coverage.md.")
    return EXIT_OK


# =============================================================================
#  Разбор аргументов
# =============================================================================

def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="exportdiff",
        description="Сравнение экспортов System32 Windows 10 и Windows 8.1.",
    )
    parser.add_argument("--win10", type=Path, default=DEFAULT_WIN10,
                        help=f"каталог System32 Windows 10 (по умолчанию {DEFAULT_WIN10})")
    parser.add_argument("--win81", type=Path, default=DEFAULT_WIN81,
                        help=f"каталог System32 Windows 8.1 (по умолчанию {DEFAULT_WIN81})")

    sub = parser.add_subparsers(dest="command")
    sub.add_parser("all", help="весь конвейер одной командой (по умолчанию)")
    sub.add_parser("export", help="прочитать экспорты обеих систем в JSON")
    sub.add_parser("diff", help="посчитать missing.json")
    sub.add_parser("coverage", help="собрать coverage.json и docs/coverage.md")
    sub.add_parser("stubs", help="сгенерировать заготовки .def/.c")
    sub.add_parser("baseline", help="сгенерировать базу для fwd81diag")
    sub.add_parser("check", help="CI: проверить актуальность генерируемых файлов")

    arguments = parser.parse_args(argv)
    command = arguments.command or "all"

    try:
        import pefile  # noqa: F401
    except ImportError:
        if command in ("all", "export"):
            log("ОШИБКА: не установлен модуль pefile.")
            log("Поставь его так:  py -3 -m pip install pefile")
            return EXIT_ERROR

    if command == "all":
        return cmd_all(arguments.win10, arguments.win81)
    if command == "export":
        return cmd_export(arguments.win10, arguments.win81)
    if command == "diff":
        return cmd_diff()
    if command == "coverage":
        cmd_coverage()
        return EXIT_OK
    if command == "stubs":
        return cmd_stubs()
    if command == "baseline":
        return cmd_baseline()
    if command == "check":
        return cmd_check()

    parser.print_help()
    return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
