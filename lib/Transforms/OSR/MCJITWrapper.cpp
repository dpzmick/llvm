#include "llvm/Transforms/OSR/MCJITWrapper.h"

using namespace llvm;

void MCJITWrapper::addModule(std::unique_ptr<Module> M) {
  Module* borrow = M.get();
  EE->addModule(std::move(M));

  EE->generateCodeForModule(borrow);
  EE->finalizeObject();

  // force the compile so that we actually have pointers

  for (auto& F : *borrow) {
    if (F.isDeclaration()) continue;

    auto addr = (void*)EE->getFunctionAddress(F.getName());
    mapping[addr] = &F;
  }
}

Function* MCJITWrapper::getFunctionForPtr(void* ptr) {
  auto el = mapping.find(ptr);
  if (el == mapping.end())
    return nullptr;

  return el->second;
}
