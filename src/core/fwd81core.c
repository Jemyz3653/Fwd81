// SPDX-License-Identifier: LGPL-2.1-or-later
//
// fwd81core.dll -- injection core of Fwd81.
//
// STATUS: milestone M2. The core does two things so far:
//   * registers itself as an Application Verifier provider on DLL_PROCESS_VERIFIER
//     (this is what makes the IFEO/VerifierDlls injection actually take hold);
//   * logs the numeric reason of every entry-point call, so the log PROVES how
//     the DLL was loaded. Real AVRF injection delivers DLL_PROCESS_VERIFIER (4)
//     BEFORE DLL_PROCESS_ATTACH (1); a plain LoadLibrary only ever gives ATTACH.
//     That ordering is the success criterion for M2.
//
// The loader hooks (LdrLoadDll, LdrGetProcedureAddressForCaller, apiset) land in
// M3 -- see docs/architecture.md.
//
// HARD RULES for every change here:
//   1. No C runtime. Memory via Rtl*, our own memset/memcpy (fwd81_mem.c).
//   2. Entry point does registration and logging only -- no LoadLibrary, no
//      waits, no touching other modules (loader lock).
//   3. ntdll.dll is the only permitted import (tools/check_image.py enforces it).

#include <windows.h>

#include "fwd81log.h"
#include "fwd81probe_ldr.h"

// DLL_PROCESS_VERIFIER may be missing from older SDK headers.
#ifndef DLL_PROCESS_VERIFIER
#define DLL_PROCESS_VERIFIER 4
#endif

// --- Application Verifier provider protocol -----------------------------------
//
// Эти структуры лежат в avrfsdk.h из WDK, в обычном SDK их нет — объявляем сами.
// Источник: документация Application Verifier и заголовок avrfsdk.h.
//
// Зачем это нужно. Когда механизм VerifierDlls грузит нашу DLL, он вызывает её
// точку входа с причиной DLL_PROCESS_VERIFIER и ждёт, что мы вернём описатель
// провайдера. Если не вернуть корректный описатель, часть процессов вообще не
// стартует. Поэтому регистрация провайдера — обязательный шаг, а не мелочь.

typedef VOID (NTAPI *FWD81_AVRF_DLL_LOAD_CB)(PWSTR DllName, PVOID DllBase,
                                             SIZE_T DllSize, PVOID Reserved);
typedef VOID (NTAPI *FWD81_AVRF_DLL_UNLOAD_CB)(PWSTR DllName, PVOID DllBase,
                                               SIZE_T DllSize, PVOID Reserved);
typedef VOID (NTAPI *FWD81_AVRF_NTDLLHEAPFREE_CB)(PVOID AllocationBase,
                                                  SIZE_T AllocationSize);

typedef struct _FWD81_AVRF_THUNK {
    PCHAR ThunkName;
    PVOID ThunkOldAddress;
    PVOID ThunkNewAddress;
} FWD81_AVRF_THUNK, *PFWD81_AVRF_THUNK;

typedef struct _FWD81_AVRF_DLL_DESCRIPTOR {
    PWCH              DllName;
    DWORD             DllFlags;
    PVOID             DllAddress;
    PFWD81_AVRF_THUNK DllThunks;
} FWD81_AVRF_DLL_DESCRIPTOR, *PFWD81_AVRF_DLL_DESCRIPTOR;

typedef struct _FWD81_AVRF_PROVIDER_DESCRIPTOR {
    DWORD                      Length;
    PFWD81_AVRF_DLL_DESCRIPTOR ProviderDlls;
    FWD81_AVRF_DLL_LOAD_CB     ProviderDllLoadCallback;
    FWD81_AVRF_DLL_UNLOAD_CB   ProviderDllUnloadCallback;
    PWSTR                      VerifierImage;
    DWORD                      VerifierFlags;
    DWORD                      VerifierDebug;
    PVOID                      RtlpGetStackTraceAddress;
    PVOID                      RtlpDebugPageHeapCreate;
    PVOID                      RtlpDebugPageHeapDestroy;
    FWD81_AVRF_NTDLLHEAPFREE_CB ProviderNtdllHeapFreeCallback;
} FWD81_AVRF_PROVIDER_DESCRIPTOR, *PFWD81_AVRF_PROVIDER_DESCRIPTOR;

// Пустые обработчики загрузки/выгрузки чужих DLL. Пока ничего не перехватываем
// (это M3) — но иметь ненулевые обработчики надёжнее, чем оставлять NULL.
static VOID NTAPI Fwd81OnDllLoad(PWSTR name, PVOID base, SIZE_T size, PVOID reserved)
{
    UNREFERENCED_PARAMETER(name);
    UNREFERENCED_PARAMETER(base);
    UNREFERENCED_PARAMETER(size);
    UNREFERENCED_PARAMETER(reserved);
}

static VOID NTAPI Fwd81OnDllUnload(PWSTR name, PVOID base, SIZE_T size, PVOID reserved)
{
    UNREFERENCED_PARAMETER(name);
    UNREFERENCED_PARAMETER(base);
    UNREFERENCED_PARAMETER(size);
    UNREFERENCED_PARAMETER(reserved);
}

// Таблица перехватываемых DLL — пока пустая, только терминатор (DllName == NULL).
static FWD81_AVRF_DLL_DESCRIPTOR g_ProviderDlls[] = {
    { NULL, 0, NULL, NULL },
};

// Наш описатель провайдера. Возвращаем его на DLL_PROCESS_VERIFIER.
static FWD81_AVRF_PROVIDER_DESCRIPTOR g_ProviderDescriptor = {
    sizeof(FWD81_AVRF_PROVIDER_DESCRIPTOR),  // Length
    g_ProviderDlls,                          // ProviderDlls
    Fwd81OnDllLoad,                          // ProviderDllLoadCallback
    Fwd81OnDllUnload,                        // ProviderDllUnloadCallback
    NULL, 0, 0, NULL, NULL, NULL, NULL       // остальное заполняет система
};

// --- Точка входа --------------------------------------------------------------

BOOL WINAPI Fwd81CoreEntry(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(instance);

    switch (reason) {
    case DLL_PROCESS_VERIFIER:
        // Настоящее внедрение через Application Verifier. Приходит РАНЬШЕ, чем
        // DLL_PROCESS_ATTACH; при обычном LoadLibrary этой причины не бывает.
        Fwd81LogEventNum("info", L"точка входа ядра, reason=", (unsigned long long)reason);
        if (reserved != NULL) {
            *(PFWD81_AVRF_PROVIDER_DESCRIPTOR *)reserved = &g_ProviderDescriptor;
            Fwd81LogEvent("info", L"провайдер Application Verifier зарегистрирован (ok)");
        } else {
            Fwd81LogEvent("error", L"DLL_PROCESS_VERIFIER без указателя на описатель — провайдер НЕ зарегистрирован");
        }
        // M3 (эксперимент, только под FWD81_LDR_EXPERIMENT): расставить точки
        // останова ДО снаппинга статических импортов главного образа. Под IFEO
        // это самый ранний момент, куда мы успеваем — здесь реальные точки нужны.
#ifdef FWD81_LDR_EXPERIMENT
        Fwd81LdrProbeArm(1 /* arm_real */);
#endif
        break;

    case DLL_PROCESS_ATTACH:
        // M3: сюда встанут перехваты загрузчика — тоже как регистрация.
        Fwd81LogEventNum("info", L"точка входа ядра, reason=", (unsigned long long)reason);
#ifdef FWD81_LDR_EXPERIMENT
        // Под `run`/LoadLibrary VERIFIER не приходит. Реальные точки тут бесполезны
        // и опасны — только самопроверка. Единожды: внутри стоит защёлка.
        Fwd81LdrProbeArm(0 /* arm_real */);
#endif
        break;

    case DLL_PROCESS_DETACH:
        Fwd81LogEventNum("info", L"точка входа ядра, reason=", (unsigned long long)reason);
#ifdef FWD81_LDR_EXPERIMENT
        Fwd81LdrProbeDisarm();  // снять точки и обработчик до выгрузки ядра
#endif
        break;

    default:
        break;
    }

    return TRUE;
}
