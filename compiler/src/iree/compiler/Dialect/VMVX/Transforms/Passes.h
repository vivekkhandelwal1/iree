// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_COMPILER_DIALECT_VMVX_TRANSFORMS_PASSES_H_
#define IREE_COMPILER_DIALECT_VMVX_TRANSFORMS_PASSES_H_

#include "iree/compiler/Dialect/VMVX/IR/VMVXOps.h"
#include "llvm/ADT/StringMap.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/LLVM.h"

namespace mlir::iree_compiler::IREE::VMVX {

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

// Adds a set of passes to the given pass manager that configure the required
// VMVX transforms and tiling parameters.
void buildVMVXConfigurationPassPipeline(OpPassManager &variantPassManager);

// Adds a set of passes to the given pass manager that run the required VMVX
// transforms in the canonical order.
//
// Most translation code should prefer to use this instead of manually adding
// the passes themselves to ensure that expected pass ordering is observed.
//
// The expected usage is:
//   <run conversion from TF/HLO/etc to flow>
//   buildVMVXConfigurationPassPipeline & run
//   buildVMVXTransformPassPipeline & run
//   <serialize VM module>
void buildVMVXTransformPassPipeline(OpPassManager &variantPassManager);

//===----------------------------------------------------------------------===//
// Register all Passes
//===----------------------------------------------------------------------===//

#define GEN_PASS_DECL
#include "iree/compiler/Dialect/VMVX/Transforms/Passes.h.inc"

void registerVMVXPasses();

} // namespace mlir::iree_compiler::IREE::VMVX

#endif // IREE_COMPILER_DIALECT_VMVX_TRANSFORMS_PASSES_H_
