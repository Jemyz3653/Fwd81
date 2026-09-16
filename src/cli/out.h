// SPDX-License-Identifier: MIT
//
// Вывод текста в UTF-8 для утилит Fwd81. Общий для fwd81cli и его модулей.

#ifndef FWD81_OUT_H
#define FWD81_OUT_H

// Печатает строку UTF-16 в стандартный вывод. На настоящей консоли — через
// WriteConsoleW; при перенаправлении в файл — конвертируя в UTF-8, чтобы
// русский текст не превращался в «кракозябры».
void OutText(const wchar_t *text);

// То же плюс перевод строки.
void OutLine(const wchar_t *text);

#endif // FWD81_OUT_H
