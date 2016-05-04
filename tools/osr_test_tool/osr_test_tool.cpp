//===- lli.cpp - LLVM Interpreter / Dynamic compiler ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This utility provides a simple wrapper around the LLVM Execution Engines,
// which allow the direct execution of LLVM programs through a Just-In-Time
// compiler, or through an interpreter if no JIT is available for this platform.
//
// It runs some OSR passes before it executes the code.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/OSR/OsrPass.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cerrno>
#include <assert.h>

using namespace llvm;

#define DEBUG_TYPE "osr_test_tool"

int main(int argc, char **argv, char * const *envp) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  assert(1 && "what");

  LLVMContext &Context = getGlobalContext();

  // If we have a native target, initialize it to ensure it is linked in and
  // usable by the JIT.
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  // Load the bitcode...
  SMDiagnostic Err;
  std::unique_ptr<Module> Owner = parseIRFile(argv[1], Err, Context);
  Module *Mod = Owner.get();
  if (!Mod) {
    Err.print(argv[0], errs());
    return 1;
  }


  if (std::error_code EC = Mod->materializeAll()) {
    errs() << argv[0] << ": bitcode didn't read correctly.\n";
    errs() << "Reason: " << EC.message() << "\n";
    exit(1);
  }

  auto Main = Mod->getFunction("main");
  if (!Main) {
    Err.print(argv[0], errs());
    return 1;
  }

  // Now we create the JIT.
  auto InitialModule = llvm::make_unique<Module>("empty", Context);
  ExecutionEngine* EE = EngineBuilder(std::move(InitialModule)).create();
  EE->getTargetMachine()->setOptLevel(CodeGenOpt::None); // !important
  // if we let the target machine optimize, it might modify the module and break
  // the hackery that makes the osr possible
  Owner->setDataLayout(EE->getDataLayout());

  // make a pass manager
  auto PM = llvm::make_unique<legacy::PassManager>();
  PM->add(createPromoteMemoryToRegisterPass());
  PM->add(createInstructionCombiningPass());
  PM->add(createCFGSimplificationPass());
  PM->add(createOsrPassPass(EE));
  //PM->add(createDeadCodeEliminationPass());
  PM->run(*Mod);

  EE->addModule(std::move(Owner));
  EE->generateCodeForModule(Mod);
  EE->finalizeObject();

  std::vector<GenericValue> args;
  errs() << "res: " << EE->runFunction(Main, args).IntVal << "\n";

  return 0;
}
