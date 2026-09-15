# SPDX-License-Identifier: MIT
#
# Корневой файл сборки Fwd81.
# Лицензионная граница описана в docs/licensing.md: файлы сборки и утилиты — MIT,
# ядро и библиотеки реализаций — под LGPL.

cmake_minimum_required(VERSION 3.25)

project(Fwd81
    VERSION 0.1.0
    DESCRIPTION "Sloy sovmestimosti: prilozheniya Windows 10 x64 na Windows 8.1 x64"
    HOMEPAGE_URL "https://github.com/Jemyz3653/Fwd81"
    LANGUAGES C)

# --- Проверки окружения -------------------------------------------------------
# Каждая проверка объясняет причину: молчаливый отказ сборки хуже понятного текста.

if(NOT WIN32)
    message(FATAL_ERROR
        "Fwd81 собирается только под Windows: проект состоит из системных DLL Windows.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "Нужен компилятор MSVC (Visual Studio 2022). Текущий: ${CMAKE_C_COMPILER_ID}.\n"
        "Запускай сборку через build.ps1 или build.bat — они сами поднимают окружение MSVC.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Fwd81 сейчас собирается только под x64. Похоже, cmake настроен на x86.\n"
        "Запускай сборку через build.ps1 — он вызывает vcvars64.bat.")
endif()

# --- Общие настройки ----------------------------------------------------------

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Все артефакты в build/bin/<конфигурация>, чтобы их было где искать руками.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

option(FWD81_WERROR "Считать предупреждения компилятора ошибками (включено в CI)" OFF)

# Целевая система — Windows 8.1 (6.3). Заголовки SDK по этой метке скрывают
# объявления, появившиеся позже, и мы сразу видим, что взяли лишнее.
# Библиотекам реализаций (src/libs) позже понадобится метка выше — они поднимут
# её у себя локально, а не здесь.
add_compile_definitions(
    _WIN32_WINNT=0x0603
    WINVER=0x0603
    NTDDI_VERSION=0x06030000
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    FWD81_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    FWD81_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    FWD81_VERSION_PATCH=${PROJECT_VERSION_PATCH})

add_compile_options(/W4 /permissive- /utf-8)
if(FWD81_WERROR)
    add_compile_options(/WX)
endif()

# Компоновщик MSVC по умолчанию пишет в заголовок "нужна Windows 10.0", и
# загрузчик 8.1 отказывается запускать такой файл ещё до поиска функций.
# 6.03 — это Windows 8.1. Подробности: docs/architecture.md, раздел «Патч заголовка».
function(fwd81_target_runs_on_81 target subsystem)
    target_link_options(${target} PRIVATE "/SUBSYSTEM:${subsystem},6.03")
endfunction()

# --- Составные части ----------------------------------------------------------
# Каталоги src/libs, src/apisets, src/diag и src/cfg пока без целей сборки:
# что и в какой вехе там появится — написано в README.md каждого каталога.

add_subdirectory(src/core)
add_subdirectory(src/cli)

message(STATUS "Fwd81 ${PROJECT_VERSION}: цели — fwd81core.dll, fwd81cli.exe")
