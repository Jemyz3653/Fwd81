// SPDX-License-Identifier: MIT
//
// Контрольный пример для пути delay-load — тот случай, который `fwd81cli run`
// (внедрение без реестра) в принципе МОЖЕТ починить, в отличие от статических
// импортов.
//
// Отличие от synthetic_absent_dll: там импорт статический, и процесс падает при
// загрузке (до main). Здесь та же fwd81_absent.dll подключена как ОТЛОЖЕННАЯ
// (/DELAYLOAD:fwd81_absent.dll). Значит загрузка DLL происходит не при старте
// процесса, а при ПЕРВОМ вызове функции — уже из работающего процесса. Поэтому:
//   * без Fwd81 — процесс доходит до main, печатает before-call, а на вызове
//     ловит отказ загрузки (delay-load помощник зовёт LoadLibrary и не находит
//     DLL) → печатает FAILED;
//   * с Fwd81 (run или IFEO) — перехват LdrLoadDll/LoadLibrary подставит DLL,
//     вызов пройдёт → печатает OK.

#include <stdio.h>
#include <windows.h>

__declspec(dllimport) int Fwd81AbsentFunction(void);

int main(void)
{
    printf("SYNTHETIC-REACHED-MAIN delay before-call\n");
    fflush(stdout);

    __try {
        int r = Fwd81AbsentFunction();  // здесь срабатывает отложенная загрузка
        printf("SYNTHETIC-DELAY-CALL-OK r=%d\n", r);
        fflush(stdout);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("SYNTHETIC-DELAY-CALL-FAILED code=0x%08lX\n",
               (unsigned long)GetExceptionCode());
        fflush(stdout);
    }
    return 0;
}
