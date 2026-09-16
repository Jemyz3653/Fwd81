// SPDX-License-Identifier: MIT
//
// Внедрение ядра через реестр Image File Execution Options (IFEO).
//
// Способ из VxKex: в ветке IFEO для конкретной программы прописываются
// GlobalFlag с флагом Application Verifier (0x100) и VerifierDlls =
// fwd81core.dll. Загрузчик Windows при старте такой программы подгружает наш
// файл раньше, чем она начнёт искать функции. Файл-поставщик обязан лежать в
// System32 — таково требование механизма (полные пути не поддерживаются),
// поэтому enable ещё и копирует туда fwd81core.dll. Обоснование и правило про
// System32 — в docs/architecture.md (раздел 5) и docs/licensing.md.
//
// Все операции, меняющие систему, требуют прав администратора. Режим dry_run
// печатает план и НИЧЕГО не пишет.

#ifndef FWD81_IFEO_H
#define FWD81_IFEO_H

// Коды возврата команд (совпадают с кодами fwd81cli).
#define FWD81_IFEO_OK        0
#define FWD81_IFEO_USAGE     1
#define FWD81_IFEO_NEED_ADMIN 6
#define FWD81_IFEO_FAILED    7

// Включить Fwd81 для программы image_name (имя exe, напр. "app.exe").
int Fwd81Enable(const wchar_t *image_name, int dry_run);

// Выключить Fwd81 для программы image_name (снять наши значения из IFEO).
int Fwd81Disable(const wchar_t *image_name, int dry_run);

// Полное удаление: снять наши значения из всех программ IFEO и убрать
// fwd81core.dll из System32.
int Fwd81Uninstall(int dry_run);

// Показать журнал работы ядра.
int Fwd81ShowLog(void);

#endif // FWD81_IFEO_H
