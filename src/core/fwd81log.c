// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Реализация журнала ядра. Только ntdll, без CRT. См. fwd81log.h.
//
// Источники сигнатур нативных функций: заголовок winternl.h из Windows SDK и
// документация Microsoft на функции Nt*/Rtl*. Каждая из них есть в Windows 8.1
// (проверять по data/exports-win81.json, когда появится копия System32 из 8.1).

#include <windows.h>
#include <winternl.h>

#include "fwd81log.h"

// --- Константы NtCreateFile, которых нет в пользовательских заголовках --------
// (обычно живут в ntifs.h/wdm.h из WDK). Значения документированы Microsoft.

#ifndef FILE_OPEN_IF
#define FILE_OPEN_IF 0x00000003
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_APPEND_DATA
#define FILE_APPEND_DATA 0x00000004
#endif
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE 0x00000040
#endif

// Специальное смещение «в конец файла» для NtWriteFile.
#define FWD81_WRITE_TO_END_LOW  0xFFFFFFFF
#define FWD81_WRITE_TO_END_HIGH -1

// --- Прототипы функций ntdll, которых нет в winternl.h ------------------------
// Объявляем сами; линкуемся с ntdll.lib. Если winternl.h что-то из этого уже
// объявил ровно так же — повторное идентичное объявление в C допустимо.

// В WDK тип полей называется CSHORT (== short). В пользовательском SDK его нет,
// поэтому берём обычный SHORT — размер и раскладка те же.
typedef struct _FWD81_TIME_FIELDS {
    SHORT Year;
    SHORT Month;
    SHORT Day;
    SHORT Hour;
    SHORT Minute;
    SHORT Second;
    SHORT Milliseconds;
    SHORT Weekday;
} FWD81_TIME_FIELDS;

NTSTATUS NTAPI RtlQueryEnvironmentVariable_U(PVOID Environment,
                                             PUNICODE_STRING Name,
                                             PUNICODE_STRING Value);

NTSTATUS NTAPI NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                            POBJECT_ATTRIBUTES ObjectAttributes,
                            PIO_STATUS_BLOCK IoStatusBlock,
                            PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                            ULONG ShareAccess, ULONG CreateDisposition,
                            ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);

NTSTATUS NTAPI NtWriteFile(HANDLE FileHandle, HANDLE Event,
                           PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                           PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
                           ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);

NTSTATUS NTAPI NtQuerySystemTime(PLARGE_INTEGER SystemTime);
NTSTATUS NTAPI RtlSystemTimeToLocalTime(PLARGE_INTEGER SystemTime,
                                        PLARGE_INTEGER LocalTime);
VOID NTAPI RtlTimeToTimeFields(PLARGE_INTEGER Time, FWD81_TIME_FIELDS *TimeFields);
NTSTATUS NTAPI RtlUnicodeToUTF8N(PCHAR Utf8, ULONG Utf8Size, PULONG BytesWritten,
                                 PCWSTR Unicode, ULONG UnicodeSize);

// --- Маленькие помощники: сборка строки в стековый буфер, без CRT -------------

typedef struct {
    char  *data;
    ULONG  capacity;
    ULONG  length;
} Fwd81Buffer;

static void BufInit(Fwd81Buffer *b, char *storage, ULONG capacity)
{
    b->data = storage;
    b->capacity = capacity;
    b->length = 0;
}

static void BufAppendBytes(Fwd81Buffer *b, const char *bytes, ULONG count)
{
    ULONG i;
    for (i = 0; i < count && b->length < b->capacity; i++)
        b->data[b->length++] = bytes[i];
}

static void BufAppendAsciiZ(Fwd81Buffer *b, const char *text)
{
    ULONG i = 0;
    while (text[i] != 0 && b->length < b->capacity)
        b->data[b->length++] = text[i++];
}

// Беззнаковое число в десятичном виде, ширина не меньше min_digits (для дат).
static void BufAppendUnsigned(Fwd81Buffer *b, unsigned long long value, int min_digits)
{
    char tmp[24];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (n < min_digits)
        tmp[n++] = '0';
    while (n > 0 && b->length < b->capacity)
        b->data[b->length++] = tmp[--n];
}

// UTF-16 -> UTF-8 в буфер (для пути к образу процесса).
static void BufAppendWide(Fwd81Buffer *b, PCWSTR text, ULONG wchar_count)
{
    ULONG written = 0;
    if (wchar_count == 0)
        return;
    if (b->length >= b->capacity)
        return;
    if (RtlUnicodeToUTF8N(b->data + b->length, b->capacity - b->length, &written,
                          text, wchar_count * (ULONG)sizeof(WCHAR)) == 0) {
        b->length += written;
    }
}

// --- Доступ к сведениям о текущем процессе ------------------------------------

static void AppendProcessImage(Fwd81Buffer *b)
{
    PTEB teb = NtCurrentTeb();
    PPEB peb;
    PRTL_USER_PROCESS_PARAMETERS params;

    if (teb == NULL)
        return;
    peb = teb->ProcessEnvironmentBlock;
    if (peb == NULL || peb->ProcessParameters == NULL)
        return;
    params = peb->ProcessParameters;
    BufAppendWide(b, params->ImagePathName.Buffer,
                  params->ImagePathName.Length / (ULONG)sizeof(WCHAR));
}

static unsigned long long CurrentProcessId(void)
{
    PROCESS_BASIC_INFORMATION pbi;
    ULONG returned = 0;
    // NtCurrentProcess() == (HANDLE)-1.
    if (NtQueryInformationProcess((HANDLE)(LONG_PTR)-1, ProcessBasicInformation,
                                  &pbi, sizeof(pbi), &returned) == 0) {
        return (unsigned long long)(ULONG_PTR)pbi.UniqueProcessId;
    }
    return 0;
}

// --- Путь к файлу журнала -----------------------------------------------------

// Собирает NT-путь "\??\<LOCALAPPDATA>\Fwd81\logs" (без последнего сегмента,
// если want_file==0) или "...\fwd81.log" (если want_file!=0) в out.
// Возвращает число символов или 0 при неудаче.
static ULONG BuildNtPath(WCHAR *out, ULONG out_chars, int want_file, int logs_subdir)
{
    static const WCHAR prefix[] = L"\\??\\";
    static const WCHAR name[]   = L"LOCALAPPDATA";
    static const WCHAR fwd[]    = L"\\Fwd81";
    static const WCHAR logs[]   = L"\\logs";
    static const WCHAR file[]   = L"\\fwd81.log";

    UNICODE_STRING var_name;
    UNICODE_STRING var_value;
    WCHAR value_storage[512];
    ULONG pos = 0;
    ULONG i;

    var_name.Buffer = (PWSTR)name;
    var_name.Length = (USHORT)((sizeof(name) / sizeof(WCHAR) - 1) * sizeof(WCHAR));
    var_name.MaximumLength = var_name.Length;

    var_value.Buffer = value_storage;
    var_value.Length = 0;
    var_value.MaximumLength = (USHORT)sizeof(value_storage);

    if (RtlQueryEnvironmentVariable_U(NULL, &var_name, &var_value) != 0)
        return 0;
    if (var_value.Length == 0)
        return 0;

#define APPEND_LITERAL(lit)                                                   \
    for (i = 0; i < (sizeof(lit) / sizeof(WCHAR)) - 1; i++) {                  \
        if (pos + 1 >= out_chars) return 0;                                   \
        out[pos++] = (lit)[i];                                                \
    }

    APPEND_LITERAL(prefix);

    // Значение LOCALAPPDATA.
    for (i = 0; i < var_value.Length / sizeof(WCHAR); i++) {
        if (pos + 1 >= out_chars) return 0;
        out[pos++] = var_value.Buffer[i];
    }

    APPEND_LITERAL(fwd);
    if (logs_subdir) {
        APPEND_LITERAL(logs);
    }
    if (want_file) {
        APPEND_LITERAL(file);
    }
#undef APPEND_LITERAL

    out[pos] = 0;
    return pos;
}

// Создать один каталог по NT-пути (если ещё нет). Ошибки молча игнорируем.
static void EnsureDirectory(const WCHAR *nt_path, ULONG nt_len)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    HANDLE handle = NULL;

    name.Buffer = (PWSTR)nt_path;
    name.Length = (USHORT)(nt_len * sizeof(WCHAR));
    name.MaximumLength = name.Length;
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    if (NtCreateFile(&handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attr, &iosb,
                     NULL, FILE_ATTRIBUTE_NORMAL,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                     FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                     NULL, 0) == 0) {
        NtClose(handle);
    }
}

// --- Публичная функция --------------------------------------------------------

static void LogEventImpl(const char *level, const wchar_t *event,
                         int has_number, unsigned long long number)
{
    WCHAR dir_path[600];
    WCHAR file_path[620];
    ULONG dir_len;
    ULONG file_len;

    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    HANDLE handle = NULL;

    LARGE_INTEGER system_time;
    LARGE_INTEGER local_time;
    FWD81_TIME_FIELDS tf;

    char line_storage[1024];
    Fwd81Buffer line;
    LARGE_INTEGER offset;

    ULONG event_len = 0;

    // 1. Каталоги %LOCALAPPDATA%\Fwd81 и \Fwd81\logs.
    dir_len = BuildNtPath(dir_path, 600, 0, 0);   // ...\Fwd81
    if (dir_len == 0)
        return;
    EnsureDirectory(dir_path, dir_len);

    dir_len = BuildNtPath(dir_path, 600, 0, 1);   // ...\Fwd81\logs
    if (dir_len == 0)
        return;
    EnsureDirectory(dir_path, dir_len);

    // 2. Путь к файлу журнала.
    file_len = BuildNtPath(file_path, 620, 1, 1); // ...\Fwd81\logs\fwd81.log
    if (file_len == 0)
        return;

    // 3. Сборка строки: <UTC> [level] pid=<n> <event> image=<path>\n
    if (NtQuerySystemTime(&system_time) != 0)
        return;
    if (RtlSystemTimeToLocalTime(&system_time, &local_time) != 0)
        local_time = system_time;
    RtlTimeToTimeFields(&local_time, &tf);

    BufInit(&line, line_storage, sizeof(line_storage));
    BufAppendUnsigned(&line, (unsigned long long)tf.Year, 4);
    BufAppendAsciiZ(&line, "-");
    BufAppendUnsigned(&line, (unsigned long long)tf.Month, 2);
    BufAppendAsciiZ(&line, "-");
    BufAppendUnsigned(&line, (unsigned long long)tf.Day, 2);
    BufAppendAsciiZ(&line, " ");
    BufAppendUnsigned(&line, (unsigned long long)tf.Hour, 2);
    BufAppendAsciiZ(&line, ":");
    BufAppendUnsigned(&line, (unsigned long long)tf.Minute, 2);
    BufAppendAsciiZ(&line, ":");
    BufAppendUnsigned(&line, (unsigned long long)tf.Second, 2);
    BufAppendAsciiZ(&line, " [");
    BufAppendAsciiZ(&line, level ? level : "info");
    BufAppendAsciiZ(&line, "] pid=");
    BufAppendUnsigned(&line, CurrentProcessId(), 1);
    BufAppendAsciiZ(&line, " ");
    if (event != NULL) {
        while (event[event_len] != 0)
            event_len++;
        BufAppendWide(&line, event, event_len);
    }
    if (has_number)
        BufAppendUnsigned(&line, number, 1);
    BufAppendAsciiZ(&line, " image=");
    AppendProcessImage(&line);
    BufAppendAsciiZ(&line, "\n");

    // 4. Открыть на дозапись и записать.
    name.Buffer = file_path;
    name.Length = (USHORT)(file_len * sizeof(WCHAR));
    name.MaximumLength = name.Length;
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    if (NtCreateFile(&handle, FILE_APPEND_DATA | SYNCHRONIZE, &attr, &iosb,
                     NULL, FILE_ATTRIBUTE_NORMAL,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                     FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                     NULL, 0) != 0) {
        return;
    }

    offset.LowPart = FWD81_WRITE_TO_END_LOW;
    offset.HighPart = FWD81_WRITE_TO_END_HIGH;
    NtWriteFile(handle, NULL, NULL, NULL, &iosb, line.data, line.length, &offset, NULL);
    NtClose(handle);
}

void Fwd81LogEvent(const char *level, const wchar_t *event)
{
    LogEventImpl(level, event, 0, 0);
}

void Fwd81LogEventNum(const char *level, const wchar_t *event, unsigned long long number)
{
    LogEventImpl(level, event, 1, number);
}
