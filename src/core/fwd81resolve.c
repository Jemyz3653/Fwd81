// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Реализация резолвера. Без CRT: свои сравнения строк. См. fwd81resolve.h.

#include <windows.h>

#include "fwd81resolve.h"

// --- Свои сравнения строк (CRT в ядре нет) ------------------------------------

static WCHAR LowerW(WCHAR c)
{
    if (c >= L'A' && c <= L'Z')
        return (WCHAR)(c - L'A' + L'a');
    return c;
}

// Сравнение имён библиотек без учёта регистра (имена ASCII-совместимы).
static BOOL WideEqualNoCase(const wchar_t *a, const wchar_t *b)
{
    while (*a && *b) {
        if (LowerW(*a) != LowerW(*b))
            return FALSE;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static BOOL AsciiEqual(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return FALSE;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

// --- Тестовая подстановка (область M3) ----------------------------------------
//
// Заглушка, которую резолвер отдаёт вместо отсутствующей функции. Возвращает
// заметное значение (0x7781 — «fwd81»), чтобы в выводе synthetic было видно:
// r=30593 означает, что вызвалась именно наша подстановка, а не оригинал.
static int Fwd81TestStub(void)
{
    return 0x7781;
}

// Имена, которые мы обрабатываем в M3 (из tests/synthetic). В M4 их заменит
// настоящий список missing/coverage.
static const wchar_t *const kHandledDlls[] = {
    L"fwd81_absent.dll",
    NULL
};

static const char *const kHandledProcs[] = {
    "Fwd81AbsentFunction",
    "Fwd81AbsentProcedure",
    NULL
};

BOOL Fwd81HandlesMissingDll(const wchar_t *dll)
{
    int i;
    if (dll == NULL)
        return FALSE;
    for (i = 0; kHandledDlls[i] != NULL; i++) {
        if (WideEqualNoCase(dll, kHandledDlls[i]))
            return TRUE;
    }
    return FALSE;
}

void *Fwd81ResolveProcedure(const char *name)
{
    int i;
    if (name == NULL)
        return NULL;
    for (i = 0; kHandledProcs[i] != NULL; i++) {
        if (AsciiEqual(name, kHandledProcs[i]))
            return (void *)Fwd81TestStub;
    }
    return NULL;
}
