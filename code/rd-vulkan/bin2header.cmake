# Embeds a compiled SPIR-V binary as a C uint32_t array, so the renderer
# doesn't need to ship/find loose .spv files at runtime. Portable (CMake-only,
# no xxd/python dependency) - SPIR-V is always a stream of 32-bit words, so
# hex bytes are simply grouped 8-at-a-time (little-endian) into each element.
#
# Expects: INPUT_FILE, OUTPUT_FILE, VAR_NAME

file(READ "${INPUT_FILE}" hex_content HEX)
string(LENGTH "${hex_content}" hex_length)
math(EXPR word_count "${hex_length} / 8")

set(output "// Generated at build time from ${INPUT_FILE} - do not edit.\n")
string(APPEND output "static const uint32_t ${VAR_NAME}[] = {\n")

foreach(i RANGE 0 ${word_count})
	if(i EQUAL ${word_count})
		break()
	endif()
	math(EXPR byte_offset "${i} * 8")
	string(SUBSTRING "${hex_content}" ${byte_offset} 2 b0)
	math(EXPR byte_offset "${byte_offset} + 2")
	string(SUBSTRING "${hex_content}" ${byte_offset} 2 b1)
	math(EXPR byte_offset "${byte_offset} + 2")
	string(SUBSTRING "${hex_content}" ${byte_offset} 2 b2)
	math(EXPR byte_offset "${byte_offset} + 2")
	string(SUBSTRING "${hex_content}" ${byte_offset} 2 b3)
	string(APPEND output "0x${b3}${b2}${b1}${b0},\n")
endforeach()

string(APPEND output "};\n")

file(WRITE "${OUTPUT_FILE}" "${output}")
