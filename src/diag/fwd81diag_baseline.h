// SPDX-License-Identifier: MIT
//
// ГЕНЕРИРУЕТСЯ. Не правь руками:
//   py -3 tools/exportdiff/exportdiff.py baseline
//
// База для fwd81diag.exe: какие функции есть в Windows 8.1 и что покрыто нами.
// Сгенерировано: 2026-09-15 16:58 UTC

#ifndef FWD81DIAG_BASELINE_H
#define FWD81DIAG_BASELINE_H

#define FWD81_BASELINE_AVAILABLE 0

// Имена функций, экспортируемых Windows 8.1 (объединение по всем DLL).
static const char *const FWD81_PRESENT_IN_81[] = {
    0
};
#define FWD81_PRESENT_IN_81_COUNT 0

// Функции, которые реализованы у нас, и их категория (F/I/S/N).
struct Fwd81Coverage { const char *function; char category; };
static const struct Fwd81Coverage FWD81_COVERAGE[] = {
    { 0, 0 }
};
#define FWD81_COVERAGE_COUNT 0

#endif // FWD81DIAG_BASELINE_H
