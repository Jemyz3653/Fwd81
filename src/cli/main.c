// SPDX-License-Identifier: MIT
//
// fwd81cli.exe -- command line tool of Fwd81.
//
// STATUS: milestone M0. Only `version` and `help` do real work. Every other
// command reports honestly that it is not implemented yet and names the
// milestone that will implement it; none of them pretends to succeed.

#include <windows.h>
#include <wchar.h>

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

// Writing UTF-16 text so that it survives both a console window and a redirect
// into a file. WriteConsoleW only works on a real console handle; when stdout is
// redirected we have to convert to UTF-8 ourselves, otherwise Russian text turns
// into garbage in the log the user sends us.
static void OutText(const wchar_t *text)
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
        L"\n"
        L"Запланировано (сейчас команда честно откажется работать):\n"
        L"  enable <exe>         включить Fwd81 для программы через реестр    [веха M2]\n"
        L"  disable <exe>        выключить Fwd81 для программы                [веха M2]\n"
        L"  run <exe> [аргументы]  разовый запуск без записи в реестр         [веха M3]\n"
        L"  patch <exe>          понизить требование к версии Windows в файле [веха M6]\n"
        L"  list                 список программ, для которых включён Fwd81   [веха M6]\n"
        L"  log                  показать журнал работы ядра                  [веха M2]\n"
        L"  uninstall            убрать fwd81core.dll из System32 и очистить\n"
        L"                       все ключи реестра IFEO                        [веха M2]\n"
        L"\n"
        L"Коды возврата: 0 — успех, 1 — ошибка в команде, 2 — ещё не реализовано.\n");
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

    if (_wcsicmp(command, L"enable") == 0 || _wcsicmp(command, L"disable") == 0)
        return NotImplemented(L"Команды `enable` и `disable` появятся в вехе M2\n"
                              L"(внедрение ядра через реестр Image File Execution Options).\n");

    if (_wcsicmp(command, L"run") == 0)
        return NotImplemented(L"Команда `run` появится в вехе M3 (разовый запуск без реестра).\n");

    if (_wcsicmp(command, L"patch") == 0)
        return NotImplemented(L"Команда `patch` появится в вехе M6\n"
                              L"(понижение требования к версии Windows в заголовке файла).\n");

    if (_wcsicmp(command, L"list") == 0)
        return NotImplemented(L"Команда `list` появится в вехе M6 (профили программ).\n");

    if (_wcsicmp(command, L"log") == 0)
        return NotImplemented(L"Команда `log` появится в вехе M2 (журнал работы ядра).\n");

    if (_wcsicmp(command, L"uninstall") == 0)
        return NotImplemented(L"Команда `uninstall` появится в вехе M2.\n"
                              L"Она уберёт fwd81core.dll из System32 и вычистит все ключи\n"
                              L"реестра IFEO, которые создавал Fwd81. Удаление будет полным.\n");

    OutText(L"Неизвестная команда. Запусти `fwd81cli help`.\n");
    return FWD81_EXIT_USAGE;
}
