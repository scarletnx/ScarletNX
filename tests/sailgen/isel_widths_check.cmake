# sailgen_isel_widths: --isel over more primitives than the 64 of 4 widths its evaluations once had room for (SAILGEN,
# DIR, OUT): 65 additions at 8, 16, 32 and 64 bits, each in the report. Past that room the evaluations were written
# beyond their block, which the sanitizer build stops on
set(ops "")
foreach(i RANGE 1 65)
    string(APPEND ops "op add${i} 8 16 32 64 (A, B)\nRESULT := A + B;\nend\n\n")
endforeach()
file(MAKE_DIRECTORY "${OUT}")
file(WRITE "${OUT}/widths.ops" "${ops}")
execute_process(COMMAND "${SAILGEN}" --isel "${DIR}/isel_fixture.pages" --ops "${OUT}/widths.ops" -o "${OUT}/widths.inc"
                --report "${OUT}/widths.report" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sailgen_isel_widths: snx-sailgen exited ${rc} on 65 primitives of 4 widths")
endif()
file(STRINGS "${OUT}/widths.report" headers REGEX "^add[0-9]+ (8|16|32|64):$")
list(LENGTH headers n)
message("sailgen_isel_widths: ${n} evaluations")
