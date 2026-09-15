# exportdiff — сравнение экспортов System32 (MIT)

Отвечает на главный вопрос проекта: **каких функций нет в Windows 8.1**. Всё
остальное — покрытие, заготовки, база для `fwd81diag` — выводится из его работы.

## Запуск

```
py -3 tools/exportdiff/exportdiff.py all
```

Пути по умолчанию: Windows 10 → `C:\Windows\System32`, Windows 8.1 →
`C:\Fwd81\data\system32-win81`. Переопределяются:

```
py -3 tools/exportdiff/exportdiff.py --win81 D:\где-то\system32 all
```

## Команды

| Команда | Что делает |
|---|---|
| `all` | весь конвейер по порядку (по умолчанию) |
| `export` | прочитать экспорты обеих систем в `data/exports-*.json` |
| `diff` | посчитать `data/missing.json` — чего нет в 8.1 |
| `coverage` | собрать `data/coverage.json` и `docs/coverage.md` |
| `stubs` | сгенерировать заготовки `.def`/`.c` для непокрытых функций |
| `baseline` | сгенерировать `src/diag/fwd81diag_baseline.h` для `fwd81diag.exe` |
| `check` | для CI: проверить, что `docs/coverage.md` соответствует пометкам в коде |

## Пока нет данных 8.1

Виртуальной машины с 8.1 ещё нет, каталог `data/system32-win81` пуст. Это **не
ошибка**. Инструмент печатает понятное сообщение и помечает результат явным
признаком «данных нет» (`available: false`), а не пустым списком: пустой
`missing.json` означал бы «ничего не отсутствует» — это была бы ложь.

Когда появится копия System32 из 8.1 — положить её в `data/system32-win81`,
выполнить `all`, перегенерированные `docs/coverage.md` и
`src/diag/fwd81diag_baseline.h` закоммитить. Порядок — в `docs/ЖУРНАЛ.md`.

## Откуда берётся покрытие

Единственный источник правды о том, что реализовано, — пометки прямо в
исходниках `src/libs`:

```
// FWD81-COVER: kernel32!SetThreadDescription F 1607 обёртка через SetThreadName
```

Поля: `dll!функция`, категория `F/I/S/N`, версия появления, свободный
комментарий. `exportdiff` собирает их в `coverage.json` и `docs/coverage.md`.
Пока `src/libs` пуст, покрытие пустое — и это честный ноль, а не заглушка.

## Что кладётся в репозиторий, а что нет

`exports-*.json`, `missing.json`, `coverage.json` и каталог заготовок
`_generated_stubs` — производные от данных 8.1, в git не попадают (см.
`.gitignore`). В репозитории живут только `docs/coverage.md` и
`src/diag/fwd81diag_baseline.h` — их достаточно, чтобы собрать `fwd81diag`.
