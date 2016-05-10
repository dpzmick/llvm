#include "llvm/Transforms/OSR/MCJITWrapper.h"

using namespace llvm;

void MCJITWrapper::addModule(std::unique_ptr<Module> M) {
  Module* borrow = M.get();
  EE->addModule(std::move(M));

  EE->generateCodeForModule(borrow);
  EE->finalizeObject();

  // force the compile so that we actually have pointers

  for (auto& F : *borrow) {
    auto addr = (void*)EE->getFunctionAddress(F.getName());
    mapping[addr] = &F;
  }

  modules.push_back(borrow);
}

Function* MCJITWrapper::getFunctionForPtr(void* ptr) {
  return mapping[ptr];
}

void MCJITWrapper::rebuildMapping() {
  mapping.clear();

  for (auto* m : modules) {
    for (auto& F : *m) {
      auto addr = (void*)EE->getFunctionAddress(F.getName());
      mapping[addr] = &F;
    }
  }
}
