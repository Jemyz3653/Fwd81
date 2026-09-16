// SPDX-License-Identifier: MIT
//
// Контрольный пример M3 (отказ №3): программа статически импортирует из настоящей
// kernel32.dll функцию Fwd81AbsentProcedure, которой там нет. Библиотека найдётся,
// а функция — нет: загрузчик при старте вернёт STATUS_ENTRYPOINT_NOT_FOUND и не
// даст дойти до main. Это ровно «Точка входа X не найдена в библиотеке DLL Y»,
// и снова через статический импорт — тот путь, что и у настоящих программ.
//
// Ожидаемое поведение:
//   * без Fwd81 — падение при загрузке, main НЕ достигается;
//   * с Fwd81 — перехват LdrGetProcedureAddress подставит адрес заглушки,
//     процесс дойдёт до main и напечатает маркер.

#include <stdio.h>

__declspec(dllimport) int Fwd81AbsentProcedure(void);

int main(void)
{
    volatile int r = Fwd81AbsentProcedure();  // ссылка удерживает статический импорт
    printf("SYNTHETIC-REACHED-MAIN absent_proc r=%d\n", r);
    fflush(stdout);
    return 0;
}
