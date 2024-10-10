//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "torch-mlir/Conversion/TorchToLinalg/TorchToLinalg.h"

#include "PopulatePatterns.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/IR/Matchers.h"
#include "torch-mlir/Conversion/TorchToLinalg/Utils.h"
#include "torch-mlir/Conversion/Utils/Utils.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/Utils/TorchUpstream.h"
#include "torch-mlir/Dialect/Torch/Utils/Utils.h"
#include <algorithm>

using namespace mlir;
using namespace mlir::torch;
using namespace mlir::torch::Torch;

namespace {

static void getZeroPoint(Value value, Value &zeropoint) {
  if (auto make = value.getDefiningOp<Aten_MakePerTensorQuantizedTensorOp>()) {
    zeropoint = make.getZeroPoint();
  }
}

// for uint8 types, we shift down by 128 so that we can faithfully
// represent the quantization with signed i8 types.
static void signShift(PatternRewriter &rewriter, Location loc, Value &arg,
                      Value &zp, bool isUnsignedType, int64_t numBits) {
  if (!isUnsignedType)
    return;
  int64_t minSI = -(1 << (numBits - 1));
  Value minSIValue = rewriter.create<arith::ConstantIntOp>(
      loc, minSI, cast<mlir::IntegerType>(zp.getType()).getWidth());
  zp = rewriter.create<arith::AddIOp>(loc, zp, minSIValue);
  minSIValue = rewriter.create<arith::ConstantIntOp>(loc, minSI, numBits);
  arg = torch_to_linalg::createElementwiseLinalgGeneric(
      rewriter, loc, ValueRange{arg},
      cast<TensorType>(arg.getType()).getElementType(),
      [&](OpBuilder &b, Location loc, ValueRange payloadArgs) {
        Value result =
            rewriter.create<arith::AddIOp>(loc, payloadArgs[0], minSIValue);
        b.create<linalg::YieldOp>(loc, result);
      });
}

static Value transposeValue(Location loc, Value value, ArrayRef<int64_t> perms,
                            PatternRewriter &rewriter) {
  auto valueTy = cast<RankedTensorType>(value.getType());
  auto inShape = valueTy.getShape();
  llvm::SmallVector<int64_t> outShape;
  llvm::SmallVector<Value> dynDims;
  for (size_t i = 0; i < perms.size(); ++i) {
    outShape.push_back(inShape[perms[i]]);
    if (ShapedType::isDynamic(inShape[perms[i]])) {
      dynDims.push_back(rewriter.create<tensor::DimOp>(loc, value, perms[i]));
    }
  }

  auto outTy = RankedTensorType::get(outShape, valueTy.getElementType());
  Value empty = rewriter.create<tensor::EmptyOp>(loc, outTy, dynDims);
  Value transpose =
      rewriter.create<linalg::TransposeOp>(loc, value, empty, perms)
          ->getResult(0);
  return transpose;
}

class ConvertAtenMmOp : public OpConversionPattern<AtenMmOp> {
public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult
  matchAndRewrite(AtenMmOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value lhs = adaptor.getSelf();
    Value rhs = adaptor.getMat2();

    // A user can write an errorneous program where `aten.mm` is in fact called
    // with operands of invalid rank or dtype. We cannot convert to linalg in
    // this case or we will get a verifier error, which corresponds to breaking
    // of *internal* compiler invariants, and for a user manifests as a compiler
    // crash in the worst case (such as we try to canonicalize/fold/print the
    // invalid op before the verifier gets to see it -- also release builds of a
    // mature compiler usually have the verifier turned off for compile time
    // reasons).
    //
    // The compiler cannot crash even if the user wrote an erroneous program!
    if (failed(verifyLinalgCompatibleTypes(op, rewriter)))
      return failure();

    RankedTensorType lhsType = cast<RankedTensorType>(lhs.getType());
    RankedTensorType rhsType = cast<RankedTensorType>(rhs.getType());

    if (lhsType.getRank() != 2 || rhsType.getRank() != 2) {
      return rewriter.notifyMatchFailure(
          op, "expected both operands to aten.mm to be rank 2");
    }

    ValueTensorType lhsTorchType =
        cast<ValueTensorType>(op.getSelf().getType());
    ValueTensorType rhsTorchType =
        cast<ValueTensorType>(op.getMat2().getType());

    Value lhsZeroPoint, rhsZeroPoint;
    getZeroPoint(op.getSelf(), lhsZeroPoint);
    getZeroPoint(op.getMat2(), rhsZeroPoint);

    if (static_cast<bool>(lhsZeroPoint) != static_cast<bool>(rhsZeroPoint)) {
      return rewriter.notifyMatchFailure(
          op, "unsupported: aten.mm with mixed quantization");
    }

    if (lhsTorchType.getDtype() != rhsTorchType.getDtype()) {
      if (!lhsZeroPoint) {
        return rewriter.notifyMatchFailure(
            op, "unsupported: aten.mm with different input element types");
      }
      // Allows quantized types to mismatch since they will be cast to the same
      // type.
    }

    bool isUnsigned = torch_to_linalg::isUnsignedTorchType(lhsTorchType);
    bool isUnsignedR = torch_to_linalg::isUnsignedTorchType(rhsTorchType);

    Value lhsDim0 = rewriter.create<tensor::DimOp>(loc, lhs, 0);
    Value rhsDim1 = rewriter.create<tensor::DimOp>(loc, rhs, 1);

    if (!isAssumingStrictSymbolicShapes(rewriter)) {
      Value lhsDim1 = rewriter.create<tensor::DimOp>(loc, lhs, 1);
      Value rhsDim0 = rewriter.create<tensor::DimOp>(loc, rhs, 0);
      Value contractingDimEqual = rewriter.create<arith::CmpIOp>(
          loc, arith::CmpIPredicate::eq, lhsDim1, rhsDim0);
      rewriter.create<cf::AssertOp>(
          loc, contractingDimEqual,
          rewriter.getStringAttr(
              "mismatching contracting dimension for torch.aten.mm"));
    }

    TensorType resultType =
        cast<TensorType>(getTypeConverter()->convertType(op.getType()));
    Type elementType = resultType.getElementType();
    auto accumulatorDType =
        getDefaultAccType(rewriter, lhsType.getElementType());
    if (accumulatorDType != resultType.getElementType()) {
      elementType = accumulatorDType;
    }
    Value zeroFill = createZeroInitTensor(
        rewriter, loc, ValueRange{lhsDim0, rhsDim1}, elementType);

    Value matmul;
    if (lhsZeroPoint) {
      lhsZeroPoint = typeConverter->materializeTargetConversion(
          rewriter, loc,
          getTypeConverter()->convertType(lhsZeroPoint.getType()),
          lhsZeroPoint);
      rhsZeroPoint = typeConverter->materializeTargetConversion(
          rewriter, loc,
          getTypeConverter()->convertType(rhsZeroPoint.getType()),
          rhsZeroPoint);
      lhsZeroPoint = rewriter.create<arith::TruncIOp>(
          loc, rewriter.getI32Type(), lhsZeroPoint);
      rhsZeroPoint = rewriter.create<arith::TruncIOp>(
          loc, rewriter.getI32Type(), rhsZeroPoint);

      // change uint8 quantization -> int8 quantization
      int64_t numBits =
          cast<mlir::IntegerType>(lhsType.getElementType()).getWidth();
      signShift(rewriter, loc, lhs, lhsZeroPoint, isUnsigned, numBits);
      numBits = cast<mlir::IntegerType>(rhsType.getElementType()).getWidth();
      signShift(rewriter, loc, rhs, rhsZeroPoint, isUnsignedR, numBits);

      matmul =
          rewriter
              .create<linalg::QuantizedMatmulOp>(
                  loc, zeroFill.getType(),
                  ValueRange{lhs, rhs, lhsZeroPoint, rhsZeroPoint}, zeroFill)
              .getResult(0);
    } else if (isUnsigned) {
      auto matmulOp = rewriter.create<linalg::MatmulOp>(
          loc, zeroFill.getType(), ValueRange{lhs, rhs}, zeroFill);
      matmulOp.setCast(linalg::TypeFn::cast_unsigned);
      matmul = matmulOp->getResult(0);
    } else {
      matmul = rewriter
                   .create<linalg::MatmulOp>(loc, zeroFill.getType(),
                                             ValueRange{lhs, rhs}, zeroFill)
                   .getResult(0);
    }

    if (accumulatorDType != resultType.getElementType()) {
      matmul = torch_to_linalg::convertTensorToElementType(
          rewriter, loc, matmul, resultType.getElementType());
    }
    // When constructed with just dynamic sizes, EmptyOp will have a result
    // type which has all `?`'s for dimensions, which might not be the result
    // type of `op`. The constraints on later linalg ops means that the result
    // of the MatmulOp will have this type too. So cast it to the desired type
    // so that in the end we have the original result type.
    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, resultType, matmul);

    return success();
  }
};
} // namespace

namespace {
class ConvertAtenFlipOp : public OpConversionPattern<AtenFlipOp> {
public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult
  matchAndRewrite(AtenFlipOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    Location loc = op->getLoc();
    Value self = adaptor.getSelf();
    auto selfRank =
        cast<RankedTensorType>(adaptor.getSelf().getType()).getRank();

    SmallVector<int64_t> axis;
    if (!matchPattern(adaptor.getDims(), m_TorchListOfConstantInts(axis)))
      return rewriter.notifyMatchFailure(op,
                                         "only constant dim lists supported");
    for (unsigned i = 0, e = axis.size(); i < e; i++) {
      axis[i] = toPositiveDim(axis[i], selfRank);
      if (!isValidDim(axis[i], selfRank)) {
        return rewriter.notifyMatchFailure(op, "axis is statically invalid");
      }
    }

    Value flipped = torch_to_linalg::flipTensor(rewriter, loc, self, axis);
    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, self.getType(), flipped);
    return success();
  }
};
} // namespace

namespace {
class ConvertAtenMatmulOp : public OpConversionPattern<AtenMatmulOp> {
public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult
  matchAndRewrite(AtenMatmulOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value lhs = adaptor.getSelf();
    Value rhs = adaptor.getOther();

    if (failed(verifyLinalgCompatibleTypes(op, rewriter))) {
      return failure();
    }
    auto lhsType = cast<RankedTensorType>(lhs.getType());
    auto rhsType = cast<RankedTensorType>(rhs.getType());

    auto lhsTorchType = cast<ValueTensorType>(op.getSelf().getType());
    auto rhsTorchType = cast<ValueTensorType>(op.getOther().getType());

    // Get the rank of both matrix.
    unsigned lhsRank = lhsType.getRank();
    unsigned rhsRank = rhsType.getRank();

    Value lhsZeroPoint, rhsZeroPoint;
    getZeroPoint(op.getSelf(), lhsZeroPoint);
    getZeroPoint(op.getOther(), rhsZeroPoint);

    if (static_cast<bool>(lhsZeroPoint) != static_cast<bool>(rhsZeroPoint)) {
      return rewriter.notifyMatchFailure(
          op, "unsupported: aten.matmul with mixed quantization");
    }

    bool isUnsigned = torch_to_linalg::isUnsignedTorchType(lhsTorchType);
    bool isUnsignedR = torch_to_linalg::isUnsignedTorchType(rhsTorchType);

    if (!lhsZeroPoint && lhsTorchType.getDtype() != rhsTorchType.getDtype()) {
      // Allows quantized types to mismatch
      return rewriter.notifyMatchFailure(
          op, "unsupported: aten.matmul with different input element types");
    }

    Type newResultType = getTypeConverter()->convertType(op.getType());
    auto resultType = cast<RankedTensorType>(newResultType);
    Type elementType = resultType.getElementType();

    if (lhsZeroPoint) {
      // get each zero point ready to pass to a quantized_matmul
      lhsZeroPoint = typeConverter->materializeTargetConversion(
          rewriter, loc,
          getTypeConverter()->convertType(lhsZeroPoint.getType()),
          lhsZeroPoint);
      rhsZeroPoint = typeConverter->materializeTargetConversion(
          rewriter, loc,
          getTypeConverter()->convertType(rhsZeroPoint.getType()),
          rhsZeroPoint);
      lhsZeroPoint = rewriter.create<arith::TruncIOp>(
          loc, rewriter.getI32Type(), lhsZeroPoint);
      rhsZeroPoint = rewriter.create<arith::TruncIOp>(
          loc, rewriter.getI32Type(), rhsZeroPoint);

      // change uint8 quantization -> int8 quantization
      int64_t numBits =
          cast<mlir::IntegerType>(lhsType.getElementType()).getWidth();
      signShift(rewriter, loc, lhs, lhsZeroPoint, isUnsigned, numBits);
      numBits = cast<mlir::IntegerType>(rhsType.getElementType()).getWidth();
      signShift(rewriter, loc, rhs, rhsZeroPoint, isUnsignedR, numBits);

      // for quantized vec-vec, vec-mat, and mat-vec cases, lower to
      // expand/collapse + quantized_matmul
      bool lhsVec = (lhsRank == 1 && rhsRank <= 2);
      bool rhsVec = (lhsRank <= 2 && rhsRank == 1);

      if (lhsVec || rhsVec) {
        SmallVector<ReassociationIndices> reassociation(1);
        reassociation[0].push_back(0);
        reassociation[0].push_back(1);

        if (lhsVec) {
          // unsqueeze lhs to a matrix
          int64_t lhsDim = lhsType.getShape()[0];
          auto lhsUnsqueezeType = RankedTensorType::get(
              ArrayRef<int64_t>{1, lhsDim}, lhsType.getElementType());
          lhs = rewriter.create<tensor::ExpandShapeOp>(loc, lhsUnsqueezeType,
                                                       lhs, reassociation);
        }
        if (rhsVec) {
          // unsqueeze rhs to a matrix
          int64_t rhsDim = rhsType.getShape()[0];
          auto rhsUnsqueezeType = RankedTensorType::get(
              ArrayRef<int64_t>{rhsDim, 1}, rhsType.getElementType());
          rhs = rewriter.create<tensor::ExpandShapeOp>(loc, rhsUnsqueezeType,
                                                       rhs, reassociation);
        }
        // get quantized_matmul and squeeze result
        Value lhsDim0 = getDimOp(rewriter, loc, lhs, 0);
        Value lhsDim1 = getDimOp(rewriter, loc, lhs, 1);
        Value rhsDim0 = getDimOp(rewriter, loc, rhs, 0);
        Value rhsDim1 = getDimOp(rewriter, loc, rhs, 1);
        checkDimEqualHelper(rewriter, loc, lhsDim1, rhsDim0);

        Value zeroTensor = createZeroInitTensor(
            rewriter, loc, ValueRange{lhsDim0, rhsDim1}, elementType);
        Value matmul = rewriter
                           .create<linalg::QuantizedMatmulOp>(
                               loc, zeroTensor.getType(),
                               ValueRange{lhs, rhs, lhsZeroPoint, rhsZeroPoint},
                               zeroTensor)
                           .getResult(0);
        int64_t resultRank = resultType.getRank();
        if (resultRank == 0) {
          // in vec-vec case, need to collapse result to a scalar
          reassociation.clear();
        }
        matmul = rewriter.create<tensor::CollapseShapeOp>(
            loc, resultType, matmul, reassociation);
        rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, matmul);
        return success();
      }
      // the remaining quantized cases (Mat-Mat and broadcast -> BMM) are
      // covered in the relevant section below
    }

    // The different cases of torch_matmul op is mentioned here:
    // https://pytorch.org/docs/stable/generated/torch.matmul.html

    // First Case: Dot Product.
    if (lhsRank == 1 && rhsRank == 1) {
      Value lhsDim0 = getDimOp(rewriter, loc, lhs, 0);
      Value rhsDim0 = getDimOp(rewriter, loc, rhs, 0);

      checkDimEqualHelper(rewriter, loc, lhsDim0, rhsDim0);

      Value zeroTensor = createZeroInitTensor(rewriter, loc, {}, elementType);
      Value dotProd =
          rewriter
              .create<linalg::DotOp>(loc, zeroTensor.getType(),
                                     ValueRange{lhs, rhs}, zeroTensor)
              .getResult(0);
      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, dotProd);
      return success();
    }

    // Second Case: Vec-Mat Multiplication.
    if (lhsRank == 1 && rhsRank == 2) {
      Value lhsDim0 = getDimOp(rewriter, loc, lhs, 0);
      Value rhsDim0 = getDimOp(rewriter, loc, rhs, 0);
      Value rhsDim1 = getDimOp(rewriter, loc, rhs, 1);
      checkDimEqualHelper(rewriter, loc, lhsDim0, rhsDim0);

      Value zeroTensor =
          createZeroInitTensor(rewriter, loc, ValueRange{rhsDim1}, elementType);
      Value matmul =
          rewriter
              .create<linalg::VecmatOp>(loc, zeroTensor.getType(),
                                        ValueRange{lhs, rhs}, zeroTensor)
              .getResult(0);
      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, matmul);
      return success();
    }

    // Third Case: Matrix-Vec Multiplication.
    if (lhsRank == 2 && rhsRank == 1) {
      Value lhsDim0 = getDimOp(rewriter, loc, lhs, 0);
      Value lhsDim1 = getDimOp(rewriter, loc, lhs, 1);
      Value rhsDim0 = getDimOp(rewriter, loc, rhs, 0);
      checkDimEqualHelper(rewriter, loc, lhsDim1, rhsDim0);

      Value zeroTensor =
          createZeroInitTensor(rewriter, loc, ValueRange{lhsDim0}, elementType);
      Value matmul =
          rewriter
              .create<linalg::MatvecOp>(loc, zeroTensor.getType(),
                                        ValueRange{lhs, rhs}, zeroTensor)
              .getResult(0);
      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, matmul);
      return success();
    }

    // Fourth Case: Mat-Mat Multiplication.
    if (lhsRank == 2 && rhsRank == 2) {
      Value lhsDim0 = getDimOp(rewriter, loc, lhs, 0);
      Value lhsDim1 = getDimOp(rewriter, loc, lhs, 1);
      Value rhsDim0 = getDimOp(rewriter, loc, rhs, 0);
      Value rhsDim1 = getDimOp(rewriter, loc, rhs, 1);
      checkDimEqualHelper(rewriter, loc, lhsDim1, rhsDim0);

      Value zeroTensor = createZeroInitTensor(
          rewriter, loc, ValueRange{lhsDim0, rhsDim1}, elementType);
      Value matmul;
      if (lhsZeroPoint) {
        matmul = rewriter
                     .create<linalg::QuantizedMatmulOp>(
                         loc, zeroTensor.getType(),
                         ValueRange{lhs, rhs, lhsZeroPoint, rhsZeroPoint},
                         zeroTensor)
                     .getResult(0);
      } else {
        matmul = rewriter
                     .create<linalg::MatmulOp>(loc, zeroTensor.getType(),
                                               ValueRange{lhs, rhs}, zeroTensor)
                     .getResult(0);
      }
      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, matmul);
      return success();
    }

    // Fifth Case: Batch-Matrix Multiplication.
    // TODO: Handle batch matrix multiplication when one of the matrix is unity
    // rank and the other has batch dimension.
    if (lhsRank > 1 && rhsRank > 1) {
      unsigned maxRank = std::max(lhsRank, rhsRank);
      unsigned minRank = std::min(lhsRank, rhsRank);
      unsigned batchRank = maxRank - 2;

      // At least one of the matrix must have rank greater than 2.
      if (batchRank <= 0) {
        return rewriter.notifyMatchFailure(op, "expected batch dimensions");
      }

      // The `broadcastedBatchShape` contains batch dimensions of the resultant
      // matrix.
      SmallVector<Value> broadcastedBatchShape(batchRank);
      Value maxRankMatrix = (lhsRank > rhsRank) ? lhs : rhs;
      Value maxDim;
      // Compute broadcasted batch dimensions if the batch dimensions of
      // the matrices are broadcastable.
      for (unsigned i = 1; i <= batchRank; i++) {
        if (i <= minRank - 2) {
          Value lhsDim = getDimOp(rewriter, loc, lhs, lhsRank - 2 - i);
          Value rhsDim = getDimOp(rewriter, loc, rhs, rhsRank - 2 - i);
          maxDim = rewriter.createOrFold<arith::MaxUIOp>(loc, lhsDim, rhsDim);
        } else {
          maxDim = getDimOp(rewriter, loc, maxRankMatrix, maxRank - 2 - i);
        }
        broadcastedBatchShape[batchRank - i] = maxDim;
      }

      Value lhsDim0 = getDimOp(rewriter, loc, lhs, lhsRank - 2);
      Value lhsDim1 = getDimOp(rewriter, loc, lhs, lhsRank - 1);
      Value rhsDim0 = getDimOp(rewriter, loc, rhs, rhsRank - 2);
      Value rhsDim1 = getDimOp(rewriter, loc, rhs, rhsRank - 1);
      checkDimEqualHelper(rewriter, loc, lhsDim1, rhsDim0);

      // Compute broadcasted shape of both the matrices in integer format.
      SmallVector<Value> lhsBroadcastToShape(broadcastedBatchShape);
      lhsBroadcastToShape.push_back(lhsDim0);
      lhsBroadcastToShape.push_back(lhsDim1);
      SmallVector<Value> rhsBroadcastToShape(broadcastedBatchShape);
      rhsBroadcastToShape.push_back(rhsDim0);
      rhsBroadcastToShape.push_back(rhsDim1);
      for (unsigned i = 0; i < maxRank; i++) {
        lhsBroadcastToShape[i] =
            castIndexToInt64(rewriter, loc, lhsBroadcastToShape[i]);
        rhsBroadcastToShape[i] =
            castIndexToInt64(rewriter, loc, rhsBroadcastToShape[i]);
      }

      // Broadcast the batch dimensions of both the matrices.
      Value broadcastedLhs, broadcastedRhs;
      // TODO: Improve usage of static shape information.
      SmallVector<int64_t> lhsTargetShape(lhsBroadcastToShape.size(),
                                          ShapedType::kDynamic);
      auto lhsBroadcastType = RankedTensorType::get(
          lhsTargetShape, lhsType.getElementType(), lhsType.getEncoding());
      if (failed(torch_to_linalg::broadcastToGivenShape(
              op, rewriter, lhs, lhsBroadcastToShape, lhsBroadcastType,
              broadcastedLhs))) {
        return rewriter.notifyMatchFailure(
            op, "unable to perform broadcast operation");
      }
      SmallVector<int64_t> rhsTargetShape(rhsBroadcastToShape.size(),
                                          ShapedType::kDynamic);
      auto rhsBroadcastType = RankedTensorType::get(
          rhsTargetShape, rhsType.getElementType(), rhsType.getEncoding());
      if (failed(torch_to_linalg::broadcastToGivenShape(
              op, rewriter, rhs, rhsBroadcastToShape, rhsBroadcastType,
              broadcastedRhs))) {
        return rewriter.notifyMatchFailure(
            op, "unable to perform broadcast operation");
      }

      if (maxRank == 3) {
        Value zeroTensor = createZeroInitTensor(
            rewriter, loc,
            ValueRange{broadcastedBatchShape[0], lhsDim0, rhsDim1},
            elementType);
        Value matmul;
        if (lhsZeroPoint) {
          matmul = rewriter
                       .create<linalg::QuantizedBatchMatmulOp>(
                           loc, zeroTensor.getType(),
                           ValueRange{broadcastedLhs, broadcastedRhs,
                                      lhsZeroPoint, rhsZeroPoint},
                           zeroTensor)
                       .getResult(0);
          rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType,
                                                      matmul);
          return success();
        }
        matmul = rewriter
                     .create<linalg::BatchMatmulOp>(
                         loc, zeroTensor.getType(),
                         ValueRange{broadcastedLhs, broadcastedRhs}, zeroTensor)
                     .getResult(0);
        rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, matmul);
        return success();
      }

      // Check if the result of the matrix multiplication has more than one
      // dynamic batch dimensions.
      SmallVector<int64_t> batchDimsInt =
          makeShapeTorchCompatible(resultType.getShape());
      batchDimsInt.pop_back();
      batchDimsInt.pop_back();
      bool multipleDynamicBatchDims =
          llvm::count(batchDimsInt, kUnknownSize) > 1;

      // TODO: Lowering to `linalg.BatchMatmul` is only possible when there is
      // at most one dynamic batch dimension due to limited support of the
      // `tensor.ExpandShape` op.
      if (!multipleDynamicBatchDims) {
        // Collapse the batch dimensions into one dimension. The resultant rank
        // will always be 3.
        SmallVector<ReassociationIndices> reassociation(3);
        for (unsigned i = 0, j = 0; i < maxRank; i++) {
          if (i >= batchRank)
            j++;
          reassociation[j].push_back(i);
        }
        Value collapsedLhs = rewriter.create<tensor::CollapseShapeOp>(
            op->getLoc(), broadcastedLhs, reassociation);
        Value collapsedRhs = rewriter.create<tensor::CollapseShapeOp>(
            op->getLoc(), broadcastedRhs, reassociation);

        // Compute the result shape after collapsing the batch dimensions.
        SmallVector<Value> collapsedResultShape;
        collapsedResultShape.push_back(broadcastedBatchShape[0]);
        for (unsigned i = 1; i < batchRank; i++) {
          collapsedResultShape[0] = rewriter.createOrFold<arith::MulIOp>(
              loc, collapsedResultShape[0], broadcastedBatchShape[i]);
        }
        collapsedResultShape.push_back(lhsDim0);
        collapsedResultShape.push_back(rhsDim1);
        SmallVector<OpFoldResult> updatedCollapseResultShape =
            getAsOpFoldResult(collapsedResultShape);

        Value initTensor = rewriter.create<tensor::EmptyOp>(
            loc, updatedCollapseResultShape, elementType);
        Value c0 = rewriter.create<arith::ConstantOp>(
            loc, rewriter.getZeroAttr(elementType));
        Value zeroTensor =
            rewriter.create<linalg::FillOp>(loc, c0, initTensor).getResult(0);
        Value batchMatMul;

        if (lhsZeroPoint) {
          batchMatMul = rewriter
                            .create<linalg::QuantizedBatchMatmulOp>(
                                loc, zeroTensor.getType(),
                                ValueRange{collapsedLhs, collapsedRhs,
                                           lhsZeroPoint, rhsZeroPoint},
                                zeroTensor)
                            .getResult(0);
        } else {
          batchMatMul =
              rewriter
                  .create<linalg::BatchMatmulOp>(
                      loc, zeroTensor.getType(),
                      ValueRange{collapsedLhs, collapsedRhs}, zeroTensor)
                  .getResult(0);
        }
        Value expandResult = rewriter.create<tensor::ExpandShapeOp>(
            loc, resultType, batchMatMul, reassociation);
        rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType,
                                                    expandResult);
        return success();
      }

      SmallVector<AffineExpr> lhsExpr;
      SmallVector<AffineExpr> rhsExpr;
      SmallVector<AffineExpr> outExpr;
      SmallVector<utils::IteratorType> iteratorTypes(
          batchRank, utils::IteratorType::parallel);
      for (unsigned i = 0; i < batchRank; i++) {
        lhsExpr.push_back(rewriter.getAffineDimExpr(i));
        rhsExpr.push_back(rewriter.getAffineDimExpr(i));
        outExpr.push_back(rewriter.getAffineDimExpr(i));
      }
      lhsExpr.insert(lhsExpr.end(), {rewriter.getAffineDimExpr(batchRank),
                                     rewriter.getAffineDimExpr(batchRank + 1)});
      rhsExpr.insert(rhsExpr.end(), {rewriter.getAffineDimExpr(batchRank + 1),
                                     rewriter.getAffineDimExpr(batchRank + 2)});
      outExpr.insert(outExpr.end(), {rewriter.getAffineDimExpr(batchRank),
                                     rewriter.getAffineDimExpr(batchRank + 2)});

      SmallVector<Value> resultShape(broadcastedBatchShape);
      resultShape.insert(resultShape.end(), {lhsDim0, rhsDim1});
      Value zeroTensor =
          createZeroInitTensor(rewriter, loc, resultShape, elementType);
      auto indexingMaps = AffineMap::inferFromExprList(
          {lhsExpr, rhsExpr, outExpr}, rewriter.getContext());
      iteratorTypes.insert(iteratorTypes.end(),
                           {utils::IteratorType::parallel,
                            utils::IteratorType::reduction,
                            utils::IteratorType::parallel});

      Value finalRes =
          rewriter
              .create<linalg::GenericOp>(
                  loc, zeroTensor.getType(),
                  ValueRange{broadcastedLhs, broadcastedRhs}, zeroTensor,
                  /*indexingMaps=*/indexingMaps,
                  /*iteratorTypes=*/iteratorTypes,
                  [&](OpBuilder &b, Location loc, ValueRange args) {
                    Value l = args[0], r = args[1], res = args[2];
                    Value mul = b.create<arith::MulFOp>(loc, l, r);
                    Value add = b.create<arith::AddFOp>(loc, mul, res);
                    b.create<linalg::YieldOp>(loc, add);
                  })
              .getResult(0);

      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, finalRes);
      return success();
    }
    return failure();
  }
};
} // namespace

namespace {
class ConvertAtenBmmOp : public OpConversionPattern<AtenBmmOp> {
public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult
  matchAndRewrite(AtenBmmOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (failed(verifyLinalgCompatibleTypes(op, rewriter)))
      return failure();
    Location loc = op->getLoc();
    Value lhs = adaptor.getSelf();
    Value rhs = adaptor.getMat2();
    RankedTensorType lhsType = cast<RankedTensorType>(lhs.getType());
    RankedTensorType rhsType = cast<RankedTensorType>(rhs.getType());
    Type newResultType = getTypeConverter()->convertType(op.getType());
    Type resultElementType =
        cast<RankedTensorType>(newResultType).getElementType();
    Type lhsElementType = cast<RankedTensorType>(lhsType).getElementType();
    Type rhsElementType = cast<RankedTensorType>(rhsType).getElementType();

    if (lhsType.getRank() != 3 || rhsType.getRank() != 3) {
      return rewriter.notifyMatchFailure(
          op, "expected both operands to aten.bmm to be rank 3");
    }

    // Convert the inputs element type equivalent to the result' element type.
    if (lhsElementType != rhsElementType) {
      if (lhsElementType != resultElementType) {
        // True if the lhs element type is not equal to the result' element
        // type.
        lhs = torch_to_linalg::convertTensorToElementType(rewriter, loc, lhs,
                                                          resultElementType);
      } else {
        // True if the rhs element type is not equal to the result' element
        // type.
        rhs = torch_to_linalg::convertTensorToElementType(rewriter, loc, rhs,
                                                          resultElementType);
      }
    }

    Value lhsDim0 = getDimOp(rewriter, loc, lhs, 0);
    Value lhsDim1 = getDimOp(rewriter, loc, lhs, 1);
    Value lhsDim2 = getDimOp(rewriter, loc, lhs, 2);
    Value rhsDim0 = getDimOp(rewriter, loc, rhs, 0);
    Value rhsDim1 = getDimOp(rewriter, loc, rhs, 1);
    Value rhsDim2 = getDimOp(rewriter, loc, rhs, 2);

    // Check the batch numbers are equal.
    checkDimEqualHelper(rewriter, loc, lhsDim0, rhsDim0);

    // Check the matrixs shapes are valid for mulplication.
    checkDimEqualHelper(rewriter, loc, lhsDim2, rhsDim1);

    Value initTensor0 = createZeroInitTensor(
        rewriter, loc, ValueRange{lhsDim0, lhsDim1, rhsDim2},
        resultElementType);

    Value bmm =
        rewriter
            .create<linalg::BatchMatmulOp>(loc, initTensor0.getType(),
                                           ValueRange{lhs, rhs}, initTensor0)
            .getResult(0);
    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, bmm);
    return success();
  }
};
} // namespace

namespace {
class ConvertAtenConvolutionOp : public OpConversionPattern<AtenConvolutionOp> {
public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult
  matchAndRewrite(AtenConvolutionOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    MLIRContext *context = op->getContext();
    Value input = adaptor.getInput();   /* in form of N*C*H*W */
    Value weight = adaptor.getWeight(); /* in form of F*C/G*H*W */
    Value bias = adaptor.getBias();
    auto resultTy = cast<ValueTensorType>(op.getType());

    Value inputZp, weightZp;
    bool inputUnsigned = false;
    bool weightUnsigned = false;
    if (auto make = op.getInput()
                        .getDefiningOp<Aten_MakePerTensorQuantizedTensorOp>()) {
      input = make.getSelf();
      inputZp = make.getZeroPoint();
      input = typeConverter->materializeTargetConversion(
          rewriter, loc, typeConverter->convertType(input.getType()), input);
      inputZp = typeConverter->materializeTargetConversion(
          rewriter, loc, typeConverter->convertType(inputZp.getType()),
          inputZp);
      inputZp =
          rewriter.create<arith::TruncIOp>(loc, rewriter.getI32Type(), inputZp);
      auto torchDtype = cast<ValueTensorType>(make.getType()).getDtype();
      inputUnsigned = torch_to_linalg::isUnsignedTorchType(torchDtype);
    }

    if (auto make = op.getWeight()
                        .getDefiningOp<Aten_MakePerTensorQuantizedTensorOp>()) {
      weight = make.getSelf();
      weightZp = make.getZeroPoint();

      weight = typeConverter->materializeTargetConversion(
          rewriter, loc, typeConverter->convertType(weight.getType()), weight);
      weightZp = typeConverter->materializeTargetConversion(
          rewriter, loc, typeConverter->convertType(weightZp.getType()),
          weightZp);
      weightZp = rewriter.create<arith::TruncIOp>(loc, rewriter.getI32Type(),
                                                  weightZp);
      auto torchDtype = cast<ValueTensorType>(make.getType()).getDtype();
      weightUnsigned = torch_to_linalg::isUnsignedTorchType(torchDtype);
    }

    if (static_cast<bool>(inputZp) != static_cast<bool>(weightZp)) {
      return rewriter.notifyMatchFailure(
          op, "lhs and rhs of convolution must either be both int or fp");
    }

    if (inputZp && !isa<Torch::NoneType>(bias.getType())) {
      auto biasDTy = cast<RankedTensorType>(bias.getType()).getElementType();
      if (!biasDTy.isInteger(32)) {
        return rewriter.notifyMatchFailure(
            op, "quantized result ty should be i32 accumulator");
      }
    }

    bool transposed = true;
    if (!matchPattern(op.getTransposed(), m_TorchConstantBool(&transposed)))
      return rewriter.notifyMatchFailure(
          op, "unimplemented: only constant transposed supported");

    auto inputDTy = cast<RankedTensorType>(input.getType()).getElementType();
    auto weightDTy = cast<RankedTensorType>(weight.getType()).getElementType();
    auto resultDTy = resultTy.toBuiltinTensor().getElementType();

    if (!isa<mlir::FloatType, mlir::IntegerType>(inputDTy) ||
        !isa<mlir::FloatType, mlir::IntegerType>(weightDTy) ||
        !isa<mlir::FloatType, mlir::IntegerType>(resultDTy))
      return op.emitError("unimplemented: non-fp not-int type");
    size_t inRank = cast<RankedTensorType>(input.getType()).getRank();
    size_t numSpatialDims = inRank - 2;
    if (numSpatialDims < 1 || numSpatialDims > 3)
      return rewriter.notifyMatchFailure(
          op, "unimplemented: only 1d-3d convolution currently supported");

    Type intType = IntegerType::get(context, 64);
    auto castIndexToInt = [&](Value v) {
      return rewriter.createOrFold<arith::IndexCastOp>(loc, intType, v);
    };

    SmallVector<Value> paddingIntValues;
    if (!getListConstructElements(op.getPadding(), paddingIntValues))
      return rewriter.notifyMatchFailure(
          op, "only support padding from a list construct");
    paddingIntValues = getTypeConvertedValues(rewriter, loc, getTypeConverter(),
                                              paddingIntValues);
    SmallVector<Value> outputPaddingIntValues;
    if (!getListConstructElements(op.getOutputPadding(),
                                  outputPaddingIntValues))
      return rewriter.notifyMatchFailure(
          op, "only support output_padding from a list construct");
    outputPaddingIntValues = getTypeConvertedValues(
        rewriter, loc, getTypeConverter(), outputPaddingIntValues);
    SmallVector<int64_t> strideInts;
    if (!matchPattern(op.getStride(), m_TorchListOfConstantInts(strideInts)))
      return rewriter.notifyMatchFailure(op,
                                         "only support constant int strides");
    SmallVector<int64_t> dilationInts;
    if (!matchPattern(op.getDilation(),
                      m_TorchListOfConstantInts(dilationInts)))
      return rewriter.notifyMatchFailure(op,
                                         "only support constant int dilations");

    Value inBatch = getDimOp(rewriter, loc, input, 0);
    Value inChannels = getDimOp(rewriter, loc, input, 1);
    SmallVector<Value> inDims;
    for (size_t i = 2; i < inRank; i++)
      inDims.push_back(getDimOp(rewriter, loc, input, i));
    Value weightBatch = getDimOp(rewriter, loc, weight, 0);
    Value weightChannels = getDimOp(rewriter, loc, weight, 1);
    SmallVector<Value> weightDims;
    for (size_t i = 2; i < inRank; i++)
      weightDims.push_back(getDimOp(rewriter, loc, weight, i));

    // Checks for valid group size
    int64_t numGroups;
    if (!matchPattern(op.getGroups(), m_TorchConstantInt(&numGroups)))
      return rewriter.notifyMatchFailure(op,
                                         "only constant group size supported.");
    Value groups = castIntToIndex(rewriter, loc, adaptor.getGroups());

    auto validate = [&](Value toValidate, std::string err) {
      Value c0 =
          rewriter.create<arith::ConstantOp>(loc, rewriter.getIndexAttr(0));
      Value inputValid = rewriter.create<arith::CmpIOp>(
          loc, arith::CmpIPredicate::eq, c0,
          rewriter.create<arith::RemSIOp>(loc, toValidate, groups));
      rewriter.create<cf::AssertOp>(loc, inputValid,
                                    rewriter.getStringAttr(err));
    };
    validate(inChannels,
             "invalid: groups must divide input channel size evenly.");
    validate(weightBatch,
             "invalid: groups must divide weight batch size evenly.");
    SmallVector<Value> dilationIntValues =
        getAsConstantIntValues(rewriter, loc, dilationInts);
    SmallVector<Value> strideIntValues =
        getAsConstantIntValues(rewriter, loc, strideInts);

    // convert any uint8 quantization to int8 quantization
    if (auto integerType = dyn_cast<mlir::IntegerType>(inputDTy)) {
      int64_t width = integerType.getWidth();
      signShift(rewriter, loc, input, inputZp, inputUnsigned, width);
    }
    if (auto integerType = dyn_cast<mlir::IntegerType>(weightDTy)) {
      int64_t width = integerType.getWidth();
      signShift(rewriter, loc, weight, weightZp, weightUnsigned, width);
    }
    // Pad the input tensor according to padding.
    SmallVector<Value> outDims{inBatch, weightBatch};
    Value paddedInput;
    Value pad = inputZp;
    if (!pad) {
      if (isa<mlir::FloatType>(inputDTy))
        pad = rewriter.create<arith::ConstantOp>(
            op.getLoc(), rewriter.getFloatAttr(inputDTy, 0.0));
      if (isa<mlir::IntegerType>(inputDTy))
        pad = rewriter.create<arith::ConstantOp>(
            op.getLoc(), rewriter.getIntegerAttr(inputDTy, 0));
    }
    if (pad.getType() != inputDTy) {
      if (isa<mlir::FloatType>(inputDTy))
        pad = rewriter.create<arith::TruncFOp>(op.getLoc(), inputDTy, pad);

      if (isa<mlir::IntegerType>(inputDTy))
        pad = rewriter.create<arith::TruncIOp>(op.getLoc(), inputDTy, pad);
    }
    if (transposed) {
      Value c0 =
          rewriter.create<arith::ConstantOp>(loc, rewriter.getIndexAttr(0));
      Value c1 =
          rewriter.create<arith::ConstantOp>(loc, rewriter.getIndexAttr(1));
      Value c2 =
          rewriter.create<arith::ConstantOp>(loc, rewriter.getIndexAttr(2));

      // Transpose and flip weight
      SmallVector<Value> weightInitDims = getTensorSizes(rewriter, loc, weight);
      std::iter_swap(weightInitDims.begin(), weightInitDims.begin() + 1);
      outDims[1] = weightInitDims[0];
      Value weightInitTensor =
          createZeroInitTensor(rewriter, loc, weightInitDims, weightDTy);
      SmallVector<utils::IteratorType> iteratorTypes(
          inRank, utils::IteratorType::parallel);
      SmallVector<AffineMap> indexingMaps{
          AffineMap::getMultiDimIdentityMap(inRank, context)};
      weight = rewriter
                   .create<linalg::GenericOp>(
                       loc, weightInitTensor.getType(), ValueRange{},
                       weightInitTensor, indexingMaps, iteratorTypes,
                       [&](OpBuilder &b, Location loc, ValueRange args) {
                         SmallVector<Value> indices;
                         for (size_t i = 0; i < inRank; i++)
                           indices.push_back(b.create<linalg::IndexOp>(loc, i));
                         std::iter_swap(indices.begin(), indices.begin() + 1);
                         // Flip only the spatial dimensions (from 2 to inRank)
                         for (size_t flipDim = 2; flipDim < inRank; flipDim++) {
                           indices[flipDim] = b.create<arith::SubIOp>(
                               loc,
                               b.create<arith::SubIOp>(
                                   loc, weightInitDims[flipDim], c1),
                               indices[flipDim]);
                         }
                         Value res =
                             b.create<tensor::ExtractOp>(loc, weight, indices)
                                 .getResult();
                         b.create<linalg::YieldOp>(loc, res);
                       })
                   .getResult(0);

      // Calculate padded input size, allocate tensor
      SmallVector<Value> outerSizes{inBatch, inChannels};
      SmallVector<Value> innerSizes{inBatch, inChannels};
      SmallVector<Value> offsets{c0, c0};
      for (size_t i = 0; i < numSpatialDims; i++) {
        Value innerSize = rewriter.create<arith::SubIOp>(loc, inDims[i], c1);
        innerSize = rewriter.create<arith::MulIOp>(
            loc, innerSize, castIntToIndex(rewriter, loc, strideIntValues[i]));
        innerSize = rewriter.create<arith::AddIOp>(loc, innerSize, c1);

        Value offset = rewriter.create<arith::SubIOp>(loc, weightDims[i], c1);
        offset = rewriter.create<arith::MulIOp>(
            loc, offset, castIntToIndex(rewriter, loc, dilationIntValues[i]));
        offset = rewriter.create<arith::SubIOp>(
            loc, offset, castIntToIndex(rewriter, loc, paddingIntValues[i]));

        Value outerSize = rewriter.create<arith::MulIOp>(loc, offset, c2);
        outerSize = rewriter.create<arith::AddIOp>(loc, outerSize, innerSize);
        outerSize = rewriter.create<arith::AddIOp>(
            loc, outerSize,
            castIntToIndex(rewriter, loc, outputPaddingIntValues[i]));

        outerSizes.push_back(outerSize);
        offsets.push_back(offset);
      }

      // Allocate padded input tensor
      Value initTensor =
          createInitTensor(rewriter, loc, outerSizes, inputDTy, pad);

      // Insert input into allocated tensor
      SmallVector<Value> strideIndexValues{c1, c1};
      for (auto stride : strideIntValues)
        strideIndexValues.push_back(castIntToIndex(rewriter, loc, stride));
      SmallVector<Value> insertSizes = getTensorSizes(rewriter, loc, input);

      paddedInput = rewriter.create<tensor::InsertSliceOp>(
          loc, torch_to_linalg::removeSizeInformation(rewriter, loc, input),
          initTensor, offsets, insertSizes, strideIndexValues);

      // Calculate output dims
      for (size_t i = 0; i < numSpatialDims; i++)
        outDims.push_back(torch_to_linalg::getOutputDimForConvTransposeOps(
            rewriter, loc, inDims[i], paddingIntValues[i], dilationIntValues[i],
            castIndexToInt(weightDims[i]), strideIntValues[i],
            outputPaddingIntValues[i]));

      // Set stride to 1
      strideInts.clear();
      strideInts.append(numSpatialDims, 1);
    } else {
      // Pad input
      paddedInput = torch_to_linalg::getDynamicZeroPaddedTensor(
          op, rewriter, input, paddingIntValues, /*unpaddedDims=*/2, pad);

      // Calculate output dims
      for (size_t i = 0; i < numSpatialDims; i++)
        outDims.push_back(torch_to_linalg::getOutputDimForConvOps(
            rewriter, loc, inDims[i], paddingIntValues[i], dilationIntValues[i],
            castIndexToInt(weightDims[i]), strideIntValues[i]));
    }

    Type accumulatorDType = getDefaultAccType(rewriter, inputDTy);
    Value initTensor = rewriter.create<tensor::EmptyOp>(
        loc, getAsOpFoldResult(outDims), accumulatorDType);

    Value outputTensor;
    if (accumulatorDType != resultDTy && !isa<Torch::NoneType>(bias.getType()))
      bias = torch_to_linalg::convertTensorToElementType(rewriter, loc, bias,
                                                         accumulatorDType);
    if (isa<Torch::NoneType>(bias.getType())) {
      Value c0;
      if (isa<mlir::FloatType>(accumulatorDType)) {
        c0 = rewriter.create<arith::ConstantOp>(
            loc, FloatAttr::get(accumulatorDType, 0.0));
      } else if (isa<mlir::IntegerType>(accumulatorDType)) {
        c0 = rewriter.create<arith::ConstantOp>(
            loc, IntegerAttr::get(accumulatorDType, 0));
      }
      outputTensor =
          rewriter.create<linalg::FillOp>(loc, c0, initTensor).getResult(0);

    } else {
      auto biasType = cast<RankedTensorType>(bias.getType());
      if (biasType.getRank() != 1)
        return rewriter.notifyMatchFailure(op, "expect bias to be rank 1");

      auto resultRank = cast<RankedTensorType>(initTensor.getType()).getRank();
      SmallVector<int64_t, 4> addedDimensions;
      // bias is used to initialize the channels - dimension 1 of
      // output
      for (int i = 0; i < resultRank; ++i)
        if (i != 1)
          addedDimensions.push_back(i);
      outputTensor = rewriter
                         .create<linalg::BroadcastOp>(loc, bias, initTensor,
                                                      addedDimensions)
                         ->getResult(0);
    }

    auto stridesAttr = rewriter.getI64VectorAttr(strideInts);
    auto dilationAttr = rewriter.getI64VectorAttr(dilationInts);

    Value inputStride =
        rewriter.create<arith::FloorDivSIOp>(loc, inChannels, groups);
    Value weightStride =
        rewriter.create<arith::FloorDivSIOp>(loc, weightBatch, groups);

    SmallVector<Value> zeroOffsets(inRank, rewriter.create<arith::ConstantOp>(
                                               loc, rewriter.getIndexAttr(0)));
    SmallVector<Value> unitStrides(inRank, rewriter.create<arith::ConstantOp>(
                                               loc, rewriter.getIndexAttr(1)));
    SmallVector<Value> outDimSlice(outDims);
    outDimSlice[1] = weightStride;
    SmallVector<Value> inputSliceSizes{inBatch, inputStride};
    inputSliceSizes.append(inDims);
    SmallVector<Value> weightSliceSizes{weightStride, weightChannels};
    weightSliceSizes.append(weightDims);

    Value conv;
    // the code so far is able to respect all numSpatialDims
    // the code below this point is numSpatialDims specific and numGroups
    // specific
    // TODO: factor out the above code into a helper function, and then separate
    // convolution into:
    // - grouped 1d-3d
    // - grouped 1d-3d (quantized)
    // - ungrouped 1d-3d
    if (numGroups == 1 && !inputZp) {
      switch (numSpatialDims) {
      case 1:
        conv = rewriter
                   .create<linalg::Conv1DNcwFcwOp>(
                       loc, outputTensor.getType(),
                       ValueRange{paddedInput, weight}, outputTensor,
                       stridesAttr, dilationAttr)
                   .getResult(0);
        break;
      case 2:
        conv = rewriter
                   .create<linalg::Conv2DNchwFchwOp>(
                       loc, outputTensor.getType(),
                       ValueRange{paddedInput, weight}, outputTensor,
                       stridesAttr, dilationAttr)
                   .getResult(0);
        break;
      case 3:
        conv = rewriter
                   .create<linalg::Conv3DNcdhwFcdhwOp>(
                       loc, outputTensor.getType(),
                       ValueRange{paddedInput, weight}, outputTensor,
                       stridesAttr, dilationAttr)
                   .getResult(0);
        break;
      default:
        return rewriter.notifyMatchFailure(
            op, "unimplemented: only 1D, 2D, and 3D convolution supported");
      };
      Type newResultType = getTypeConverter()->convertType(op.getType());
      if (accumulatorDType != resultDTy) {
        Type resultElementType =
            cast<RankedTensorType>(newResultType).getElementType();
        conv = torch_to_linalg::convertTensorToElementType(rewriter, loc, conv,
                                                           resultElementType);
      }
      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, conv);
      return success();
    }

    if (numGroups == 1 && inputZp) {
      // The quantized version uses a different channel ordering so we need to
      // permute the tensors in order to use the existing path. We should
      // eventually directly support this channel ordering.
      llvm::SmallVector<int64_t> inPerms, weightPerms;
      inPerms.push_back(0); // N stays at the front for input.
      // Then we expect the spatial dimensions
      for (size_t i = 0; i < numSpatialDims; ++i) {
        inPerms.push_back(i + 2);
        weightPerms.push_back(i + 2);
      }
      inPerms.push_back(1);
      weightPerms.append({1, 0});

      paddedInput = transposeValue(op.getLoc(), paddedInput, inPerms, rewriter);
      weight = transposeValue(op.getLoc(), weight, weightPerms, rewriter);
      outputTensor =
          transposeValue(op.getLoc(), outputTensor, inPerms, rewriter);

      switch (numSpatialDims) {
      case 2:
        conv = rewriter
                   .create<linalg::Conv2DNhwcHwcfQOp>(
                       loc, outputTensor.getType(),
                       ValueRange{paddedInput, weight, inputZp, weightZp},
                       outputTensor, stridesAttr, dilationAttr)
                   .getResult(0);
        break;
      case 3:
        conv = rewriter
                   .create<linalg::Conv3DNdhwcDhwcfQOp>(
                       loc, outputTensor.getType(),
                       ValueRange{paddedInput, weight, inputZp, weightZp},
                       outputTensor, stridesAttr, dilationAttr)
                   .getResult(0);
        break;
      default:
        return rewriter.notifyMatchFailure(
            op, "unimplemented: only 1D, 2D, and 3D convolution supported");
      };

      llvm::SmallVector<int64_t> outPerms;
      outPerms.push_back(0);
      outPerms.push_back(inPerms.size() - 1);
      for (size_t i = 0; i < numSpatialDims; ++i) {
        outPerms.push_back(i + 1);
      }
      conv = transposeValue(op.getLoc(), conv, outPerms, rewriter);

      Type newResultType = getTypeConverter()->convertType(op.getType());
      if (accumulatorDType != resultDTy) {
        Type resultElementType =
            cast<RankedTensorType>(newResultType).getElementType();
        conv = torch_to_linalg::convertTensorToElementType(rewriter, loc, conv,
                                                           resultElementType);
      }
      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, conv);
      return success();
    }

    // Special depthwise case: Cin = Cout = groups.
    // Note: pytorch considers Cin == groups (Cout possibly a non-zero multiple
    // of groups) to be depthwise in their documentation, but the linalg ops
    // apparently disagree.
    auto inShape = makeShapeTorchCompatible(
        cast<RankedTensorType>(input.getType()).getShape());
    auto weightShape = makeShapeTorchCompatible(
        cast<RankedTensorType>(weight.getType()).getShape());
    if (inShape[1] == numGroups && weightShape[0] == numGroups &&
        weightShape[1] == 1) {
      // Collapse weight shape (C/G == 1)
      SmallVector<ReassociationIndices> collapsedDims = {{0, 1}};
      SmallVector<int64_t> collapsedShape{weightShape[0] * weightShape[1]};
      for (unsigned i = 0; i < numSpatialDims; i++) {
        collapsedDims.push_back({i + 2});
        collapsedShape.push_back(weightShape[i + 2]);
      }
      Type collapsedType = RankedTensorType::get(
          makeShapeLLVMCompatible(collapsedShape), weightDTy);
      Value collapsedWeight = rewriter.create<tensor::CollapseShapeOp>(
          loc, collapsedType, weight, collapsedDims);
      if (!inputZp) {
        switch (numSpatialDims) {
        case 1:
          conv = rewriter
                     .create<linalg::DepthwiseConv1DNcwCwOp>(
                         loc, outputTensor.getType(),
                         ValueRange{paddedInput, collapsedWeight}, outputTensor,
                         stridesAttr, dilationAttr)
                     .getResult(0);
          break;
        case 2:
          conv = rewriter
                     .create<linalg::DepthwiseConv2DNchwChwOp>(
                         loc, outputTensor.getType(),
                         ValueRange{paddedInput, collapsedWeight}, outputTensor,
                         stridesAttr, dilationAttr)
                     .getResult(0);
          break;
        default:
          return rewriter.notifyMatchFailure(
              op, "unimplemented: only 1D and 2D depthwise convolution "
                  "supported for special case of group convolution");
        };
      } else {
        if (numSpatialDims != 2)
          return rewriter.notifyMatchFailure(
              op, "unimplemented: only 2D depthwise quantized convolution "
                  "supported for special case of group convolution");

        // currently, the only named depthwise qconv op is nhwc_hwc
        // input: nchw -> nhwc; weight (collapsed): chw -> hwc
        // linalg conv result nhwc -> nchw
        // inPerms = [0, 2, 3, 1]
        // weightPerms = [1, 2, 0]
        // resultPerms = [0, 3, 1, 2]
        llvm::SmallVector<int64_t> inPerms, weightPerms, resultPerms;
        inPerms.push_back(0);
        resultPerms.append({0, static_cast<int64_t>(numSpatialDims + 1)});
        for (size_t i = 0; i < numSpatialDims; ++i) {
          inPerms.push_back(i + 2);
          weightPerms.push_back(i + 1);
          resultPerms.push_back(i + 1);
        }
        inPerms.push_back(1);
        weightPerms.push_back(0);

        paddedInput =
            transposeValue(op.getLoc(), paddedInput, inPerms, rewriter);
        collapsedWeight =
            transposeValue(op.getLoc(), collapsedWeight, weightPerms, rewriter);
        outputTensor =
            transposeValue(op.getLoc(), outputTensor, inPerms, rewriter);

        conv =
            rewriter
                .create<linalg::DepthwiseConv2DNhwcHwcQOp>(
                    loc, outputTensor.getType(),
                    ValueRange{paddedInput, collapsedWeight, inputZp, weightZp},
                    outputTensor, stridesAttr, dilationAttr)
                .getResult(0);
        // convert output nhwc -> nchw
        conv = transposeValue(op.getLoc(), conv, resultPerms, rewriter);
      }

      Type newResultType = getTypeConverter()->convertType(op.getType());
      if (accumulatorDType != resultDTy) {
        Type resultElementType =
            cast<RankedTensorType>(newResultType).getElementType();
        conv = torch_to_linalg::convertTensorToElementType(rewriter, loc, conv,
                                                           resultElementType);
      }
      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, conv);
      return success();
    }

    if (numSpatialDims != 2)
      return rewriter.notifyMatchFailure(
          op, "unimplemented: only 2D grouped convolution supported");

    // Grouped case, use the grouped conv linalg op
    auto expandGroups = [&](Value tensor, size_t dim) {
      auto inType = cast<RankedTensorType>(tensor.getType());
      auto inShape = makeShapeTorchCompatible(inType.getShape());

      SmallVector<int64_t> outShape;
      for (auto i = 0; i < (long)inShape.size(); i++) {
        if (i == 1) {
          outShape.push_back(numGroups);
        }
        if (i == (long)dim) {
          outShape.push_back(inShape[i] == kUnknownSize
                                 ? kUnknownSize
                                 : inShape[i] / numGroups);
        } else {
          outShape.push_back(inShape[i]);
        }
      }

      SmallVector<ReassociationIndices> indices;
      for (auto i = 0; i <= (long)inShape.size(); i++) {
        if (i == (long)dim) {
          indices.push_back({i, ++i});
          continue;
        }
        indices.push_back({i});
      }

      auto retType = inType.clone(makeShapeLLVMCompatible(outShape));
      return rewriter.create<tensor::ExpandShapeOp>(loc, retType, tensor,
                                                    indices);
    };

    // expand F,C,H,W -> G,F/G,C,H,W
    auto expandWeight = [&](Value tensor) {
      auto inType = cast<RankedTensorType>(tensor.getType());
      auto inShape = makeShapeTorchCompatible(inType.getShape());

      SmallVector<int64_t> outShape{
          numGroups,
          (inShape[0] == kUnknownSize ? kUnknownSize : inShape[0] / numGroups)};
      outShape.append(inShape.begin() + 1, inShape.end());

      SmallVector<ReassociationIndices> indices{{0, 1}};
      for (auto i = 2; i <= (long)inShape.size(); i++)
        indices.push_back({i});

      auto retType = inType.clone(makeShapeLLVMCompatible(outShape));
      return rewriter.create<tensor::ExpandShapeOp>(loc, retType, tensor,
                                                    indices);
    };

    Value paddedInputExpanded = expandGroups(paddedInput, 1);
    Value weightExpanded = expandWeight(weight);
    auto expandOutputTensor = expandGroups(outputTensor, 1);

    // TODO: add 1D and 3D case
    if (!inputZp) {
      conv = rewriter
                 .create<linalg::Conv2DNgchwGfchwOp>(
                     loc, expandOutputTensor.getResultType(),
                     ValueRange{paddedInputExpanded, weightExpanded},
                     expandOutputTensor.getResult(), stridesAttr, dilationAttr)
                 .getResult(0);
    } else {
      conv = rewriter
                 .create<linalg::Conv2DNgchwGfchwQOp>(
                     loc, expandOutputTensor.getResultType(),
                     ValueRange{paddedInputExpanded, weightExpanded, inputZp,
                                weightZp},
                     expandOutputTensor.getResult(), stridesAttr, dilationAttr)
                 .getResult(0);
    }
    conv = rewriter.create<tensor::CollapseShapeOp>(
        loc, outputTensor.getType(), conv,
        expandOutputTensor.getReassociationIndices());
    Type newResultType = getTypeConverter()->convertType(op.getType());
    if (accumulatorDType != resultDTy) {
      Type resultElementType =
          cast<RankedTensorType>(newResultType).getElementType();
      conv = torch_to_linalg::convertTensorToElementType(rewriter, loc, conv,
                                                         resultElementType);
    }
    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, newResultType, conv);
    return success();
  }
};
} // namespace

class ConvertAten_TrilinearOp : public OpConversionPattern<Aten_TrilinearOp> {
public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult
  matchAndRewrite(Aten_TrilinearOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();

    // Input Tensors
    Value i1 = op.getI1();
    Value i2 = op.getI2();
    Value i3 = op.getI3();

    RankedTensorType i1Type = cast<RankedTensorType>(i1.getType());
    auto i1Shape = i1Type.getShape();
    RankedTensorType i2Type = cast<RankedTensorType>(i2.getType());
    auto i2Shape = i2Type.getShape();
    RankedTensorType i3Type = cast<RankedTensorType>(i3.getType());
    auto i3Shape = i3Type.getShape();

    // Expansions
    SmallVector<int64_t> expand1;
    SmallVector<int64_t> expand2;
    SmallVector<int64_t> expand3;
    if (!matchPattern(op.getExpand1(), m_TorchListOfConstantInts(expand1))) {
      return rewriter.notifyMatchFailure(op, "expand1 should be constant");
    }
    if (!matchPattern(op.getExpand2(), m_TorchListOfConstantInts(expand2))) {
      return rewriter.notifyMatchFailure(op, "expand2 should be constant");
    }
    if (!matchPattern(op.getExpand3(), m_TorchListOfConstantInts(expand3))) {
      return rewriter.notifyMatchFailure(op, "expand3 should be constant");
    }

    SmallVector<int64_t> sumDim;
    if (!matchPattern(op.getSumdim(), m_TorchListOfConstantInts(sumDim))) {
      return rewriter.notifyMatchFailure(op, "sumDim should be constant");
    }

    int64_t unrollDim;
    if (!matchPattern(op.getUnrollDim(), m_TorchConstantInt(&unrollDim))) {
      return rewriter.notifyMatchFailure(op, "unrollDim should be constant");
    }

    int64_t totalDims = i1Shape.size() + expand1.size();

    // Create bitsets that correspond to specified dimensions in inputs
    SmallVector<bool> expand1Flags(totalDims, false);
    SmallVector<bool> expand2Flags(totalDims, false);
    SmallVector<bool> expand3Flags(totalDims, false);
    for (int64_t dim : expand1) {
      expand1Flags[dim] = true;
    }
    for (int64_t dim : expand2) {
      expand2Flags[dim] = true;
    }
    for (int64_t dim : expand3) {
      expand3Flags[dim] = true;
    }

    SmallVector<int64_t> sumDimFlags(totalDims, 0);
    for (int64_t dim : sumDim) {
      sumDimFlags[dim] = true;
    }

    SmallVector<int64_t> sumDims12, sumDims23;
    SmallVector<OpFoldResult> outputShape;
    int64_t unrollSize = -1;
    Value output;
    for (int64_t i = 0; i < totalDims; ++i) {
      int64_t size = 0;
      Value indexValue =
          rewriter.create<ConstantIntOp>(loc, rewriter.getI64IntegerAttr(0));
      if (expand1Flags[i]) {
        i1 = rewriter.create<AtenUnsqueezeOp>(loc, i1Type, i1, indexValue);
      } else {
        size = i1Shape[i];
      }
      if (expand2Flags[i]) {
        i2 = rewriter.create<AtenUnsqueezeOp>(loc, i2Type, i2, indexValue);
      } else {
        size = i2Shape[i];
      }
      if (expand3Flags[i]) {
        i3 = rewriter.create<AtenUnsqueezeOp>(loc, i3Type, i3, indexValue);
        if (sumDimFlags[i] && (i != unrollDim)) {
          sumDims12.push_back(i);
        }
      } else {
        size = i3Shape[i];
        if (sumDimFlags[i] && (i != unrollDim)) {
          sumDims23.push_back(i);
        }
      }

      outputShape.push_back(rewriter.getIndexAttr(size));
      if (i == unrollDim)
        unrollSize = size;

      int64_t slicemul1 = (expand1Flags[unrollDim] ? 0 : 1);
      int64_t slicemul2 = (expand2Flags[unrollDim] ? 0 : 1);
      int64_t slicemul3 = (expand3Flags[unrollDim] ? 0 : 1);

      // TODO: How do we determine the output type here (lowest precision type)
      output = rewriter.create<tensor::EmptyOp>(loc, outputShape,
                                                i1Type.getElementType());
      RankedTensorType outputType = cast<RankedTensorType>(output.getType());

      int64_t outputRank = outputType.getRank();
      Value cstOne = rewriter.create<arith::ConstantIndexOp>(loc, 1);
      if (i1Shape.size() != 0 && i2Shape.size() != 0 && i3Shape.size() != 0) {
        if (!sumDimFlags[unrollDim]) {
          for (int64_t k = 0; k < unrollSize; ++k) {
            Value kValue = rewriter.create<arith::ConstantIndexOp>(loc, k);
            Value unrollDimValue =
                rewriter.create<arith::ConstantIndexOp>(loc, unrollDim);
            SmallVector<Value> narrowIndices{
                rewriter.create<arith::ConstantIndexOp>(loc, k * slicemul1),
                rewriter.create<arith::ConstantIndexOp>(loc, k * slicemul2),
                rewriter.create<arith::ConstantIndexOp>(loc, k * slicemul3)};
            Value slice_i1 = rewriter.create<AtenNarrowOp>(
                loc, outputType, i1, unrollDimValue, narrowIndices[0], cstOne);
            Value slice_i2 = rewriter.create<AtenNarrowOp>(
                loc, outputType, i2, unrollDimValue, narrowIndices[1], cstOne);
            Value slice_i3 = rewriter.create<AtenNarrowOp>(
                loc, outputType, i3, unrollDimValue, narrowIndices[2], cstOne);

            Value mul12 = rewriter.create<AtenMulTensorOp>(loc, outputType,
                                                           slice_i1, slice_i2);
            for (int64_t dim : sumDims12) {
              Value dimValue =
                  rewriter.create<arith::ConstantIndexOp>(loc, dim);
              mul12 =
                  rewriter.create<AtenSumOp>(loc, outputType, mul12, dimValue);
            }

            Value mulResult = rewriter.create<AtenMulTensorOp>(loc, outputType,
                                                               mul12, slice_i3);
            for (int64_t dim : sumDims23) {
              Value dimValue =
                  rewriter.create<arith::ConstantIndexOp>(loc, dim);
              mulResult = rewriter.create<AtenSumOp>(loc, outputType, mulResult,
                                                     dimValue);
            }

            output = rewriter.create<AtenNarrowOp>(
                loc, outputType, output, unrollDimValue, kValue, cstOne);

            rewriter.create<AtenAddTensorOp>(loc, outputType, output, mulResult,
                                             cstOne);
          }
        } else {
          for (int64_t k = 0; k < unrollSize; ++k) {
            Value unrollDimValue =
                rewriter.create<arith::ConstantIndexOp>(loc, unrollDim);
            SmallVector<Value> narrowIndices{
                rewriter.create<arith::ConstantIndexOp>(loc, k * slicemul1),
                rewriter.create<arith::ConstantIndexOp>(loc, k * slicemul2),
                rewriter.create<arith::ConstantIndexOp>(loc, k * slicemul3)};
            Value slice_i1 = rewriter.create<AtenNarrowOp>(
                loc, outputType, i1, unrollDimValue, narrowIndices[0], cstOne);
            Value slice_i2 = rewriter.create<AtenNarrowOp>(
                loc, outputType, i2, unrollDimValue, narrowIndices[1], cstOne);
            Value slice_i3 = rewriter.create<AtenNarrowOp>(
                loc, outputType, i3, unrollDimValue, narrowIndices[2], cstOne);

            Value mul12 = rewriter.create<AtenMulTensorOp>(loc, outputType,
                                                           slice_i1, slice_i2);
            for (int64_t dim : sumDims12) {
              Value dimValue =
                  rewriter.create<arith::ConstantIndexOp>(loc, dim);
              mul12 =
                  rewriter.create<AtenSumOp>(loc, outputType, mul12, dimValue);
            }

            Value mulResult = rewriter.create<AtenMulTensorOp>(loc, outputType,
                                                               mul12, slice_i3);
            for (int64_t dim : sumDims23) {
              Value dimValue =
                  rewriter.create<arith::ConstantIndexOp>(loc, dim);
              mulResult = rewriter.create<AtenSumOp>(loc, outputType, mulResult,
                                                     dimValue);
            }

            output = rewriter.create<AtenAddTensorOp>(loc, outputType, output,
                                                      mulResult, cstOne);
          }
        }
      }

      for (int64_t i = outputRank - 1; i >= 0; --i) {
        if (sumDimFlags[i]) {
          Value indexValue = rewriter.create<arith::ConstantIndexOp>(loc, i);
          output = rewriter.create<AtenSqueezeDimOp>(loc, outputType, output,
                                                     indexValue);
        }
      }
    }

    rewriter.replaceOp(op, output);
    return success();
  }
};

void mlir::torch::torch_to_linalg::populateLinearPatternsAndLegality(
    TypeConverter &typeConverter, RewritePatternSet &patterns,
    ConversionTarget &target) {
  MLIRContext *context = patterns.getContext();
  target.addIllegalOp<AtenMmOp>();
  patterns.add<ConvertAtenMmOp>(typeConverter, context);
  target.addIllegalOp<AtenFlipOp>();
  patterns.add<ConvertAtenFlipOp>(typeConverter, context);
  target.addIllegalOp<AtenMatmulOp>();
  patterns.add<ConvertAtenMatmulOp>(typeConverter, context);
  target.addIllegalOp<AtenBmmOp>();
  patterns.add<ConvertAtenBmmOp>(typeConverter, context);
  target.addIllegalOp<AtenConvolutionOp>();
  patterns.add<ConvertAtenConvolutionOp>(typeConverter, context);
  target.addIllegalOp<Aten_TrilinearOp>();
  patterns.add<ConvertAten_TrilinearOp>(typeConverter, context);
}
