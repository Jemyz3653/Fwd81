// SPDX-License-Identifier: MIT
//
// fwd81cli.exe -- command line tool of Fwd81.
//
// STATUS: milestone M2. Working: version, help, diag (via fwd81diag.exe),
// enable/disable/uninstall/log (IFEO injection, see ifeo.c). Commands that are
// not implemented yet report so honestly and name their milestone.

#include <windows.h>
#include <wchar.h>

#include "out.h"
#include "ifeo.h"
#include "run.h"

#define FWD81_STR2(x) #x
#define FWD81_STR(x)  FWD81_STR2(x)
#define FWD81_VERSION_STRING \
    FWD81_STR(FWD81_VERSION_MAJOR) "." \
    FWD81_STR(FWD81_VERSION_MINOR) "." \
    FWD81_STR(FWD81_VERSION_PATCH)

// Exit codes. Kept stable from the start: scripts will depend on them.
#define FWD81_EXIT_OK              0
#define FWD81_EXIT_USAGE           1
#define FWD81_EXIT_NOT_IMPLEMENTED 2

// Есть ли среди аргументов флаг --dry-run.
static int HasDryRun(int argc, wchar_t **argv)
{
    int i;
    for (i = 2; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--dry-run") == 0)
            return 1;
    }
    return 0;
}

// Первый аргумент после команды, не начинающийся с "-" (имя программы).
static const wchar_t *FirstOperand(int argc, wchar_t **argv)
{
    int i;
    for (i = 2; i < argc; i++) {
        if (argv[i][0] != L'-')
            return argv[i];
    }
    return NULL;
}

static void PrintVersion(void)
{
    OutText(L"Fwd81 " FWD81_VERSION_STRING L" (веха M0: каркас, рабочей логики ещё нет)\n"
            L"Слой совместимости: приложения Windows 10 x64 на Windows 8.1 x64\n"
            L"https://github.com/Jemyz3653/Fwd81\n");
}

static void PrintUsage(void)
{
    PrintVersion();
    OutText(
        L"\n"
        L"Использование: fwd81cli <команда> [аргументы]\n"
        L"\n"
        L"Работает сейчас:\n"
        L"  version              версия и состояние сборки\n"
        L"  help                 эта справка\n"
        L"  diag [--verbose] <exe>  разбор программы: запустится ли она на 8.1\n"
        L"  enable [--dry-run] <exe>   включить Fwd81 для программы (реестр + System32)\n"
        L"  disable [--dry-run] <exe>  выключить Fwd81 для программы\n"
        L"  uninstall [--dry-run]      убрать fwd81core.dll из System32 и все ключи IFEO\n"
        L"  log                        показать журнал работы ядра\n"
        L"  run <exe> [аргументы]      разовый запуск с внедрением ядра, без реестра\n"
        L"\n"
        L"  enable/disable/uninstall меняют систему и требуют прав администратора.\n"
        L"  --dry-run показывает, что будет сделано, ничего не записывая.\n"
        L"  run — инструмент разработки: чинит delay-load и рантайм LoadLibrary,\n"
        L"        но НЕ статические импорты EXE (это только путь IFEO).\n"
        L"\n"
        L"Запланировано (сейчас команда честно откажется работать):\n"
        L"  patch <exe>          понизить требование к версии Windows в файле [веха M6]\n"
        L"  list                 список программ, для которых включён Fwd81   [веха M6]\n"
        L"\n"
        L"Коды возврата: 0 успех, 1 ошибка в команде, 2 ещё не реализовано,\n"
        L"6 нужны права администратора, 7 операция не удалась.\n");
}

static int NotImplemented(const wchar_t *message)
{
    OutText(message);
    return FWD81_EXIT_NOT_IMPLEMENTED;
}

// `fwd81cli diag ...` не дублирует разбор PE, а зовёт fwd81diag.exe — анализ
// живёт в одном месте (правило «одна сущность — один путь»). Ищем его рядом с
// собой: обе программы кладутся в один каталог.
static int RunDiag(int argc, wchar_t **argv)
{
    wchar_t self[MAX_PATH];
    wchar_t *last_slash;
    wchar_t command[32768];  // потолок командной строки Windows
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD length;
    DWORD exit_code = FWD81_EXIT_USAGE;
    int i;

    length = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        OutText(L"Не удалось определить собственный путь для запуска fwd81diag.\n");
        return FWD81_EXIT_USAGE;
    }

    last_slash = wcsrchr(self, L'\\');
    if (last_slash == NULL) {
        OutText(L"Не удалось определить каталог для запуска fwd81diag.\n");
        return FWD81_EXIT_USAGE;
    }
    *(last_slash + 1) = L'\0';

    // Собираем: "<каталог>\fwd81diag.exe" <аргументы после `diag`>.
    command[0] = L'\0';
    wcscat_s(command, 32768, L"\"");
    wcscat_s(command, 32768, self);
    wcscat_s(command, 32768, L"fwd81diag.exe\"");
    for (i = 2; i < argc; i++) {
        wcscat_s(command, 32768, L" \"");
        wcscat_s(command, 32768, argv[i]);
        wcscat_s(command, 32768, L"\"");
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        OutText(L"Не удалось запустить fwd81diag.exe (он должен лежать рядом с fwd81cli).\n");
        return FWD81_EXIT_USAGE;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *command;

    if (argc < 2) {
        PrintUsage();
        return FWD81_EXIT_USAGE;
    }

    command = argv[1];

    if (_wcsicmp(command, L"version") == 0 ||
        _wcsicmp(command, L"--version") == 0 ||
        _wcsicmp(command, L"-v") == 0) {
        PrintVersion();
        return FWD81_EXIT_OK;
    }

    if (_wcsicmp(command, L"help") == 0 ||
        _wcsicmp(command, L"--help") == 0 ||
        _wcsicmp(command, L"-h") == 0 ||
        _wcsicmp(command, L"/?") == 0) {
        PrintUsage();
        return FWD81_EXIT_OK;
    }

    if (_wcsicmp(command, L"diag") == 0) {
        if (argc < 3) {
            OutText(L"Использование: fwd81cli diag [--verbose] <путь-к-программе.exe>\n");
            return FWD81_EXIT_USAGE;
        }
        return RunDiag(argc, argv);
    }

    if (_wcsicmp(command, L"enable") == 0) {
        const wchar_t *exe = FirstOperand(argc, argv);
        if (exe == NULL) {
            OutText(L"Использование: fwd81cli enable [--dry-run] <путь-или-имя.exe>\n");
            return FWD81_EXIT_USAGE;
        }
        return Fwd81Enable(exe, HasDryRun(argc, argv));
    }

    if (_wcsicmp(command, L"disable") == 0) {
        const wchar_t *exe = FirstOperand(argc, argv);
        if (exe == NULL) {
            OutText(L"Использование: fwd81cli disable [--dry-run] <путь-или-имя.exe>\n");
            return FWD81_EXIT_USAGE;
        }
        return Fwd81Disable(exe, HasDryRun(argc, argv));
    }

    if (_wcsicmp(command, L"uninstall") == 0)
        return Fwd81Uninstall(HasDryRun(argc, argv));

    if (_wcsicmp(command, L"log") == 0)
        return Fwd81ShowLog();

    if (_wcsicmp(command, L"run") == 0)
        return Fwd81Run(argc, argv);

    if (_wcsicmp(command, L"patch") == 0)
        return NotImplemented(L"Команда `patch` появится в вехе M6\n"
                              L"(понижение требования к версии Windows в заголовке файла).\n");

    if (_wcsicmp(command, L"list") == 0)
        return NotImplemented(L"Команда `list` появится в вехе M6 (профили программ).\n");

    OutText(L"Неизвестная команда. Запусти `fwd81cli help`.\n");
    return FWD81_EXIT_USAGE;
}
