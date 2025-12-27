//===-- Instrument.cpp ------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// New pass manager implementation for LLVM 17+
//
//===----------------------------------------------------------------------===//

#include "ModuleHelper.h"
#include "Passes.h"

#include "klee/Support/CompilerWarning.h"
#include "klee/Support/ErrorHandling.h"

DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/LowerAtomicPass.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils.h"
DISABLE_WARNING_POP

using namespace llvm;
using namespace klee;

void klee::instrument(bool CheckDivZero, bool CheckOvershift,
                      llvm::Module *module) {
  // Inject checks prior to optimization... we also perform the
  // invariant transformations that we will end up doing later so that
  // optimize is seeing what is as close as possible to the final
  // module.

  // Run RaiseAsmPass
  RaiseAsmPass raiseAsm;
  raiseAsm.runOnModule(*module);

  // Run Scalarizer pass - this will scalarize as much code as possible so that
  // the Executor does not need to handle operands of vector type for most
  // instructions other than InsertElementInst and ExtractElementInst.
  // NOTE: Must come before division/overshift checks because those passes
  // don't know how to handle vector instructions.
  legacy::PassManager pm;
  pm.add(createScalarizerPass());
  pm.add(createLowerAtomicPass());
  pm.run(*module);

  // Run div/overshift checks
  if (CheckDivZero) {
    DivCheckPass divCheck;
    divCheck.runOnModule(*module);
  }
  if (CheckOvershift) {
    OvershiftCheckPass overshiftCheck;
    overshiftCheck.runOnModule(*module);
  }

  // Run intrinsic cleaner
  const llvm::DataLayout &targetData = module->getDataLayout();
  IntrinsicCleanerPass intrinsicCleaner(targetData);
  intrinsicCleaner.runOnModule(*module);
}

void klee::checkModule(bool DontVerify, llvm::Module *module) {
  // Run verifier if requested
  if (!DontVerify) {
    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyModule(*module, &errorStream)) {
      klee_error("Module verification failed: %s", errorStr.c_str());
    }
  }

  // Run operand type check pass
  InstructionOperandTypeCheckPass operandTypeCheckPass;
  operandTypeCheckPass.runOnModule(*module);

  // Enforce the operand type invariants that the Executor expects. This
  // implicitly depends on the "Scalarizer" pass to be run in order to succeed
  // in the presence of vector instructions.
  if (!operandTypeCheckPass.checkPassed()) {
    klee_error("Unexpected instruction operand types detected");
  }
}
