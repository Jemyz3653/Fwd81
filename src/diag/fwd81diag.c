// SPDX-License-Identifier: MIT
//
// fwd81diag.exe -- анализатор: запустится ли программа Windows 10 на Windows 8.1.
//
// Ничего не запускает и не изменяет. Читает EXE, достаёт из него:
//   * требуемую версию Windows (из заголовка PE) -- отказ №1 из architecture.md;
//   * список функций, которые программа берёт из системных библиотек
//     (обычный импорт и отложенная загрузка) -- отказы №2 и №3.
// Затем сверяет функции с базой: что есть в 8.1 и что реализовано у нас
// (встроенный заголовок fwd81diag_baseline.h, его генерирует exportdiff), и
// выдаёт вердикт.
//
// Утилита обязана запускаться на самой 8.1: чистый Win32, статическая CRT,
// метка подсистемы 6.03. Разбор чужого, возможно битого, файла не должен ронять
// программу -- отсюда проверки границ на каждом шаге.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fwd81diag_baseline.h"

#define FWD81_EXIT_WILL_RUN         0  // запустится
#define FWD81_EXIT_RUN_LIMITED      3  // запустится с ограничениями
#define FWD81_EXIT_WILL_NOT_RUN     4  // не запустится
#define FWD81_EXIT_UNKNOWN          5  // вердикт невозможен (нет базы 8.1)
#define FWD81_EXIT_USAGE            1  // ошибка в аргументах
#define FWD81_EXIT_CANNOT_READ      2  // файл не прочитать / не PE

#define WIN81_SUBSYSTEM_MAJOR 6
#define WIN81_SUBSYSTEM_MINOR 3

// ---------------------------------------------------------------------------
//  Вывод UTF-8 (см. пояснение в src/cli/main.c)
// ---------------------------------------------------------------------------

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
    utf8 = (char *)malloc((size_t)bytes);
    if (utf8 == NULL)
        return;
    WideCharToMultiByte(CP_UTF8, 0, text, (int)length, utf8, bytes, NULL, NULL);
    WriteFile(out, utf8, (DWORD)bytes, &written, NULL);
    free(utf8);
}

static void OutLine(const wchar_t *text)
{
    OutText(text);
    OutText(L"\n");
}

// ---------------------------------------------------------------------------
//  Чтение файла в память
// ---------------------------------------------------------------------------

typedef struct {
    unsigned char *data;
    size_t         size;
} FileBuffer;

static BOOL ReadWholeFile(const wchar_t *path, FileBuffer *out)
{
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read;
    unsigned char *buffer;

    out->data = NULL;
    out->size = 0;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;

    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (LONGLONG)0x7fffffff) {
        CloseHandle(file);
        return FALSE;
    }

    buffer = (unsigned char *)malloc((size_t)size.QuadPart);
    if (buffer == NULL) {
        CloseHandle(file);
        return FALSE;
    }

    if (!ReadFile(file, buffer, (DWORD)size.QuadPart, &read, NULL) ||
        read != (DWORD)size.QuadPart) {
        free(buffer);
        CloseHandle(file);
        return FALSE;
    }

    CloseHandle(file);
    out->data = buffer;
    out->size = (size_t)size.QuadPart;
    return TRUE;
}

// ---------------------------------------------------------------------------
//  Разбор PE. Все смещения проверяются на выход за границу буфера.
// ---------------------------------------------------------------------------

typedef struct {
    const FileBuffer *file;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sections;
    WORD section_count;
} PeImage;

// Указатель внутри буфера, если [offset, offset+need) не вышли за границу.
static const void *AtOffset(const FileBuffer *file, size_t offset, size_t need)
{
    if (offset > file->size || need > file->size - offset)
        return NULL;
    return file->data + offset;
}

// Перевод RVA (адрес, как если бы файл был загружен) в смещение в файле на диске.
static size_t RvaToOffset(const PeImage *pe, DWORD rva)
{
    WORD i;
    for (i = 0; i < pe->section_count; i++) {
        const IMAGE_SECTION_HEADER *s = &pe->sections[i];
        DWORD size = s->SizeOfRawData;
        if (rva >= s->VirtualAddress && rva < s->VirtualAddress + size)
            return (size_t)s->PointerToRawData + (rva - s->VirtualAddress);
    }
    return (size_t)-1;  // RVA не попал ни в одну секцию
}

// Строка ASCII по RVA, с проверкой, что она заканчивается внутри файла.
static const char *StringAtRva(const PeImage *pe, DWORD rva)
{
    size_t offset = RvaToOffset(pe, rva);
    size_t i;
    if (offset == (size_t)-1 || offset >= pe->file->size)
        return NULL;
    for (i = offset; i < pe->file->size; i++) {
        if (pe->file->data[i] == 0)
            return (const char *)(pe->file->data + offset);
    }
    return NULL;  // строка не оканчивается нулём внутри файла
}

// Разобрать заголовки. FALSE, если это не x64 PE.
static BOOL ParsePe(const FileBuffer *file, PeImage *pe, const wchar_t **why)
{
    const IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    size_t nt_offset;

    memset(pe, 0, sizeof(*pe));
    pe->file = file;

    dos = (const IMAGE_DOS_HEADER *)AtOffset(file, 0, sizeof(IMAGE_DOS_HEADER));
    if (dos == NULL || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        *why = L"это не исполняемый файл Windows (нет метки MZ).";
        return FALSE;
    }

    nt_offset = (size_t)dos->e_lfanew;
    nt = (IMAGE_NT_HEADERS64 *)AtOffset(file, nt_offset, sizeof(IMAGE_NT_HEADERS64));
    if (nt == NULL || nt->Signature != IMAGE_NT_SIGNATURE) {
        *why = L"повреждённый или неполный заголовок PE.";
        return FALSE;
    }

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        *why = L"файл не 64-разрядный (x64). Fwd81 сейчас работает только с x64.";
        return FALSE;
    }

    pe->nt = nt;
    pe->section_count = nt->FileHeader.NumberOfSections;

    // Таблица секций идёт сразу за optional-заголовком.
    {
        size_t sec_offset = nt_offset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
                          + nt->FileHeader.SizeOfOptionalHeader;
        size_t need = (size_t)pe->section_count * sizeof(IMAGE_SECTION_HEADER);
        pe->sections = (IMAGE_SECTION_HEADER *)AtOffset(file, sec_offset, need);
        if (pe->sections == NULL) {
            *why = L"повреждённая таблица секций.";
            return FALSE;
        }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
//  База 8.1 и покрытие
// ---------------------------------------------------------------------------

// Двоичный поиск в отсортированном массиве имён (FWD81_PRESENT_IN_81).
static BOOL PresentIn81(const char *name)
{
    int lo = 0, hi = FWD81_PRESENT_IN_81_COUNT - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, FWD81_PRESENT_IN_81[mid]);
        if (cmp == 0) return TRUE;
        if (cmp < 0) hi = mid - 1;
        else         lo = mid + 1;
    }
    return FALSE;
}

// Наша категория для функции, или 0, если мы её не покрываем.
static char CoverageOf(const char *name)
{
    int i;
    for (i = 0; i < FWD81_COVERAGE_COUNT; i++) {
        if (strcmp(name, FWD81_COVERAGE[i].function) == 0)
            return FWD81_COVERAGE[i].category;
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  Обход импортируемых функций
// ---------------------------------------------------------------------------

// Итог по всем функциям, накапливается при обходе.
typedef struct {
    int total;          // всего именованных импортов
    int missing_hard;   // нет в 8.1 и мы не покрываем -> блокирует запуск
    int missing_stub;   // нет в 8.1, покрыто заглушкой S -> ограничение
    int missing_soft;   // нет в 8.1, покрыто F/I -> нормально
} ImportStats;

// Обработать одну функцию по имени.
static void HandleFunction(const char *name, ImportStats *stats, BOOL verbose)
{
    char category;
    wchar_t wide[512];

    stats->total++;

    if (!FWD81_BASELINE_AVAILABLE) {
        if (verbose) {
            MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 512);
            OutText(L"    ");
            OutLine(wide);
        }
        return;
    }

    if (PresentIn81(name))
        return;  // функция есть в 8.1 — вопросов нет

    category = CoverageOf(name);
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 512);

    if (category == 0) {
        stats->missing_hard++;
        OutText(L"    [нет в 8.1, НЕ покрыто]  ");
        OutLine(wide);
    } else if (category == 'S') {
        stats->missing_stub++;
        OutText(L"    [нет в 8.1, заглушка S]  ");
        OutLine(wide);
    } else if (category == 'N') {
        stats->missing_hard++;
        OutText(L"    [нет в 8.1, невозможно N] ");
        OutLine(wide);
    } else {
        stats->missing_soft++;
        if (verbose) {
            OutText(L"    [нет в 8.1, покрыто]     ");
            OutLine(wide);
        }
    }
}

// Обход одной таблицы имён (INT) — общей для обычного и отложенного импорта.
static void WalkThunks(const PeImage *pe, DWORD int_rva, ImportStats *stats, BOOL verbose)
{
    size_t offset = RvaToOffset(pe, int_rva);
    const IMAGE_THUNK_DATA64 *thunk;

    if (offset == (size_t)-1)
        return;

    for (;;) {
        ULONGLONG value;
        thunk = (const IMAGE_THUNK_DATA64 *)AtOffset(pe->file, offset, sizeof(*thunk));
        if (thunk == NULL)
            return;
        value = thunk->u1.AddressOfData;
        if (value == 0)
            return;  // конец таблицы

        if (!(value & IMAGE_ORDINAL_FLAG64)) {
            // Импорт по имени: value — RVA структуры IMAGE_IMPORT_BY_NAME.
            const IMAGE_IMPORT_BY_NAME *by_name;
            size_t name_offset = RvaToOffset(pe, (DWORD)value);
            if (name_offset != (size_t)-1) {
                by_name = (const IMAGE_IMPORT_BY_NAME *)
                          AtOffset(pe->file, name_offset, sizeof(WORD) + 1);
                if (by_name != NULL) {
                    const char *fn = StringAtRva(pe, (DWORD)value + FIELD_OFFSET(IMAGE_IMPORT_BY_NAME, Name));
                    if (fn != NULL)
                        HandleFunction(fn, stats, verbose);
                }
            }
        }
        // Импорт по ординалу пропускаем: по имени его не сверить с базой.
        offset += sizeof(*thunk);
    }
}

// Обычная таблица импорта.
static void WalkImports(const PeImage *pe, ImportStats *stats, BOOL verbose)
{
    IMAGE_DATA_DIRECTORY dir = pe->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    size_t offset;
    const IMAGE_IMPORT_DESCRIPTOR *desc;

    if (dir.VirtualAddress == 0 || dir.Size == 0)
        return;
    offset = RvaToOffset(pe, dir.VirtualAddress);
    if (offset == (size_t)-1)
        return;

    for (;;) {
        DWORD int_rva;
        const char *dll;
        wchar_t wide[512];

        desc = (const IMAGE_IMPORT_DESCRIPTOR *)AtOffset(pe->file, offset, sizeof(*desc));
        if (desc == NULL || desc->Name == 0)
            return;

        dll = StringAtRva(pe, desc->Name);
        if (dll != NULL) {
            MultiByteToWideChar(CP_UTF8, 0, dll, -1, wide, 512);
            OutText(L"  ");
            OutLine(wide);
        }

        // OriginalFirstThunk (таблица имён) надёжнее FirstThunk: последнюю
        // загрузчик перезаписывает адресами. Если её нет — берём FirstThunk.
        int_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        WalkThunks(pe, int_rva, stats, verbose);

        offset += sizeof(*desc);
    }
}

// Таблица отложенной загрузки (delay-load). Современный формат — RVA-based.
static void WalkDelayImports(const PeImage *pe, ImportStats *stats, BOOL verbose)
{
    IMAGE_DATA_DIRECTORY dir =
        pe->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    size_t offset;

    if (dir.VirtualAddress == 0 || dir.Size == 0)
        return;
    offset = RvaToOffset(pe, dir.VirtualAddress);
    if (offset == (size_t)-1)
        return;

    for (;;) {
        const IMAGE_DELAYLOAD_DESCRIPTOR *desc =
            (const IMAGE_DELAYLOAD_DESCRIPTOR *)AtOffset(pe->file, offset, sizeof(*desc));
        const char *dll;
        wchar_t wide[512];

        if (desc == NULL || desc->DllNameRVA == 0)
            return;

        dll = StringAtRva(pe, desc->DllNameRVA);
        if (dll != NULL) {
            MultiByteToWideChar(CP_UTF8, 0, dll, -1, wide, 512);
            OutText(L"  ");
            OutText(wide);
            OutLine(L"  (отложенная загрузка)");
        }

        WalkThunks(pe, desc->ImportNameTableRVA, stats, verbose);
        offset += sizeof(*desc);
    }
}

// ---------------------------------------------------------------------------
//  Отчёт и вердикт
// ---------------------------------------------------------------------------

static void PrintRequiredVersion(const PeImage *pe, BOOL *version_blocks)
{
    WORD sub_major = pe->nt->OptionalHeader.MajorSubsystemVersion;
    WORD sub_minor = pe->nt->OptionalHeader.MinorSubsystemVersion;
    wchar_t line[256];

    *version_blocks = (sub_major > WIN81_SUBSYSTEM_MAJOR) ||
        (sub_major == WIN81_SUBSYSTEM_MAJOR && sub_minor > WIN81_SUBSYSTEM_MINOR);

    swprintf(line, 256, L"Требуемая версия Windows (из заголовка): %u.%u",
             sub_major, sub_minor);
    OutLine(line);

    if (*version_blocks) {
        OutLine(L"  ВНИМАНИЕ: это выше 6.3 (Windows 8.1). Загрузчик 8.1 откажется");
        OutLine(L"  запускать файл ещё до поиска функций. Лечится командой");
        OutLine(L"  `fwd81cli patch` (появится в вехе M6): она понизит эту метку.");
    } else {
        OutLine(L"  Это не выше 6.3 — по версии заголовка 8.1 файл примет.");
    }
    OutLine(L"");
}

static int PrintVerdict(const ImportStats *stats, BOOL version_blocks)
{
    wchar_t line[256];

    OutLine(L"----------------------------------------------------------------");
    swprintf(line, 256, L"Импортируемых функций (по имени): %d", stats->total);
    OutLine(line);

    if (!FWD81_BASELINE_AVAILABLE) {
        OutLine(L"");
        OutLine(L"База Windows 8.1 ещё НЕ снята — сравнить функции не с чем.");
        OutLine(L"Показан только список импортов и требуемая версия Windows.");
        OutLine(L"Чтобы получить вердикт по функциям: скопируй System32 из 8.1 в");
        OutLine(L"data/system32-win81, выполни");
        OutLine(L"  py -3 tools/exportdiff/exportdiff.py all");
        OutLine(L"и пересобери fwd81diag.");
        OutLine(L"");
        if (version_blocks)
            OutLine(L"ВЕРДИКТ по заголовку: без `patch` не запустится (версия выше 8.1).");
        else
            OutLine(L"ВЕРДИКТ по заголовку: версия подходит; про функции — неизвестно.");
        return FWD81_EXIT_UNKNOWN;
    }

    swprintf(line, 256, L"  нет в 8.1 и не покрыто нами: %d", stats->missing_hard);
    OutLine(line);
    swprintf(line, 256, L"  нет в 8.1, покрыто заглушкой:  %d", stats->missing_stub);
    OutLine(line);
    swprintf(line, 256, L"  нет в 8.1, покрыто полноценно: %d", stats->missing_soft);
    OutLine(line);
    OutLine(L"");

    if (stats->missing_hard > 0) {
        OutLine(L"ВЕРДИКТ: НЕ ЗАПУСТИТСЯ.");
        OutLine(L"Есть функции, которых нет в 8.1 и которые мы пока не покрываем —");
        OutLine(L"программа упадёт с «точка входа не найдена». Список помечен выше.");
        return FWD81_EXIT_WILL_NOT_RUN;
    }
    if (version_blocks) {
        OutLine(L"ВЕРДИКТ: ЗАПУСТИТСЯ С ОГРАНИЧЕНИЯМИ.");
        OutLine(L"По функциям всё покрыто, но заголовок требует версию выше 8.1 —");
        OutLine(L"сначала нужен `fwd81cli patch`.");
        return FWD81_EXIT_RUN_LIMITED;
    }
    if (stats->missing_stub > 0) {
        OutLine(L"ВЕРДИКТ: ЗАПУСТИТСЯ С ОГРАНИЧЕНИЯМИ.");
        OutLine(L"Часть недостающих функций закрыта заглушками (категория S): они не");
        OutLine(L"падают, но и настоящей работы не делают. Что именно — помечено выше.");
        return FWD81_EXIT_RUN_LIMITED;
    }
    OutLine(L"ВЕРДИКТ: ЗАПУСТИТСЯ.");
    OutLine(L"Все функции либо есть в 8.1, либо полноценно покрыты нами.");
    return FWD81_EXIT_WILL_RUN;
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------

static void PrintUsage(void)
{
    OutLine(L"fwd81diag — анализ: запустится ли программа Windows 10 на Windows 8.1.");
    OutLine(L"");
    OutLine(L"Использование: fwd81diag [--verbose] <путь-к-программе.exe>");
    OutLine(L"");
    OutLine(L"  --verbose    показывать и функции, с которыми всё в порядке");
    OutLine(L"");
    OutLine(L"Ничего не запускает и не меняет. Только читает файл.");
    OutLine(L"Коды возврата: 0 запустится, 3 с ограничениями, 4 не запустится,");
    OutLine(L"5 вердикт невозможен (нет базы 8.1), 1 ошибка аргументов, 2 не прочитать файл.");
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *target = NULL;
    BOOL verbose = FALSE;
    int i;
    FileBuffer file;
    PeImage pe;
    const wchar_t *why = L"";
    ImportStats stats;
    BOOL version_blocks = FALSE;
    int verdict;
    wchar_t line[1024];

    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--verbose") == 0 || _wcsicmp(argv[i], L"-v") == 0)
            verbose = TRUE;
        else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0 ||
                 _wcsicmp(argv[i], L"/?") == 0) {
            PrintUsage();
            return 0;
        } else if (target == NULL)
            target = argv[i];
        else {
            OutLine(L"Слишком много аргументов. Ожидается один путь к программе.");
            return FWD81_EXIT_USAGE;
        }
    }

    if (target == NULL) {
        PrintUsage();
        return FWD81_EXIT_USAGE;
    }

    if (!ReadWholeFile(target, &file)) {
        OutText(L"Не удалось прочитать файл: ");
        OutLine(target);
        return FWD81_EXIT_CANNOT_READ;
    }

    if (!ParsePe(&file, &pe, &why)) {
        OutText(L"Это не подходящий файл: ");
        OutLine(why);
        free(file.data);
        return FWD81_EXIT_CANNOT_READ;
    }

    swprintf(line, 1024, L"Программа: %s", target);
    OutLine(line);
    if (!FWD81_BASELINE_AVAILABLE)
        OutLine(L"(база 8.1 не снята — вердикт по функциям будет недоступен)");
    OutLine(L"");

    PrintRequiredVersion(&pe, &version_blocks);

    OutLine(L"Импортируемые библиотеки и функции:");
    memset(&stats, 0, sizeof(stats));
    WalkImports(&pe, &stats, verbose);
    WalkDelayImports(&pe, &stats, verbose);
    OutLine(L"");

    verdict = PrintVerdict(&stats, version_blocks);

    free(file.data);
    return verdict;
}
