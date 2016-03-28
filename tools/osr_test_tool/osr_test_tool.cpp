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
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cerrno>

using namespace llvm;

#define DEBUG_TYPE "osr_test_tool"

//===----------------------------------------------------------------------===//
// main Driver function
//
int main(int argc, char **argv, char * const *envp) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

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


  // can't use owner anymore, we've moved it
  auto Main = Mod->getFunction("main");
  if (!Main) {
    Err.print(argv[0], errs());
    return 1;
  }

  // Now we create the JIT.
  ExecutionEngine* EE = EngineBuilder(std::move(Owner)).create();
  EE->generateCodeForModule(Mod);

  // even for things like malloc this fails
  int (*ptr)() = ( int (*)() ) EE->getFunctionAddress("malloc");
  errs() << "ptr is: " << (void*)ptr << "\n"; // this is always null
  errs() << *Main << "\n";
  errs() << *Mod << "\n";

  errs() << ptr() << "\n";

  errs() << "made it\n";

  return 0;
}
