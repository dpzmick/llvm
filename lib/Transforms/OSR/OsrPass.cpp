#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/OSR/OsrPass.h"
#include "llvm/Transforms/OSR/Liveness.hpp"
#include "llvm/Transforms/OSR/StateMap.hpp"

#include <iostream>

#define DEBUG_TYPE "OsrPass"
#define LOG_HEADER DEBUG_TYPE << "::" << __func__ << " "

using namespace llvm;

char OsrPass::ID = 0;

ModulePass* llvm::createOsrPassPass(ExecutionEngine* EE) {
  return new OsrPass(EE);
}

INITIALIZE_PASS_BEGIN(OsrPass, "osr", "osr", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(OsrPass, "osr", "osr", false, false)

OsrPass::OsrPass(ExecutionEngine *EE)
: ModulePass(ID),
  EE(EE)
{
  assert(EE);
  initializeOsrPassPass(*PassRegistry::getPassRegistry());
}

static void removeOsrPoint(Instruction *src);
static void correctSSA(Function *cont, Instruction *cont_lpad,
		       std::vector<Value*> &values_to_set,
		       ValueToValueMapTy &VMap,
		       ValueToValueMapTy &VMap_updates,
		       SmallVectorImpl<PHINode*> *inserted_PHI_nodes);
		       

static void* generator(Function* F, ExecutionEngine* EE,
                       Instruction* osr_point, std::set<const Value*>* vars)
{
  errs() << *F;
  errs() << *osr_point << "\n";

  // Clone the previous function into the new one and create a state map between
  // the two values.
  // This isn't the function we're going to compile, just a temporary we copy
  // the function into.
  ValueToValueMapTy VMap;
  auto *NF = CloneFunction(F, VMap, false);
  StateMap SM(F, NF, &VMap, true);
  /*
  auto *ft = FunctionType::get(F->getReturnType(), arg_types, false);
  std::unique_ptr<Module> M(new Module("funky_mod", getGlobalContext()));
  Function* NF = dyn_cast<Function>(M->getOrInsertFunction("new_f", ft));
  */
  /*
  auto it = NF->arg_begin();
  for (const Value* v : *vars) {
	  VMap[v] = &*(it++);
  }
  SmallVector<ReturnInst*, 16> returns;
  CloneFunctionInto(NF, F, VMap, true, returns);
  */

  /*
  size_t cutoff = F->getArgumentList().size();
  errs() << "need to skip the first " << cutoff << " elements\n";
  */
  // Remove the OSR point in the new function.
  auto *osr_point_cont = cast<Instruction>(SM.OneToOne[osr_point]);
  errs() << "--------\n";
  errs() << *osr_point_cont;
  removeOsrPoint(osr_point_cont);

  std::vector<Type*> arg_types;
  for (const Value* v : *vars) {
	  arg_types.push_back(v->getType());
  }

  // TODO: random module name, use twine.
  std::unique_ptr<Module> M(new Module("funky_mod", getGlobalContext()));
  auto *ft = FunctionType::get(NF->getReturnType(), arg_types, false);
  auto *CF = Function::Create(
	  ft, NF->getLinkage(), Twine("osrcont_", NF->getName()), M.get());
  // Set argument names and determine the values of the new arguments
  ValueToValueMapTy args_to_cont_args;
  auto cont_arg_it = CF->arg_begin();
  for (const Value *v : *vars) {
	  args_to_cont_args[v] = &*cont_arg_it;
//	  args_to_cont_args.insert(std::pair<Value*, WeakVH>(v, &*cont_arg_it));
	  if (v->hasName()) {
		  (cont_arg_it++)->setName(Twine(v->getName(), "_osr"));
	  } else {
		  (cont_arg_it++)->setName(Twine("__osr"));
	  }
  }
  assert(cont_arg_t == CF->arg_end());

  // Duplicate the body of F2 into the body of JF.
  ValueToValueMapTy NF_to_CF_VMap;
  for (auto BI = NF->begin(), BE = NF->end();
       BI != BE;
       ++BI) {
	  auto *CBB = CloneBasicBlock(&*BI, NF_to_CF_VMap, "", CF, nullptr);
	  NF_to_CF_VMap[&*BI] = CBB;
	  if (BI->hasAddressTaken()) {
		  Constant *prev_addr = BlockAddress::get(NF, &*BI);
		  NF_to_CF_VMap[prev_addr] = BlockAddress::get(CF, CBB);
	  }
  }

  auto *lpad = SM.LandingPadMap[osr_point];
  auto *cont_lpad = cast<Instruction>(NF_to_CF_VMap[lpad]);

  // Apply attributes
  auto src_attrs = NF->getAttributes();
  Function::arg_iterator arg_it = CF->arg_begin();
  size_t arg_no = 1;
  for (const Value *src : *vars) {
	  Argument *arg = &*arg_it;
	  arg_it++;
	  if (isa<Argument>(src)) {
		  auto attrs = src_attrs.getParamAttributes(arg_no);
		  if (attrs.getNumSlots() > 0)
			  arg->addAttr(attrs);
		  arg_no++;
	  }
  }

  // Generate a new entry point for the continuation function.
  LivenessAnalysis live(NF);
  auto live_vars = live.getLiveOutValues(lpad->getParent());
  std::vector<Value*> values_to_set;
  for (auto v : values_to_set) {
	  values_to_set.push_back(v);
  }

  auto entry_point_pair = SM.genContinuationFunctionEntry(
	  getGlobalContext(), osr_point, lpad, cont_lpad, values_to_set,
	  args_to_cont_args);

  auto *prev_entry = &CF->getEntryBlock();
  auto *new_entry = entry_point_pair.first;
  auto *NF_to_CF_changes = entry_point_pair.second;

  // Undef dead arguments.
  for (auto &dst : NF->args()) {
	  auto it = NF_to_CF_changes->find(&dst);
	  Value *v = ((it != NF_to_CF_changes->end())
		      ? cast<Value>(&dst)                               // Live argument
		      : cast<Value>(UndefValue::get(dst.getType())));   // Dead argument
	  NF_to_CF_VMap[&dst] = v;
	  //insert(std::pair<const Argument*, const Value*>(&dst, v));
  }
  
  // Fix operand references
  auto *BE = &CF->back();
  for (auto *BB = cast<BasicBlock>(NF_to_CF_VMap[&NF->front()]);
       BB != BE;
       BB = BB->getNextNode()) {
	  for (auto II = BB->begin(); II != BB->end(); ++II)
		  RemapInstruction(&*II, NF_to_CF_VMap, RF_NoModuleLevelChanges);
  }
  new_entry->insertInto(CF, prev_entry);

  SmallVector<PHINode*, 8> inserted_phi_nodes;
  correctSSA(CF, cont_lpad, values_to_set, NF_to_CF_VMap, *NF_to_CF_changes,
	     &inserted_phi_nodes);
  
  errs() << "continuation function:\n\n" << *CF << "\n";

  verifyFunction(*CF, &errs());
  auto *mod = M.get();
  auto PM = llvm::make_unique<legacy::PassManager>();
  PM->add(createCFGSimplificationPass());
  PM->run(*mod);
  EE->addModule(std::move(M));
  EE->generateCodeForModule(mod);
  EE->finalizeObject();

  return (void*)EE->getPointerToFunction(CF);
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
      for (const auto& user : var->users()) {
        errs() << *var << " is used in " << *user << "\n";
      }
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

// Remove OSR from tinyVM. I think this is pretty much the only way to do this.
static void
removeOsrPoint(Instruction *src)
{
	auto *splitBB = src->getParent();
	if (src != &splitBB->getInstList().front())
		return;
	auto *predBB = splitBB->getSinglePredecessor();
	if (predBB == nullptr)
		return;
	auto *TI = predBB->getTerminator();
	if (BranchInst* BI = dyn_cast<BranchInst>(TI)) {
		if (BI->getNumSuccessors() != 2)
			return;
		BasicBlock* FireOSRBlock = BI->getSuccessor(0);
		FireOSRBlock->eraseFromParent();
		TI->eraseFromParent();
		BranchInst* brToSplitBB = BranchInst::Create(splitBB);
		predBB->getInstList().push_back(brToSplitBB);
		for (BasicBlock::reverse_iterator revIt = ++(predBB->rbegin()),
			     revEnd = predBB->rend(); revIt != revEnd; ) {
			Instruction *I = &*revIt;
			if (isInstructionTriviallyDead(I, nullptr)) {
				I->eraseFromParent();
				revEnd = predBB->rend();
			} else {
				return;
			}
		}
	}
}

static void
correctSSA(Function *cont, Instruction *cont_lpad,
	   std::vector<Value*> &values_to_set,
	   ValueToValueMapTy &VMap,
	   ValueToValueMapTy &VMap_updates,
	   SmallVectorImpl<PHINode*> *inserted_PHI_nodes)
{
	auto *entry_point = &cont->getEntryBlock();
	auto *lpad_block = cont_lpad->getParent();
	bool lpad_is_first_non_phi = cont_lpad == lpad_block->getFirstNonPHI();
	std::vector<Value*> work_queue;
	BasicBlock *split_block;
	if (lpad_is_first_non_phi) {
		for (Value *orig : values_to_set) {
			if (isa<Argument>(orig))
				continue;
			auto prev_inst = cast<Instruction>(VMap[orig]);
			if (prev_inst->getParent() == lpad_block) {
				if (PHINode *node =
				    dyn_cast<PHINode>(prev_inst)) {
					Value *next_value = VMap_updates[orig];
					node->addIncoming(next_value,
							  entry_point);
					continue;
				}
			}
			work_queue.push_back(orig);
		}
	} else {
		for (Value *orig : values_to_set) {
			if (!isa<Argument>(orig))
				work_queue.push_back(orig);
		}
		split_block = lpad_block->splitBasicBlock(cont_lpad, "cont_split");
		auto *BI = cast<BranchInst>(entry_point->getTerminator());
		BI->setSuccessor(0, split_block);
	}

	if (work_queue.empty())
		return;

	SSAUpdater updater(inserted_PHI_nodes);
	PHINode *last_inserted = nullptr;
	if (lpad_is_first_non_phi)
		split_block = lpad_block->splitBasicBlock(cont_lpad,
							  "cont_split");
	for (Value *orig : work_queue) {
		Value *prev_value = VMap[orig];
		auto *prev_inst = cast<Instruction>(VMap[orig]);
		Value *next_value = VMap_updates[orig];
		if (prev_value->hasName()) {
			updater.Initialize(
				prev_value->getType(), StringRef(
					Twine(prev_value->getName(), "_fix")
					.str()));
		} else {
			updater.Initialize(
				prev_value->getType(), StringRef("__fix"));
		}
		updater.AddAvailableValue(prev_inst->getParent(), prev_value);
		updater.AddAvailableValue(entry_point, next_value);

		Value::use_iterator UE = prev_value->use_end();
		for (Value::use_iterator UI = prev_value->use_begin(); UI != UE; ) {
			Use &U = *(UI++);
			updater.RewriteUseAfterInsertions(U);
		}
	}

	if (lpad_is_first_non_phi) {
		split_block->replaceAllUsesWith(lpad_block);
		lpad_block->getInstList().back().eraseFromParent();
		lpad_block->getInstList().splice(lpad_block->end(),
						 split_block->getInstList());
		split_block->eraseFromParent();
	}
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
