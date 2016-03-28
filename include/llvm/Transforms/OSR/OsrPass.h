#ifndef OSRPASS_H
#define OSRPASS_H

#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/Dominators.h"

#include <memory>

namespace llvm {

  // I haven't written this generally at all
  //
  // For now just trying to get a feel for what this will entail by writing
  // the inliner pass from the paper (sort of).
  //
  // Can refactor significantly later, once we have a better idea of what the
  // interface needs to be.

  // this is a module pass so that we can add new functions to the module
  // without having the pass then called on the new functions
  class OsrPass : public ModulePass {
  public:
    static char ID; // Pass identification
    OsrPass() : ModulePass(ID) { }

    bool runOnModule(Module&) override;

    inline void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<LoopInfoWrapperPass>();
      AU.addPreserved<LoopInfoWrapperPass>();
      ModulePass::getAnalysisUsage(AU);
    }

  private:

    // adds a loop counter to a loop
    // this should become it's own loop pass or something (doesn't seem to already
    // exist)
    // run the loop instrument pass, run the osr pass, run DCE
    Value* instrumentLoopWithCounters( Loop &L );

    // adds an conditional to the end of the given basic block, which will jump
    // to the osr basic block if the condition is true.
    void addOsrConditionCounterGE(Value&, uint64_t, BasicBlock&, BasicBlock&);

    bool runOnFunction(Function&);
  };
}

#endif
