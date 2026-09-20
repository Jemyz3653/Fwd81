// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Реализация инструментовки загрузчика. Только ntdll, без CRT. См. .h.
//
// Целевые адреса сняты статическим анализом (dbh + capstone) с ntdll ЭТОЙ
// машины (Windows 11, 10.0.26100.8328). На другой сборке ntdll адреса иные,
// поэтому перед расстановкой точек проверяется отпечаток сборки (TimeDateStamp);
// не совпал — точки не ставим, пишем в журнал и выходим (fail-safe).

#include <windows.h>
#include <winternl.h>

#include "fwd81log.h"
#include "fwd81probe_ldr.h"

// --- Отпечаток и адреса целевой ntdll (RVA от базы модуля) --------------------

#define FWD81_NTDLL_TIMESTAMP 0x0022B8E0u  // Win11 10.0.26100.8328

// Функции загрузчика (RVA). Порядок = номер DR-регистра.
#define RVA_LdrpLoadDependentModuleInternal 0x0000d2e0u  // загрузка статической зависимости (отказ №2)
#define RVA_LdrpGetProcedureAddress         0x00058da0u  // разрешение функции, пролог
#define RVA_LdrpGetProcedureAddress_snap    0x000590f0u  // вход из LdrpSnapModule (+0x350) (отказ №3)
#define RVA_LdrLoadDll_exported             0x00059200u  // экспорт — контроль (не должен сработать статически)

// --- Прототипы ntdll, которых нет в winternl.h --------------------------------

#ifndef CONTEXT_DEBUG_REGISTERS
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_AMD64 | 0x00000010L)
#endif
#ifndef EXCEPTION_SINGLE_STEP
#define EXCEPTION_SINGLE_STEP STATUS_SINGLE_STEP
#endif

#define FWD81_EFLAGS_RF 0x00010000u        // Resume Flag
#define FWD81_NT_CURRENT_THREAD ((HANDLE)(LONG_PTR)-2)

NTSTATUS NTAPI NtSetContextThread(HANDLE Thread, PCONTEXT Context);
PVOID NTAPI RtlAddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);

// --- Состояние ----------------------------------------------------------------

static volatile LONG g_armed = 0;             // чтобы сработать единожды
static ULONG_PTR     g_targets[4] = { 0, 0, 0, 0 };
static const wchar_t *g_names[4] = {
    L"ldrprobe: сработала LdrpLoadDependentModuleInternal, rip=",
    L"ldrprobe: сработала LdrpGetProcedureAddress(пролог), rip=",
    L"ldrprobe: сработала LdrpGetProcedureAddress+0x350(снаппинг), rip=",
    L"ldrprobe: сработала LdrLoadDll(экспорт, контроль), rip=",
};
static volatile LONG g_selftest_hit = 0;

// --- Поиск базы ntdll через список модулей PEB --------------------------------

static BOOL WideEndsWithNtdll(const UNICODE_STRING *s)
{
    static const wchar_t suffix[] = L"ntdll.dll";
    USHORT chars = (USHORT)(s->Length / sizeof(WCHAR));
    USHORT slen = (USHORT)((sizeof(suffix) / sizeof(WCHAR)) - 1);
    USHORT i;
    if (s->Buffer == NULL || chars < slen)
        return FALSE;
    for (i = 0; i < slen; i++) {
        WCHAR a = s->Buffer[chars - slen + i];
        WCHAR b = suffix[i];
        if (a >= L'A' && a <= L'Z') a = (WCHAR)(a - L'A' + L'a');
        if (a != b)
            return FALSE;
    }
    return TRUE;
}

static PVOID GetNtdllBase(void)
{
    PTEB teb = NtCurrentTeb();
    PPEB peb;
    PPEB_LDR_DATA ldr;
    PLIST_ENTRY head, cur;

    if (teb == NULL)
        return NULL;
    peb = teb->ProcessEnvironmentBlock;
    if (peb == NULL || peb->Ldr == NULL)
        return NULL;
    ldr = peb->Ldr;
    head = &ldr->InMemoryOrderModuleList;
    for (cur = head->Flink; cur != head; cur = cur->Flink) {
        PLDR_DATA_TABLE_ENTRY e =
            CONTAINING_RECORD(cur, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (WideEndsWithNtdll(&e->FullDllName))
            return e->DllBase;
    }
    return NULL;
}

static DWORD NtdllTimeDateStamp(PVOID base)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (const IMAGE_NT_HEADERS *)((const BYTE *)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return nt->FileHeader.TimeDateStamp;
}

// --- Установка debug-регистров на текущем потоке -------------------------------

// enable_mask: биты L0..L3 (какие DR включены). targets[i] игнорируется, если бит снят.
static void SetDebugRegisters(ULONG_PTR d0, ULONG_PTR d1, ULONG_PTR d2, ULONG_PTR d3,
                              DWORD enable_bits)
{
    CONTEXT ctx;
    // memset нам предоставляет fwd81_mem.c
    RtlSecureZeroMemory(&ctx, sizeof(ctx));  // ntdll, без CRT
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    ctx.Dr0 = d0;
    ctx.Dr1 = d1;
    ctx.Dr2 = d2;
    ctx.Dr3 = d3;
    // DR7: биты L0,L1,L2,L3 = 0,2,4,6. RW/LEN = 0 (исполнение, 1 байт) для всех.
    ctx.Dr7 = enable_bits;
    ctx.Dr6 = 0;
    NtSetContextThread(FWD81_NT_CURRENT_THREAD, &ctx);
}

// --- Тощий обработчик исключений ----------------------------------------------
//
// Строго по условию: снять адрес -> записать в журнал -> отключить эту точку
// (чтобы записать единожды и не тормозить) -> продолжить. Никакой другой логики.

static LONG NTAPI Fwd81VectoredHandler(PEXCEPTION_POINTERS info)
{
    PCONTEXT ctx;
    DWORD hit;
    int i;

    if (info->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    ctx = info->ContextRecord;
    hit = (DWORD)(ctx->Dr6 & 0xf);  // B0..B3
    if (hit == 0)
        return EXCEPTION_CONTINUE_SEARCH;

    for (i = 0; i < 4; i++) {
        if (hit & (1u << i)) {
            if (i == 0 && g_selftest_hit == 0) {
                // Во время самопроверки DR0 указывает на пробник.
                Fwd81LogEvent("info", L"ldrprobe: самопроверка — обработчик исключения сработал");
            } else {
                Fwd81LogEventNum("info", g_names[i], (unsigned long long)ctx->Rip);
            }
            ctx->Dr7 &= ~(1u << (i * 2));  // снять бит Li — больше не срабатывать
        }
    }

    ctx->Dr6 = 0;
    ctx->EFlags |= FWD81_EFLAGS_RF;  // перешагнуть инструкцию, не зациклившись
    return EXCEPTION_CONTINUE_EXECUTION;
}

// --- Самопроверка: доказать, что связка DR+VEH работает и не роняет процесс ---

#pragma optimize("", off)
static void Fwd81BpProbe(void)
{
    // Пустая функция. На её первую инструкцию ставится DR0; вызов приводит к
    // срабатыванию обработчика. Оптимизация выключена, чтобы её не вырезали.
    volatile int x = 0;
    (void)x;
}
#pragma optimize("", on)

// --- Точка сборки -------------------------------------------------------------

void Fwd81LdrProbeArm(void)
{
    PVOID base;
    DWORD stamp;

    if (InterlockedCompareExchange(&g_armed, 1, 0) != 0)
        return;  // уже сработало

    if (RtlAddVectoredExceptionHandler(1, Fwd81VectoredHandler) == NULL) {
        Fwd81LogEvent("error", L"ldrprobe: не удалось поставить обработчик исключений");
        return;
    }

    // 1. Самопроверка: DR0 на пробник, вызвать, обработчик должен записать «ok».
    SetDebugRegisters((ULONG_PTR)Fwd81BpProbe, 0, 0, 0, 0x1 /* L0 */);
    Fwd81BpProbe();
    g_selftest_hit = 1;
    Fwd81LogEvent("info", L"ldrprobe: самопроверка пройдена (процесс жив)");

    // 2. Страховка: адреса сняты под конкретную сборку ntdll.
    base = GetNtdllBase();
    if (base == NULL) {
        Fwd81LogEvent("error", L"ldrprobe: не нашёл базу ntdll — точки не ставлю");
        return;
    }
    stamp = NtdllTimeDateStamp(base);
    if (stamp != FWD81_NTDLL_TIMESTAMP) {
        Fwd81LogEventNum("info",
            L"ldrprobe: ntdll другой сборки (адреса не для неё), точки НЕ ставлю; TimeDateStamp=",
            (unsigned long long)stamp);
        return;
    }

    // 3. Реальные точки на функциях загрузчика (текущий поток).
    g_targets[0] = (ULONG_PTR)base + RVA_LdrpLoadDependentModuleInternal;
    g_targets[1] = (ULONG_PTR)base + RVA_LdrpGetProcedureAddress;
    g_targets[2] = (ULONG_PTR)base + RVA_LdrpGetProcedureAddress_snap;
    g_targets[3] = (ULONG_PTR)base + RVA_LdrLoadDll_exported;
    SetDebugRegisters(g_targets[0], g_targets[1], g_targets[2], g_targets[3],
                      0x55 /* L0|L1|L2|L3 */);
    Fwd81LogEventNum("info", L"ldrprobe: точки расставлены, база ntdll=",
                     (unsigned long long)(ULONG_PTR)base);
}
