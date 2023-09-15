add_llvm_library(LLVMBOLTUtils
  CommandLineOpts.cpp
  Utils.cpp

  DISABLE_LLVM_LINK_LLVM_DYLIB

  LINK_LIBS
  ${LLVM_PTHREAD_LIB}

  DEPENDS
  llvm_vcsrevision_h

  LINK_COMPONENTS
  Support
  )

# get the latest git tag (fall back to an empty tag when configuring from
# a release tarball without a .git directory)
execute_process(
  COMMAND git describe --tags --dirty=.dirty --always
  WORKING_DIRECTORY ${LLVM_MAIN_SRC_DIR}
  RESULT_VARIABLE GIT_DESCRIBE_RESULT
  ERROR_QUIET
  OUTPUT_VARIABLE GIT_TAG_ENC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT GIT_DESCRIBE_RESULT EQUAL 0)
  set(GIT_TAG_ENC "")
endif()

# parse tag into string
string(STRIP "${GIT_TAG_ENC}" GIT_TAG)

# write the latest tag to a text file
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BoltRevision.inc "\"${GIT_TAG}\"")