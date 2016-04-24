#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/OSR/OsrPass.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/Transforms/OSR/Liveness.hpp"

#include <iostream>

#define DEBUG_TYPE "OsrPass"
#define LOG_HEADER DEBUG_TYPE << "::" << __func__ << " "

using namespace llvm;

char OsrPass::ID = 0;

ModulePass* llvm::createOsrPassPass(ExecutionEngine* EE) {
  return new OsrPass(EE);
}

INITIALIZE_PASS_BEGIN(OsrPass, "osr", "osr", false, false)
INITIALIZE_PASS_END(OsrPass, "osr", "osr", false, false)

static void* generator(Function* F, ExecutionEngine* EE,
                       Instruction* osr_point, std::set<const Value*>* vars)
{
  //Module* M = new Module("funky_mod", getGlobalContext());
  errs() << *F;
  errs() << *osr_point << "\n";

  errs() << "size: " << vars->size() << "\n";

  size_t count = 0;
  for (const auto& k : *vars) {
    errs() << *k << "\n";
    errs() << ++count << "\n\n";
  }

  errs() << "\n\nI'm free\n\n";

  return (void*)nullptr;
}

// need this to be a function pass so I can add the osr block to the module.
// if this is a function pass, the pass gets run on any modules that are added,
// so this becomes infinite loop
bool OsrPass::runOnModule( Module &M ) {
  bool flag = false;
  for (auto &F : M) {
    flag = flag || runOnFunction(F);
  }
  return flag;
}

bool OsrPass::runOnFunction( Function &F )
{
  if (F.isDeclaration()) return false;

  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();

  errs() << F << "\n\n";

  // the vars at the end of this loop's header are the relevant live variables
  // we need to get these before we start adding new things to the function
  LivenessAnalysis live(&F);
  auto relevantLiveVars = new std::set<const Value*>();
  for (auto &Loop : LI) {
    for (auto var : live.getLiveOutValues(Loop->getHeader())) {
      relevantLiveVars->insert(var);
      errs() << "adding: " << *var << "\n";
    }
  }

  errs() << "presize: " << relevantLiveVars->size() << "\n";
  errs() << F << "\n\n";

  // now that we have the important information from the original function, we
  // are going to start fiddling around with it

  // TODO will need an osr block for each loop in the function
  BasicBlock* osrBB = BasicBlock::Create(getGlobalContext(), "osr", &F);
  IRBuilder<> OsrBuilder(osrBB);

  Instruction* cond = nullptr;
  for (auto &Loop : LI) {
    Value* counter = instrumentLoopWithCounters(*Loop);
    cond = addOsrConditionCounterGE(*counter, 1000, *Loop->getHeader(), *osrBB);
  }
  errs() << F << "\n\n";

  if (!cond) return false;

  // create the osr stub code
  auto getIntPtr = [&](uintptr_t ptr) {
    return ConstantInt::get(Type::getInt64Ty(F.getContext()), ptr);
  };
  auto voidPtr = Type::getInt8PtrTy(F.getContext());

  // now we need to get an fp to a new function which takes all of our live vars
  // as args and returns the same thing as the original function.
  // we return the result of a call to this function

  // types for continuation func
  std::vector<Type*> cont_args_types;
  for (auto v : *relevantLiveVars) {
    cont_args_types.push_back(v->getType());
  }

  std::vector<Value*> cont_args_values;
  for (auto v : *relevantLiveVars) {
    cont_args_values.push_back(const_cast<Value*>(v));
  }

  auto cont_ret      = Type::getInt32Ty(F.getContext());
  auto cont_type     = FunctionType::get(cont_ret, cont_args_types, false);
  auto cont_ptr_type = PointerType::getUnqual(cont_type);

  std::vector<Type*> stub_args_types;
  stub_args_types.push_back(voidPtr);
  stub_args_types.push_back(voidPtr);
  stub_args_types.push_back(voidPtr);
  stub_args_types.push_back(voidPtr);

  std::vector<Value*> stub_args_values;
  stub_args_values.push_back(
      OsrBuilder.CreateIntToPtr(
        getIntPtr((uintptr_t)&F),
        stub_args_types[0]));

  stub_args_values.push_back(
      OsrBuilder.CreateIntToPtr(
        getIntPtr((uintptr_t)EE),
        stub_args_types[1]
        ));

  stub_args_values.push_back(
      OsrBuilder.CreateIntToPtr(
        getIntPtr((uintptr_t)cond),
        stub_args_types[1]
        ));

  stub_args_values.push_back(
      OsrBuilder.CreateIntToPtr(
        getIntPtr((uintptr_t)relevantLiveVars),
        stub_args_types[1]
        ));

  auto stub_ret      = Type::getInt8PtrTy(F.getContext());
  auto stub_type     = FunctionType::get(stub_ret, stub_args_types, false);
  auto stub_ptr_type = PointerType::getUnqual(stub_type);

  auto stub_int_val = getIntPtr((uintptr_t)generator);
  auto stub_ptr    = OsrBuilder.CreateIntToPtr(stub_int_val, stub_ptr_type);
  auto stub_result = OsrBuilder.CreateCall(stub_ptr, stub_args_values);

  auto cont_ptr = OsrBuilder.CreatePointerCast(stub_result, cont_ptr_type);
  auto cont_result = OsrBuilder.CreateCall(cont_ptr, cont_args_values);

  errs() << F << "\n\n";
  OsrBuilder.CreateRet(cont_result);
  return true;
}

Instruction* OsrPass::addOsrConditionCounterGE(Value &counter,
                                               uint64_t limit,
                                               BasicBlock &BB,
                                               BasicBlock &OsrBB)
{
  BasicBlock::iterator insertPoint = BB.getFirstInsertionPt();
  auto newBB = BB.splitBasicBlock(insertPoint, "loop.cont");

  IRBuilder<> Builder(&BB);
  auto term = BB.getTerminator();
  term->eraseFromParent();

  auto osr_cond = Builder.CreateICmpSGE(&counter, Builder.getInt64(limit), "osr.cond");
  return Builder.CreateCondBr(osr_cond, &OsrBB, newBB);
}

Value* OsrPass::instrumentLoopWithCounters( Loop &L )
{
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
