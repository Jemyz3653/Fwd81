// SPDX-License-Identifier: LGPL-2.1-or-later
//
// fwd81core.dll -- injection core of Fwd81.
//
// STATUS: milestone M0. This file deliberately contains no logic. Its only job
// today is to prove that the no-CRT build works end to end on this toolchain:
// custom entry point, /NODEFAULTLIB, ntdll.dll as the single import.
//
// The loader hooks (LdrLoadDll, LdrGetProcedureAddressForCaller, apiset
// resolution) land in M2/M3 -- see docs/architecture.md.
//
// HARD RULES for every future change in this file:
//   1. No C runtime. No printf, no malloc, no memcpy from the CRT.
//      Memory comes from RtlAllocateHeap, strings from our own helpers.
//   2. Nothing but registration may happen inside the entry point. The Windows
//      loader lock is held while it runs; calling LoadLibrary, waiting on a
//      synchronisation object or touching another module from here deadlocks
//      the whole process.
//   3. ntdll.dll is the only permitted import, enforced by
//      tools/check_image.py.

#include <windows.h>

#include "fwd81log.h"

// Entry point of the DLL. Named via /ENTRY:Fwd81CoreEntry so that the linker
// does not pull in _DllMainCRTStartup (which initialises the CRT we do not have).
// The Windows loader calls it with the same three arguments as DllMain.
//
// Source of the signature: DllMain entry point,
// https://learn.microsoft.com/windows/win32/dlls/dllmain
BOOL WINAPI Fwd81CoreEntry(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(reserved);

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        // M2: пока единственное дело ядра — записать факт своей загрузки.
        // Это только запись в журнал через ntdll: ни LoadLibrary, ни ожиданий,
        // ни обращений к чужим модулям — правило про loader lock соблюдено.
        // Перехваты загрузчика появятся в M3 (тоже как регистрация).
        Fwd81LogEvent("info", L"ядро fwd81core загружено в процесс");
        break;

    case DLL_PROCESS_DETACH:
        Fwd81LogEvent("info", L"ядро fwd81core выгружено из процесса");
        break;

    default:
        break;
    }

    return TRUE;
}
