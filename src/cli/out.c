// SPDX-License-Identifier: MIT
//
// Реализация вывода UTF-8. См. out.h.

#include <windows.h>
#include <wchar.h>

#include "out.h"

void OutText(const wchar_t *text)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode;
    DWORD  written;
    size_t length;
    int    bytes;
    char  *utf8;

    if (out == NULL || out == INVALID_HANDLE_VALUE)
        return;

    length = wcslen(text);
    if (length == 0)
        return;

    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, text, (DWORD)length, &written, NULL);
        return;
    }

    bytes = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL);
    if (bytes <= 0)
        return;

    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)bytes);
    if (utf8 == NULL)
        return;

    WideCharToMultiByte(CP_UTF8, 0, text, (int)length, utf8, bytes, NULL, NULL);
    WriteFile(out, utf8, (DWORD)bytes, &written, NULL);
    HeapFree(GetProcessHeap(), 0, utf8);
}

void OutLine(const wchar_t *text)
{
    OutText(text);
    OutText(L"\n");
}
