# pico_link_at_slot(<target> <slot index>)
#
# Links a game somewhere other than the base of flash, so the launcher can own
# the base and every bundled game can sit at an address of its own. See
# LAUNCHER.md for the map and the boot handoff.
#
# The RP2040 executes in place and a game is linked to absolute addresses, so
# two games cannot both live where they were linked. Giving each one its own
# slot is what makes a bundle possible without relocation, which the pico port
# does not implement.
#
# The linker script is the SDK's own, transformed at configure time rather than
# copied into this repo. memmap_default.ld gets its FLASH line from a generated
# `pico_flash_region.ld` one liner; we substitute that one line for the slot's
# origin and length and leave the other 200 lines of the SDK's script alone, so
# an SDK upgrade cannot leave a stale duplicate behind here.

include_guard(GLOBAL)

# Slot 0 is the launcher at the base of flash. Games start at slot 1. Sizes are
# in bytes; a game is about 135 KB today, so 512 KB a slot is roughly four
# times headroom, and 23 slots fit under the 12 MB the SDK leaves us.
set(PICO_SLOT_BASE 0x10000000 CACHE INTERNAL "flash base")
set(PICO_SLOT_SIZE 524288 CACHE INTERNAL "bytes per slot")
set(PICO_SLOT_COUNT 23 CACHE INTERNAL "slots for games, launcher excluded")

function(pico_slot_address SLOT OUT_VAR)
    math(EXPR address "${PICO_SLOT_BASE} + ${SLOT} * ${PICO_SLOT_SIZE}"
         OUTPUT_FORMAT HEXADECIMAL)
    set(${OUT_VAR} ${address} PARENT_SCOPE)
endfunction()

function(pico_link_at_slot TARGET SLOT)
    if(SLOT LESS 1 OR SLOT GREATER ${PICO_SLOT_COUNT})
        message(FATAL_ERROR
            "slot ${SLOT} is outside 1..${PICO_SLOT_COUNT}")
    endif()

    pico_slot_address(${SLOT} origin)

    set(source ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2040/memmap_default.ld)
    if(NOT EXISTS ${source})
        message(FATAL_ERROR "no SDK linker script at ${source}")
    endif()

    set(generated ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_slot${SLOT}.ld)
    file(READ ${source} script)

    # The line we are here to replace. If the SDK ever stops generating its
    # flash region this way, fail loudly rather than link a game at the base
    # of flash and let the launcher overwrite itself at runtime.
    if(NOT script MATCHES "INCLUDE \"pico_flash_region.ld\"")
        message(FATAL_ERROR
            "${source} no longer includes pico_flash_region.ld; the slot "
            "linker script needs updating for this SDK version")
    endif()
    string(REPLACE "INCLUDE \"pico_flash_region.ld\""
           "FLASH(rx) : ORIGIN = ${origin}, LENGTH = ${PICO_SLOT_SIZE}"
           script "${script}")
    file(WRITE ${generated} "${script}")

    pico_set_linker_script(${TARGET} ${generated})
    message(STATUS "${TARGET}: slot ${SLOT} at ${origin}")
endfunction()
