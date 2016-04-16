add_llvm_library(LLVMOsr
  OsrPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/OSR
  )
