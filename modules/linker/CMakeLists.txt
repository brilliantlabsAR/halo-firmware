# Alif SDK Linker Module

if(CONFIG_ALIF_LINKER_WORKAROUNDS)
  if(CONFIG_ALIF_DISCARD_EXCEPTION_TABLES)
    # Add linker script to discard exception tables
    zephyr_linker_sources(SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/discard_exception_tables.ld)
  endif()

  if(CONFIG_ALIF_FIX_SYMBOL_WARNINGS)
    # Add linker script to fix symbol table placement warnings
    zephyr_linker_sources(SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/symbol_tables.ld)
  endif()
endif()
