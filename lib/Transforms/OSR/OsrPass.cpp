#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
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

#define DEBUG_TYPE "OsrPass"
#define LOG_HEADER DEBUG_TYPE << "::" << __func__ << " "

using namespace llvm;

char OsrPass::ID = 0;

ModulePass* llvm::createOsrPassPass(MCJITWrapper* MC, bool dump_ir) {
  return new OsrPass(MC, dump_ir);
}

INITIALIZE_PASS_BEGIN(OsrPass, "osr", "osr", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(OsrPass, "osr", "osr", false, false)

OsrPass::OsrPass(MCJITWrapper *MC, bool dump_ir)
: ModulePass(ID),
  MC(MC),
  dump_ir(dump_ir)
{
  assert(MC);
  initializeOsrPassPass(*PassRegistry::getPassRegistry());
}

static size_t mod_count = 0;

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
  auto *NF = CloneFunction(F, VMap, true);

  auto* osr_point_cont = cast<BranchInst>(VMap[osr_point]);
  BasicBlock::iterator II(osr_point_cont);
  auto *lpad = BranchInst::Create(osr_point_cont->getSuccessor(1));

  auto* to_rm = osr_point_cont->getSuccessor(0);
  ReplaceInstWithInst(osr_point_cont->getParent()->getInstList(), II,
		      lpad);
  to_rm->removeFromParent();

  StateMap SM(F, NF, &VMap, true);

  std::unique_ptr<Module> M(new Module("funky_mod" + std::to_string(mod_count++),
        getGlobalContext()));

  for (auto& f: *F->getParent()) {
    Function::Create(f.getFunctionType(),
        Function::ExternalLinkage,
        f.getName(),
        M.get());

    // doing a replacement here doesn't seem to work. I'm doing another
    // (very slow) replacement at the bottom of this function
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

  // wowee this is slow but the other thing wasn't working
  for (auto& f : *CF->getParent()) {
    for (auto& bb : f) {
      for (auto& i : bb) {
        if (auto call = dyn_cast<CallInst>(&i)) {
          auto* orig = call->getCalledFunction();
          if (!orig) continue;

          auto* func = CF->getParent()->getFunction(orig->getName());
          call->setCalledFunction(func);
        }
      }
    }
  }

  // do a small amount of passes here to remove any code which is not used
  // and make sure that all uses of the arguments are readily exposed
  auto PM = llvm::make_unique<legacy::PassManager>();
  PM->add(createInstructionCombiningPass());
  PM->add(createAggressiveDCEPass());
  PM->add(createCFGSimplificationPass());
  PM->add(createLoopSimplifyPass());
  PM->add(createLoopDeletionPass());
  PM->add(createLoopIdiomPass());
  PM->add(createCFGSimplificationPass());
  PM->run(*M);

  *out_newfn = CF;
  return M;
}

// does a simple scan of the live values
// if any of them are being used in a call via a function pointer, replaces
// all of the calls with the function
static void* indirect_inline_generator(Function* F,
                                       MCJITWrapper* MC,
                                       Instruction* osr_point,
                                       std::set<const Value*>* vars,
                                       // pointer to array of pointers of
                                       // function pointer argument values
                                       void** fp_arg_values,
                                       bool dump_ir)
{
  // first, figure out what it is we want to fiddle around with
  // each arg has a corresponding element in the fp_args_values array
  std::vector<std::pair<Value*,Function*>> fs_to_inline;
  size_t counter = 0;
  for (const auto& val : *vars) {
    const auto& ty = val->getType();
    if (!ty->isPointerTy())                           continue;
    if (!ty->getPointerElementType()->isFunctionTy()) continue;

    auto* func = MC->getFunctionForPtr(fp_arg_values[counter]);
    if (!func) {
      DEBUG(errs() << "could not find a function to inline\n");
    } else {
      fs_to_inline.push_back(std::make_pair(const_cast<Value*>(val), func));
    }
    counter++;
  }

#ifndef NDEBUG
  for (auto p : fs_to_inline) {
    errs() << "trying to inline the calls to " << p.second->getName()
           << " which are bound to the value " << p.first->getName() << "\n";
  }
#endif

  Function* CF = nullptr;
  auto M = generator(F, osr_point, vars, &CF);
  auto *mod = M.get();
  assert(CF);
  assert(mod);

  // replace all the function calls with direct function calls
  for (auto& p : fs_to_inline) {
    // need to copy the function being inlined into the new module so that we
    // can inline it + add attributes
    ValueToValueMapTy VMap;
    Function* myFunToInline = CloneFunction(p.second, VMap, true);
    myFunToInline->addFnAttr(Attribute::AlwaysInline);
    myFunToInline->setLinkage(Function::LinkageTypes::PrivateLinkage);
    mod->getFunctionList().push_back(myFunToInline);

    // find the arg to CF which corresponds to the arg to F, if there is one
    for (auto& a : CF->getArgumentList()) {
      // hows this for a hack?
      auto hack = Twine(p.first->getName(), "_osr");
      if (a.getName().str() == hack.str()) {
        // only one instance of every type is ever created, the pointer cmp is
        // valid
        assert(a.getType() == p.first->getType());

        for (const auto& use : a.users()) {
          if (const auto& call = dyn_cast<CallInst>(use)) {
            if (call->getCalledValue() != &a) continue;
            call->setCalledFunction(myFunToInline);
          }
        }
      }
    }
  }

  auto PM = llvm::make_unique<legacy::PassManager>();
  PM->add(createAlwaysInlinerPass());

  PassManagerBuilder pmb;
  pmb.OptLevel = 3;
  pmb.populateModulePassManager(*PM.get());

  // for some reason we seem to often need this at the end
  PM->add(createCFGSimplificationPass());

  PM->run(*mod);

  if (dump_ir) {
    std::error_code ec;
    raw_fd_ostream out(
        F->getParent()->getName().str() + "_osr_module" + std::to_string(mod_count) + ".bc",
        ec, sys::fs::F_None);

    WriteBitcodeToFile(mod, out);
  }

  MC->addModule(std::move(M));

  // I can't believe it actually works
  return (void*)MC->getPointerToFunction(CF);
}

// need this to be a function pass so I can add the osr block to the module.
// if this is a function pass, the pass gets run on any modules that are added,
// so this becomes infinite loop
bool OsrPass::runOnModule( Module &M ) {
  bool flag = false;
  for (auto &F : M) {
    flag = runOnFunction(F) || flag;
  }

  if (dump_ir) {
    std::error_code ec;
    raw_fd_ostream out(
        M.getName().str() + "_after_pass.bc",
        ec, sys::fs::F_None);

    WriteBitcodeToFile(&M, out);
  }

  return flag;
}

bool OsrPass::runOnFunction( Function &F )
{
  if (F.isDeclaration()) return false;

  // some helpful things
  auto getIntPtr = [&](uintptr_t ptr) {
    return ConstantInt::get(Type::getInt64Ty(F.getContext()), ptr);
  };
  auto voidPtr = Type::getInt8PtrTy(F.getContext());

  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();

  // get all the live vars before we fiddle with anything
  LivenessAnalysis live(&F);
  auto relevantLiveVars = std::map<const Loop*, std::set<const Value*>*>();
  for (auto& Loop : LI) {
    relevantLiveVars[Loop] = new std::set<const Value*>();
    for (auto& var : live.getLiveOutValues(Loop->getHeader())) {
      relevantLiveVars[Loop]->insert(var);
    }
  }

  // check that one of the live vars is an fp
  size_t nfps = 0;
  for (auto& lv : relevantLiveVars) {
    for (auto& vs : *lv.second) {
      const auto& ty = vs->getType();
      if (!ty->isPointerTy())                           continue;
      if (!ty->getPointerElementType()->isFunctionTy()) continue;
      nfps++;
    }
  }

  if (nfps == 0) {
    return false;
  }

  // for every loop, add the osr instrumentation
  size_t osrcounter = 0;
  for (auto& Loop : LI) {
    // create an osr block for each loop in the function
    std::string osrname = "osr" + std::to_string(osrcounter++);
    BasicBlock* osrBB = BasicBlock::Create(getGlobalContext(), osrname, &F);
    IRBuilder<> OsrBuilder(osrBB);

    Value* loopcounter = instrumentLoopWithCounters(*Loop, relevantLiveVars[Loop]);
    Instruction* cond = addOsrConditionCounterGE(*loopcounter,
        1000,
        *Loop->getHeader(),
        *osrBB);
    if (!cond) continue;

    // create the osr stub code
    // get an fp to a new function which takes all of our live vars
    // as args and returns the same thing as the original function.
    // we return the result of a call to this function

    std::vector<Type*> cont_args_types;
    std::vector<Value*> cont_args_values;

    size_t nfps = 0;
    for (auto& v : *relevantLiveVars[Loop]) {
      const auto& ty = v->getType();
      if (!ty->isPointerTy())                           continue;
      if (!ty->getPointerElementType()->isFunctionTy()) continue;
      nfps++;
    }

    // the array will contain nfps elements
    auto atype = ArrayType::get(Type::getInt8PtrTy(F.getContext()), nfps);
    auto array = OsrBuilder.CreateAlloca(atype, 0);

    size_t counter = 0;
    for (auto& v : *relevantLiveVars[Loop]) {
      cont_args_types.push_back(v->getType());
      cont_args_values.push_back(const_cast<Value*>(v));

      const auto& ty = v->getType();
      if (!ty->isPointerTy())                           continue;
      if (!ty->getPointerElementType()->isFunctionTy()) continue;

      std::vector<Value*> args;
      args.push_back(ConstantInt::get(Type::getInt64Ty(F.getContext()), 0));
      args.push_back(ConstantInt::get(Type::getInt64Ty(F.getContext()), counter++));
      auto el = OsrBuilder.CreateGEP(array, args);

      // pass the address of the function being called
      auto val = OsrBuilder.CreateBitCast(const_cast<Value*>(v), Type::getInt8PtrTy(F.getContext()));

      OsrBuilder.CreateStore(val, el);
    }
    assert(counter == nfps);

    auto cont_ret      = F.getReturnType();
    auto cont_type     = FunctionType::get(cont_ret, cont_args_types, false);
    auto cont_ptr_type = PointerType::getUnqual(cont_type);

    std::vector<Type*> stub_args_types;
    stub_args_types.push_back(voidPtr);
    stub_args_types.push_back(voidPtr);
    stub_args_types.push_back(voidPtr);
    stub_args_types.push_back(voidPtr);
    // this is a bit abusive. Then again, all of this code is that way
    stub_args_types.push_back(array->getType());
    stub_args_types.push_back(Type::getInt1Ty(getGlobalContext()));

    std::vector<Value*> stub_args_values;
    stub_args_values.push_back(
        OsrBuilder.CreateIntToPtr(
          getIntPtr((uintptr_t)&F),
          stub_args_types[0]));

    stub_args_values.push_back(
        OsrBuilder.CreateIntToPtr(
          getIntPtr((uintptr_t)MC),
          stub_args_types[1]
          ));

    stub_args_values.push_back(
        OsrBuilder.CreateIntToPtr(
          getIntPtr((uintptr_t)cond),
          stub_args_types[2]
          ));

    stub_args_values.push_back(
        OsrBuilder.CreateIntToPtr(
          getIntPtr((uintptr_t)relevantLiveVars[Loop]),
          stub_args_types[3]
          ));

    stub_args_values.push_back(array);

    stub_args_values.push_back(
        ConstantInt::get(Type::getInt1Ty(getGlobalContext()), dump_ir));

    auto stub_ret      = Type::getInt8PtrTy(F.getContext());
    auto stub_type     = FunctionType::get(stub_ret, stub_args_types, false);
    auto stub_ptr_type = PointerType::getUnqual(stub_type);

    auto stub_int_val = getIntPtr((uintptr_t)indirect_inline_generator);
    auto stub_ptr    = OsrBuilder.CreateIntToPtr(stub_int_val, stub_ptr_type);
    auto stub_result = OsrBuilder.CreateCall(stub_ptr, stub_args_values);

    auto cont_ptr = OsrBuilder.CreatePointerCast(stub_result, cont_ptr_type);
    auto cont_result = OsrBuilder.CreateCall(cont_ptr, cont_args_values);
    cont_result->setTailCall(true);

    OsrBuilder.CreateRet(cont_result);
  }

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
