################################################################################
# Autogenerated by build_tools/bazel_to_cmake/bazel_to_cmake.py from           #
# compiler/src/iree/compiler/Codegen/Dialect/GPU/IR/BUILD.bazel                #
#                                                                              #
# Use iree_cmake_extra_content from iree/build_defs.oss.bzl to add arbitrary   #
# CMake-only content.                                                          #
#                                                                              #
# To disable autogeneration for this file entirely, delete this header.        #
################################################################################

iree_add_all_subdirs()

iree_cc_library(
  NAME
    IREEGPUDialect
  HDRS
    "DerivedConfigUtils.h"
    "GPULoweringConfigUtils.h"
    "GPUTileSwizzleUtils.h"
    "IREEGPUAttrs.h"
    "IREEGPUDialect.h"
    "IREEGPUEnums.h"
    "IREEGPUInterfaces.h"
    "IREEGPUOps.h"
  TEXTUAL_HDRS
    "IREEGPUAttrs.cpp.inc"
    "IREEGPUAttrs.h.inc"
    "IREEGPUDialect.cpp.inc"
    "IREEGPUDialect.h.inc"
    "IREEGPUEnums.cpp.inc"
    "IREEGPUEnums.h.inc"
    "IREEGPUInterfaces.cpp.inc"
    "IREEGPUInterfaces.h.inc"
    "IREEGPUOps.cpp.inc"
    "IREEGPUOps.h.inc"
  SRCS
    "DerivedConfigUtils.cpp"
    "GPULoweringConfigUtils.cpp"
    "GPUTileSwizzleUtils.cpp"
    "IREEGPUAttrs.cpp"
    "IREEGPUDialect.cpp"
    "IREEGPUInterfaces.cpp"
    "IREEGPUOps.cpp"
    "PromotionImpls.cpp"
  DEPS
    ::IREEGPUAttrs
    ::IREEGPUDialectGen
    ::IREEGPUEnums
    ::IREEGPUInterfaces
    ::IREEGPUOpsGen
    LLVMSupport
    MLIRAMDGPUDialect
    MLIRAffineDialect
    MLIRArithDialect
    MLIRArithUtils
    MLIRControlFlowInterfaces
    MLIRGPUDialect
    MLIRIR
    MLIRLinalgDialect
    MLIRParser
    MLIRSCFDialect
    MLIRSideEffectInterfaces
    MLIRSupport
    MLIRTensorDialect
    MLIRTilingInterface
    MLIRVectorDialect
    iree::compiler::Codegen::Dialect::Codegen::IR::IREECodegenDialect
    iree::compiler::Codegen::Dialect::Codegen::Utils
    iree::compiler::Codegen::Dialect::VectorExt::IR::IREEVectorExtDialect
    iree::compiler::Codegen::Utils::VectorOpUtils
    iree::compiler::Dialect::Encoding::Utils
    iree::compiler::Dialect::LinalgExt::IR
    iree::compiler::Dialect::TensorExt::IR
    iree::compiler::Utils
    iree::compiler::bindings::c::headers
  PUBLIC
)

iree_tablegen_library(
  NAME
    IREEGPUDialectGen
  TD_FILE
    "IREEGPUDialect.td"
  OUTS
    --gen-dialect-decls IREEGPUDialect.h.inc
    --gen-dialect-defs IREEGPUDialect.cpp.inc
)

iree_tablegen_library(
  NAME
    IREEGPUEnums
  TD_FILE
    "IREEGPUEnums.td"
  OUTS
    --gen-enum-decls IREEGPUEnums.h.inc
    --gen-enum-defs IREEGPUEnums.cpp.inc
)

iree_tablegen_library(
  NAME
    IREEGPUAttrs
  TD_FILE
    "IREEGPUAttrs.td"
  OUTS
    --gen-attrdef-decls IREEGPUAttrs.h.inc
    --gen-attrdef-defs IREEGPUAttrs.cpp.inc
)

iree_tablegen_library(
  NAME
    IREEGPUInterfaces
  TD_FILE
    "IREEGPUInterfaces.td"
  OUTS
    --gen-attr-interface-decls IREEGPUInterfaces.h.inc
    --gen-attr-interface-defs IREEGPUInterfaces.cpp.inc
)

iree_tablegen_library(
  NAME
    IREEGPUOpsGen
  TD_FILE
    "IREEGPUOps.td"
  OUTS
    --gen-op-decls IREEGPUOps.h.inc
    --gen-op-defs IREEGPUOps.cpp.inc
)

iree_tablegen_doc(
  NAME
    IREEGPUDialectDocGen
  CATEGORY
    "Dialects"
  TD_FILE
    "IREEGPUOps.td"
  OUTS
    --gen-dialect-doc --dialect=iree_gpu IREEGPUDialect.md
)

### BAZEL_TO_CMAKE_PRESERVES_ALL_CONTENT_BELOW_THIS_LINE ###
