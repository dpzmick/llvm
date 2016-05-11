#ifndef MCJITWRAPPER_H
#define MCJITWRAPPER_H

#include "llvm/ExecutionEngine/ExecutionEngine.h"

namespace llvm {

/**
 * A helper for an execution engine which does some handy things
 * Does not own the EE
 *
 * Only use this to add things to the EE + query for pointers to things
 */
class MCJITWrapper {
  public:
    inline MCJITWrapper(ExecutionEngine* jit) : EE(jit) { }
    void addModule(std::unique_ptr<Module>);
    Function* getFunctionForPtr(void*);
    inline void* getPointerToFunction(Function* f) {
      return EE->getPointerToFunction(f);
    }

  private:
    ExecutionEngine* EE;
    std::map<void*, Function*> mapping;
};
}

#endif
