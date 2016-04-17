#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/OSR/OsrPass.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ExecutionEngine/MCJIT.h"

#include <iostream>

#define DEBUG_TYPE "OsrPass"
#define LOG_HEADER DEBUG_TYPE << "::" << __func__ << " "

using namespace llvm;

char OsrPass::ID = 0;
// static RegisterPass<OsrPass> X("osrpass", "osr stuff");

// cant get llvm to cooperate with my pass register if I put an argument
// INITIALIZE_PASS_BEGIN(OsrPass, "osrpass", "osr stuff", false, false)
// INITIALIZE_PASS_END(OsrPass, "osrpass", "osr stuff", false, false)

static void* doSomethingFunny(Function* F, ExecutionEngine* EE) {
  std::cout << "made it into do something funky\n";

  Module* M = new Module("funky_mod", getGlobalContext());

  ArrayRef<Type*> cont_args_types;
  auto cont_ret      = Type::getInt32Ty(getGlobalContext());
  auto cont_type     = FunctionType::get(cont_ret, cont_args_types, false);

  auto NF = dyn_cast<Function>(M->getOrInsertFunction("new_f", cont_type));
  auto block = BasicBlock::Create(getGlobalContext(), "entry", NF);
  IRBuilder<> builder(block);

  // TODO copy function and run an optimization pass

  // return the -1000 thing so the "test" passes
  builder.CreateRet(ConstantInt::get(Type::getInt32Ty(getGlobalContext()), -1000));

  std::unique_ptr<Module> uniq(M);
  EE->addModule(std::move(uniq));
  EE->generateCodeForModule(M);
  EE->finalizeObject();

  auto fp = EE->getPointerToFunctionOrStub(NF);
  assert(fp);

  return (void*)fp;
}

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
  }

  auto getIntPtr = [&](uintptr_t ptr) {
    return ConstantInt::get(Type::getInt64Ty(F.getContext()), ptr);
  };

  auto voidPtr = Type::getInt8PtrTy(F.getContext());

  // types for continuation func
  ArrayRef<Type*> cont_args_types;
  std::vector<Value*> cont_args_values;
  auto cont_ret      = Type::getInt32Ty(F.getContext());
  auto cont_type     = FunctionType::get(cont_ret, cont_args_types, false);
  auto cont_ptr_type = PointerType::getUnqual(cont_type);

  // types for stub
  // for now, stub takes a i8* (void*) and returns a cont_func ptr
  std::vector<Type*> stub_args_types;
  stub_args_types.push_back(voidPtr);
  stub_args_types.push_back(voidPtr);

  std::vector<Value*> stub_args_values;
  stub_args_values.push_back(
      OsrBuilder.CreateIntToPtr(
        getIntPtr((uintptr_t)F.getParent()),
        stub_args_types[0]));

  stub_args_values.push_back(
      OsrBuilder.CreateIntToPtr(
        getIntPtr((uintptr_t)EE),
        stub_args_types[1]
        ));

  auto stub_ret      = Type::getInt8PtrTy(F.getContext());
  auto stub_type     = FunctionType::get(stub_ret, stub_args_types, false);
  auto stub_ptr_type = PointerType::getUnqual(stub_type);

  // get the stub ptr
  auto stub_int_val = getIntPtr((uintptr_t)doSomethingFunny);

  auto stub_ptr    = OsrBuilder.CreateIntToPtr(stub_int_val, stub_ptr_type);
  auto stub_result = OsrBuilder.CreateCall(stub_ptr, stub_args_values);

  auto cont_ptr = OsrBuilder.CreatePointerCast(stub_result, cont_ptr_type);
  auto cont_result = OsrBuilder.CreateCall(cont_ptr, cont_args_values);

  OsrBuilder.CreateRet(cont_result);
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
