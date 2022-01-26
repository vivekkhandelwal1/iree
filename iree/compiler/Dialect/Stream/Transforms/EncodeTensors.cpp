// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// TODO(benvanik): have a stream/upstream equivalent of the flow.dispatch.* ops.
#include "iree/compiler/Dialect/Flow/IR/FlowDialect.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/Stream/IR/StreamDialect.h"
#include "iree/compiler/Dialect/Stream/IR/StreamOps.h"
#include "iree/compiler/Dialect/Stream/IR/StreamTypes.h"
#include "iree/compiler/Dialect/Stream/Transforms/PassDetail.h"
#include "iree/compiler/Dialect/Stream/Transforms/Passes.h"
#include "iree/compiler/Dialect/Util/IR/UtilDialect.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "iree/compiler/Dialect/Util/IR/UtilTypes.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Stream {
namespace {

//===----------------------------------------------------------------------===//
// Encoding utilities
//===----------------------------------------------------------------------===//

// Asserts that the given encoding is supported by this code right now.
// Non-trivial dense tensor encodings need special handling.
static LogicalResult checkEncoding(Operation *op, RankedTensorType encodingType,
                                   ValueRange encodingDims,
                                   PatternRewriter &rewriter) {
  if (encodingType.getEncoding()) {
    return rewriter.notifyMatchFailure(op, [=](Diagnostic &d) {
      d << "unsupported tensor encoding: " << encodingType;
    });
  }
  return success();
}

// Returns an 8-bit aligned element byte count.
static int64_t getElementByteSize(Type elementType) {
  int64_t bitCount = elementType.getIntOrFloatBitWidth();
  int64_t byteCount = std::max<unsigned>(8, llvm::PowerOf2Ceil(bitCount)) / 8;
  return byteCount;
}

// Aligns an element type to a byte-aligned power of 2 bit width.
//
// Examples:
//   i1  -> i8
//   i4  -> i8
//   i11 -> i16
//   i33 -> i64
static Type alignElementType(Type originalType) {
  // Only handle integers; floats (today) in MLIR all have aligned widths.
  auto elementType = originalType.dyn_cast<IntegerType>();
  if (!elementType) return originalType;

  // Align the element type to a power of two byte size.
  auto alignedBitWidth = getElementByteSize(elementType) * 8;
  if (elementType.getIntOrFloatBitWidth() == alignedBitWidth) {
    // Already aligned.
    return originalType;
  }
  return IntegerType::get(elementType.getContext(), alignedBitWidth,
                          elementType.getSignedness());
}

// Aligns the element type of a tensor<> to a byte-aligned power of 2 bit width.
static RankedTensorType alignTensorType(RankedTensorType originalType) {
  auto elementType = originalType.getElementType();
  auto alignedType = alignElementType(elementType);
  if (alignedType == elementType) return originalType;
  return RankedTensorType::get(originalType.getShape(), alignedType,
                               originalType.getEncoding());
}

// Returns the element count of a tensor with optional dynamic dimensions.
// Many of these will be static and since this is used _a lot_ we do a bit of
// work to try to avoid a bunch of trivially foldable ops.
static Value calculateElementCount(Location loc, RankedTensorType tensorType,
                                   ValueRange dynamicDims, int64_t multiplier,
                                   PatternRewriter &rewriter) {
  // Calculate all static dims first, if any.
  int64_t staticCount = multiplier;
  for (unsigned i = 0; i < tensorType.getRank(); ++i) {
    if (!tensorType.isDynamicDim(i)) staticCount *= tensorType.getDimSize(i);
  }

  // Scale by dynamic dims, if present.
  auto value =
      rewriter.create<arith::ConstantIndexOp>(loc, staticCount).getResult();
  for (auto dim : dynamicDims) {
    value = rewriter.createOrFold<arith::MulIOp>(loc, value, dim);
  }
  return value;
}

// Returns a ConstantIndexOp with the value of the given dimension.
static Value makeTensorDim(Location loc, RankedTensorType tensorType,
                           ValueRange dynamicDims, unsigned i,
                           PatternRewriter &rewriter) {
  // Static dimension early-out:
  if (!tensorType.isDynamicDim(i)) {
    return rewriter.create<arith::ConstantIndexOp>(loc,
                                                   tensorType.getDimSize(i));
  }

  // Map from absolute dimension index to the compact dynamic index.
  unsigned di = 0;
  for (unsigned j = 0; j < i; ++j) {
    if (tensorType.isDynamicDim(j)) ++di;
  }
  return dynamicDims[di];
}

// Returns an element offset within a dense tensor based on indices.
// TODO(benvanik): when partially static try to avoid emitting so much IR.
static Value calculateElementOffset(Location loc, RankedTensorType tensorType,
                                    ValueRange dynamicDims, ValueRange indices,
                                    PatternRewriter &rewriter) {
  assert(indices.size() == tensorType.getRank());
  auto offset = rewriter.createOrFold<arith::ConstantIndexOp>(loc, 0);
  for (size_t i = 0; i < indices.size(); ++i) {
    auto axisOffset = indices[i];
    for (size_t j = i + 1; j < tensorType.getRank(); ++j) {
      auto axisDim = makeTensorDim(loc, tensorType, dynamicDims, j, rewriter);
      axisOffset =
          rewriter.createOrFold<arith::MulIOp>(loc, axisOffset, axisDim);
    }
    offset = rewriter.createOrFold<arith::AddIOp>(loc, offset, axisOffset);
  }
  return offset;
}

// Returns an element offset within a dense tensor based on indices, in bytes.
static Value calculateElementByteOffset(Location loc,
                                        RankedTensorType tensorType,
                                        ValueRange dynamicDims,
                                        ValueRange indices,
                                        PatternRewriter &rewriter) {
  return rewriter.createOrFold<arith::MulIOp>(
      loc,
      calculateElementOffset(loc, tensorType, dynamicDims, indices, rewriter),
      rewriter.create<arith::ConstantIndexOp>(
          loc, getElementByteSize(tensorType.getElementType())));
}

//===----------------------------------------------------------------------===//
// stream.tensor.import
//===----------------------------------------------------------------------===//

struct EncodeTensorImportOp
    : public OpRewritePattern<IREE::Stream::TensorImportOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorImportOp op,
                                PatternRewriter &rewriter) const override {
    auto resultType = op.result_encoding().cast<RankedTensorType>();
    auto resultDims = op.result_encoding_dims();
    if (failed(checkEncoding(op, resultType, resultDims, rewriter))) {
      return failure();
    }

    // TODO(benvanik): decompose this into a conditional or call to a transfer
    // utility function. Want to compare the source type (somehow) and then
    // clone or directly use the input somehow. For now we punt to HAL.

    return rewriter.notifyMatchFailure(op, "tensor import not handled");
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.export
//===----------------------------------------------------------------------===//

struct EncodeTensorExportOp
    : public OpRewritePattern<IREE::Stream::TensorExportOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorExportOp op,
                                PatternRewriter &rewriter) const override {
    auto sourceType = op.source_encoding().cast<RankedTensorType>();
    auto sourceDims = op.source_encoding_dims();
    if (failed(checkEncoding(op, sourceType, sourceDims, rewriter))) {
      return failure();
    }

    // TODO(benvanik): decompose this into a conditional or call to a transfer
    // utility function. Want to compare the source type (somehow) and then
    // clone or directly use the input somehow. For now we punt to HAL.

    return rewriter.notifyMatchFailure(op, "tensor export not handled");
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.sizeof
//===----------------------------------------------------------------------===//

struct EncodeTensorSizeOfOp
    : public OpRewritePattern<IREE::Stream::TensorSizeOfOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorSizeOfOp op,
                                PatternRewriter &rewriter) const override {
    auto encodingType = op.encoding().cast<RankedTensorType>();
    auto encodingDims = op.encoding_dims();
    if (failed(checkEncoding(op, encodingType, encodingDims, rewriter))) {
      return failure();
    }

    // Dense: element count * element size.
    auto elementByteSize = getElementByteSize(encodingType.getElementType());
    auto totalSize = calculateElementCount(
        op.getLoc(), encodingType, encodingDims, elementByteSize, rewriter);
    rewriter.replaceOp(op, totalSize);

    return success();
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.constant
//===----------------------------------------------------------------------===//

struct EncodeTensorConstantOp
    : public OpRewritePattern<IREE::Stream::TensorConstantOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorConstantOp op,
                                PatternRewriter &rewriter) const override {
    auto resultType = op.result_encoding().cast<RankedTensorType>();
    auto resultDims = op.result_encoding_dims();
    if (failed(checkEncoding(op, resultType, resultDims, rewriter))) {
      return failure();
    }

    // TODO(benvanik): compute the size based on the contents of the elements
    // and perform arbitrary unpacking logic here, such as doing partial splats/
    // scatters/etc ala run-length-encoding. Lots of models have constants that
    // are very low entropy and instead of a compression algorithm a simple RLE
    // may be enough - even if just for the suffix.

    // TODO(benvanik): bit pack and emit a __builtin_zext_i1_i8 builtin.
    // Really we should be doing bitpacking at the flow/linalg level - doing it
    // here only saves us file size as we'd have to allocate the extended memory
    // and keep it around. If we see models with large unaligned constants we
    // can make the tradeoff for minimizing file size vs minimizing startup
    // cost.

    // Sub-byte aligned constants need to be expanded to a power of 2
    // byte-aligned width. This is unfortunate: it's wasted bits in the final
    // binary that we could otherwise use productively.
    auto alignedType = alignTensorType(resultType);
    ElementsAttr encodedAttr = op.value();
    if (alignedType != resultType) {
      if (auto sourceAttr = encodedAttr.dyn_cast<DenseIntElementsAttr>()) {
        auto alignedBitWidth = alignedType.getElementTypeBitWidth();
        encodedAttr = sourceAttr.mapValues(
            alignedType.getElementType(), [=](APInt sourceValue) {
              // NOTE: this is super slow! We should be doing a conversion in
              // a loop ourselves - don't want to be mapping for millions of
              // elements.
              return sourceValue.zext(alignedBitWidth);
            });
      }
    }

    // Dense:
    auto resultSize = calculateElementCount(
        op.getLoc(), alignedType, resultDims,
        getElementByteSize(alignedType.getElementType()), rewriter);
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncConstantOp>(
        op, op.result().getType(), encodedAttr, resultSize, op.affinityAttr());

    return success();
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.splat
//===----------------------------------------------------------------------===//

// Canonicalizes a fill pattern into a power of 2 byte-aligned integer type.
// The stream dialect splat/fill ops require one of I8, I16, or I32 - any other
// type must be converted to one of those here. This prevents encoding policy
// such as what to do with i1 or float types from leaking into lower levels of
// the stack: fill ops are just setting bytes.
//
// The other reason to handle things here is that the fill pattern must be
// <= 32-bits - if it's over that we need to insert a dispatch to perform the
// fill and the only time we can do that in the pipeline is here.
// This is a somewhat annoying abstraction leak from the HAL which also has a
// 32-bit fill limit, but that is an abstraction leak from the underlying APIs
// and hardware (Metal/Vulkan/CUDA/etc) that also don't have 64-bit fills.
// Instead of forcing all runtime implementations to include emulation for
// 64-bit fills we take care of that here on an as-needed basis.
//
// Returns the pattern converted to one of [i8, i16, i32, i64] (with i64 needing
// to be handled via emulation) or nullptr if the type is unsupported.
static Value canonicalizeFillPattern(Value pattern, PatternRewriter &rewriter) {
  auto loc = pattern.getLoc();

  // Get floats into integer form.
  auto patternType = pattern.getType();
  unsigned bitWidth = patternType.getIntOrFloatBitWidth();
  if (patternType.isa<FloatType>()) {
    pattern = rewriter.createOrFold<arith::BitcastOp>(
        loc, rewriter.getIntegerType(bitWidth), pattern);
  }

  // HACK: extend i1 to i8. This is really not something we should be doing here
  // in optimized programs as this is a super shady operation.
  if (patternType.isInteger(1)) {
    return rewriter.createOrFold<arith::ExtUIOp>(loc, rewriter.getI8Type(),
                                                 pattern);
  } else if ((bitWidth % 8) != 0) {
    // We'd need some policy to determine how to handle non-byte-aligned widths.
    return {};
  }

  // 8/16/32-bit value pass through (possibly after a bitcast).
  return pattern;
}

struct EncodeTensorSplatOp
    : public OpRewritePattern<IREE::Stream::TensorSplatOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorSplatOp op,
                                PatternRewriter &rewriter) const override {
    auto resultType = op.result_encoding().cast<RankedTensorType>();
    auto resultDims = op.result_encoding_dims();
    if (failed(checkEncoding(op, resultType, resultDims, rewriter))) {
      return failure();
    }

    // Dense:

    // Canonicalize the fill pattern into one of [i8, i16, i32, i64].
    auto pattern = canonicalizeFillPattern(op.value(), rewriter);
    if (!pattern) {
      return rewriter.notifyMatchFailure(
          op, "unsupported pattern width; encoding policy required");
    } else if (pattern.getType().getIntOrFloatBitWidth() > 32) {
      // We emulate 64-bit support with a stream.builtin.splat.i64.
      rewriter.replaceOpWithNewOp<IREE::Stream::BuiltinSplatI64Op>(
          op, op.result().getType(), pattern, op.result_size(),
          op.affinityAttr());
    } else {
      rewriter.replaceOpWithNewOp<IREE::Stream::AsyncSplatOp>(
          op, op.result().getType(), pattern, op.result_size(),
          op.affinityAttr());
    }

    return success();
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.clone
//===----------------------------------------------------------------------===//

struct EncodeTensorCloneOp
    : public OpRewritePattern<IREE::Stream::TensorCloneOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorCloneOp op,
                                PatternRewriter &rewriter) const override {
    auto sourceType = op.source_encoding().cast<RankedTensorType>();
    auto sourceDims = op.source_encoding_dims();
    if (failed(checkEncoding(op, sourceType, sourceDims, rewriter))) {
      return failure();
    }
    auto resultType = op.result_encoding().cast<RankedTensorType>();
    auto resultDims = op.result_encoding_dims();
    if (failed(checkEncoding(op, resultType, resultDims, rewriter))) {
      return failure();
    }

    // Dense:
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncCloneOp>(
        op, op.result().getType(), op.source(), op.source_size(),
        op.result_size(), op.affinityAttr());

    return success();
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.slice
//===----------------------------------------------------------------------===//

struct EncodeTensorSliceOp
    : public OpRewritePattern<IREE::Stream::TensorSliceOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorSliceOp op,
                                PatternRewriter &rewriter) const override {
    auto sourceType = op.source_encoding().cast<RankedTensorType>();
    auto sourceDims = op.source_encoding_dims();
    if (failed(checkEncoding(op, sourceType, sourceDims, rewriter))) {
      return failure();
    }
    auto resultType = op.result_encoding().cast<RankedTensorType>();
    auto resultDims = op.result_encoding_dims();
    if (failed(checkEncoding(op, resultType, resultDims, rewriter))) {
      return failure();
    }

    // Dense:
    auto sourceOffset = calculateElementByteOffset(
        op.getLoc(), sourceType, sourceDims, op.start_indices(), rewriter);
    auto sourceEnd = rewriter.createOrFold<arith::AddIOp>(
        op.getLoc(), sourceOffset, op.result_size());
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncSliceOp>(
        op, op.result().getType(), op.source(), op.source_size(), sourceOffset,
        sourceEnd, op.result_size(), op.affinityAttr());

    return success();
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.fill
//===----------------------------------------------------------------------===//

struct EncodeTensorFillOp
    : public OpRewritePattern<IREE::Stream::TensorFillOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorFillOp op,
                                PatternRewriter &rewriter) const override {
    auto targetType = op.target_encoding().cast<RankedTensorType>();
    auto targetDims = op.target_encoding_dims();
    if (failed(checkEncoding(op, targetType, targetDims, rewriter))) {
      return failure();
    }

    // Dense:
    auto targetOffset = calculateElementByteOffset(
        op.getLoc(), targetType, targetDims, op.start_indices(), rewriter);
    auto targetLength = calculateElementByteOffset(
        op.getLoc(), targetType, targetDims, op.lengths(), rewriter);
    auto targetEnd = rewriter.createOrFold<arith::AddIOp>(
        op.getLoc(), targetOffset, targetLength);

    // Canonicalize the fill pattern into one of [i8, i16, i32, i64].
    auto pattern = canonicalizeFillPattern(op.value(), rewriter);
    if (!pattern) {
      return rewriter.notifyMatchFailure(
          op, "unsupported pattern width; encoding policy required");
    } else if (pattern.getType().getIntOrFloatBitWidth() > 32) {
      rewriter.replaceOpWithNewOp<IREE::Stream::BuiltinFillI64Op>(
          op, op.result().getType(), op.target(), op.target_size(),
          targetOffset, targetEnd, targetLength, pattern, op.affinityAttr());
    } else {
      rewriter.replaceOpWithNewOp<IREE::Stream::AsyncFillOp>(
          op, op.result().getType(), op.target(), op.target_size(),
          targetOffset, targetEnd, targetLength, pattern, op.affinityAttr());
    }

    return success();
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.update
//===----------------------------------------------------------------------===//

struct EncodeTensorUpdateOp
    : public OpRewritePattern<IREE::Stream::TensorUpdateOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorUpdateOp op,
                                PatternRewriter &rewriter) const override {
    auto updateType = op.update_encoding().cast<RankedTensorType>();
    auto updateDims = op.update_encoding_dims();
    if (failed(checkEncoding(op, updateType, updateDims, rewriter))) {
      return failure();
    }
    auto targetType = op.target_encoding().cast<RankedTensorType>();
    auto targetDims = op.target_encoding_dims();
    if (failed(checkEncoding(op, targetType, targetDims, rewriter))) {
      return failure();
    }

    // Dense:
    auto targetOffset = calculateElementByteOffset(
        op.getLoc(), targetType, targetDims, op.start_indices(), rewriter);
    auto targetEnd = rewriter.createOrFold<arith::AddIOp>(
        op.getLoc(), targetOffset, op.update_size());
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncUpdateOp>(
        op, op.result().getType(), op.target(), op.target_size(), targetOffset,
        targetEnd, op.update(), op.update_size(), op.affinityAttr());

    return success();
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.load
//===----------------------------------------------------------------------===//

struct EncodeTensorLoadOp
    : public OpRewritePattern<IREE::Stream::TensorLoadOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorLoadOp op,
                                PatternRewriter &rewriter) const override {
    auto sourceType = op.source_encoding().cast<RankedTensorType>();
    auto sourceDims = op.source_encoding_dims();
    if (failed(checkEncoding(op, sourceType, sourceDims, rewriter))) {
      return failure();
    }

    // Dense:
    auto sourceOffset = calculateElementByteOffset(
        op.getLoc(), sourceType, sourceDims, op.indices(), rewriter);
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncLoadOp>(
        op, op.result().getType(), op.source(), op.source_size(), sourceOffset);

    return success();
  }
};

//===----------------------------------------------------------------------===//
// stream.tensor.store
//===----------------------------------------------------------------------===//

struct EncodeTensorStoreOp
    : public OpRewritePattern<IREE::Stream::TensorStoreOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::TensorStoreOp op,
                                PatternRewriter &rewriter) const override {
    auto targetType = op.target_encoding().cast<RankedTensorType>();
    auto targetDims = op.target_encoding_dims();
    if (failed(checkEncoding(op, targetType, targetDims, rewriter))) {
      return failure();
    }

    // Dense:
    auto targetOffset = calculateElementByteOffset(
        op.getLoc(), targetType, targetDims, op.indices(), rewriter);
    rewriter.replaceOpWithNewOp<IREE::Stream::AsyncStoreOp>(
        op, op.target(), op.target_size(), targetOffset, op.value());

    return success();
  }
};

//===----------------------------------------------------------------------===//
// -iree-stream-encode-host-tensors
//===----------------------------------------------------------------------===//

class EncodeHostTensorsPass
    : public EncodeHostTensorsBase<EncodeHostTensorsPass> {
 public:
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<mlir::StandardOpsDialect>();
    registry.insert<mlir::arith::ArithmeticDialect>();
    registry.insert<IREE::Stream::StreamDialect>();
    registry.insert<IREE::Util::UtilDialect>();
  }

  void runOnOperation() override {
    OwningRewritePatternList patterns(&getContext());
    patterns.insert<
        EncodeTensorImportOp, EncodeTensorExportOp, EncodeTensorSizeOfOp,
        EncodeTensorConstantOp, EncodeTensorSplatOp, EncodeTensorCloneOp,
        EncodeTensorSliceOp, EncodeTensorFillOp, EncodeTensorUpdateOp,
        EncodeTensorLoadOp, EncodeTensorStoreOp>(&getContext());
    FrozenRewritePatternSet frozenPatterns(std::move(patterns));
    if (failed(applyPatternsAndFoldGreedily(getOperation(), frozenPatterns))) {
      return signalPassFailure();
    }
  }
};

//===----------------------------------------------------------------------===//
// stream.binding.subspan
//===----------------------------------------------------------------------===//

// Aligns the element type of a !flow.dispatch.tensor<> to a byte-aligned power
// of 2 bit width.
static IREE::Flow::DispatchTensorType alignDispatchTensorType(
    IREE::Flow::DispatchTensorType originalType) {
  auto elementType = originalType.getElementType();
  auto alignedType = alignElementType(elementType);
  if (alignedType == elementType) return originalType;
  return IREE::Flow::DispatchTensorType::get(
      originalType.getAccess(), originalType.getShape(), alignedType);
}

// Aligns binding element types to power-of-two byte boundaries.
// The loads and stores to the binding will need to be updated to perform the
// truncation and extension as required.
//
// We could do more handling here; today we are just doing sub-byte alignment
// conversion to ensure both host and device agree upon the number of bytes in
// a resource.
struct EncodeBindingSubspanOp
    : public OpRewritePattern<IREE::Stream::BindingSubspanOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Stream::BindingSubspanOp op,
                                PatternRewriter &rewriter) const override {
    auto originalType =
        op.result().getType().dyn_cast<IREE::Flow::DispatchTensorType>();
    if (!originalType) {
      return rewriter.notifyMatchFailure(op, "binding type not supported");
    }

    // Align the element type, if needed.
    auto alignedType = alignDispatchTensorType(originalType);
    if (originalType == alignedType) return failure();  // already aligned.

    // Directly swap the type with the one, changing all uses in the IR.
    // This works because
    rewriter.updateRootInPlace(op, [&]() { op.result().setType(alignedType); });

    return success();
  }
};

//===----------------------------------------------------------------------===//
// flow.dispatch.tensor.load
//===----------------------------------------------------------------------===//

struct EncodeDispatchTensorLoadOp
    : public OpRewritePattern<IREE::Flow::DispatchTensorLoadOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Flow::DispatchTensorLoadOp op,
                                PatternRewriter &rewriter) const override {
    auto targetType = op.result().getType().cast<RankedTensorType>();

    // Align the element type, if needed.
    auto alignedType = alignTensorType(targetType);
    if (targetType == alignedType) return failure();  // already aligned.

    // Loads always truncate from an byte aligned type to a sub-byte one.
    assert(targetType.getElementTypeBitWidth() <
               alignedType.getElementTypeBitWidth() &&
           "loads must truncate");

    // Truncate the byte -> sub-byte type; e.g. i8 -> i1.
    auto loadedValue = op.getResult();
    rewriter.setInsertionPointAfterValue(loadedValue);
    auto truncOp =
        rewriter.create<arith::TruncIOp>(op.getLoc(), targetType, loadedValue);
    rewriter.updateRootInPlace(op, [&]() {
      loadedValue.replaceAllUsesExcept(truncOp, truncOp);
      loadedValue.setType(alignedType);
    });
    return success();
  }
};

//===----------------------------------------------------------------------===//
// flow.dispatch.tensor.store
//===----------------------------------------------------------------------===//

struct EncodeDispatchTensorStoreOp
    : public OpRewritePattern<IREE::Flow::DispatchTensorStoreOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(IREE::Flow::DispatchTensorStoreOp op,
                                PatternRewriter &rewriter) const override {
    auto sourceType = op.value().getType().cast<RankedTensorType>();

    // Align the element type, if needed.
    auto alignedType = alignTensorType(sourceType);
    if (sourceType == alignedType) return failure();  // already aligned.

    // Stores always extend from a sub-byte aligned type to a byte aligned one.
    assert(sourceType.getElementTypeBitWidth() <
               alignedType.getElementTypeBitWidth() &&
           "stores must extend");

    // Extend the sub-byte -> byte type; e.g. i1 -> i8.
    auto extOp =
        rewriter.create<arith::ExtUIOp>(op.getLoc(), alignedType, op.value());
    rewriter.updateRootInPlace(
        op, [&]() { op.valueMutable().assign(extOp.getResult()); });
    return success();
  }
};

//===----------------------------------------------------------------------===//
// -iree-stream-encode-device-tensors
//===----------------------------------------------------------------------===//

class EncodeDeviceTensorsPass
    : public EncodeDeviceTensorsBase<EncodeDeviceTensorsPass> {
 public:
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<mlir::StandardOpsDialect>();
    registry.insert<mlir::arith::ArithmeticDialect>();
    registry.insert<IREE::Flow::FlowDialect>();
    registry.insert<IREE::Stream::StreamDialect>();
    registry.insert<IREE::Util::UtilDialect>();
  }

  void runOnOperation() override {
    OwningRewritePatternList patterns(&getContext());
    patterns.insert<EncodeBindingSubspanOp, EncodeDispatchTensorLoadOp,
                    EncodeDispatchTensorStoreOp>(&getContext());
    FrozenRewritePatternSet frozenPatterns(std::move(patterns));
    if (failed(applyPatternsAndFoldGreedily(getOperation(), frozenPatterns))) {
      return signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<OperationPass<>> createEncodeHostTensorsPass() {
  return std::make_unique<EncodeHostTensorsPass>();
}

std::unique_ptr<OperationPass<>> createEncodeDeviceTensorsPass() {
  return std::make_unique<EncodeDeviceTensorsPass>();
}

}  // namespace Stream
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
