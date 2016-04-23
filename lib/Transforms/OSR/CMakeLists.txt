add_llvm_library(LLVMOsr
  OsrPass.cpp
  Liveness.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/OSR
  )
