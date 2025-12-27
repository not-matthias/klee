// FIXME: This file is a bastard child of opt.cpp and llvm-ld's
// Optimize.cpp. This stuff should live in common code.

//===- Optimize.cpp - Optimize a complete program -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements all optimization of the linked module for llvm-ld.
// New pass manager implementation for LLVM 17+.
//
//===----------------------------------------------------------------------===//

#include "ModuleHelper.h"
#include "Passes.h"

#include "klee/Config/Version.h"
#include "klee/Support/OptionCategories.h"

#include "klee/Support/CompilerWarning.h"

DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/IPO/StripSymbols.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils.h"
DISABLE_WARNING_POP

using namespace llvm;
using namespace klee;

namespace {
static cl::opt<bool>
    DisableInline("disable-inlining",
                  cl::desc("Do not run the inliner pass (default=false)"),
                  cl::init(false), cl::cat(klee::ModuleCat));

static cl::opt<bool> DisableInternalize(
    "disable-internalize",
    cl::desc("Do not mark all symbols as internal (default=false)"),
    cl::init(false), cl::cat(klee::ModuleCat));

static cl::opt<bool> VerifyEach("verify-each",
                                cl::desc("Verify intermediate results of all "
                                         "optimization passes (default=false)"),
                                cl::init(false), cl::cat(klee::ModuleCat));

static cl::opt<bool>
    Strip("strip-all",
          cl::desc("Strip all symbol information from executable (default=false)"),
          cl::init(false), cl::cat(klee::ModuleCat));

static cl::opt<bool>
    StripDebug("strip-debug",
               cl::desc("Strip debugger symbol info from executable (default=false)"),
               cl::init(false), cl::cat(klee::ModuleCat));

} // namespace

/// Optimize - Perform link time optimizations. This will run the scalar
/// optimizations, any loaded plugin-optimization modules, and then the
/// inter-procedural optimizations if applicable.
void klee::optimizeModule(llvm::Module *M,
                          llvm::ArrayRef<const char *> preservedFunctions) {
  // Create the analysis managers
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Create the pass builder and register analysis passes
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // Build optimization pipeline using O2 level
  OptimizationLevel Level = DisableInline ? OptimizationLevel::O1
                                          : OptimizationLevel::O2;
  ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(Level);

  // Add internalization if enabled
  if (!DisableInternalize) {
    auto PreserveFunctions = [=](const llvm::GlobalValue &GV) {
      StringRef GVName = GV.getName();
      for (const char *fun : preservedFunctions)
        if (GVName == fun)
          return true;
      return false;
    };
    MPM.addPass(InternalizePass(PreserveFunctions));
  }

  // Add global DCE to clean up after internalization
  MPM.addPass(GlobalDCEPass());

  // If the -s or -S command line options were specified, strip the symbols
  if (Strip) {
    MPM.addPass(StripSymbolsPass());
  } else if (StripDebug) {
    MPM.addPass(StripDebugDeclarePass());
  }

  // Final cleanup passes
  MPM.addPass(GlobalDCEPass());

  // Run the pipeline
  MPM.run(*M, MAM);

  // Verify if requested
  if (VerifyEach) {
    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyModule(*M, &errorStream)) {
      llvm::errs() << "Verification failed after optimization: " << errorStr << "\n";
    }
  }
}

void klee::optimiseAndPrepare(bool OptimiseKLEECall, bool Optimize,
                              SwitchImplType SwitchType, std::string EntryPoint,
                              llvm::ArrayRef<const char *> preservedFunctions,
                              llvm::Module *module) {
  // Preserve all functions containing klee-related function calls from being
  // optimised around
  if (!OptimiseKLEECall) {
    OptNonePass optNone;
    optNone.runOnModule(*module);
  }

  if (Optimize)
    optimizeModule(module, preservedFunctions);

  // Needs to happen after linking (since ctors/dtors can be modified)
  // and optimization (since global optimization can rewrite lists).
  injectStaticConstructorsAndDestructors(module, EntryPoint);

  // Finally, run the passes that maintain invariants we expect during
  // interpretation. We run the intrinsic cleaner just in case we
  // linked in something with intrinsics but any external calls are
  // going to be unresolved. We really need to handle the intrinsics
  // directly I think?
  legacy::PassManager pm3;
  pm3.add(createCFGSimplificationPass());

  switch (SwitchType) {
  case SwitchImplType::eSwitchTypeInternal:
    break;
  case SwitchImplType::eSwitchTypeSimple:
    pm3.add(new LowerSwitchPass());
    break;
  case SwitchImplType::eSwitchTypeLLVM:
    pm3.add(createLowerSwitchPass());
    break;
  }
  pm3.run(*module);

  // Run remaining passes directly
  const llvm::DataLayout &targetData = module->getDataLayout();
  IntrinsicCleanerPass intrinsicCleaner(targetData);
  intrinsicCleaner.runOnModule(*module);

  // Run scalarizer
  legacy::PassManager pm4;
  pm4.add(createScalarizerPass());
  pm4.run(*module);

  // Run phi cleaner and function alias
  PhiCleanerPass phiCleaner;
  for (auto &F : *module) {
    if (!F.isDeclaration()) {
      phiCleaner.runOnFunction(F);
    }
  }

  FunctionAliasPass functionAlias;
  functionAlias.runOnModule(*module);
}
