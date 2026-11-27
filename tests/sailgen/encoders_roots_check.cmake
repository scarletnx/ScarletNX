# sailgen_encoders_roots: --encoders --roots (SAILGEN, DIR the in-tree A64 Sail's, OUT): a roots file of two decode
# functions gives the encoders of the clauses that call them, and those alone
file(MAKE_DIRECTORY "${OUT}")
file(WRITE "${OUT}/two.roots"
     "root decode_add_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_simd\n"
     "root decode_eor_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_logical_bsl_eor\n")
execute_process(COMMAND "${SAILGEN}" --encoders __DecodeA64 --roots "${OUT}/two.roots" -o "${OUT}/two_enc.inc"
                        "${DIR}/a64.sail" RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sailgen_encoders_roots: snx-sailgen exited ${rc} on two roots: ${err}")
endif()
file(STRINGS "${OUT}/two_enc.inc" encoders REGEX "^static inline SnxU32 sge_a64_")
set(names "")
foreach(line IN LISTS encoders)
    string(REGEX REPLACE "^static inline SnxU32 (sge_a64_[a-z0-9_]+)\\(.*" "\\1" name "${line}")
    string(APPEND names " ${name}")
endforeach()
list(LENGTH encoders n)
message("sailgen_encoders_roots: ${n} encoders:${names}")
