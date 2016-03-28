#include "OsrPass.h"
#include "Liveness.hpp"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"

#include <iostream>

using namespace llvm;

char OsrPass::ID = 0;
static RegisterPass<OsrPass> X("osrpass", "osr stuff");

#define DEBUG_TYPE "OsrPass"
#define LOG_HEADER DEBUG_TYPE << "::" << __func__ << " "

void OsrPass::addOsrConditionCounterGE(Value &counter,
                                       uint64_t limit,
                                       BasicBlock &BB,
                                       BasicBlock &OsrBB)
{
  DEBUG(errs() << LOG_HEADER << "entered\n");

  // insert new osr condition instructions after this
  BasicBlock::iterator insertPoint = BB.getFirstInsertionPt();

  DEBUG(errs() << LOG_HEADER << "splitting loop at " << *insertPoint << "\n");
  auto newBB = BB.splitBasicBlock(insertPoint, "loop.cont");

  // start building the osr condition
  IRBuilder<> Builder(&BB);
  auto term = BB.getTerminator();
  term->eraseFromParent();

  auto osr_cond = Builder.CreateICmpSGE(&counter, Builder.getInt64(limit), "osr.cond");
  Builder.CreateCondBr(osr_cond, &OsrBB, newBB);
}

// need this to be a function pass so I can add the osr block
bool OsrPass::runOnModule( Module &M )
{
  DEBUG(errs() << LOG_HEADER << "entered\n");

  for (auto &F : M) {
    runOnFunction(F);
  }

  return true;
}

bool OsrPass::runOnFunction( Function &F )
{
  DEBUG(errs() << LOG_HEADER << "entered\n");

  if (F.isDeclaration()) return false;

  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();

  BasicBlock* osrBB = BasicBlock::Create(getGlobalContext(), "osr", &F);
  IRBuilder<> OsrBuilder(osrBB);

  for (auto &Loop : LI) {
    Value * counter = instrumentLoopWithCounters(*Loop);
    addOsrConditionCounterGE(*counter, 1000, *Loop->getHeader(), *osrBB);

    // TODO set up osr + function call here
    // I've had a ton of trouble with this
    //
    // a simple test with resolved osr:
    // - create a version of the function at compile time, pass it through a
    //   bunch of opt passes. (copy the function, modify it to pick up where the
    //   loop would have left off (change args))
    // - optimize the new function heavily
    // - emit both of these functions in the module
    // - at run time, when the counter is tripped, switch to the optimized
    //   version
    //
    // This is essentially resolved osr. Doesn't really do anything useful but
    // it will at least help to establish the machinery we need
  }

  // TODO return whatever we get from the osr call
  OsrBuilder.CreateRet(OsrBuilder.getInt32(-1000));
  return true;
}

Value* OsrPass::instrumentLoopWithCounters( Loop &L )
{
  DEBUG(errs() << LOG_HEADER << "entered\n");

  // TODO try to use getCanonicalInductionVariable
  // maybe just return that if this works

  auto header = L.getHeader();
  BasicBlock::iterator it = header->begin();

  auto phi = PHINode::Create(Type::getInt64Ty(header->getContext()), 2, "loop_counter", &*it);

  SmallVector<BasicBlock*, 16> ebs;
  L.getLoopLatches(ebs);

  for (auto &exit_b : ebs ) {
    IRBuilder<> B(exit_b);

    TerminatorInst* oldTerm = exit_b->getTerminator();
    oldTerm->removeFromParent();

    // increment the counter
    auto v = B.CreateAdd(phi, B.getInt64(1), "new_loop_counter");
    phi->addIncoming(v, exit_b);

    B.Insert(oldTerm);
  }

  for (auto it = pred_begin(header), et = pred_end(header); it != et; ++it) {
    // TODO this is slow
    bool found = false;
    for (auto &p : ebs) {
      if (p == *it)
        found = true;
    }

    if (found)
      continue;

    auto v = ConstantInt::get(Type::getInt64Ty(header->getContext()), 0);
    phi->addIncoming(v, *it);
  }

  return phi;
}
