# SPDX-License-Identifier: MIT
#
# Контрольный пример вехи M3. Две программы, которые НАРОЧНО не запускаются без
# Fwd81 — ровно тем путём, каким загрузчик разрешает зависимости настоящих
# программ (статический импорт), а не искусственным LoadLibrary в рантайме.
#
# Хитрость сборки: импорт из несуществующей DLL нельзя слинковать напрямую, но
# можно через импорт-библиотеку, сгенерированную lib.exe из .def. Она
# удовлетворяет символ на этапе линковки; провал происходит при ЗАГРУЗКЕ — как у
# настоящей программы.

find_program(FWD81_LIB_EXE NAMES lib.exe lib REQUIRED)

# Сгенерировать импорт-библиотеку из .def.
function(fwd81_import_lib def_name out_lib)
    add_custom_command(
        OUTPUT "${out_lib}"
        COMMAND "${FWD81_LIB_EXE}" /nologo
                "/def:${CMAKE_CURRENT_SOURCE_DIR}/${def_name}"
                "/out:${out_lib}" /machine:x64
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${def_name}"
        COMMENT "Импорт-библиотека из ${def_name}"
        VERBATIM)
endfunction()

set(ABSENT_DLL_LIB  "${CMAKE_CURRENT_BINARY_DIR}/absent_dll.lib")
set(ABSENT_PROC_LIB "${CMAKE_CURRENT_BINARY_DIR}/absent_proc.lib")
fwd81_import_lib(absent_dll.def  "${ABSENT_DLL_LIB}")
fwd81_import_lib(absent_proc.def "${ABSENT_PROC_LIB}")
add_custom_target(synthetic_import_libs DEPENDS "${ABSENT_DLL_LIB}" "${ABSENT_PROC_LIB}")

function(fwd81_add_synthetic name source import_lib)
    add_executable(${name} ${source})
    add_dependencies(${name} synthetic_import_libs)
    target_link_libraries(${name} PRIVATE "${import_lib}")
    set_target_properties(${name} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    fwd81_target_runs_on_81(${name} CONSOLE)
endfunction()

fwd81_add_synthetic(synthetic_absent_dll  synthetic_absent_dll.c  "${ABSENT_DLL_LIB}")
fwd81_add_synthetic(synthetic_absent_proc synthetic_absent_proc.c "${ABSENT_PROC_LIB}")

# Третья программа — отложенная загрузка той же несуществующей DLL. Отличается
# только флагами компоновки: та же импорт-библиотека, но DLL помечена как
# delay-load, поэтому её загрузка откладывается до первого вызова функции.
fwd81_add_synthetic(synthetic_delay_absent synthetic_delay_absent.c "${ABSENT_DLL_LIB}")
target_link_libraries(synthetic_delay_absent PRIVATE delayimp)
target_link_options(synthetic_delay_absent PRIVATE "/DELAYLOAD:fwd81_absent.dll")
