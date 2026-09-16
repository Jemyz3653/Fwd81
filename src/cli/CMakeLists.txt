# SPDX-License-Identifier: MIT
#
# fwd81cli.exe — командная утилита.
#
# Два требования из ТЗ, которые видно прямо здесь:
#   * утилита обязана запускаться на самой Windows 8.1 — отсюда статическая
#     библиотека языка C (не нужен ни vcruntime140.dll, ни обновление UCRT
#     KB2999226 на машине пользователя) и метка подсистемы 6.03;
#   * архитектура только x64.

add_executable(fwd81cli
    main.c
    out.c
    ifeo.c)

# MultiThreaded / MultiThreadedDebug = статическая CRT (/MT, /MTd).
set_target_properties(fwd81cli PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# advapi32 — работа с реестром (IFEO) и токеном процесса (проверка прав админа).
target_link_libraries(fwd81cli PRIVATE advapi32)

fwd81_target_runs_on_81(fwd81cli CONSOLE)
