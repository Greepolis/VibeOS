# convert_efi_image.cmake
# Convert a bootloader image to an EFI PE32+ application and validate headers.

if(NOT DEFINED INPUT_ELF OR INPUT_ELF STREQUAL "")
    message(FATAL_ERROR "INPUT_ELF is required")
endif()
if(NOT DEFINED OUTPUT_EFI OR OUTPUT_EFI STREQUAL "")
    message(FATAL_ERROR "OUTPUT_EFI is required")
endif()
if(NOT DEFINED OBJCOPY_TOOL OR OBJCOPY_TOOL STREQUAL "")
    message(FATAL_ERROR "OBJCOPY_TOOL is required")
endif()
if(NOT DEFINED PYTHON_EXECUTABLE OR PYTHON_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "PYTHON_EXECUTABLE is required")
endif()
if(NOT DEFINED EFI_PE_FIXUP OR EFI_PE_FIXUP STREQUAL "")
    message(FATAL_ERROR "EFI_PE_FIXUP is required")
endif()
if(NOT EXISTS "${INPUT_ELF}")
    message(FATAL_ERROR "Bootloader input image not found: ${INPUT_ELF}")
endif()
if(NOT EXISTS "${EFI_PE_FIXUP}")
    message(FATAL_ERROR "EFI PE fixup script not found: ${EFI_PE_FIXUP}")
endif()

get_filename_component(_output_dir "${OUTPUT_EFI}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_dir}")
file(REMOVE "${OUTPUT_EFI}")

set(_objcopy_keep_sections
    -j .text
    -j .data
    -j .rodata
    -j .sdata
    -j .dynamic
    -j .dynstr
    -j .dynsym
    -j .rel
    -j .rela
    -j .rel.*
    -j .rela.*
    -j .reloc
)

function(_run_objcopy _target _rc_out _log_out)
    execute_process(
        COMMAND "${OBJCOPY_TOOL}"
            ${_objcopy_keep_sections}
            "--output-target=${_target}"
            "--subsystem=10"
            "${INPUT_ELF}"
            "${OUTPUT_EFI}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    set(_log "target=${_target}\nstdout=${_stdout}\nstderr=${_stderr}")
    set(${_rc_out} "${_rc}" PARENT_SCOPE)
    set(${_log_out} "${_log}" PARENT_SCOPE)
endfunction()

function(_read_hex_lower _path _offset _limit _out)
    file(READ "${_path}" _hex HEX OFFSET ${_offset} LIMIT ${_limit})
    string(TOLOWER "${_hex}" _hex)
    set(${_out} "${_hex}" PARENT_SCOPE)
endfunction()

function(_le16_hex_to_dec _hex _out)
    string(LENGTH "${_hex}" _len)
    if(NOT _len EQUAL 4)
        message(FATAL_ERROR "Invalid 16-bit LE field length for '${_hex}'")
    endif()
    string(SUBSTRING "${_hex}" 0 2 _b0)
    string(SUBSTRING "${_hex}" 2 2 _b1)
    set(_be "${_b1}${_b0}")
    math(EXPR _dec "0x${_be}" OUTPUT_FORMAT DECIMAL)
    set(${_out} "${_dec}" PARENT_SCOPE)
endfunction()

function(_le32_hex_to_dec _hex _out)
    string(LENGTH "${_hex}" _len)
    if(NOT _len EQUAL 8)
        message(FATAL_ERROR "Invalid 32-bit LE field length for '${_hex}'")
    endif()
    string(SUBSTRING "${_hex}" 0 2 _b0)
    string(SUBSTRING "${_hex}" 2 2 _b1)
    string(SUBSTRING "${_hex}" 4 2 _b2)
    string(SUBSTRING "${_hex}" 6 2 _b3)
    set(_be "${_b3}${_b2}${_b1}${_b0}")
    math(EXPR _dec "0x${_be}" OUTPUT_FORMAT DECIMAL)
    set(${_out} "${_dec}" PARENT_SCOPE)
endfunction()

set(_conversion_ok FALSE)
set(_primary_log "")
set(_fallback_log "")
set(_tertiary_log "")

# Try primary target: efi-app-x86_64 (UEFI PE32+ format, preferred on Linux)
_run_objcopy("efi-app-x86_64" _primary_rc _primary_log)
if(_primary_rc EQUAL 0)
    set(_conversion_ok TRUE)
else()
    # Fallback 1: pei-x86-64 (Windows PE format)
    _run_objcopy("pei-x86-64" _fallback_rc _fallback_log)
    if(_fallback_rc EQUAL 0)
        set(_conversion_ok TRUE)
    else()
        # Fallback 2: pe-i386 (32-bit PE, last resort)
        _run_objcopy("pe-i386" _tertiary_rc _tertiary_log)
        if(_tertiary_rc EQUAL 0)
            set(_conversion_ok TRUE)
        endif()
    endif()
endif()

if(NOT _conversion_ok)
    message(FATAL_ERROR
        "Failed to convert bootloader to EFI application.\n"
        "Primary attempt (efi-app-x86_64):\n${_primary_log}\n"
        "Fallback attempt (pei-x86-64):\n${_fallback_log}\n"
        "Tertiary attempt (pe-i386):\n${_tertiary_log}\n"
    )
endif()

if(NOT EXISTS "${OUTPUT_EFI}")
    message(FATAL_ERROR "Objcopy reported success but output EFI file was not generated: ${OUTPUT_EFI}")
endif()

execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" "${EFI_PE_FIXUP}" "${OUTPUT_EFI}"
    RESULT_VARIABLE _fixup_rc
    OUTPUT_VARIABLE _fixup_stdout
    ERROR_VARIABLE _fixup_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT _fixup_rc EQUAL 0)
    message(FATAL_ERROR
        "EFI PE fixup/validation failed for ${OUTPUT_EFI}\n"
        "stdout=${_fixup_stdout}\n"
        "stderr=${_fixup_stderr}"
    )
endif()

file(SIZE "${OUTPUT_EFI}" _efi_size)
if(_efi_size LESS 128)
    message(FATAL_ERROR "EFI output too small to be valid: ${OUTPUT_EFI} (size=${_efi_size})")
endif()

_read_hex_lower("${OUTPUT_EFI}" 0 2 _mz_hex)
if(NOT _mz_hex STREQUAL "4d5a")
    message(FATAL_ERROR "EFI output missing MZ DOS header magic: ${OUTPUT_EFI} (magic=${_mz_hex})")
endif()

_read_hex_lower("${OUTPUT_EFI}" 60 4 _lfanew_hex)
_le32_hex_to_dec("${_lfanew_hex}" _lfanew)

if(_lfanew LESS 64)
    message(FATAL_ERROR "EFI output has invalid e_lfanew (< 64): ${OUTPUT_EFI} (e_lfanew=${_lfanew})")
endif()
math(EXPR _pe_sig_end "${_lfanew} + 4")
if(_pe_sig_end GREATER _efi_size)
    message(FATAL_ERROR "EFI output has out-of-range PE signature offset: ${OUTPUT_EFI} (e_lfanew=${_lfanew}, size=${_efi_size})")
endif()

_read_hex_lower("${OUTPUT_EFI}" ${_lfanew} 4 _pe_sig_hex)
if(NOT _pe_sig_hex STREQUAL "50450000")
    message(FATAL_ERROR "EFI output missing PE signature: ${OUTPUT_EFI} (signature=${_pe_sig_hex})")
endif()

math(EXPR _opt_magic_off "${_lfanew} + 24")
math(EXPR _opt_magic_end "${_opt_magic_off} + 2")
if(_opt_magic_end GREATER _efi_size)
    message(FATAL_ERROR "EFI output optional header is out-of-range: ${OUTPUT_EFI}")
endif()

_read_hex_lower("${OUTPUT_EFI}" ${_opt_magic_off} 2 _opt_magic_hex)
if(NOT _opt_magic_hex STREQUAL "0b02")
    message(FATAL_ERROR "EFI output is not PE32+ (expected magic 0x20b): ${OUTPUT_EFI} (magic=${_opt_magic_hex})")
endif()

math(EXPR _subsystem_off "${_opt_magic_off} + 68")
math(EXPR _subsystem_end "${_subsystem_off} + 2")
if(_subsystem_end GREATER _efi_size)
    message(FATAL_ERROR "EFI output subsystem field is out-of-range: ${OUTPUT_EFI}")
endif()

_read_hex_lower("${OUTPUT_EFI}" ${_subsystem_off} 2 _subsystem_hex)
_le16_hex_to_dec("${_subsystem_hex}" _subsystem)
if(NOT _subsystem EQUAL 10)
    message(FATAL_ERROR "EFI output subsystem is not EFI application (10): ${OUTPUT_EFI} (subsystem=${_subsystem})")
endif()

math(EXPR _characteristics_off "${_lfanew} + 22")
_read_hex_lower("${OUTPUT_EFI}" ${_characteristics_off} 2 _characteristics_hex)
_le16_hex_to_dec("${_characteristics_hex}" _characteristics)
math(EXPR _relocs_stripped "${_characteristics} & 1")
if(_relocs_stripped)
    message(FATAL_ERROR "EFI output still has RELOCS_STRIPPED set: ${OUTPUT_EFI}")
endif()

math(EXPR _reloc_dir_off "${_opt_magic_off} + 112 + (5 * 8)")
math(EXPR _reloc_size_off "${_reloc_dir_off} + 4")
_read_hex_lower("${OUTPUT_EFI}" ${_reloc_dir_off} 4 _reloc_rva_hex)
_read_hex_lower("${OUTPUT_EFI}" ${_reloc_size_off} 4 _reloc_size_hex)
_le32_hex_to_dec("${_reloc_rva_hex}" _reloc_rva)
_le32_hex_to_dec("${_reloc_size_hex}" _reloc_size)
if(_reloc_rva EQUAL 0 OR _reloc_size LESS 8)
    message(FATAL_ERROR "EFI output has no valid base relocation directory: ${OUTPUT_EFI}")
endif()

message(STATUS "Converted and validated EFI bootloader: ${OUTPUT_EFI} (${_fixup_stdout})")
