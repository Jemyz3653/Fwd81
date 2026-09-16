// SPDX-License-Identifier: MIT
//
// Реализация `fwd81cli run`. См. run.h — там про потолок возможностей.
//
// Как внедряем: классический приём. Создаём процесс приостановленным, пишем в
// его память путь к fwd81core.dll, запускаем в нём поток на LoadLibraryW (адрес
// LoadLibraryW в kernel32 одинаков во всех процессах сессии), ждём загрузки,
// затем размораживаем главный поток. Ядро оказывается в процессе до его точки
// входа, но ПОСЛЕ разрешения статических импортов — отсюда потолок из run.h.

#include <windows.h>
#include <wchar.h>

#include "out.h"
#include "run.h"

#define FWD81_RUN_USAGE  1
#define FWD81_RUN_FAILED 7
#define FWD81_RUN_OK_STILL_RUNNING 8  // внедрили, но программа долго работает

#define CORE_DLL_NAME L"fwd81core.dll"

// Путь fwd81core.dll рядом с fwd81cli.exe.
static BOOL SelfDirCoreDll(wchar_t *out, DWORD out_chars)
{
    wchar_t self[MAX_PATH];
    wchar_t *slash;
    DWORD len = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return FALSE;
    slash = wcsrchr(self, L'\\');
    if (slash == NULL)
        return FALSE;
    *(slash + 1) = L'\0';
    if (wcslen(self) + wcslen(CORE_DLL_NAME) >= out_chars)
        return FALSE;
    wcscpy_s(out, out_chars, self);
    wcscat_s(out, out_chars, CORE_DLL_NAME);
    return TRUE;
}

// Собрать командную строку: "<target>" arg arg ... в свежий буфер (её надо
// отдавать CreateProcessW изменяемой). Вызывающий освобождает через free.
static wchar_t *BuildCommandLine(int argc, wchar_t **argv)
{
    size_t total = 3;  // кавычки вокруг target + завершающий ноль
    int i;
    wchar_t *cmd;

    for (i = 2; i < argc; i++)
        total += wcslen(argv[i]) + 3;  // пробел + возможные кавычки

    cmd = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, total * sizeof(wchar_t));
    if (cmd == NULL)
        return NULL;

    cmd[0] = L'\0';
    wcscat_s(cmd, total, L"\"");
    wcscat_s(cmd, total, argv[2]);   // target
    wcscat_s(cmd, total, L"\"");
    for (i = 3; i < argc; i++) {
        wcscat_s(cmd, total, L" ");
        wcscat_s(cmd, total, argv[i]);
    }
    return cmd;
}

// Внедрить DLL по пути dll в процесс proc. FALSE при неудаче.
static BOOL InjectDll(HANDLE proc, const wchar_t *dll)
{
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    LPVOID remote;
    HMODULE kernel32;
    FARPROC load;
    HANDLE thread;
    DWORD load_result = 0;
    BOOL ok = FALSE;

    kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == NULL)
        return FALSE;
    load = GetProcAddress(kernel32, "LoadLibraryW");
    if (load == NULL)
        return FALSE;

    remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == NULL)
        return FALSE;

    if (!WriteProcessMemory(proc, remote, dll, bytes, NULL)) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return FALSE;
    }

    thread = CreateRemoteThread(proc, NULL, 0,
                                (LPTHREAD_START_ROUTINE)load, remote, 0, NULL);
    if (thread != NULL) {
        WaitForSingleObject(thread, 15000);
        GetExitCodeThread(thread, &load_result);  // младшие биты HMODULE, 0 = не загрузилось
        CloseHandle(thread);
        ok = (load_result != 0);
    }

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

int Fwd81Run(int argc, wchar_t **argv)
{
    wchar_t core[MAX_PATH];
    wchar_t *cmd;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = FWD81_RUN_FAILED;

    if (argc < 3) {
        OutText(L"Использование: fwd81cli run <программа.exe> [аргументы]\n");
        return FWD81_RUN_USAGE;
    }
    if (!SelfDirCoreDll(core, MAX_PATH)) {
        OutLine(L"Не удалось найти fwd81core.dll рядом с fwd81cli.");
        return FWD81_RUN_FAILED;
    }

    cmd = BuildCommandLine(argc, argv);
    if (cmd == NULL) {
        OutLine(L"Нехватка памяти при сборке командной строки.");
        return FWD81_RUN_FAILED;
    }

    OutText(L"Запускаю с внедрением ядра (без реестра): ");
    OutLine(argv[2]);
    OutLine(L"Напоминание: run чинит только delay-load и рантайм LoadLibrary,");
    OutLine(L"статические импорты EXE — задача пути IFEO (см. architecture.md).");

    // Гасим модальные окна загрузчика в дочернем процессе (он наследует режим).
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(argv[2], cmd, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        OutText(L"Не удалось запустить программу. Проверь путь: ");
        OutLine(argv[2]);
        HeapFree(GetProcessHeap(), 0, cmd);
        return FWD81_RUN_FAILED;
    }
    HeapFree(GetProcessHeap(), 0, cmd);

    if (!InjectDll(pi.hProcess, core)) {
        OutLine(L"Внедрение ядра не удалось — процесс не запускаю, снимаю его.");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return FWD81_RUN_FAILED;
    }

    OutLine(L"Ядро внедрено, размораживаю программу.");
    ResumeThread(pi.hThread);

    // Ждём завершения. Для наших быстрых тестов этого хватает; долгую/оконную
    // программу не держим силой — по таймауту отпускаем, не убивая.
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
        {
            wchar_t line[64];
            swprintf(line, 64, L"Программа завершилась, код возврата: %lu", exit_code);
            OutLine(line);
        }
    } else {
        OutLine(L"Программа ещё работает — не жду больше и не снимаю её.");
        exit_code = FWD81_RUN_OK_STILL_RUNNING;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exit_code;
}
