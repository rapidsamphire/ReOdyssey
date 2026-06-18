if(NOT DEFINED INPUT_FILE)
    message(FATAL_ERROR "INPUT_FILE is required")
endif()

if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

if(NOT DEFINED ARRAY_NAME)
    message(FATAL_ERROR "ARRAY_NAME is required")
endif()

file(READ "${INPUT_FILE}" input_hex HEX)
string(LENGTH "${input_hex}" input_hex_length)
math(EXPR input_size "${input_hex_length} / 2")

set(output_text "// Auto-generated from ${INPUT_FILE}; do not edit.\n\n")
string(APPEND output_text "#pragma once\n\n")
string(APPEND output_text "#include <cstddef>\n\n")
string(APPEND output_text "inline constexpr unsigned char ${ARRAY_NAME}[] = {")

set(offset 0)
set(byte_index 0)
while(offset LESS input_hex_length)
    math(EXPR line_index "${byte_index} % 12")
    if(line_index EQUAL 0)
        string(APPEND output_text "\n    ")
    endif()

    string(SUBSTRING "${input_hex}" "${offset}" 2 byte_hex)
    string(TOUPPER "${byte_hex}" byte_hex)
    string(APPEND output_text "0x${byte_hex},")

    math(EXPR byte_index "${byte_index} + 1")
    math(EXPR offset "${offset} + 2")

    math(EXPR line_index "${byte_index} % 12")
    if(NOT line_index EQUAL 0)
        string(APPEND output_text " ")
    endif()
endwhile()

string(APPEND output_text "\n};\n\n")
string(APPEND output_text "inline constexpr std::size_t ${ARRAY_NAME}_size = ${input_size};\n")

get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT_FILE}" "${output_text}")
