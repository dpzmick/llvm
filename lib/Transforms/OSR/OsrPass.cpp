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
// static RegisterPass<OsrPass> X("osrpass", "osr stuff");

// cant get llvm to cooperate with my pass register if I put an argument
// INITIALIZE_PASS_BEGIN(OsrPass, "osrpass", "osr stuff", false, false)
// INITIALIZE_PASS_END(OsrPass, "osrpass", "osr stuff", false, false)
//
static Function *printf_prototype(LLVMContext &ctx, Module *mod) {
  FunctionType *printf_type =
      TypeBuilder<int(char *, ...), false>::get(getGlobalContext());

  Function *func = cast<Function>(mod->getOrInsertFunction(
      "printf", printf_type,
      AttributeSet().addAttribute(mod->getContext(), 1U, Attribute::NoAlias)));

  return func;
}

Constant* geti8StrVal(Module& M, char const* str, Twine const& name) {
  LLVMContext& ctx = getGlobalContext();
  Constant* strConstant = ConstantDataArray::getString(ctx, str);
  GlobalVariable* GVStr =
      new GlobalVariable(M, strConstant->getType(), true,
                         GlobalValue::InternalLinkage, strConstant, name);
  Constant* zero = Constant::getNullValue(IntegerType::getInt32Ty(ctx));
  Constant* indices[] = {zero, zero};
  Constant* strVal = ConstantExpr::getGetElementPtr(strConstant->getType(), GVStr, indices, true);
  return strVal;
}

static void* doSomethingFunny(Function* F, ExecutionEngine* EE, Instruction* osrcond) {
  std::cout << "made it into do something funky\n";
  errs() << *F << "\n";

  LivenessAnalysis live(F);
  std::cout << "live at instruction: " << live.getLiveOutValues(osrcond->getParent());

  // restart the loop with the live variables set
  Module* M = new Module("funky_mod", getGlobalContext());

  std::vector<Type*> cont_args_types;
  for (auto lv : live.getLiveOutValues(osrcond->getParent())) {
    cont_args_types.push_back(lv->getType());
  }

  auto cont_ret      = Type::getInt32Ty(getGlobalContext());
  auto cont_type     = FunctionType::get(cont_ret, cont_args_types, false);

  auto NF = dyn_cast<Function>(M->getOrInsertFunction("new_f", cont_type));
  auto block = BasicBlock::Create(getGlobalContext(), "entry", NF);
  IRBuilder<> builder(block);

  // TODO copy function and run an optimization pass
  auto printf_fun = printf_prototype(NF->getParent()->getContext(), NF->getParent());

  Constant* c = geti8StrVal(*NF->getParent(), "liveness arg wow %d\n", "printstr");
  for (auto &arg : NF->getArgumentList()) {
    std::vector<Value*> args;
    args.push_back(c);
    args.push_back(&arg);
    builder.CreateCall(printf_fun, args);
  }

  // return the -1000 thing so the "test" passes
  builder.CreateRet(ConstantInt::get(Type::getInt32Ty(getGlobalContext()), -1000));

  errs() << *M << "\n";

  std::unique_ptr<Module> uniq(M);
  EE->addModule(std::move(uniq));
  EE->generateCodeForModule(M);
  EE->finalizeObject();

  auto fp = EE->getPointerToFunctionOrStub(NF);
  assert(fp);

  return (void*)fp;
}

Instruction* OsrPass::addOsrConditionCounterGE(Value &counter,
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
  return Builder.CreateCondBr(osr_cond, &OsrBB, newBB);
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

  // TODO will need an osr block for each loop in the function
  BasicBlock* osrBB = BasicBlock::Create(getGlobalContext(), "osr", &F);
  IRBuilder<> OsrBuilder(osrBB);
  // TODO assuming that the function returns an int
  // adding an empty return so that we can run live var analysis without it
  // blowing up
  Instruction* bullshitReturn = OsrBuilder.CreateRet(ConstantInt::get(F.getReturnType(), 0));


  // TODO assuming single condition added
  Instruction* cond = nullptr;
  for (auto &Loop : LI) {
    Value* counter = instrumentLoopWithCounters(*Loop);
    cond = addOsrConditionCounterGE(*counter, 1000, *Loop->getHeader(), *osrBB);
  }

  if (!cond) {
    return false;
  }

  // make this here so it runs on the function which includes the newly added
  // counter
  LivenessAnalysis live(&F);

  // okay we can get rid of this now
  bullshitReturn->removeFromParent();

  auto getIntPtr = [&](uintptr_t ptr) {
    return ConstantInt::get(Type::getInt64Ty(F.getContext()), ptr);
  };

  auto voidPtr = Type::getInt8PtrTy(F.getContext());

  // now we need to get an fp to a new function which takes all of our live vars
  // as args and returns the same thing as the original function.
  // we return the result of a call to this function
  auto relevantLive = live.getLiveOutValues(cond->getParent());

  // types for continuation func
  std::vector<Type*> cont_args_types;
  for (auto v : relevantLive) {
    cont_args_types.push_back(v->getType());
  }

  std::vector<Value*> cont_args_values;
  for (auto v : relevantLive) {
    cont_args_values.push_back(const_cast<Value*>(v)); // fuck the rules
  }

  auto cont_ret      = Type::getInt32Ty(F.getContext());
  auto cont_type     = FunctionType::get(cont_ret, cont_args_types, false);
  auto cont_ptr_type = PointerType::getUnqual(cont_type);

  // types for stub
  // for now, stub takes a i8* (void*) and returns a cont_func ptr
  std::vector<Type*> stub_args_types;
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

  // memory leek
  stub_args_values.push_back(
      OsrBuilder.CreateIntToPtr(
        getIntPtr((uintptr_t)cond),
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
