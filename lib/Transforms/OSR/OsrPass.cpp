#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/OSR/OsrPass.h"
#include "llvm/Transforms/OSR/Liveness.hpp"
#include "llvm/Transforms/OSR/StateMap.hpp"

#include <iostream>

#define DEBUG_TYPE "OsrPass"
#define LOG_HEADER DEBUG_TYPE << "::" << __func__ << " "

using namespace llvm;

static std::map<std::string, std::set<Function*>> possible_functions;

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

static void correctSSA(Function *cont, Instruction *cont_lpad,
		       std::vector<Value*> &values_to_set,
		       ValueToValueMapTy &VMap,
		       ValueToValueMapTy &VMap_updates,
		       SmallVectorImpl<PHINode*> *inserted_PHI_nodes);

static std::unique_ptr<Module> generator(Function* F,
                                         Instruction* osr_point,
                                         std::set<const Value*>* vars,
                                         Function** out_newfn)
{
  ValueToValueMapTy VMap;
  auto *NF = CloneFunction(F, VMap, false);

  auto* osr_point_cont = cast<BranchInst>(VMap[osr_point]);
  BasicBlock::iterator II(osr_point_cont);
  auto *lpad = BranchInst::Create(osr_point_cont->getSuccessor(1));

  auto* to_rm = osr_point_cont->getSuccessor(0);
  ReplaceInstWithInst(osr_point_cont->getParent()->getInstList(), II,
		      lpad);
  to_rm->removeFromParent();

  StateMap SM(F, NF, &VMap, true);

  // TODO: random module name, use twine.
  std::unique_ptr<Module> M(new Module("funky_mod", getGlobalContext()));
  for (auto& f: *F->getParent()) {
    if (!f.isDeclaration()) continue;
    Function::Create(f.getFunctionType(),
        Function::ExternalLinkage,
        f.getName(),
        M.get());
  }

  std::vector<Type*> arg_types;
  for (const Value* v : *vars) {
	  arg_types.push_back(v->getType());
  }
  auto *ft = FunctionType::get(NF->getReturnType(), arg_types, false);
  auto *CF = Function::Create(
	  ft, NF->getLinkage(), Twine("osrcont_", NF->getName()), M.get());
  // Set argument names and determine the values of the new arguments
  ValueToValueMapTy args_to_cont_args;
  auto cont_arg_it = CF->arg_begin();
  for (const Value *v : *vars) {
	  args_to_cont_args[v] = &*cont_arg_it;
	  if (v->hasName()) {
		  (cont_arg_it++)->setName(Twine(v->getName(), "_osr"));
	  } else {
		  (cont_arg_it++)->setName(Twine("__osr"));
	  }
  }
  assert(cont_arg_it == CF->arg_end());

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
  for (auto v : live_vars) {
	  Value *vc = const_cast<Value*>(v);
	  values_to_set.push_back(vc);
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
	  if (it != NF_to_CF_changes->end()) {
		  NF_to_CF_VMap[&dst] = it->second;
	  } else {
		  NF_to_CF_VMap[&dst] = UndefValue::get(dst.getType());
	  }
  }

  // Fix operand references
  Function::iterator BE = CF->end();
  for (Function::iterator BB = CF->begin();
       BB != BE;
       ++BB) {
	  for (auto II = BB->begin(); II != BB->end(); ++II) {
		  RemapInstruction(&*II, NF_to_CF_VMap, RF_NoModuleLevelChanges);
	  }
  }
  new_entry->insertInto(CF, prev_entry);

  SmallVector<PHINode*, 8> inserted_phi_nodes;
  correctSSA(CF, cont_lpad, values_to_set, NF_to_CF_VMap, *NF_to_CF_changes,
	     &inserted_phi_nodes);

  verifyFunction(*CF, &errs());
  *out_newfn = CF;
  return M;
}

// does a simple scan of the live values
// if any of them are being used in a call via a function pointer, replaces
// all of the calls with the function
static void* indirect_inline_generator(Function* F,
                                       ExecutionEngine* EE,
                                       Instruction* osr_point,
                                       std::set<const Value*>* vars,
                                       // pointer to array of pointers of
                                       // function pointer argument values
                                       void** fp_arg_values)
{
  std::vector<Value*> function_calls;

  // first, figure out what it is we want to fiddle around with
  // each arg has a corresponding element in the fp_args_values array
  std::vector<std::pair<Value*,Function*>> fs_to_inline;
  size_t counter = 0;
  for (const auto& arg : F->getArgumentList()) {
    auto ty = arg.getType();
    if (!ty->isPointerTy())                           continue;
    if (!ty->getPointerElementType()->isFunctionTy()) continue;

    // find the function that this call actually called
    for (auto* func : possible_functions[F->getName()]) {
      void* addr = (void*)EE->getFunctionAddress(func->getName());
      if (!addr) {
        errs() << "could not find the address for one of the possible functions\n";
      }

      if (addr == fp_arg_values[counter]) {
        fs_to_inline.push_back(std::make_pair(const_cast<Argument*>(&arg), func));
      }
    }

    counter++;
  }

  if (!fs_to_inline.size()) {
    errs() << "could not any functions which we want to try inlining\n";
    return (void*)EE->getFunctionAddress(F->getName());
  }

  for (auto p : fs_to_inline) {
    errs() << "trying to inline the calls to " << p.second->getName()
           << " which are bound to the value " << p.first->getName() << "\n";
  }

  Function* CF = nullptr;
  auto M = generator(F, osr_point, vars, &CF);
  auto *mod = M.get();
  assert(CF);
  assert(mod);

  // replace all the function calls with direct function calls
  for (auto p : fs_to_inline) {
    // need to copy the function being inlined into the new module so that we
    // can inline it + add attributes
    ValueToValueMapTy VMap;
    Function* myFunToInline = CloneFunction(p.second, VMap, false);
    myFunToInline->addFnAttr(Attribute::AlwaysInline);
    myFunToInline->setLinkage(Function::LinkageTypes::PrivateLinkage);
    mod->getFunctionList().push_back(myFunToInline);

    // find the arg to CF which corresponds to the arg to F, if there is one
    // hows this for a hack?
    auto hack = Twine(p.first->getName(), "_osr");
    for (auto& a : CF->getArgumentList()) {
      if (a.getName().str() == hack.str()) {
        // only one instance of every type is ever created, the pointer cmp is
        // valid
        assert(a.getType() == p.first->getType());

        for (auto use : a.users()) {
          if (auto call = dyn_cast<CallInst>(use)) {
            errs () << "fp is used in call " << *call << "\n";
            call->setCalledFunction(myFunToInline);
          }
        }
      }
    }
  }

  verifyFunction(*CF, &errs());
  errs() << "mod before error\n" << *mod << "\n";

  auto PM = llvm::make_unique<legacy::PassManager>();
  PM->add(createDeadCodeEliminationPass());
  PM->add(createCFGSimplificationPass());
  // actually do the inlining here
  PM->add(createAlwaysInlinerPass());
  PM->run(*mod);

  errs() << *mod << "\n";

  EE->addModule(std::move(M));
  EE->generateCodeForModule(mod);
  EE->finalizeObject();

  // I can't believe it actually works
  return (void*)EE->getPointerToFunction(CF);
}

// need this to be a function pass so I can add the osr block to the module.
// if this is a function pass, the pass gets run on any modules that are added,
// so this becomes infinite loop
bool OsrPass::runOnModule( Module &M ) {
  bool flag = false;
  // find every call to the function and store what function is used as an
  // argument
  // this means that the inliner can only operate if the function call is made
  // where the argument is inlineable exists in the same module as the function
  // definition
  for (auto &F : M) {
    for (const auto* u : F.users()) {
      if (const auto* call = dyn_cast<CallInst>(u)) {
        for (const auto& arg : call->arg_operands()) {
          if (isa<Function>(*arg)) {
            Function* func = dyn_cast<Function>(arg);
            possible_functions[F.getName()].insert(func);
          }
        }
      }
    }

    flag = flag || runOnFunction(F);
  }
  return flag;
}

bool OsrPass::runOnFunction( Function &F )
{
  if (F.isDeclaration()) return false;

  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();

  // the vars at the end of this loop's header are the relevant live variables
  // we need to get these before we start adding new things to the function
  LivenessAnalysis live(&F);
  auto *relevantLiveVars = new std::set<const Value*>();
  for (auto &Loop : LI) {
    for (auto var : live.getLiveOutValues(Loop->getHeader())) {
      relevantLiveVars->insert(var);
    }
  }

  // now that we have the important information from the original function, we
  // are going to start fiddling around with it

  // TODO will need an osr block for each loop in the function
  BasicBlock* osrBB = BasicBlock::Create(getGlobalContext(), "osr", &F);
  IRBuilder<> OsrBuilder(osrBB);

  Instruction* cond = nullptr;
  for (auto &Loop : LI) {
	  Value* counter = instrumentLoopWithCounters(*Loop, relevantLiveVars);
    cond = addOsrConditionCounterGE(*counter, 1000, *Loop->getHeader(),
        *osrBB);
  }
  if (!cond) return false;

  // some helpful things
  auto getIntPtr = [&](uintptr_t ptr) {
    return ConstantInt::get(Type::getInt64Ty(F.getContext()), ptr);
  };
  auto voidPtr = Type::getInt8PtrTy(F.getContext());

  // create the osr stub code
  // danger, here be dragons
  // first, get the values of every live variable which is a function pointer
  size_t nfps = 0;
  for (auto& arg: F.getArgumentList()) {
    auto ty = arg.getType();
    if (!ty->isPointerTy())                           continue;
    if (!ty->getPointerElementType()->isFunctionTy()) continue;
    nfps++;
  }

  // the array will contain nfps elements
  auto atype = ArrayType::get(Type::getInt8PtrTy(F.getContext()), nfps);
  auto array = OsrBuilder.CreateAlloca(atype, 0);

  size_t counter = 0;
  for (auto& arg: F.getArgumentList()) {
    auto ty = arg.getType();
    if (!ty->isPointerTy())                           continue;
    if (!ty->getPointerElementType()->isFunctionTy()) continue;

    std::vector<Value*> args;
    args.push_back(ConstantInt::get(Type::getInt64Ty(F.getContext()), 0));
    args.push_back(ConstantInt::get(Type::getInt64Ty(F.getContext()), counter++));
    auto el = OsrBuilder.CreateGEP(array, args);

    // pass the address of the function being called
    auto val = OsrBuilder.CreateBitCast(&arg, Type::getInt8PtrTy(F.getContext()));

    OsrBuilder.CreateStore(val, el);
  }
  assert(counter == nfps);

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
  // this is a bit abusive. Then again, all of this code is that way
  stub_args_types.push_back(array->getType());

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
        stub_args_types[2]
        ));

  stub_args_values.push_back(
      OsrBuilder.CreateIntToPtr(
        getIntPtr((uintptr_t)relevantLiveVars),
        stub_args_types[3]
        ));

  stub_args_values.push_back(array);

  auto stub_ret      = Type::getInt8PtrTy(F.getContext());
  auto stub_type     = FunctionType::get(stub_ret, stub_args_types, false);
  auto stub_ptr_type = PointerType::getUnqual(stub_type);

  auto stub_int_val = getIntPtr((uintptr_t)indirect_inline_generator);
  auto stub_ptr    = OsrBuilder.CreateIntToPtr(stub_int_val, stub_ptr_type);
  auto stub_result = OsrBuilder.CreateCall(stub_ptr, stub_args_values);

  auto cont_ptr = OsrBuilder.CreatePointerCast(stub_result, cont_ptr_type);
  auto cont_result = OsrBuilder.CreateCall(cont_ptr, cont_args_values);

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
	if (lpad_is_first_non_phi)
		split_block = lpad_block->splitBasicBlock(cont_lpad,
							  "cont_split");
	for (Value *orig : work_queue) {
		Value *prev_value = VMap[orig];
		auto *prev_inst = cast<Instruction>(VMap[orig]);
		Value *next_value = VMap_updates[orig];
		if (next_value == nullptr) {
			errs() << "next value is null:\n\tprev_value = "<<  *orig << "\n";
			continue;
		}
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

Value* OsrPass::instrumentLoopWithCounters( Loop &L, std::set<const Value*> *relevant )
{
  // TODO try to use getCanonicalInductionVariable
  // maybe just return that if this works

  auto header = L.getHeader();
  BasicBlock::iterator it = header->begin();

  auto phi = PHINode::Create(Type::getInt64Ty(header->getContext()), 2, "loop_counter", &*it);
  relevant->insert(cast<Value>(phi));

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
