//===- ConvertStandardToLLVM.h - Convert to the LLVM dialect ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provides a dialect conversion targeting the LLVM IR dialect.  By default, it
// converts Standard ops and types and provides hooks for dialect-specific
// extensions to the conversion.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_CONVERSION_STANDARDTOLLVM_CONVERTSTANDARDTOLLVM_H
#define MLIR_CONVERSION_STANDARDTOLLVM_CONVERTSTANDARDTOLLVM_H

#include "mlir/Conversion/StandardToLLVM/ConvertStandardToLLVMPass.h"
#include "mlir/Transforms/DialectConversion.h"

namespace llvm {
class IntegerType;
class LLVMContext;
class Module;
class Type;
} // namespace llvm

namespace mlir {

class BaseMemRefType;
class ComplexType;
class LLVMTypeConverter;
class UnrankedMemRefType;

namespace LLVM {
class LLVMDialect;
class LLVMPointerType;
} // namespace LLVM

/// Callback to convert function argument types. It converts a MemRef function
/// argument to a list of non-aggregate types containing descriptor
/// information, and an UnrankedmemRef function argument to a list containing
/// the rank and a pointer to a descriptor struct.
LogicalResult structFuncArgTypeConverter(LLVMTypeConverter &converter,
                                         Type type,
                                         SmallVectorImpl<Type> &result);

/// Callback to convert function argument types. It converts MemRef function
/// arguments to bare pointers to the MemRef element type.
LogicalResult barePtrFuncArgTypeConverter(LLVMTypeConverter &converter,
                                          Type type,
                                          SmallVectorImpl<Type> &result);

/// Conversion from types in the Standard dialect to the LLVM IR dialect.
class LLVMTypeConverter : public TypeConverter {
  /// Give structFuncArgTypeConverter access to memref-specific functions.
  friend LogicalResult
  structFuncArgTypeConverter(LLVMTypeConverter &converter, Type type,
                             SmallVectorImpl<Type> &result);

public:
  using TypeConverter::convertType;

  /// Create an LLVMTypeConverter using the default LowerToLLVMOptions.
  LLVMTypeConverter(MLIRContext *ctx);

  /// Create an LLVMTypeConverter using custom LowerToLLVMOptions.
  LLVMTypeConverter(MLIRContext *ctx, const LowerToLLVMOptions &options);

  /// Convert a function type.  The arguments and results are converted one by
  /// one and results are packed into a wrapped LLVM IR structure type. `result`
  /// is populated with argument mapping.
  Type convertFunctionSignature(FunctionType funcTy, bool isVariadic,
                                SignatureConversion &result);

  /// Convert a non-empty list of types to be returned from a function into a
  /// supported LLVM IR type.  In particular, if more than one value is
  /// returned, create an LLVM IR structure type with elements that correspond
  /// to each of the MLIR types converted with `convertType`.
  Type packFunctionResults(ArrayRef<Type> types);

  /// Convert a type in the context of the default or bare pointer calling
  /// convention. Calling convention sensitive types, such as MemRefType and
  /// UnrankedMemRefType, are converted following the specific rules for the
  /// calling convention. Calling convention independent types are converted
  /// following the default LLVM type conversions.
  Type convertCallingConventionType(Type type);

  /// Promote the bare pointers in 'values' that resulted from memrefs to
  /// descriptors. 'stdTypes' holds the types of 'values' before the conversion
  /// to the LLVM-IR dialect (i.e., MemRefType, or any other builtin type).
  void promoteBarePtrsToDescriptors(ConversionPatternRewriter &rewriter,
                                    Location loc, ArrayRef<Type> stdTypes,
                                    SmallVectorImpl<Value> &values);

  /// Returns the MLIR context.
  MLIRContext &getContext();

  /// Returns the LLVM dialect.
  LLVM::LLVMDialect *getDialect() { return llvmDialect; }

  const LowerToLLVMOptions &getOptions() const { return options; }

  /// Promote the LLVM representation of all operands including promoting MemRef
  /// descriptors to stack and use pointers to struct to avoid the complexity
  /// of the platform-specific C/C++ ABI lowering related to struct argument
  /// passing.
  SmallVector<Value, 4> promoteOperands(Location loc, ValueRange opOperands,
                                        ValueRange operands,
                                        OpBuilder &builder);

  /// Promote the LLVM struct representation of one MemRef descriptor to stack
  /// and use pointer to struct to avoid the complexity of the platform-specific
  /// C/C++ ABI lowering related to struct argument passing.
  Value promoteOneMemRefDescriptor(Location loc, Value operand,
                                   OpBuilder &builder);

  /// Converts the function type to a C-compatible format, in particular using
  /// pointers to memref descriptors for arguments.
  Type convertFunctionTypeCWrapper(FunctionType type);

  /// Returns the data layout to use during and after conversion.
  const llvm::DataLayout &getDataLayout() { return options.dataLayout; }

  /// Gets the LLVM representation of the index type. The returned type is an
  /// integer type with the size configured for this type converter.
  Type getIndexType();

  /// Gets the bitwidth of the index type when converted to LLVM.
  unsigned getIndexTypeBitwidth() { return options.indexBitwidth; }

  /// Gets the pointer bitwidth.
  unsigned getPointerBitwidth(unsigned addressSpace = 0);

protected:
  /// Pointer to the LLVM dialect.
  LLVM::LLVMDialect *llvmDialect;

private:
  /// Convert a function type.  The arguments and results are converted one by
  /// one.  Additionally, if the function returns more than one value, pack the
  /// results into an LLVM IR structure type so that the converted function type
  /// returns at most one result.
  Type convertFunctionType(FunctionType type);

  /// Convert the index type.  Uses llvmModule data layout to create an integer
  /// of the pointer bitwidth.
  Type convertIndexType(IndexType type);

  /// Convert an integer type `i*` to `!llvm<"i*">`.
  Type convertIntegerType(IntegerType type);

  /// Convert a floating point type: `f16` to `f16`, `f32` to
  /// `f32` and `f64` to `f64`.  `bf16` is not supported
  /// by LLVM.
  Type convertFloatType(FloatType type);

  /// Convert complex number type: `complex<f16>` to `!llvm<"{ half, half }">`,
  /// `complex<f32>` to `!llvm<"{ float, float }">`, and `complex<f64>` to
  /// `!llvm<"{ double, double }">`. `complex<bf16>` is not supported.
  Type convertComplexType(ComplexType type);

  /// Convert a memref type into an LLVM type that captures the relevant data.
  Type convertMemRefType(MemRefType type);

  /// Convert a memref type into a list of LLVM IR types that will form the
  /// memref descriptor. If `unpackAggregates` is true the `sizes` and `strides`
  /// arrays in the descriptors are unpacked to individual index-typed elements,
  /// else they are are kept as rank-sized arrays of index type. In particular,
  /// the list will contain:
  /// - two pointers to the memref element type, followed by
  /// - an index-typed offset, followed by
  /// - (if unpackAggregates = true)
  ///    - one index-typed size per dimension of the memref, followed by
  ///    - one index-typed stride per dimension of the memref.
  /// - (if unpackArrregates = false)
  ///   - one rank-sized array of index-type for the size of each dimension
  ///   - one rank-sized array of index-type for the stride of each dimension
  ///
  /// For example, memref<?x?xf32> is converted to the following list:
  /// - `!llvm<"float*">` (allocated pointer),
  /// - `!llvm<"float*">` (aligned pointer),
  /// - `i64` (offset),
  /// - `i64`, `i64` (sizes),
  /// - `i64`, `i64` (strides).
  /// These types can be recomposed to a memref descriptor struct.
  SmallVector<Type, 5> getMemRefDescriptorFields(MemRefType type,
                                                 bool unpackAggregates);

  /// Convert an unranked memref type into a list of non-aggregate LLVM IR types
  /// that will form the unranked memref descriptor. In particular, this list
  /// contains:
  /// - an integer rank, followed by
  /// - a pointer to the memref descriptor struct.
  /// For example, memref<*xf32> is converted to the following list:
  /// i64 (rank)
  /// !llvm<"i8*"> (type-erased pointer).
  /// These types can be recomposed to a unranked memref descriptor struct.
  SmallVector<Type, 2> getUnrankedMemRefDescriptorFields();

  // Convert an unranked memref type to an LLVM type that captures the
  // runtime rank and a pointer to the static ranked memref desc
  Type convertUnrankedMemRefType(UnrankedMemRefType type);

  /// Convert a memref type to a bare pointer to the memref element type.
  Type convertMemRefToBarePtr(BaseMemRefType type);

  // Convert a 1D vector type into an LLVM vector type.
  Type convertVectorType(VectorType type);

  /// Options for customizing the llvm lowering.
  LowerToLLVMOptions options;
};

/// Helper class to produce LLVM dialect operations extracting or inserting
/// values to a struct.
class StructBuilder {
public:
  /// Construct a helper for the given value.
  explicit StructBuilder(Value v);
  /// Builds IR creating an `undef` value of the descriptor type.
  static StructBuilder undef(OpBuilder &builder, Location loc,
                             Type descriptorType);

  /*implicit*/ operator Value() { return value; }

protected:
  // LLVM value
  Value value;
  // Cached struct type.
  Type structType;

protected:
  /// Builds IR to extract a value from the struct at position pos
  Value extractPtr(OpBuilder &builder, Location loc, unsigned pos);
  /// Builds IR to set a value in the struct at position pos
  void setPtr(OpBuilder &builder, Location loc, unsigned pos, Value ptr);
};

class ComplexStructBuilder : public StructBuilder {
public:
  /// Construct a helper for the given complex number value.
  using StructBuilder::StructBuilder;
  /// Build IR creating an `undef` value of the complex number type.
  static ComplexStructBuilder undef(OpBuilder &builder, Location loc,
                                    Type type);

  // Build IR extracting the real value from the complex number struct.
  Value real(OpBuilder &builder, Location loc);
  // Build IR inserting the real value into the complex number struct.
  void setReal(OpBuilder &builder, Location loc, Value real);

  // Build IR extracting the imaginary value from the complex number struct.
  Value imaginary(OpBuilder &builder, Location loc);
  // Build IR inserting the imaginary value into the complex number struct.
  void setImaginary(OpBuilder &builder, Location loc, Value imaginary);
};

/// Helper class to produce LLVM dialect operations extracting or inserting
/// elements of a MemRef descriptor. Wraps a Value pointing to the descriptor.
/// The Value may be null, in which case none of the operations are valid.
class MemRefDescriptor : public StructBuilder {
public:
  /// Construct a helper for the given descriptor value.
  explicit MemRefDescriptor(Value descriptor);
  /// Builds IR creating an `undef` value of the descriptor type.
  static MemRefDescriptor undef(OpBuilder &builder, Location loc,
                                Type descriptorType);
  /// Builds IR creating a MemRef descriptor that represents `type` and
  /// populates it with static shape and stride information extracted from the
  /// type.
  static MemRefDescriptor fromStaticShape(OpBuilder &builder, Location loc,
                                          LLVMTypeConverter &typeConverter,
                                          MemRefType type, Value memory);

  /// Builds IR extracting the allocated pointer from the descriptor.
  Value allocatedPtr(OpBuilder &builder, Location loc);
  /// Builds IR inserting the allocated pointer into the descriptor.
  void setAllocatedPtr(OpBuilder &builder, Location loc, Value ptr);

  /// Builds IR extracting the aligned pointer from the descriptor.
  Value alignedPtr(OpBuilder &builder, Location loc);

  /// Builds IR inserting the aligned pointer into the descriptor.
  void setAlignedPtr(OpBuilder &builder, Location loc, Value ptr);

  /// Builds IR extracting the offset from the descriptor.
  Value offset(OpBuilder &builder, Location loc);

  /// Builds IR inserting the offset into the descriptor.
  void setOffset(OpBuilder &builder, Location loc, Value offset);
  void setConstantOffset(OpBuilder &builder, Location loc, uint64_t offset);

  /// Builds IR extracting the pos-th size from the descriptor.
  Value size(OpBuilder &builder, Location loc, unsigned pos);
  Value size(OpBuilder &builder, Location loc, Value pos, int64_t rank);

  /// Builds IR inserting the pos-th size into the descriptor
  void setSize(OpBuilder &builder, Location loc, unsigned pos, Value size);
  void setConstantSize(OpBuilder &builder, Location loc, unsigned pos,
                       uint64_t size);

  /// Builds IR extracting the pos-th size from the descriptor.
  Value stride(OpBuilder &builder, Location loc, unsigned pos);

  /// Builds IR inserting the pos-th stride into the descriptor
  void setStride(OpBuilder &builder, Location loc, unsigned pos, Value stride);
  void setConstantStride(OpBuilder &builder, Location loc, unsigned pos,
                         uint64_t stride);

  /// Returns the (LLVM) pointer type this descriptor contains.
  LLVM::LLVMPointerType getElementPtrType();

  /// Builds IR populating a MemRef descriptor structure from a list of
  /// individual values composing that descriptor, in the following order:
  /// - allocated pointer;
  /// - aligned pointer;
  /// - offset;
  /// - <rank> sizes;
  /// - <rank> shapes;
  /// where <rank> is the MemRef rank as provided in `type`.
  static Value pack(OpBuilder &builder, Location loc,
                    LLVMTypeConverter &converter, MemRefType type,
                    ValueRange values);

  /// Builds IR extracting individual elements of a MemRef descriptor structure
  /// and returning them as `results` list.
  static void unpack(OpBuilder &builder, Location loc, Value packed,
                     MemRefType type, SmallVectorImpl<Value> &results);

  /// Returns the number of non-aggregate values that would be produced by
  /// `unpack`.
  static unsigned getNumUnpackedValues(MemRefType type);

private:
  // Cached index type.
  Type indexType;
};

/// Helper class allowing the user to access a range of Values that correspond
/// to an unpacked memref descriptor using named accessors. This does not own
/// the values.
class MemRefDescriptorView {
public:
  /// Constructs the view from a range of values. Infers the rank from the size
  /// of the range.
  explicit MemRefDescriptorView(ValueRange range);

  /// Returns the allocated pointer Value.
  Value allocatedPtr();

  /// Returns the aligned pointer Value.
  Value alignedPtr();

  /// Returns the offset Value.
  Value offset();

  /// Returns the pos-th size Value.
  Value size(unsigned pos);

  /// Returns the pos-th stride Value.
  Value stride(unsigned pos);

private:
  /// Rank of the memref the descriptor is pointing to.
  int rank;
  /// Underlying range of Values.
  ValueRange elements;
};

class UnrankedMemRefDescriptor : public StructBuilder {
public:
  /// Construct a helper for the given descriptor value.
  explicit UnrankedMemRefDescriptor(Value descriptor);
  /// Builds IR creating an `undef` value of the descriptor type.
  static UnrankedMemRefDescriptor undef(OpBuilder &builder, Location loc,
                                        Type descriptorType);

  /// Builds IR extracting the rank from the descriptor
  Value rank(OpBuilder &builder, Location loc);
  /// Builds IR setting the rank in the descriptor
  void setRank(OpBuilder &builder, Location loc, Value value);
  /// Builds IR extracting ranked memref descriptor ptr
  Value memRefDescPtr(OpBuilder &builder, Location loc);
  /// Builds IR setting ranked memref descriptor ptr
  void setMemRefDescPtr(OpBuilder &builder, Location loc, Value value);

  /// Builds IR populating an unranked MemRef descriptor structure from a list
  /// of individual constituent values in the following order:
  /// - rank of the memref;
  /// - pointer to the memref descriptor.
  static Value pack(OpBuilder &builder, Location loc,
                    LLVMTypeConverter &converter, UnrankedMemRefType type,
                    ValueRange values);

  /// Builds IR extracting individual elements that compose an unranked memref
  /// descriptor and returns them as `results` list.
  static void unpack(OpBuilder &builder, Location loc, Value packed,
                     SmallVectorImpl<Value> &results);

  /// Returns the number of non-aggregate values that would be produced by
  /// `unpack`.
  static unsigned getNumUnpackedValues() { return 2; }

  /// Builds IR computing the sizes in bytes (suitable for opaque allocation)
  /// and appends the corresponding values into `sizes`.
  static void computeSizes(OpBuilder &builder, Location loc,
                           LLVMTypeConverter &typeConverter,
                           ArrayRef<UnrankedMemRefDescriptor> values,
                           SmallVectorImpl<Value> &sizes);

  /// TODO: The following accessors don't take alignment rules between elements
  /// of the descriptor struct into account. For some architectures, it might be
  /// necessary to extend them and to use `llvm::DataLayout` contained in
  /// `LLVMTypeConverter`.

  /// Builds IR extracting the allocated pointer from the descriptor.
  static Value allocatedPtr(OpBuilder &builder, Location loc,
                            Value memRefDescPtr, Type elemPtrPtrType);
  /// Builds IR inserting the allocated pointer into the descriptor.
  static void setAllocatedPtr(OpBuilder &builder, Location loc,
                              Value memRefDescPtr, Type elemPtrPtrType,
                              Value allocatedPtr);

  /// Builds IR extracting the aligned pointer from the descriptor.
  static Value alignedPtr(OpBuilder &builder, Location loc,
                          LLVMTypeConverter &typeConverter, Value memRefDescPtr,
                          Type elemPtrPtrType);
  /// Builds IR inserting the aligned pointer into the descriptor.
  static void setAlignedPtr(OpBuilder &builder, Location loc,
                            LLVMTypeConverter &typeConverter,
                            Value memRefDescPtr, Type elemPtrPtrType,
                            Value alignedPtr);

  /// Builds IR extracting the offset from the descriptor.
  static Value offset(OpBuilder &builder, Location loc,
                      LLVMTypeConverter &typeConverter, Value memRefDescPtr,
                      Type elemPtrPtrType);
  /// Builds IR inserting the offset into the descriptor.
  static void setOffset(OpBuilder &builder, Location loc,
                        LLVMTypeConverter &typeConverter, Value memRefDescPtr,
                        Type elemPtrPtrType, Value offset);

  /// Builds IR extracting the pointer to the first element of the size array.
  static Value sizeBasePtr(OpBuilder &builder, Location loc,
                           LLVMTypeConverter &typeConverter,
                           Value memRefDescPtr,
                           LLVM::LLVMPointerType elemPtrPtrType);
  /// Builds IR extracting the size[index] from the descriptor.
  static Value size(OpBuilder &builder, Location loc,
                    LLVMTypeConverter typeConverter, Value sizeBasePtr,
                    Value index);
  /// Builds IR inserting the size[index] into the descriptor.
  static void setSize(OpBuilder &builder, Location loc,
                      LLVMTypeConverter typeConverter, Value sizeBasePtr,
                      Value index, Value size);

  /// Builds IR extracting the pointer to the first element of the stride array.
  static Value strideBasePtr(OpBuilder &builder, Location loc,
                             LLVMTypeConverter &typeConverter,
                             Value sizeBasePtr, Value rank);
  /// Builds IR extracting the stride[index] from the descriptor.
  static Value stride(OpBuilder &builder, Location loc,
                      LLVMTypeConverter typeConverter, Value strideBasePtr,
                      Value index, Value stride);
  /// Builds IR inserting the stride[index] into the descriptor.
  static void setStride(OpBuilder &builder, Location loc,
                        LLVMTypeConverter typeConverter, Value strideBasePtr,
                        Value index, Value stride);
};

/// Base class for operation conversions targeting the LLVM IR dialect. It
/// provides the conversion patterns with access to the LLVMTypeConverter and
/// the LowerToLLVMOptions. The class captures the LLVMTypeConverter and the
/// LowerToLLVMOptions by reference meaning the references have to remain alive
/// during the entire pattern lifetime.
class ConvertToLLVMPattern : public ConversionPattern {
public:
  ConvertToLLVMPattern(StringRef rootOpName, MLIRContext *context,
                       LLVMTypeConverter &typeConverter,
                       PatternBenefit benefit = 1);

protected:
  /// Returns the LLVM dialect.
  LLVM::LLVMDialect &getDialect() const;

  LLVMTypeConverter *getTypeConverter() const;

  /// Gets the MLIR type wrapping the LLVM integer type whose bit width is
  /// defined by the used type converter.
  Type getIndexType() const;

  /// Gets the MLIR type wrapping the LLVM integer type whose bit width
  /// corresponds to that of a LLVM pointer type.
  Type getIntPtrType(unsigned addressSpace = 0) const;

  /// Gets the MLIR type wrapping the LLVM void type.
  Type getVoidType() const;

  /// Get the MLIR type wrapping the LLVM i8* type.
  Type getVoidPtrType() const;

  /// Create an LLVM dialect operation defining the given index constant.
  Value createIndexConstant(ConversionPatternRewriter &builder, Location loc,
                            uint64_t value) const;

  // This is a strided getElementPtr variant that linearizes subscripts as:
  //   `base_offset + index_0 * stride_0 + ... + index_n * stride_n`.
  Value getStridedElementPtr(Location loc, MemRefType type, Value memRefDesc,
                             ValueRange indices,
                             ConversionPatternRewriter &rewriter) const;


  /// Returns if the givem memref type is supported.
  bool isSupportedMemRefType(MemRefType type) const;



  /// Returns if the given memref has identity maps and the element type is
  /// convertible to LLVM.
  bool isConvertibleAndHasIdentityMaps(MemRefType type) const;

  /// Returns the type of a pointer to an element of the memref.
  Type getElementPtrType(MemRefType type) const;

  /// Computes sizes, strides and buffer size in bytes of `memRefType` with
  /// identity layout. Emits constant ops for the static sizes of `memRefType`,
  /// and uses `dynamicSizes` for the others. Emits instructions to compute
  /// strides and buffer size from these sizes.
  ///
  /// For example, memref<4x?xf32> emits:
  /// `sizes[0]`   = llvm.mlir.constant(4 : index) : i64
  /// `sizes[1]`   = `dynamicSizes[0]`
  /// `strides[1]` = llvm.mlir.constant(1 : index) : i64
  /// `strides[0]` = `sizes[0]`
  /// %size        = llvm.mul `sizes[0]`, `sizes[1]` : i64
  /// %nullptr     = llvm.mlir.null : !llvm.ptr<f32>
  /// %gep         = llvm.getelementptr %nullptr[%size]
  ///                  : (!llvm.ptr<f32>, i64) -> !llvm.ptr<f32>
  /// `sizeBytes`  = llvm.ptrtoint %gep : !llvm.ptr<f32> to i64
  void getMemRefDescriptorSizes(Location loc, MemRefType memRefType,
                                ValueRange dynamicSizes,
                                ConversionPatternRewriter &rewriter,
                                SmallVectorImpl<Value> &sizes,
                                SmallVectorImpl<Value> &strides,
                                Value &sizeBytes) const;

  /// Computes the size of type in bytes.
  Value getSizeInBytes(Location loc, Type type,
                       ConversionPatternRewriter &rewriter) const;

  /// Computes total number of elements for the given shape.
  Value getNumElements(Location loc, ArrayRef<Value> shape,
                       ConversionPatternRewriter &rewriter) const;

  /// Creates and populates a canonical memref descriptor struct.
  MemRefDescriptor
  createMemRefDescriptor(Location loc, MemRefType memRefType,
                         Value allocatedPtr, Value alignedPtr,
                         ArrayRef<Value> sizes, ArrayRef<Value> strides,
                         ConversionPatternRewriter &rewriter) const;
};

/// Utility class for operation conversions targeting the LLVM dialect that
/// match exactly one source operation.
template <typename SourceOp>
class ConvertOpToLLVMPattern : public ConvertToLLVMPattern {
public:
  explicit ConvertOpToLLVMPattern(LLVMTypeConverter &typeConverter,
                                  PatternBenefit benefit = 1)
      : ConvertToLLVMPattern(SourceOp::getOperationName(),
                             &typeConverter.getContext(), typeConverter,
                             benefit) {}

  /// Wrappers around the RewritePattern methods that pass the derived op type.
  void rewrite(Operation *op, ArrayRef<Value> operands,
               ConversionPatternRewriter &rewriter) const final {
    rewrite(cast<SourceOp>(op), operands, rewriter);
  }
  LogicalResult match(Operation *op) const final {
    return match(cast<SourceOp>(op));
  }
  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const final {
    return matchAndRewrite(cast<SourceOp>(op), operands, rewriter);
  }

  /// Rewrite and Match methods that operate on the SourceOp type. These must be
  /// overridden by the derived pattern class.
  virtual void rewrite(SourceOp op, ArrayRef<Value> operands,
                       ConversionPatternRewriter &rewriter) const {
    llvm_unreachable("must override rewrite or matchAndRewrite");
  }
  virtual LogicalResult match(SourceOp op) const {
    llvm_unreachable("must override match or matchAndRewrite");
  }
  virtual LogicalResult
  matchAndRewrite(SourceOp op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const {
    if (succeeded(match(op))) {
      rewrite(op, operands, rewriter);
      return success();
    }
    return failure();
  }

private:
  using ConvertToLLVMPattern::match;
  using ConvertToLLVMPattern::matchAndRewrite;
};

namespace LLVM {
namespace detail {
/// Replaces the given operation "op" with a new operation of type "targetOp"
/// and given operands.
LogicalResult oneToOneRewrite(Operation *op, StringRef targetOp,
                              ValueRange operands,
                              LLVMTypeConverter &typeConverter,
                              ConversionPatternRewriter &rewriter);

LogicalResult vectorOneToOneRewrite(Operation *op, StringRef targetOp,
                                    ValueRange operands,
                                    LLVMTypeConverter &typeConverter,
                                    ConversionPatternRewriter &rewriter);
} // namespace detail
} // namespace LLVM

/// Generic implementation of one-to-one conversion from "SourceOp" to
/// "TargetOp" where the latter belongs to the LLVM dialect or an equivalent.
/// Upholds a convention that multi-result operations get converted into an
/// operation returning the LLVM IR structure type, in which case individual
/// values must be extracted from using LLVM::ExtractValueOp before being used.
template <typename SourceOp, typename TargetOp>
class OneToOneConvertToLLVMPattern : public ConvertOpToLLVMPattern<SourceOp> {
public:
  using ConvertOpToLLVMPattern<SourceOp>::ConvertOpToLLVMPattern;
  using Super = OneToOneConvertToLLVMPattern<SourceOp, TargetOp>;

  /// Converts the type of the result to an LLVM type, pass operands as is,
  /// preserve attributes.
  LogicalResult
  matchAndRewrite(SourceOp op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    return LLVM::detail::oneToOneRewrite(op, TargetOp::getOperationName(),
                                         operands, *this->getTypeConverter(),
                                         rewriter);
  }
};

/// Basic lowering implementation to rewrite Ops with just one result to the
/// LLVM Dialect. This supports higher-dimensional vector types.
template <typename SourceOp, typename TargetOp>
class VectorConvertToLLVMPattern : public ConvertOpToLLVMPattern<SourceOp> {
public:
  using ConvertOpToLLVMPattern<SourceOp>::ConvertOpToLLVMPattern;
  using Super = VectorConvertToLLVMPattern<SourceOp, TargetOp>;

  LogicalResult
  matchAndRewrite(SourceOp op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    static_assert(
        std::is_base_of<OpTrait::OneResult<SourceOp>, SourceOp>::value,
        "expected single result op");
    return LLVM::detail::vectorOneToOneRewrite(
        op, TargetOp::getOperationName(), operands, *this->getTypeConverter(),
        rewriter);
  }
};

/// Derived class that automatically populates legalization information for
/// different LLVM ops.
class LLVMConversionTarget : public ConversionTarget {
public:
  explicit LLVMConversionTarget(MLIRContext &ctx);
};

} // namespace mlir

#endif // MLIR_CONVERSION_STANDARDTOLLVM_CONVERTSTANDARDTOLLVM_H
