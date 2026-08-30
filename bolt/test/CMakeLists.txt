llvm_canonicalize_cmake_booleans(
  BOLT_ENABLE_RUNTIME
  )

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
  )
configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/Unit/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.cfg.py
  )

set(BOLT_TEST_PARAMS
  bolt_site_config=${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg
  )

if (BOLT_CLANG_EXE AND NOT "clang" IN_LIST LLVM_ENABLE_PROJECTS)
  add_executable(clang IMPORTED GLOBAL)
  set_property(TARGET clang PROPERTY IMPORTED_LOCATION "${BOLT_CLANG_EXE}")
endif()

if (BOLT_LLD_EXE AND NOT "lld" IN_LIST LLVM_ENABLE_PROJECTS)
  add_executable(lld IMPORTED GLOBAL)
  set_property(TARGET lld PROPERTY IMPORTED_LOCATION "${BOLT_LLD_EXE}")
endif()

list(APPEND BOLT_TEST_DEPS
  clang
  count
  FileCheck
  llc
  lld
  llvm-config
  llvm-bolt
  llvm-bolt-heatmap
  llvm-bat-dump
  llvm-dwarfdump
  llvm-dwp
  llvm-mc
  llvm-nm
  llvm-objdump
  llvm-readelf
  llvm-readobj
  llvm-strings
  llvm-strip
  llvm-objcopy
  merge-fdata
  not
  split-file
  yaml2obj
  )

add_custom_target(bolt-test-depends DEPENDS ${BOLT_TEST_DEPS})
set_target_properties(bolt-test-depends PROPERTIES FOLDER "BOLT")

# Go compiler detection for golang tests
find_program(BOLT_GC_EXE NAMES go)
if(BOLT_GC_EXE)
  execute_process(COMMAND ${BOLT_GC_EXE} version
    OUTPUT_VARIABLE BOLT_GC_VERSION_OUTPUT
    ERROR_QUIET)
  if(BOLT_GC_VERSION_OUTPUT MATCHES "go([0-9]+\\.[0-9]+)")
    set(BOLT_GC_VERSION "${CMAKE_MATCH_1}")
  endif()
  set(BOLT_FOUND_GC TRUE)
  message(STATUS "Found Go compiler: ${BOLT_GC_EXE} version ${BOLT_GC_VERSION}")
else()
  set(BOLT_FOUND_GC FALSE)
  set(BOLT_GC_VERSION "not-found")
  message(STATUS "Go compiler not found - golang tests will be skipped")
endif()

# Cache variables for lit.site.cfg.py.in
set(GO_EXECUTABLE "${BOLT_GC_EXE}" CACHE PATH "Go compiler executable")
set(BOLT_GC_EXE "${BOLT_GC_EXE}" CACHE PATH "BOLT Go compiler path")
set(BOLT_GC_VERSION "${BOLT_GC_VERSION}" CACHE STRING "BOLT Go compiler version")

add_lit_testsuite(check-bolt "Running the BOLT regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  PARAMS ${BOLT_TEST_PARAMS}
  DEPENDS ${BOLT_TEST_DEPS}
  ARGS ${BOLT_TEST_EXTRA_ARGS}
  )
set_target_properties(check-bolt PROPERTIES FOLDER "BOLT")

add_lit_testsuites(BOLT ${CMAKE_CURRENT_SOURCE_DIR}
  PARAMS ${BOLT_TEST_PARAMS}
  DEPENDS ${BOLT_TEST_DEPS}
)
