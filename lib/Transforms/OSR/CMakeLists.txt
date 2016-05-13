set(LLVM_LINK_COMPONENTS
    ipo
  )

add_llvm_library(LLVMOsr
  OsrPass.cpp
  Liveness.cpp
  MCJITWrapper.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/OSR
  )

add_dependencies(LLVMOsr intrinsics_gen)
