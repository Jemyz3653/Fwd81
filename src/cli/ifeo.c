// SPDX-License-Identifier: MIT
//
// Реализация внедрения через IFEO. См. ifeo.h.

#include <windows.h>
#include <wchar.h>

#include "out.h"
#include "ifeo.h"

#define IFEO_BASE \
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options"

#define CORE_DLL_NAME L"fwd81core.dll"
#define GLOBALFLAG_APPLICATION_VERIFIER 0x100

// --- Права администратора -----------------------------------------------------

static int IsElevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD returned = 0;
    int result = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return 0;
    if (GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &returned)) {
        result = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return result;
}

// --- Пути ---------------------------------------------------------------------

// Только имя файла из пути: "C:\a\b\app.exe" -> "app.exe".
static const wchar_t *BaseName(const wchar_t *path)
{
    const wchar_t *last = path;
    const wchar_t *p;
    for (p = path; *p; p++) {
        if (*p == L'\\' || *p == L'/')
            last = p + 1;
    }
    return last;
}

// Путь fwd81core.dll рядом с fwd81cli.exe. FALSE при неудаче.
static BOOL SelfDirCoreDll(wchar_t *out, DWORD out_chars)
{
    wchar_t self[MAX_PATH];
    DWORD len;
    wchar_t *slash;

    len = GetModuleFileNameW(NULL, self, MAX_PATH);
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

// Путь System32\fwd81core.dll. FALSE при неудаче.
static BOOL System32CoreDll(wchar_t *out, DWORD out_chars)
{
    wchar_t sys[MAX_PATH];
    UINT len = GetSystemDirectoryW(sys, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return FALSE;
    if (len + wcslen(L"\\") + wcslen(CORE_DLL_NAME) >= out_chars)
        return FALSE;
    wcscpy_s(out, out_chars, sys);
    wcscat_s(out, out_chars, L"\\");
    wcscat_s(out, out_chars, CORE_DLL_NAME);
    return TRUE;
}

// --- Работа с одним ключом IFEO -----------------------------------------------

// Есть ли у ключа значение VerifierDlls == fwd81core.dll (наша метка).
static BOOL SubkeyIsOurs(HKEY ifeo, const wchar_t *image_name)
{
    HKEY key;
    wchar_t value[MAX_PATH];
    DWORD type = 0;
    DWORD size = sizeof(value);
    BOOL ours = FALSE;

    if (RegOpenKeyExW(ifeo, image_name, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(key, L"VerifierDlls", NULL, &type,
                         (LPBYTE)value, &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        value[MAX_PATH - 1] = L'\0';
        ours = (_wcsicmp(value, CORE_DLL_NAME) == 0);
    }
    RegCloseKey(key);
    return ours;
}

// Снять наши значения из ключа image_name; если ключ опустел — удалить его.
static int RemoveFromSubkey(HKEY ifeo, const wchar_t *image_name)
{
    HKEY key;
    DWORD subkeys = 0, values = 0;
    LONG rc;

    rc = RegOpenKeyExW(ifeo, image_name, 0, KEY_READ | KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS)
        return FWD81_IFEO_FAILED;

    RegDeleteValueW(key, L"VerifierDlls");
    RegDeleteValueW(key, L"GlobalFlag");

    // Остались ли в ключе другие значения или подключи (чужие настройки)?
    RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL,
                     &values, NULL, NULL, NULL, NULL);
    RegCloseKey(key);

    if (subkeys == 0 && values == 0)
        RegDeleteKeyW(ifeo, image_name);  // ключ был только нашим — убираем целиком

    return FWD81_IFEO_OK;
}

// --- Команды ------------------------------------------------------------------

int Fwd81Enable(const wchar_t *image_name_arg, int dry_run)
{
    const wchar_t *image = BaseName(image_name_arg);
    wchar_t src[MAX_PATH];
    wchar_t dst[MAX_PATH];
    HKEY key;
    DWORD disposition;
    DWORD flag = GLOBALFLAG_APPLICATION_VERIFIER;
    LONG rc;

    if (image == NULL || image[0] == L'\0') {
        OutLine(L"Не указано имя программы.");
        return FWD81_IFEO_USAGE;
    }
    if (!SelfDirCoreDll(src, MAX_PATH) || !System32CoreDll(dst, MAX_PATH)) {
        OutLine(L"Не удалось определить пути к fwd81core.dll.");
        return FWD81_IFEO_FAILED;
    }

    OutText(L"Программа:        "); OutLine(image);
    OutText(L"Копировать ядро:  "); OutText(src); OutText(L"  ->  "); OutLine(dst);
    OutText(L"Ключ реестра:     HKLM\\"); OutText(IFEO_BASE); OutText(L"\\"); OutLine(image);
    OutLine(L"  GlobalFlag   = 0x100 (Application Verifier)");
    OutLine(L"  VerifierDlls = fwd81core.dll");

    if (dry_run) {
        OutLine(L"");
        OutLine(L"Это dry-run: ничего не записано. Убери --dry-run, чтобы применить.");
        return FWD81_IFEO_OK;
    }

    if (!IsElevated()) {
        OutLine(L"");
        OutLine(L"Нужны права администратора: запись в System32 и в HKLM без них недоступна.");
        OutLine(L"Запусти командную строку от имени администратора и повтори.");
        return FWD81_IFEO_NEED_ADMIN;
    }

    if (!CopyFileW(src, dst, FALSE)) {
        OutLine(L"");
        OutLine(L"Не удалось скопировать fwd81core.dll в System32.");
        return FWD81_IFEO_FAILED;
    }

    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, IFEO_BASE, 0, NULL, 0,
                         KEY_CREATE_SUB_KEY, NULL, &key, &disposition);
    if (rc != ERROR_SUCCESS) {
        OutLine(L"Не удалось открыть ветку IFEO в реестре.");
        return FWD81_IFEO_FAILED;
    }
    RegCloseKey(key);

    {
        HKEY sub;
        wchar_t path[512];
        wcscpy_s(path, 512, IFEO_BASE);
        wcscat_s(path, 512, L"\\");
        wcscat_s(path, 512, image);
        rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0,
                             KEY_SET_VALUE, NULL, &sub, &disposition);
        if (rc != ERROR_SUCCESS) {
            OutLine(L"Не удалось создать ключ программы в IFEO.");
            return FWD81_IFEO_FAILED;
        }
        RegSetValueExW(sub, L"GlobalFlag", 0, REG_DWORD,
                       (const BYTE *)&flag, sizeof(flag));
        RegSetValueExW(sub, L"VerifierDlls", 0, REG_SZ,
                       (const BYTE *)CORE_DLL_NAME,
                       (DWORD)((wcslen(CORE_DLL_NAME) + 1) * sizeof(wchar_t)));
        RegCloseKey(sub);
    }

    OutLine(L"");
    OutLine(L"Готово. Fwd81 включён для этой программы.");
    OutLine(L"Проверить факт загрузки ядра можно командой `fwd81cli log` после её запуска.");
    return FWD81_IFEO_OK;
}

int Fwd81Disable(const wchar_t *image_name_arg, int dry_run)
{
    const wchar_t *image = BaseName(image_name_arg);
    HKEY ifeo;
    int result;

    if (image == NULL || image[0] == L'\0') {
        OutLine(L"Не указано имя программы.");
        return FWD81_IFEO_USAGE;
    }

    OutText(L"Выключить Fwd81 для программы: "); OutLine(image);
    OutText(L"Ключ: HKLM\\"); OutText(IFEO_BASE); OutText(L"\\"); OutLine(image);
    OutLine(L"  снимаются значения GlobalFlag и VerifierDlls; чужие настройки не трогаются.");

    if (dry_run) {
        OutLine(L"");
        OutLine(L"Это dry-run: ничего не изменено.");
        return FWD81_IFEO_OK;
    }
    if (!IsElevated()) {
        OutLine(L"");
        OutLine(L"Нужны права администратора для правки HKLM.");
        return FWD81_IFEO_NEED_ADMIN;
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, IFEO_BASE, 0,
                      KEY_READ | KEY_SET_VALUE, &ifeo) != ERROR_SUCCESS) {
        OutLine(L"Ветка IFEO не открывается — возможно, для этой программы Fwd81 и не был включён.");
        return FWD81_IFEO_OK;
    }
    result = RemoveFromSubkey(ifeo, image);
    RegCloseKey(ifeo);

    OutLine(L"");
    OutLine(result == FWD81_IFEO_OK
            ? L"Готово. Fwd81 выключен для этой программы (файл в System32 оставлен; убрать его — `uninstall`)."
            : L"Ключ этой программы не найден — похоже, Fwd81 для неё не был включён.");
    return FWD81_IFEO_OK;
}

int Fwd81Uninstall(int dry_run)
{
    HKEY ifeo;
    wchar_t name[MAX_PATH];
    wchar_t core[MAX_PATH];
    DWORD index = 0;
    DWORD name_len;
    int found = 0;
    wchar_t our_keys[64][MAX_PATH];
    int our_count = 0;
    int i;

    OutLine(L"Полное удаление Fwd81 из системы:");
    OutLine(L"  1) снять наши значения во всех программах IFEO;");
    OutLine(L"  2) убрать fwd81core.dll из System32.");
    OutLine(L"");

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, IFEO_BASE, 0,
                      KEY_READ | KEY_SET_VALUE, &ifeo) == ERROR_SUCCESS) {
        // Сначала собираем список наших ключей (менять во время перечисления нельзя).
        for (index = 0; our_count < 64; index++) {
            name_len = MAX_PATH;
            if (RegEnumKeyExW(ifeo, index, name, &name_len, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
                break;
            if (SubkeyIsOurs(ifeo, name)) {
                wcscpy_s(our_keys[our_count], MAX_PATH, name);
                our_count++;
            }
        }
        for (i = 0; i < our_count; i++) {
            OutText(L"  IFEO: "); OutLine(our_keys[i]);
            found++;
            if (!dry_run && IsElevated())
                RemoveFromSubkey(ifeo, our_keys[i]);
        }
        RegCloseKey(ifeo);
    }
    if (found == 0)
        OutLine(L"  в IFEO наших записей не найдено.");

    if (System32CoreDll(core, MAX_PATH)) {
        if (GetFileAttributesW(core) != INVALID_FILE_ATTRIBUTES) {
            OutText(L"  System32: "); OutLine(core);
            if (!dry_run && IsElevated())
                DeleteFileW(core);
        } else {
            OutLine(L"  fwd81core.dll в System32 не найден.");
        }
    }

    OutLine(L"");
    if (dry_run) {
        OutLine(L"Это dry-run: ничего не удалено.");
        return FWD81_IFEO_OK;
    }
    if (!IsElevated()) {
        OutLine(L"Нужны права администратора: без них нельзя ни править HKLM, ни удалять из System32.");
        return FWD81_IFEO_NEED_ADMIN;
    }
    OutLine(L"Готово. Fwd81 удалён из системы.");
    return FWD81_IFEO_OK;
}

// --- Журнал -------------------------------------------------------------------

int Fwd81ShowLog(void)
{
    wchar_t path[MAX_PATH];
    DWORD len;
    HANDLE file;
    HANDLE out;
    BYTE buffer[8192];
    DWORD read, written;

    len = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        OutLine(L"Не удалось определить %LOCALAPPDATA%.");
        return FWD81_IFEO_FAILED;
    }
    if (wcscat_s(path, MAX_PATH, L"\\Fwd81\\logs\\fwd81.log") != 0) {
        OutLine(L"Слишком длинный путь к журналу.");
        return FWD81_IFEO_FAILED;
    }

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        OutText(L"Журнал пуст или ещё не создан: "); OutLine(path);
        OutLine(L"Он появится, когда ядро загрузится хотя бы в один процесс.");
        return FWD81_IFEO_OK;
    }

    // Журнал уже в UTF-8 — отдаём байты в стандартный вывод как есть.
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    while (ReadFile(file, buffer, sizeof(buffer), &read, NULL) && read > 0)
        WriteFile(out, buffer, read, &written, NULL);
    CloseHandle(file);
    return FWD81_IFEO_OK;
}
