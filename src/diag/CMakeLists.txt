# SPDX-License-Identifier: MIT
#
# fwd81diag.exe — анализатор программ. Главный инструмент проекта.
#
# Как и fwd81cli: статическая CRT и метка подсистемы 6.03, чтобы запускаться
# на самой Windows 8.1. Только x64.
#
# Заголовок fwd81diag_baseline.h генерирует exportdiff. Он лежит в этом же
# каталоге и закоммичен: сейчас — в «пустом» виде (данных 8.1 ещё нет), позже
# будет перегенерирован по копии System32 из 8.1. Порядок — в docs/ЖУРНАЛ.md.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fwd81diag_baseline.h")
    message(FATAL_ERROR
        "Нет файла fwd81diag_baseline.h. Сгенерируй его:\n"
        "  py -3 tools/exportdiff/exportdiff.py baseline")
endif()

add_executable(fwd81diag fwd81diag.c)

# С настоящей базой 8.1 в fwd81diag_baseline.h встроены десятки тысяч имён
# функций. Такой объём инициализаторов превышает лимит секций обычного
# объектного файла (ошибка C1128) — нужен /bigobj. На пустой базе флаг не мешает.
target_compile_options(fwd81diag PRIVATE /bigobj)

set_target_properties(fwd81diag PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

fwd81_target_runs_on_81(fwd81diag CONSOLE)
