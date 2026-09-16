// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Собственные memset/memcpy для ядра.
//
// Ядро собирается с /NODEFAULTLIB, то есть без библиотеки языка C. Но
// оптимизатор MSVC сам по себе превращает циклы вида «заполнить массив одним
// значением» и «скопировать массив» в вызовы memset/memcpy — и без этих
// символов линковка падает (LNK2019). Поэтому предоставляем их сами.
//
// #pragma function(memset, memcpy) обязателен: он запрещает компилятору
// сворачивать тело самих этих функций в вызов их же встроенной версии, иначе
// получили бы бесконечную рекурсию.

#include <stddef.h>

#pragma function(memset, memcpy)

void *memset(void *dest, int value, size_t count)
{
    unsigned char *d = (unsigned char *)dest;
    while (count--)
        *d++ = (unsigned char)value;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (count--)
        *d++ = *s++;
    return dest;
}
