# SPDX-License-Identifier: LGPL-2.1-or-later
#
# fwd81core.dll — ядро внедрения. Собирается БЕЗ библиотеки языка C.
#
# Почему без неё: ядро попадает в процесс раньше, чем в этом процессе вообще
# успевает завестись библиотека языка C. Любой её вызов на этом этапе — падение.
# Отсюда весь набор флагов ниже; каждый снимает одну зависимость от CRT.
#
# Соблюдение правила проверяется не на словах: tools/check_image.py
# читает собранную DLL и падает, если в её таблице импорта появилась любая
# библиотека кроме ntdll.dll. Проверка запускается в build.ps1 и в CI.

# /RTC1 (проверки времени выполнения) из конфигурации Debug зовут функции CRT.
# Переменная переопределяется только внутри этого каталога, cli/diag не задеты.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

add_library(fwd81core SHARED
    fwd81core.c
    fwd81log.c
    fwd81_mem.c)

# Пустое значение = не добавлять флаг выбора библиотеки языка C (/MD, /MDd, /MT).
set_target_properties(fwd81core PROPERTIES MSVC_RUNTIME_LIBRARY "")

target_compile_options(fwd81core PRIVATE
    /GS-        # защита стека зовёт __security_check_cookie из CRT
    /Gs1048576  # порог, с которого функция зовёт __chkstk — тоже из CRT
    /Zl)        # не записывать в объектный файл имена библиотек по умолчанию

target_link_options(fwd81core PRIVATE
    /NODEFAULTLIB           # не подставлять libcmt/msvcrt
    /ENTRY:Fwd81CoreEntry   # своя точка входа вместо _DllMainCRTStartup
    /INCREMENTAL:NO         # добавочная компоновка создаёт переходники со ссылками на CRT
    /DYNAMICBASE /NXCOMPAT)

# Единственная разрешённая зависимость.
target_link_libraries(fwd81core PRIVATE ntdll)
