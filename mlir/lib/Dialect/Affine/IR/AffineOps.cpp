//===- AffineOps.cpp - MLIR Affine Operations -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Support/MathExtras.h"
#include "mlir/Transforms/InliningUtils.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"

using namespace mlir;
using llvm::dbgs;

#define DEBUG_TYPE "affine-analysis"

/// A utility function to check if a value is defined at the top level of
/// `region` or is an argument of `region`. A value of index type defined at the
/// top level of a `AffineScope` region is always a valid symbol for all
/// uses in that region.
static bool isTopLevelValue(Value value, Region *region) {
  if (auto arg = value.dyn_cast<BlockArgument>())
    return arg.getParentRegion() == region;
  return value.getDefiningOp()->getParentRegion() == region;
}

/// Checks if `value` known to be a legal affine dimension or symbol in `src`
/// region remains legal if the operation that uses it is inlined into `dest`
/// with the given value mapping. `legalityCheck` is either `isValidDim` or
/// `isValidSymbol`, depending on the value being required to remain a valid
/// dimension or symbol.
static bool
remainsLegalAfterInline(Value value, Region *src, Region *dest,
                        const BlockAndValueMapping &mapping,
                        function_ref<bool(Value, Region *)> legalityCheck) {
  // If the value is a valid dimension for any other reason than being
  // a top-level value, it will remain valid: constants get inlined
  // with the function, transitive affine applies also get inlined and
  // will be checked themselves, etc.
  if (!isTopLevelValue(value, src))
    return true;

  // If it's a top-level value because it's a block operand, i.e. a
  // function argument, check whether the value replacing it after
  // inlining is a valid dimension in the new region.
  if (value.isa<BlockArgument>())
    return legalityCheck(mapping.lookup(value), dest);

  // If it's a top-level value beacuse it's defined in the region,
  // it can only be inlined if the defining op is a constant or a
  // `dim`, which can appear anywhere and be valid, since the defining
  // op won't be top-level anymore after inlining.
  Attribute operandCst;
  return matchPattern(value.getDefiningOp(), m_Constant(&operandCst)) ||
         value.getDefiningOp<DimOp>();
}

/// Checks if all values known to be legal affine dimensions or symbols in `src`
/// remain so if their respective users are inlined into `dest`.
static bool
remainsLegalAfterInline(ValueRange values, Region *src, Region *dest,
                        const BlockAndValueMapping &mapping,
                        function_ref<bool(Value, Region *)> legalityCheck) {
  return llvm::all_of(values, [&](Value v) {
    return remainsLegalAfterInline(v, src, dest, mapping, legalityCheck);
  });
}

/// Checks if an affine read or write operation remains legal after inlining
/// from `src` to `dest`.
template <typename OpTy>
static bool remainsLegalAfterInline(OpTy op, Region *src, Region *dest,
                                    const BlockAndValueMapping &mapping) {
  static_assert(llvm::is_one_of<OpTy, AffineReadOpInterface,
                                AffineWriteOpInterface>::value,
                "only ops with affine read/write interface are supported");

  AffineMap map = op.getAffineMap();
  ValueRange dimOperands = op.getMapOperands().take_front(map.getNumDims());
  ValueRange symbolOperands =
      op.getMapOperands().take_back(map.getNumSymbols());
  if (!remainsLegalAfterInline(
          dimOperands, src, dest, mapping,
          static_cast<bool (*)(Value, Region *)>(isValidDim)))
    return false;
  if (!remainsLegalAfterInline(
          symbolOperands, src, dest, mapping,
          static_cast<bool (*)(Value, Region *)>(isValidSymbol)))
    return false;
  return true;
}

/// Checks if an affine apply operation remains legal after inlining from `src`
/// to `dest`.
template <>
bool remainsLegalAfterInline(AffineApplyOp op, Region *src, Region *dest,
                             const BlockAndValueMapping &mapping) {
  // If it's a valid dimension, we need to check that it remains so.
  if (isValidDim(op.getResult(), src))
    return remainsLegalAfterInline(
        op.getMapOperands(), src, dest, mapping,
        static_cast<bool (*)(Value, Region *)>(isValidDim));

  // Otherwise it must be a valid symbol, check that it remains so.
  return remainsLegalAfterInline(
      op.getMapOperands(), src, dest, mapping,
      static_cast<bool (*)(Value, Region *)>(isValidSymbol));
}

//===----------------------------------------------------------------------===//
// AffineDialect Interfaces
//===----------------------------------------------------------------------===//

namespace {
/// This class defines the interface for handling inlining with affine
/// operations.
struct AffineInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;

  //===--------------------------------------------------------------------===//
  // Analysis Hooks
  //===--------------------------------------------------------------------===//

  /// Returns true if the given region 'src' can be inlined into the region
  /// 'dest' that is attached to an operation registered to the current dialect.
  /// 'wouldBeCloned' is set if the region is cloned into its new location
  /// rather than moved, indicating there may be other users.
  bool isLegalToInline(Region *dest, Region *src, bool wouldBeCloned,
                       BlockAndValueMapping &valueMapping) const final {
    // We can inline into affine loops and conditionals if this doesn't break
    // affine value categorization rules.
    Operation *destOp = dest->getParentOp();
    if (!isa<AffineParallelOp, AffineForOp, AffineIfOp>(destOp))
      return false;

    // Multi-block regions cannot be inlined into affine constructs, all of
    // which require single-block regions.
    if (!llvm::hasSingleElement(*src))
      return false;

    // Side-effecting operations that the affine dialect cannot understand
    // should not be inlined.
    Block &srcBlock = src->front();
    for (Operation &op : srcBlock) {
      // Ops with no side effects are fine,
      if (auto iface = dyn_cast<MemoryEffectOpInterface>(op)) {
        if (iface.hasNoEffect())
          continue;
      }

      // Assuming the inlined region is valid, we only need to check if the
      // inlining would change it.
      bool remainsValid =
          llvm::TypeSwitch<Operation *, bool>(&op)
              .Case<AffineApplyOp, AffineReadOpInterface,
                    AffineWriteOpInterface>([&](auto op) {
                return remainsLegalAfterInline(op, src, dest, valueMapping);
              })
              .Default([](Operation *) {
                // Conservatively disallow inlining ops we cannot reason about.
                return false;
              });

      if (!remainsValid)
        return false;
    }

    return true;
  }

  /// Returns true if the given operation 'op', that is registered to this
  /// dialect, can be inlined into the given region, false otherwise.
  bool isLegalToInline(Operation *op, Region *region, bool wouldBeCloned,
                       BlockAndValueMapping &valueMapping) const final {
    // Always allow inlining affine operations into a region that is marked as
    // affine scope, or into affine loops and conditionals. There are some edge
    // cases when inlining *into* affine structures, but that is handled in the
    // other 'isLegalToInline' hook above.
    Operation *parentOp = region->getParentOp();
    return parentOp->hasTrait<OpTrait::AffineScope>() ||
           isa<AffineForOp, AffineParallelOp, AffineIfOp>(parentOp);
  }

  /// Affine regions should be analyzed recursively.
  bool shouldAnalyzeRecursively(Operation *op) const final { return true; }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// AffineDialect
//===----------------------------------------------------------------------===//

void AffineDialect::initialize() {
  addOperations<AffineDmaStartOp, AffineDmaWaitOp,
#define GET_OP_LIST
#include "mlir/Dialect/Affine/IR/AffineOps.cpp.inc"
                >();
  addInterfaces<AffineInlinerInterface>();
}

/// Materialize a single constant operation from a given attribute value with
/// the desired resultant type.
Operation *AffineDialect::materializeConstant(OpBuilder &builder,
                                              Attribute value, Type type,
                                              Location loc) {
  return builder.create<ConstantOp>(loc, type, value);
}

/// A utility function to check if a value is defined at the top level of an
/// op with trait `AffineScope`. If the value is defined in an unlinked region,
/// conservatively assume it is not top-level. A value of index type defined at
/// the top level is always a valid symbol.
bool mlir::isTopLevelValue(Value value) {
  if (auto arg = value.dyn_cast<BlockArgument>()) {
    // The block owning the argument may be unlinked, e.g. when the surrounding
    // region has not yet been attached to an Op, at which point the parent Op
    // is null.
    Operation *parentOp = arg.getOwner()->getParentOp();
    return parentOp && parentOp->hasTrait<OpTrait::AffineScope>();
  }
  // The defining Op may live in an unlinked block so its parent Op may be null.
  Operation *parentOp = value.getDefiningOp()->getParentOp();
  return parentOp && parentOp->hasTrait<OpTrait::AffineScope>();
}

/// Returns the closest region enclosing `op` that is held by an operation with
/// trait `AffineScope`; `nullptr` if there is no such region.
//  TODO: getAffineScope should be publicly exposed for affine passes/utilities.
static Region *getAffineScope(Operation *op) {
  auto *curOp = op;
  while (auto *parentOp = curOp->getParentOp()) {
    if (parentOp->hasTrait<OpTrait::AffineScope>())
      return curOp->getParentRegion();
    curOp = parentOp;
  }
  return nullptr;
}

// A Value can be used as a dimension id iff it meets one of the following
// conditions:
// *) It is valid as a symbol.
// *) It is an induction variable.
// *) It is the result of affine apply operation with dimension id arguments.
bool mlir::isValidDim(Value value) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  if (auto *defOp = value.getDefiningOp())
    return isValidDim(value, getAffineScope(defOp));

  // This value has to be a block argument for an op that has the
  // `AffineScope` trait or for an affine.for or affine.parallel.
  auto *parentOp = value.cast<BlockArgument>().getOwner()->getParentOp();
  return parentOp && (parentOp->hasTrait<OpTrait::AffineScope>() ||
                      isa<AffineForOp, AffineParallelOp>(parentOp));
}

// Value can be used as a dimension id iff it meets one of the following
// conditions:
// *) It is valid as a symbol.
// *) It is an induction variable.
// *) It is the result of an affine apply operation with dimension id operands.
bool mlir::isValidDim(Value value, Region *region) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  // All valid symbols are okay.
  if (isValidSymbol(value, region))
    return true;

  auto *op = value.getDefiningOp();
  if (!op) {
    // This value has to be a block argument for an affine.for or an
    // affine.parallel.
    auto *parentOp = value.cast<BlockArgument>().getOwner()->getParentOp();
    return isa<AffineForOp, AffineParallelOp>(parentOp);
  }

  // Affine apply operation is ok if all of its operands are ok.
  if (auto applyOp = dyn_cast<AffineApplyOp>(op))
    return applyOp.isValidDim(region);
  // The dim op is okay if its operand memref/tensor is defined at the top
  // level.
  if (auto dimOp = dyn_cast<DimOp>(op))
    return isTopLevelValue(dimOp.memrefOrTensor());
  return false;
}

/// Returns true if the 'index' dimension of the `memref` defined by
/// `memrefDefOp` is a statically  shaped one or defined using a valid symbol
/// for `region`.
template <typename AnyMemRefDefOp>
static bool isMemRefSizeValidSymbol(AnyMemRefDefOp memrefDefOp, unsigned index,
                                    Region *region) {
  auto memRefType = memrefDefOp.getType();
  // Statically shaped.
  if (!memRefType.isDynamicDim(index))
    return true;
  // Get the position of the dimension among dynamic dimensions;
  unsigned dynamicDimPos = memRefType.getDynamicDimIndex(index);
  return isValidSymbol(*(memrefDefOp.getDynamicSizes().begin() + dynamicDimPos),
                       region);
}

/// Returns true if the result of the dim op is a valid symbol for `region`.
static bool isDimOpValidSymbol(DimOp dimOp, Region *region) {
  // The dim op is okay if its operand memref/tensor is defined at the top
  // level.
  if (isTopLevelValue(dimOp.memrefOrTensor()))
    return true;

  // Conservatively handle remaining BlockArguments as non-valid symbols.
  // E.g. scf.for iterArgs.
  if (dimOp.memrefOrTensor().isa<BlockArgument>())
    return false;

  // The dim op is also okay if its operand memref/tensor is a view/subview
  // whose corresponding size is a valid symbol.
  Optional<int64_t> index = dimOp.getConstantIndex();
  assert(index.hasValue() &&
         "expect only `dim` operations with a constant index");
  int64_t i = index.getValue();
  return TypeSwitch<Operation *, bool>(dimOp.memrefOrTensor().getDefiningOp())
      .Case<ViewOp, SubViewOp, AllocOp>(
          [&](auto op) { return isMemRefSizeValidSymbol(op, i, region); })
      .Default([](Operation *) { return false; });
}

// A value can be used as a symbol (at all its use sites) iff it meets one of
// the following conditions:
// *) It is a constant.
// *) Its defining op or block arg appearance is immediately enclosed by an op
//    with `AffineScope` trait.
// *) It is the result of an affine.apply operation with symbol operands.
// *) It is a result of the dim op on a memref whose corresponding size is a
//    valid symbol.
bool mlir::isValidSymbol(Value value) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  // Check that the value is a top level value.
  if (isTopLevelValue(value))
    return true;

  if (auto *defOp = value.getDefiningOp())
    return isValidSymbol(value, getAffineScope(defOp));

  return false;
}

/// A value can be used as a symbol for `region` iff it meets onf of the the
/// following conditions:
/// *) It is a constant.
/// *) It is the result of an affine apply operation with symbol arguments.
/// *) It is a result of the dim op on a memref whose corresponding size is
///    a valid symbol.
/// *) It is defined at the top level of 'region' or is its argument.
/// *) It dominates `region`'s parent op.
/// If `region` is null, conservatively assume the symbol definition scope does
/// not exist and only accept the values that would be symbols regardless of
/// the surrounding region structure, i.e. the first three cases above.
bool mlir::isValidSymbol(Value value, Region *region) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  // A top-level value is a valid symbol.
  if (region && ::isTopLevelValue(value, region))
    return true;

  auto *defOp = value.getDefiningOp();
  if (!defOp) {
    // A block argument that is not a top-level value is a valid symbol if it
    // dominates region's parent op.
    Operation *regionOp = region ? region->getParentOp() : nullptr;
    if (regionOp && !regionOp->hasTrait<OpTrait::IsIsolatedFromAbove>())
      if (auto *parentOpRegion = region->getParentOp()->getParentRegion())
        return isValidSymbol(value, parentOpRegion);
    return false;
  }

  // Constant operation is ok.
  Attribute operandCst;
  if (matchPattern(defOp, m_Constant(&operandCst)))
    return true;

  // Affine apply operation is ok if all of its operands are ok.
  if (auto applyOp = dyn_cast<AffineApplyOp>(defOp))
    return applyOp.isValidSymbol(region);

  // Dim op results could be valid symbols at any level.
  if (auto dimOp = dyn_cast<DimOp>(defOp))
    return isDimOpValidSymbol(dimOp, region);

  // Check for values dominating `region`'s parent op.
  Operation *regionOp = region ? region->getParentOp() : nullptr;
  if (regionOp && !regionOp->hasTrait<OpTrait::IsIsolatedFromAbove>())
    if (auto *parentRegion = region->getParentOp()->getParentRegion())
      return isValidSymbol(value, parentRegion);

  return false;
}

// Returns true if 'value' is a valid index to an affine operation (e.g.
// affine.load, affine.store, affine.dma_start, affine.dma_wait) where
// `region` provides the polyhedral symbol scope. Returns false otherwise.
static bool isValidAffineIndexOperand(Value value, Region *region) {
  return isValidDim(value, region) || isValidSymbol(value, region);
}

/// Prints dimension and symbol list.
static void printDimAndSymbolList(Operation::operand_iterator begin,
                                  Operation::operand_iterator end,
                                  unsigned numDims, OpAsmPrinter &printer) {
  OperandRange operands(begin, end);
  printer << '(' << operands.take_front(numDims) << ')';
  if (operands.size() > numDims)
    printer << '[' << operands.drop_front(numDims) << ']';
}

/// Parses dimension and symbol list and returns true if parsing failed.
ParseResult mlir::parseDimAndSymbolList(OpAsmParser &parser,
                                        SmallVectorImpl<Value> &operands,
                                        unsigned &numDims) {
  SmallVector<OpAsmParser::OperandType, 8> opInfos;
  if (parser.parseOperandList(opInfos, OpAsmParser::Delimiter::Paren))
    return failure();
  // Store number of dimensions for validation by caller.
  numDims = opInfos.size();

  // Parse the optional symbol operands.
  auto indexTy = parser.getBuilder().getIndexType();
  return failure(parser.parseOperandList(
                     opInfos, OpAsmParser::Delimiter::OptionalSquare) ||
                 parser.resolveOperands(opInfos, indexTy, operands));
}

/// Utility function to verify that a set of operands are valid dimension and
/// symbol identifiers. The operands should be laid out such that the dimension
/// operands are before the symbol operands. This function returns failure if
/// there was an invalid operand. An operation is provided to emit any necessary
/// errors.
template <typename OpTy>
static LogicalResult
verifyDimAndSymbolIdentifiers(OpTy &op, Operation::operand_range operands,
                              unsigned numDims) {
  unsigned opIt = 0;
  for (auto operand : operands) {
    if (opIt++ < numDims) {
      if (!isValidDim(operand, getAffineScope(op)))
        return op.emitOpError("operand cannot be used as a dimension id");
    } else if (!isValidSymbol(operand, getAffineScope(op))) {
      return op.emitOpError("operand cannot be used as a symbol");
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// AffineApplyOp
//===----------------------------------------------------------------------===//

AffineValueMap AffineApplyOp::getAffineValueMap() {
  return AffineValueMap(getAffineMap(), getOperands(), getResult());
}

static ParseResult parseAffineApplyOp(OpAsmParser &parser,
                                      OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  AffineMapAttr mapAttr;
  unsigned numDims;
  if (parser.parseAttribute(mapAttr, "map", result.attributes) ||
      parseDimAndSymbolList(parser, result.operands, numDims) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();
  auto map = mapAttr.getValue();

  if (map.getNumDims() != numDims ||
      numDims + map.getNumSymbols() != result.operands.size()) {
    return parser.emitError(parser.getNameLoc(),
                            "dimension or symbol index mismatch");
  }

  result.types.append(map.getNumResults(), indexTy);
  return success();
}

static void print(OpAsmPrinter &p, AffineApplyOp op) {
  p << AffineApplyOp::getOperationName() << " " << op.mapAttr();
  printDimAndSymbolList(op.operand_begin(), op.operand_end(),
                        op.getAffineMap().getNumDims(), p);
  p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/{"map"});
}

static LogicalResult verify(AffineApplyOp op) {
  // Check input and output dimensions match.
  auto map = op.map();

  // Verify that operand count matches affine map dimension and symbol count.
  if (op.getNumOperands() != map.getNumDims() + map.getNumSymbols())
    return op.emitOpError(
        "operand count and affine map dimension and symbol count must match");

  // Verify that the map only produces one result.
  if (map.getNumResults() != 1)
    return op.emitOpError("mapping must produce one value");

  return success();
}

// The result of the affine apply operation can be used as a dimension id if all
// its operands are valid dimension ids.
bool AffineApplyOp::isValidDim() {
  return llvm::all_of(getOperands(),
                      [](Value op) { return mlir::isValidDim(op); });
}

// The result of the affine apply operation can be used as a dimension id if all
// its operands are valid dimension ids with the parent operation of `region`
// defining the polyhedral scope for symbols.
bool AffineApplyOp::isValidDim(Region *region) {
  return llvm::all_of(getOperands(),
                      [&](Value op) { return ::isValidDim(op, region); });
}

// The result of the affine apply operation can be used as a symbol if all its
// operands are symbols.
bool AffineApplyOp::isValidSymbol() {
  return llvm::all_of(getOperands(),
                      [](Value op) { return mlir::isValidSymbol(op); });
}

// The result of the affine apply operation can be used as a symbol in `region`
// if all its operands are symbols in `region`.
bool AffineApplyOp::isValidSymbol(Region *region) {
  return llvm::all_of(getOperands(), [&](Value operand) {
    return mlir::isValidSymbol(operand, region);
  });
}

OpFoldResult AffineApplyOp::fold(ArrayRef<Attribute> operands) {
  auto map = getAffineMap();

  // Fold dims and symbols to existing values.
  auto expr = map.getResult(0);
  if (auto dim = expr.dyn_cast<AffineDimExpr>())
    return getOperand(dim.getPosition());
  if (auto sym = expr.dyn_cast<AffineSymbolExpr>())
    return getOperand(map.getNumDims() + sym.getPosition());

  // Otherwise, default to folding the map.
  SmallVector<Attribute, 1> result;
  if (failed(map.constantFold(operands, result)))
    return {};
  return result[0];
}

/// Replace all occurrences of AffineExpr at position `pos` in `map` by the
/// defining AffineApplyOp expression and operands.
/// When `dimOrSymbolPosition < dims.size()`, AffineDimExpr@[pos] is replaced.
/// When `dimOrSymbolPosition >= dims.size()`,
/// AffineSymbolExpr@[pos - dims.size()] is replaced.
/// Mutate `map`,`dims` and `syms` in place as follows:
///   1. `dims` and `syms` are only appended to.
///   2. `map` dim and symbols are gradually shifted to higer positions.
///   3. Old `dim` and `sym` entries are replaced by nullptr
/// This avoids the need for any bookkeeping.
static LogicalResult replaceDimOrSym(AffineMap *map,
                                     unsigned dimOrSymbolPosition,
                                     SmallVectorImpl<Value> &dims,
                                     SmallVectorImpl<Value> &syms) {
  bool isDimReplacement = (dimOrSymbolPosition < dims.size());
  unsigned pos = isDimReplacement ? dimOrSymbolPosition
                                  : dimOrSymbolPosition - dims.size();
  Value &v = isDimReplacement ? dims[pos] : syms[pos];
  if (!v)
    return failure();

  auto affineApply = v.getDefiningOp<AffineApplyOp>();
  if (!affineApply)
    return failure();

  // At this point we will perform a replacement of `v`, set the entry in `dim`
  // or `sym` to nullptr immediately.
  v = nullptr;

  // Compute the map, dims and symbols coming from the AffineApplyOp.
  AffineMap composeMap = affineApply.getAffineMap();
  assert(composeMap.getNumResults() == 1 && "affine.apply with >1 results");
  AffineExpr composeExpr =
      composeMap.shiftDims(dims.size()).shiftSymbols(syms.size()).getResult(0);
  ValueRange composeDims =
      affineApply.getMapOperands().take_front(composeMap.getNumDims());
  ValueRange composeSyms =
      affineApply.getMapOperands().take_back(composeMap.getNumSymbols());

  // Perform the replacement and append the dims and symbols where relevant.
  MLIRContext *ctx = map->getContext();
  AffineExpr toReplace = isDimReplacement ? getAffineDimExpr(pos, ctx)
                                          : getAffineSymbolExpr(pos, ctx);
  *map = map->replace(toReplace, composeExpr, dims.size(), syms.size());
  dims.append(composeDims.begin(), composeDims.end());
  syms.append(composeSyms.begin(), composeSyms.end());

  return success();
}

/// Iterate over `operands` and fold away all those produced by an AffineApplyOp
/// iteratively. Perform canonicalization of map and operands as well as
/// AffineMap simplification. `map` and `operands` are mutated in place.
static void composeAffineMapAndOperands(AffineMap *map,
                                        SmallVectorImpl<Value> *operands) {
  if (map->getNumResults() == 0) {
    canonicalizeMapAndOperands(map, operands);
    *map = simplifyAffineMap(*map);
    return;
  }

  MLIRContext *ctx = map->getContext();
  SmallVector<Value, 4> dims(operands->begin(),
                             operands->begin() + map->getNumDims());
  SmallVector<Value, 4> syms(operands->begin() + map->getNumDims(),
                             operands->end());

  // Iterate over dims and symbols coming from AffineApplyOp and replace until
  // exhaustion. This iteratively mutates `map`, `dims` and `syms`. Both `dims`
  // and `syms` can only increase by construction.
  // The implementation uses a `while` loop to support the case of symbols
  // that may be constructed from dims ;this may be overkill.
  while (true) {
    bool changed = false;
    for (unsigned pos = 0; pos != dims.size() + syms.size(); ++pos)
      if ((changed |= succeeded(replaceDimOrSym(map, pos, dims, syms))))
        break;
    if (!changed)
      break;
  }

  // Clear operands so we can fill them anew.
  operands->clear();

  // At this point we may have introduced null operands, prune them out before
  // canonicalizing map and operands.
  unsigned nDims = 0, nSyms = 0;
  SmallVector<AffineExpr, 4> dimReplacements, symReplacements;
  dimReplacements.reserve(dims.size());
  symReplacements.reserve(syms.size());
  for (auto *container : {&dims, &syms}) {
    bool isDim = (container == &dims);
    auto &repls = isDim ? dimReplacements : symReplacements;
    for (auto en : llvm::enumerate(*container)) {
      Value v = en.value();
      if (!v) {
        assert(isDim ? !map->isFunctionOfDim(en.index())
                     : !map->isFunctionOfSymbol(en.index()) &&
                           "map is function of unexpected expr@pos");
        repls.push_back(getAffineConstantExpr(0, ctx));
        continue;
      }
      repls.push_back(isDim ? getAffineDimExpr(nDims++, ctx)
                            : getAffineSymbolExpr(nSyms++, ctx));
      operands->push_back(v);
    }
  }
  *map = map->replaceDimsAndSymbols(dimReplacements, symReplacements, nDims,
                                    nSyms);

  // Canonicalize and simplify before returning.
  canonicalizeMapAndOperands(map, operands);
  *map = simplifyAffineMap(*map);
}

void mlir::fullyComposeAffineMapAndOperands(AffineMap *map,
                                            SmallVectorImpl<Value> *operands) {
  while (llvm::any_of(*operands, [](Value v) {
    return isa_and_nonnull<AffineApplyOp>(v.getDefiningOp());
  })) {
    composeAffineMapAndOperands(map, operands);
  }
}

AffineApplyOp mlir::makeComposedAffineApply(OpBuilder &b, Location loc,
                                            AffineMap map,
                                            ArrayRef<Value> operands) {
  AffineMap normalizedMap = map;
  SmallVector<Value, 8> normalizedOperands(operands.begin(), operands.end());
  composeAffineMapAndOperands(&normalizedMap, &normalizedOperands);
  assert(normalizedMap);
  return b.create<AffineApplyOp>(loc, normalizedMap, normalizedOperands);
}

// A symbol may appear as a dim in affine.apply operations. This function
// canonicalizes dims that are valid symbols into actual symbols.
template <class MapOrSet>
static void canonicalizePromotedSymbols(MapOrSet *mapOrSet,
                                        SmallVectorImpl<Value> *operands) {
  if (!mapOrSet || operands->empty())
    return;

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");

  auto *context = mapOrSet->getContext();
  SmallVector<Value, 8> resultOperands;
  resultOperands.reserve(operands->size());
  SmallVector<Value, 8> remappedSymbols;
  remappedSymbols.reserve(operands->size());
  unsigned nextDim = 0;
  unsigned nextSym = 0;
  unsigned oldNumSyms = mapOrSet->getNumSymbols();
  SmallVector<AffineExpr, 8> dimRemapping(mapOrSet->getNumDims());
  for (unsigned i = 0, e = mapOrSet->getNumInputs(); i != e; ++i) {
    if (i < mapOrSet->getNumDims()) {
      if (isValidSymbol((*operands)[i])) {
        // This is a valid symbol that appears as a dim, canonicalize it.
        dimRemapping[i] = getAffineSymbolExpr(oldNumSyms + nextSym++, context);
        remappedSymbols.push_back((*operands)[i]);
      } else {
        dimRemapping[i] = getAffineDimExpr(nextDim++, context);
        resultOperands.push_back((*operands)[i]);
      }
    } else {
      resultOperands.push_back((*operands)[i]);
    }
  }

  resultOperands.append(remappedSymbols.begin(), remappedSymbols.end());
  *operands = resultOperands;
  *mapOrSet = mapOrSet->replaceDimsAndSymbols(dimRemapping, {}, nextDim,
                                              oldNumSyms + nextSym);

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");
}

// Works for either an affine map or an integer set.
template <class MapOrSet>
static void canonicalizeMapOrSetAndOperands(MapOrSet *mapOrSet,
                                            SmallVectorImpl<Value> *operands) {
  static_assert(llvm::is_one_of<MapOrSet, AffineMap, IntegerSet>::value,
                "Argument must be either of AffineMap or IntegerSet type");

  if (!mapOrSet || operands->empty())
    return;

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");

  canonicalizePromotedSymbols<MapOrSet>(mapOrSet, operands);

  // Check to see what dims are used.
  llvm::SmallBitVector usedDims(mapOrSet->getNumDims());
  llvm::SmallBitVector usedSyms(mapOrSet->getNumSymbols());
  mapOrSet->walkExprs([&](AffineExpr expr) {
    if (auto dimExpr = expr.dyn_cast<AffineDimExpr>())
      usedDims[dimExpr.getPosition()] = true;
    else if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>())
      usedSyms[symExpr.getPosition()] = true;
  });

  auto *context = mapOrSet->getContext();

  SmallVector<Value, 8> resultOperands;
  resultOperands.reserve(operands->size());

  llvm::SmallDenseMap<Value, AffineExpr, 8> seenDims;
  SmallVector<AffineExpr, 8> dimRemapping(mapOrSet->getNumDims());
  unsigned nextDim = 0;
  for (unsigned i = 0, e = mapOrSet->getNumDims(); i != e; ++i) {
    if (usedDims[i]) {
      // Remap dim positions for duplicate operands.
      auto it = seenDims.find((*operands)[i]);
      if (it == seenDims.end()) {
        dimRemapping[i] = getAffineDimExpr(nextDim++, context);
        resultOperands.push_back((*operands)[i]);
        seenDims.insert(std::make_pair((*operands)[i], dimRemapping[i]));
      } else {
        dimRemapping[i] = it->second;
      }
    }
  }
  llvm::SmallDenseMap<Value, AffineExpr, 8> seenSymbols;
  SmallVector<AffineExpr, 8> symRemapping(mapOrSet->getNumSymbols());
  unsigned nextSym = 0;
  for (unsigned i = 0, e = mapOrSet->getNumSymbols(); i != e; ++i) {
    if (!usedSyms[i])
      continue;
    // Handle constant operands (only needed for symbolic operands since
    // constant operands in dimensional positions would have already been
    // promoted to symbolic positions above).
    IntegerAttr operandCst;
    if (matchPattern((*operands)[i + mapOrSet->getNumDims()],
                     m_Constant(&operandCst))) {
      symRemapping[i] =
          getAffineConstantExpr(operandCst.getValue().getSExtValue(), context);
      continue;
    }
    // Remap symbol positions for duplicate operands.
    auto it = seenSymbols.find((*operands)[i + mapOrSet->getNumDims()]);
    if (it == seenSymbols.end()) {
      symRemapping[i] = getAffineSymbolExpr(nextSym++, context);
      resultOperands.push_back((*operands)[i + mapOrSet->getNumDims()]);
      seenSymbols.insert(std::make_pair((*operands)[i + mapOrSet->getNumDims()],
                                        symRemapping[i]));
    } else {
      symRemapping[i] = it->second;
    }
  }
  *mapOrSet = mapOrSet->replaceDimsAndSymbols(dimRemapping, symRemapping,
                                              nextDim, nextSym);
  *operands = resultOperands;
}

void mlir::canonicalizeMapAndOperands(AffineMap *map,
                                      SmallVectorImpl<Value> *operands) {
  canonicalizeMapOrSetAndOperands<AffineMap>(map, operands);
}

void mlir::canonicalizeSetAndOperands(IntegerSet *set,
                                      SmallVectorImpl<Value> *operands) {
  canonicalizeMapOrSetAndOperands<IntegerSet>(set, operands);
}

namespace {
/// Simplify AffineApply, AffineLoad, and AffineStore operations by composing
/// maps that supply results into them.
///
template <typename AffineOpTy>
struct SimplifyAffineOp : public OpRewritePattern<AffineOpTy> {
  using OpRewritePattern<AffineOpTy>::OpRewritePattern;

  /// Replace the affine op with another instance of it with the supplied
  /// map and mapOperands.
  void replaceAffineOp(PatternRewriter &rewriter, AffineOpTy affineOp,
                       AffineMap map, ArrayRef<Value> mapOperands) const;

  LogicalResult matchAndRewrite(AffineOpTy affineOp,
                                PatternRewriter &rewriter) const override {
    static_assert(llvm::is_one_of<AffineOpTy, AffineLoadOp, AffinePrefetchOp,
                                  AffineStoreOp, AffineApplyOp, AffineMinOp,
                                  AffineMaxOp>::value,
                  "affine load/store/apply/prefetch/min/max op expected");
    auto map = affineOp.getAffineMap();
    AffineMap oldMap = map;
    auto oldOperands = affineOp.getMapOperands();
    SmallVector<Value, 8> resultOperands(oldOperands);
    composeAffineMapAndOperands(&map, &resultOperands);
    if (map == oldMap && std::equal(oldOperands.begin(), oldOperands.end(),
                                    resultOperands.begin()))
      return failure();

    replaceAffineOp(rewriter, affineOp, map, resultOperands);
    return success();
  }
};

// Specialize the template to account for the different build signatures for
// affine load, store, and apply ops.
template <>
void SimplifyAffineOp<AffineLoadOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineLoadOp load, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineLoadOp>(load, load.getMemRef(), map,
                                            mapOperands);
}
template <>
void SimplifyAffineOp<AffinePrefetchOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffinePrefetchOp prefetch, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffinePrefetchOp>(
      prefetch, prefetch.memref(), map, mapOperands, prefetch.localityHint(),
      prefetch.isWrite(), prefetch.isDataCache());
}
template <>
void SimplifyAffineOp<AffineStoreOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineStoreOp store, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineStoreOp>(
      store, store.getValueToStore(), store.getMemRef(), map, mapOperands);
}

// Generic version for ops that don't have extra operands.
template <typename AffineOpTy>
void SimplifyAffineOp<AffineOpTy>::replaceAffineOp(
    PatternRewriter &rewriter, AffineOpTy op, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineOpTy>(op, map, mapOperands);
}
} // end anonymous namespace.

void AffineApplyOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<SimplifyAffineOp<AffineApplyOp>>(context);
}

//===----------------------------------------------------------------------===//
// Common canonicalization pattern support logic
//===----------------------------------------------------------------------===//

/// This is a common class used for patterns of the form
/// "someop(memrefcast) -> someop".  It folds the source of any memref_cast
/// into the root operation directly.
static LogicalResult foldMemRefCast(Operation *op) {
  bool folded = false;
  for (OpOperand &operand : op->getOpOperands()) {
    auto cast = operand.get().getDefiningOp<MemRefCastOp>();
    if (cast && !cast.getOperand().getType().isa<UnrankedMemRefType>()) {
      operand.set(cast.getOperand());
      folded = true;
    }
  }
  return success(folded);
}

//===----------------------------------------------------------------------===//
// AffineDmaStartOp
//===----------------------------------------------------------------------===//

// TODO: Check that map operands are loop IVs or symbols.
void AffineDmaStartOp::build(OpBuilder &builder, OperationState &result,
                             Value srcMemRef, AffineMap srcMap,
                             ValueRange srcIndices, Value destMemRef,
                             AffineMap dstMap, ValueRange destIndices,
                             Value tagMemRef, AffineMap tagMap,
                             ValueRange tagIndices, Value numElements,
                             Value stride, Value elementsPerStride) {
  result.addOperands(srcMemRef);
  result.addAttribute(getSrcMapAttrName(), AffineMapAttr::get(srcMap));
  result.addOperands(srcIndices);
  result.addOperands(destMemRef);
  result.addAttribute(getDstMapAttrName(), AffineMapAttr::get(dstMap));
  result.addOperands(destIndices);
  result.addOperands(tagMemRef);
  result.addAttribute(getTagMapAttrName(), AffineMapAttr::get(tagMap));
  result.addOperands(tagIndices);
  result.addOperands(numElements);
  if (stride) {
    result.addOperands({stride, elementsPerStride});
  }
}

void AffineDmaStartOp::print(OpAsmPrinter &p) {
  p << "affine.dma_start " << getSrcMemRef() << '[';
  p.printAffineMapOfSSAIds(getSrcMapAttr(), getSrcIndices());
  p << "], " << getDstMemRef() << '[';
  p.printAffineMapOfSSAIds(getDstMapAttr(), getDstIndices());
  p << "], " << getTagMemRef() << '[';
  p.printAffineMapOfSSAIds(getTagMapAttr(), getTagIndices());
  p << "], " << getNumElements();
  if (isStrided()) {
    p << ", " << getStride();
    p << ", " << getNumElementsPerStride();
  }
  p << " : " << getSrcMemRefType() << ", " << getDstMemRefType() << ", "
    << getTagMemRefType();
}

// Parse AffineDmaStartOp.
// Ex:
//   affine.dma_start %src[%i, %j], %dst[%k, %l], %tag[%index], %size,
//     %stride, %num_elt_per_stride
//       : memref<3076 x f32, 0>, memref<1024 x f32, 2>, memref<1 x i32>
//
ParseResult AffineDmaStartOp::parse(OpAsmParser &parser,
                                    OperationState &result) {
  OpAsmParser::OperandType srcMemRefInfo;
  AffineMapAttr srcMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> srcMapOperands;
  OpAsmParser::OperandType dstMemRefInfo;
  AffineMapAttr dstMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> dstMapOperands;
  OpAsmParser::OperandType tagMemRefInfo;
  AffineMapAttr tagMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> tagMapOperands;
  OpAsmParser::OperandType numElementsInfo;
  SmallVector<OpAsmParser::OperandType, 2> strideInfo;

  SmallVector<Type, 3> types;
  auto indexType = parser.getBuilder().getIndexType();

  // Parse and resolve the following list of operands:
  // *) dst memref followed by its affine maps operands (in square brackets).
  // *) src memref followed by its affine map operands (in square brackets).
  // *) tag memref followed by its affine map operands (in square brackets).
  // *) number of elements transferred by DMA operation.
  if (parser.parseOperand(srcMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(srcMapOperands, srcMapAttr,
                                    getSrcMapAttrName(), result.attributes) ||
      parser.parseComma() || parser.parseOperand(dstMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(dstMapOperands, dstMapAttr,
                                    getDstMapAttrName(), result.attributes) ||
      parser.parseComma() || parser.parseOperand(tagMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(tagMapOperands, tagMapAttr,
                                    getTagMapAttrName(), result.attributes) ||
      parser.parseComma() || parser.parseOperand(numElementsInfo))
    return failure();

  // Parse optional stride and elements per stride.
  if (parser.parseTrailingOperandList(strideInfo)) {
    return failure();
  }
  if (!strideInfo.empty() && strideInfo.size() != 2) {
    return parser.emitError(parser.getNameLoc(),
                            "expected two stride related operands");
  }
  bool isStrided = strideInfo.size() == 2;

  if (parser.parseColonTypeList(types))
    return failure();

  if (types.size() != 3)
    return parser.emitError(parser.getNameLoc(), "expected three types");

  if (parser.resolveOperand(srcMemRefInfo, types[0], result.operands) ||
      parser.resolveOperands(srcMapOperands, indexType, result.operands) ||
      parser.resolveOperand(dstMemRefInfo, types[1], result.operands) ||
      parser.resolveOperands(dstMapOperands, indexType, result.operands) ||
      parser.resolveOperand(tagMemRefInfo, types[2], result.operands) ||
      parser.resolveOperands(tagMapOperands, indexType, result.operands) ||
      parser.resolveOperand(numElementsInfo, indexType, result.operands))
    return failure();

  if (isStrided) {
    if (parser.resolveOperands(strideInfo, indexType, result.operands))
      return failure();
  }

  // Check that src/dst/tag operand counts match their map.numInputs.
  if (srcMapOperands.size() != srcMapAttr.getValue().getNumInputs() ||
      dstMapOperands.size() != dstMapAttr.getValue().getNumInputs() ||
      tagMapOperands.size() != tagMapAttr.getValue().getNumInputs())
    return parser.emitError(parser.getNameLoc(),
                            "memref operand count not equal to map.numInputs");
  return success();
}

LogicalResult AffineDmaStartOp::verify() {
  if (!getOperand(getSrcMemRefOperandIndex()).getType().isa<MemRefType>())
    return emitOpError("expected DMA source to be of memref type");
  if (!getOperand(getDstMemRefOperandIndex()).getType().isa<MemRefType>())
    return emitOpError("expected DMA destination to be of memref type");
  if (!getOperand(getTagMemRefOperandIndex()).getType().isa<MemRefType>())
    return emitOpError("expected DMA tag to be of memref type");

  // DMAs from different memory spaces supported.
  if (getSrcMemorySpace() == getDstMemorySpace()) {
    return emitOpError("DMA should be between different memory spaces");
  }
  unsigned numInputsAllMaps = getSrcMap().getNumInputs() +
                              getDstMap().getNumInputs() +
                              getTagMap().getNumInputs();
  if (getNumOperands() != numInputsAllMaps + 3 + 1 &&
      getNumOperands() != numInputsAllMaps + 3 + 1 + 2) {
    return emitOpError("incorrect number of operands");
  }

  Region *scope = getAffineScope(*this);
  for (auto idx : getSrcIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("src index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("src index must be a dimension or symbol identifier");
  }
  for (auto idx : getDstIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("dst index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("dst index must be a dimension or symbol identifier");
  }
  for (auto idx : getTagIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("tag index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("tag index must be a dimension or symbol identifier");
  }
  return success();
}

LogicalResult AffineDmaStartOp::fold(ArrayRef<Attribute> cstOperands,
                                     SmallVectorImpl<OpFoldResult> &results) {
  /// dma_start(memrefcast) -> dma_start
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// AffineDmaWaitOp
//===----------------------------------------------------------------------===//

// TODO: Check that map operands are loop IVs or symbols.
void AffineDmaWaitOp::build(OpBuilder &builder, OperationState &result,
                            Value tagMemRef, AffineMap tagMap,
                            ValueRange tagIndices, Value numElements) {
  result.addOperands(tagMemRef);
  result.addAttribute(getTagMapAttrName(), AffineMapAttr::get(tagMap));
  result.addOperands(tagIndices);
  result.addOperands(numElements);
}

void AffineDmaWaitOp::print(OpAsmPrinter &p) {
  p << "affine.dma_wait " << getTagMemRef() << '[';
  SmallVector<Value, 2> operands(getTagIndices());
  p.printAffineMapOfSSAIds(getTagMapAttr(), operands);
  p << "], ";
  p.printOperand(getNumElements());
  p << " : " << getTagMemRef().getType();
}

// Parse AffineDmaWaitOp.
// Eg:
//   affine.dma_wait %tag[%index], %num_elements
//     : memref<1 x i32, (d0) -> (d0), 4>
//
ParseResult AffineDmaWaitOp::parse(OpAsmParser &parser,
                                   OperationState &result) {
  OpAsmParser::OperandType tagMemRefInfo;
  AffineMapAttr tagMapAttr;
  SmallVector<OpAsmParser::OperandType, 2> tagMapOperands;
  Type type;
  auto indexType = parser.getBuilder().getIndexType();
  OpAsmParser::OperandType numElementsInfo;

  // Parse tag memref, its map operands, and dma size.
  if (parser.parseOperand(tagMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(tagMapOperands, tagMapAttr,
                                    getTagMapAttrName(), result.attributes) ||
      parser.parseComma() || parser.parseOperand(numElementsInfo) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(tagMemRefInfo, type, result.operands) ||
      parser.resolveOperands(tagMapOperands, indexType, result.operands) ||
      parser.resolveOperand(numElementsInfo, indexType, result.operands))
    return failure();

  if (!type.isa<MemRefType>())
    return parser.emitError(parser.getNameLoc(),
                            "expected tag to be of memref type");

  if (tagMapOperands.size() != tagMapAttr.getValue().getNumInputs())
    return parser.emitError(parser.getNameLoc(),
                            "tag memref operand count != to map.numInputs");
  return success();
}

LogicalResult AffineDmaWaitOp::verify() {
  if (!getOperand(0).getType().isa<MemRefType>())
    return emitOpError("expected DMA tag to be of memref type");
  Region *scope = getAffineScope(*this);
  for (auto idx : getTagIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("index to dma_wait must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("index must be a dimension or symbol identifier");
  }
  return success();
}

LogicalResult AffineDmaWaitOp::fold(ArrayRef<Attribute> cstOperands,
                                    SmallVectorImpl<OpFoldResult> &results) {
  /// dma_wait(memrefcast) -> dma_wait
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// AffineForOp
//===----------------------------------------------------------------------===//

/// 'bodyBuilder' is used to build the body of affine.for. If iterArgs and
/// bodyBuilder are empty/null, we include default terminator op.
void AffineForOp::build(OpBuilder &builder, OperationState &result,
                        ValueRange lbOperands, AffineMap lbMap,
                        ValueRange ubOperands, AffineMap ubMap, int64_t step,
                        ValueRange iterArgs, BodyBuilderFn bodyBuilder) {
  assert(((!lbMap && lbOperands.empty()) ||
          lbOperands.size() == lbMap.getNumInputs()) &&
         "lower bound operand count does not match the affine map");
  assert(((!ubMap && ubOperands.empty()) ||
          ubOperands.size() == ubMap.getNumInputs()) &&
         "upper bound operand count does not match the affine map");
  assert(step > 0 && "step has to be a positive integer constant");

  for (Value val : iterArgs)
    result.addTypes(val.getType());

  // Add an attribute for the step.
  result.addAttribute(getStepAttrName(),
                      builder.getIntegerAttr(builder.getIndexType(), step));

  // Add the lower bound.
  result.addAttribute(getLowerBoundAttrName(), AffineMapAttr::get(lbMap));
  result.addOperands(lbOperands);

  // Add the upper bound.
  result.addAttribute(getUpperBoundAttrName(), AffineMapAttr::get(ubMap));
  result.addOperands(ubOperands);

  result.addOperands(iterArgs);
  // Create a region and a block for the body.  The argument of the region is
  // the loop induction variable.
  Region *bodyRegion = result.addRegion();
  bodyRegion->push_back(new Block);
  Block &bodyBlock = bodyRegion->front();
  Value inductionVar = bodyBlock.addArgument(builder.getIndexType());
  for (Value val : iterArgs)
    bodyBlock.addArgument(val.getType());

  // Create the default terminator if the builder is not provided and if the
  // iteration arguments are not provided. Otherwise, leave this to the caller
  // because we don't know which values to return from the loop.
  if (iterArgs.empty() && !bodyBuilder) {
    ensureTerminator(*bodyRegion, builder, result.location);
  } else if (bodyBuilder) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(&bodyBlock);
    bodyBuilder(builder, result.location, inductionVar,
                bodyBlock.getArguments().drop_front());
  }
}

void AffineForOp::build(OpBuilder &builder, OperationState &result, int64_t lb,
                        int64_t ub, int64_t step, ValueRange iterArgs,
                        BodyBuilderFn bodyBuilder) {
  auto lbMap = AffineMap::getConstantMap(lb, builder.getContext());
  auto ubMap = AffineMap::getConstantMap(ub, builder.getContext());
  return build(builder, result, {}, lbMap, {}, ubMap, step, iterArgs,
               bodyBuilder);
}

static LogicalResult verify(AffineForOp op) {
  // Check that the body defines as single block argument for the induction
  // variable.
  auto *body = op.getBody();
  if (body->getNumArguments() == 0 || !body->getArgument(0).getType().isIndex())
    return op.emitOpError(
        "expected body to have a single index argument for the "
        "induction variable");

  // Verify that the bound operands are valid dimension/symbols.
  /// Lower bound.
  if (op.getLowerBoundMap().getNumInputs() > 0)
    if (failed(
            verifyDimAndSymbolIdentifiers(op, op.getLowerBoundOperands(),
                                          op.getLowerBoundMap().getNumDims())))
      return failure();
  /// Upper bound.
  if (op.getUpperBoundMap().getNumInputs() > 0)
    if (failed(
            verifyDimAndSymbolIdentifiers(op, op.getUpperBoundOperands(),
                                          op.getUpperBoundMap().getNumDims())))
      return failure();

  unsigned opNumResults = op.getNumResults();
  if (opNumResults == 0)
    return success();

  // If ForOp defines values, check that the number and types of the defined
  // values match ForOp initial iter operands and backedge basic block
  // arguments.
  if (op.getNumIterOperands() != opNumResults)
    return op.emitOpError(
        "mismatch between the number of loop-carried values and results");
  if (op.getNumRegionIterArgs() != opNumResults)
    return op.emitOpError(
        "mismatch between the number of basic block args and results");

  return success();
}

/// Parse a for operation loop bounds.
static ParseResult parseBound(bool isLower, OperationState &result,
                              OpAsmParser &p) {
  // 'min' / 'max' prefixes are generally syntactic sugar, but are required if
  // the map has multiple results.
  bool failedToParsedMinMax =
      failed(p.parseOptionalKeyword(isLower ? "max" : "min"));

  auto &builder = p.getBuilder();
  auto boundAttrName = isLower ? AffineForOp::getLowerBoundAttrName()
                               : AffineForOp::getUpperBoundAttrName();

  // Parse ssa-id as identity map.
  SmallVector<OpAsmParser::OperandType, 1> boundOpInfos;
  if (p.parseOperandList(boundOpInfos))
    return failure();

  if (!boundOpInfos.empty()) {
    // Check that only one operand was parsed.
    if (boundOpInfos.size() > 1)
      return p.emitError(p.getNameLoc(),
                         "expected only one loop bound operand");

    // TODO: improve error message when SSA value is not of index type.
    // Currently it is 'use of value ... expects different type than prior uses'
    if (p.resolveOperand(boundOpInfos.front(), builder.getIndexType(),
                         result.operands))
      return failure();

    // Create an identity map using symbol id. This representation is optimized
    // for storage. Analysis passes may expand it into a multi-dimensional map
    // if desired.
    AffineMap map = builder.getSymbolIdentityMap();
    result.addAttribute(boundAttrName, AffineMapAttr::get(map));
    return success();
  }

  // Get the attribute location.
  llvm::SMLoc attrLoc = p.getCurrentLocation();

  Attribute boundAttr;
  if (p.parseAttribute(boundAttr, builder.getIndexType(), boundAttrName,
                       result.attributes))
    return failure();

  // Parse full form - affine map followed by dim and symbol list.
  if (auto affineMapAttr = boundAttr.dyn_cast<AffineMapAttr>()) {
    unsigned currentNumOperands = result.operands.size();
    unsigned numDims;
    if (parseDimAndSymbolList(p, result.operands, numDims))
      return failure();

    auto map = affineMapAttr.getValue();
    if (map.getNumDims() != numDims)
      return p.emitError(
          p.getNameLoc(),
          "dim operand count and affine map dim count must match");

    unsigned numDimAndSymbolOperands =
        result.operands.size() - currentNumOperands;
    if (numDims + map.getNumSymbols() != numDimAndSymbolOperands)
      return p.emitError(
          p.getNameLoc(),
          "symbol operand count and affine map symbol count must match");

    // If the map has multiple results, make sure that we parsed the min/max
    // prefix.
    if (map.getNumResults() > 1 && failedToParsedMinMax) {
      if (isLower) {
        return p.emitError(attrLoc, "lower loop bound affine map with "
                                    "multiple results requires 'max' prefix");
      }
      return p.emitError(attrLoc, "upper loop bound affine map with multiple "
                                  "results requires 'min' prefix");
    }
    return success();
  }

  // Parse custom assembly form.
  if (auto integerAttr = boundAttr.dyn_cast<IntegerAttr>()) {
    result.attributes.pop_back();
    result.addAttribute(
        boundAttrName,
        AffineMapAttr::get(builder.getConstantAffineMap(integerAttr.getInt())));
    return success();
  }

  return p.emitError(
      p.getNameLoc(),
      "expected valid affine map representation for loop bounds");
}

static ParseResult parseAffineForOp(OpAsmParser &parser,
                                    OperationState &result) {
  auto &builder = parser.getBuilder();
  OpAsmParser::OperandType inductionVariable;
  // Parse the induction variable followed by '='.
  if (parser.parseRegionArgument(inductionVariable) || parser.parseEqual())
    return failure();

  // Parse loop bounds.
  if (parseBound(/*isLower=*/true, result, parser) ||
      parser.parseKeyword("to", " between bounds") ||
      parseBound(/*isLower=*/false, result, parser))
    return failure();

  // Parse the optional loop step, we default to 1 if one is not present.
  if (parser.parseOptionalKeyword("step")) {
    result.addAttribute(
        AffineForOp::getStepAttrName(),
        builder.getIntegerAttr(builder.getIndexType(), /*value=*/1));
  } else {
    llvm::SMLoc stepLoc = parser.getCurrentLocation();
    IntegerAttr stepAttr;
    if (parser.parseAttribute(stepAttr, builder.getIndexType(),
                              AffineForOp::getStepAttrName().data(),
                              result.attributes))
      return failure();

    if (stepAttr.getValue().getSExtValue() < 0)
      return parser.emitError(
          stepLoc,
          "expected step to be representable as a positive signed integer");
  }

  // Parse the optional initial iteration arguments.
  SmallVector<OpAsmParser::OperandType, 4> regionArgs, operands;
  SmallVector<Type, 4> argTypes;
  regionArgs.push_back(inductionVariable);

  if (succeeded(parser.parseOptionalKeyword("iter_args"))) {
    // Parse assignment list and results type list.
    if (parser.parseAssignmentList(regionArgs, operands) ||
        parser.parseArrowTypeList(result.types))
      return failure();
    // Resolve input operands.
    for (auto operandType : llvm::zip(operands, result.types))
      if (parser.resolveOperand(std::get<0>(operandType),
                                std::get<1>(operandType), result.operands))
        return failure();
  }
  // Induction variable.
  Type indexType = builder.getIndexType();
  argTypes.push_back(indexType);
  // Loop carried variables.
  argTypes.append(result.types.begin(), result.types.end());
  // Parse the body region.
  Region *body = result.addRegion();
  if (regionArgs.size() != argTypes.size())
    return parser.emitError(
        parser.getNameLoc(),
        "mismatch between the number of loop-carried values and results");
  if (parser.parseRegion(*body, regionArgs, argTypes))
    return failure();

  AffineForOp::ensureTerminator(*body, builder, result.location);

  // Parse the optional attribute list.
  return parser.parseOptionalAttrDict(result.attributes);
}

static void printBound(AffineMapAttr boundMap,
                       Operation::operand_range boundOperands,
                       const char *prefix, OpAsmPrinter &p) {
  AffineMap map = boundMap.getValue();

  // Check if this bound should be printed using custom assembly form.
  // The decision to restrict printing custom assembly form to trivial cases
  // comes from the will to roundtrip MLIR binary -> text -> binary in a
  // lossless way.
  // Therefore, custom assembly form parsing and printing is only supported for
  // zero-operand constant maps and single symbol operand identity maps.
  if (map.getNumResults() == 1) {
    AffineExpr expr = map.getResult(0);

    // Print constant bound.
    if (map.getNumDims() == 0 && map.getNumSymbols() == 0) {
      if (auto constExpr = expr.dyn_cast<AffineConstantExpr>()) {
        p << constExpr.getValue();
        return;
      }
    }

    // Print bound that consists of a single SSA symbol if the map is over a
    // single symbol.
    if (map.getNumDims() == 0 && map.getNumSymbols() == 1) {
      if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>()) {
        p.printOperand(*boundOperands.begin());
        return;
      }
    }
  } else {
    // Map has multiple results. Print 'min' or 'max' prefix.
    p << prefix << ' ';
  }

  // Print the map and its operands.
  p << boundMap;
  printDimAndSymbolList(boundOperands.begin(), boundOperands.end(),
                        map.getNumDims(), p);
}

unsigned AffineForOp::getNumIterOperands() {
  AffineMap lbMap = getLowerBoundMapAttr().getValue();
  AffineMap ubMap = getUpperBoundMapAttr().getValue();

  return getNumOperands() - lbMap.getNumInputs() - ubMap.getNumInputs();
}

static void print(OpAsmPrinter &p, AffineForOp op) {
  p << op.getOperationName() << ' ';
  p.printOperand(op.getBody()->getArgument(0));
  p << " = ";
  printBound(op.getLowerBoundMapAttr(), op.getLowerBoundOperands(), "max", p);
  p << " to ";
  printBound(op.getUpperBoundMapAttr(), op.getUpperBoundOperands(), "min", p);

  if (op.getStep() != 1)
    p << " step " << op.getStep();

  bool printBlockTerminators = false;
  if (op.getNumIterOperands() > 0) {
    p << " iter_args(";
    auto regionArgs = op.getRegionIterArgs();
    auto operands = op.getIterOperands();

    llvm::interleaveComma(llvm::zip(regionArgs, operands), p, [&](auto it) {
      p << std::get<0>(it) << " = " << std::get<1>(it);
    });
    p << ") -> (" << op.getResultTypes() << ")";
    printBlockTerminators = true;
  }

  p.printRegion(op.region(),
                /*printEntryBlockArgs=*/false, printBlockTerminators);
  p.printOptionalAttrDict(op.getAttrs(),
                          /*elidedAttrs=*/{op.getLowerBoundAttrName(),
                                           op.getUpperBoundAttrName(),
                                           op.getStepAttrName()});
}

/// Fold the constant bounds of a loop.
static LogicalResult foldLoopBounds(AffineForOp forOp) {
  auto foldLowerOrUpperBound = [&forOp](bool lower) {
    // Check to see if each of the operands is the result of a constant.  If
    // so, get the value.  If not, ignore it.
    SmallVector<Attribute, 8> operandConstants;
    auto boundOperands =
        lower ? forOp.getLowerBoundOperands() : forOp.getUpperBoundOperands();
    for (auto operand : boundOperands) {
      Attribute operandCst;
      matchPattern(operand, m_Constant(&operandCst));
      operandConstants.push_back(operandCst);
    }

    AffineMap boundMap =
        lower ? forOp.getLowerBoundMap() : forOp.getUpperBoundMap();
    assert(boundMap.getNumResults() >= 1 &&
           "bound maps should have at least one result");
    SmallVector<Attribute, 4> foldedResults;
    if (failed(boundMap.constantFold(operandConstants, foldedResults)))
      return failure();

    // Compute the max or min as applicable over the results.
    assert(!foldedResults.empty() && "bounds should have at least one result");
    auto maxOrMin = foldedResults[0].cast<IntegerAttr>().getValue();
    for (unsigned i = 1, e = foldedResults.size(); i < e; i++) {
      auto foldedResult = foldedResults[i].cast<IntegerAttr>().getValue();
      maxOrMin = lower ? llvm::APIntOps::smax(maxOrMin, foldedResult)
                       : llvm::APIntOps::smin(maxOrMin, foldedResult);
    }
    lower ? forOp.setConstantLowerBound(maxOrMin.getSExtValue())
          : forOp.setConstantUpperBound(maxOrMin.getSExtValue());
    return success();
  };

  // Try to fold the lower bound.
  bool folded = false;
  if (!forOp.hasConstantLowerBound())
    folded |= succeeded(foldLowerOrUpperBound(/*lower=*/true));

  // Try to fold the upper bound.
  if (!forOp.hasConstantUpperBound())
    folded |= succeeded(foldLowerOrUpperBound(/*lower=*/false));
  return success(folded);
}

/// Canonicalize the bounds of the given loop.
static LogicalResult canonicalizeLoopBounds(AffineForOp forOp) {
  SmallVector<Value, 4> lbOperands(forOp.getLowerBoundOperands());
  SmallVector<Value, 4> ubOperands(forOp.getUpperBoundOperands());

  auto lbMap = forOp.getLowerBoundMap();
  auto ubMap = forOp.getUpperBoundMap();
  auto prevLbMap = lbMap;
  auto prevUbMap = ubMap;

  canonicalizeMapAndOperands(&lbMap, &lbOperands);
  lbMap = removeDuplicateExprs(lbMap);

  canonicalizeMapAndOperands(&ubMap, &ubOperands);
  ubMap = removeDuplicateExprs(ubMap);

  // Any canonicalization change always leads to updated map(s).
  if (lbMap == prevLbMap && ubMap == prevUbMap)
    return failure();

  if (lbMap != prevLbMap)
    forOp.setLowerBound(lbOperands, lbMap);
  if (ubMap != prevUbMap)
    forOp.setUpperBound(ubOperands, ubMap);
  return success();
}

namespace {
/// This is a pattern to fold trivially empty loops.
struct AffineForEmptyLoopFolder : public OpRewritePattern<AffineForOp> {
  using OpRewritePattern<AffineForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AffineForOp forOp,
                                PatternRewriter &rewriter) const override {
    // Check that the body only contains a yield.
    if (!llvm::hasSingleElement(*forOp.getBody()))
      return failure();
    rewriter.eraseOp(forOp);
    return success();
  }
};

// This is a pattern to simplify to unit stride in simple cases.
struct AffineStrideNormalizer : public OpRewritePattern<AffineForOp> {
  using OpRewritePattern<AffineForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AffineForOp forOp,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value, 4> lbOperands(forOp.getLowerBoundOperands());
    SmallVector<Value, 4> ubOperands(forOp.getUpperBoundOperands());

    int64_t step = forOp.getStep();

    // TODO: keeping it very simple.
    if (step == 1 || !forOp.hasConstantBounds()) {
      return failure();
    }

    rewriter.startRootUpdate(forOp);

    int64_t lb = forOp.getConstantLowerBound();
    int64_t ub = forOp.getConstantUpperBound();

    forOp.setStep(1);
    forOp.setConstantUpperBound(lb + mlir::ceilDiv(ub - lb, step));

    OpBuilder b(&forOp.getBody()->front());
    auto d0 = getAffineDimExpr(0, b.getContext());
    auto ivScaleUp =
        b.create<AffineApplyOp>(forOp.getLoc(), AffineMap::get(1, 0, d0 * step),
                                forOp.getInductionVar());

    SmallPtrSet<Operation *, 1> exceptions = {ivScaleUp};
    replaceAllUsesExcept(forOp.getInductionVar(), ivScaleUp, exceptions);

    rewriter.finalizeRootUpdate(forOp);
    return success();
  }
};

} // end anonymous namespace

void AffineForOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.insert<AffineForEmptyLoopFolder, AffineStrideNormalizer>(context);
}

LogicalResult AffineForOp::fold(ArrayRef<Attribute> operands,
                                SmallVectorImpl<OpFoldResult> &results) {
  bool folded = succeeded(foldLoopBounds(*this));
  folded |= succeeded(canonicalizeLoopBounds(*this));
  return success(folded);
}

AffineBound AffineForOp::getLowerBound() {
  auto lbMap = getLowerBoundMap();
  return AffineBound(AffineForOp(*this), 0, lbMap.getNumInputs(), lbMap);
}

AffineBound AffineForOp::getUpperBound() {
  auto lbMap = getLowerBoundMap();
  auto ubMap = getUpperBoundMap();
  return AffineBound(AffineForOp(*this), lbMap.getNumInputs(),
                     lbMap.getNumInputs() + ubMap.getNumInputs(), ubMap);
}

void AffineForOp::setLowerBound(ValueRange lbOperands, AffineMap map) {
  assert(lbOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value, 4> newOperands(lbOperands.begin(), lbOperands.end());

  auto ubOperands = getUpperBoundOperands();
  newOperands.append(ubOperands.begin(), ubOperands.end());
  auto iterOperands = getIterOperands();
  newOperands.append(iterOperands.begin(), iterOperands.end());
  (*this)->setOperands(newOperands);

  (*this)->setAttr(getLowerBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setUpperBound(ValueRange ubOperands, AffineMap map) {
  assert(ubOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value, 4> newOperands(getLowerBoundOperands());
  newOperands.append(ubOperands.begin(), ubOperands.end());
  auto iterOperands = getIterOperands();
  newOperands.append(iterOperands.begin(), iterOperands.end());
  (*this)->setOperands(newOperands);

  (*this)->setAttr(getUpperBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setLowerBoundMap(AffineMap map) {
  auto lbMap = getLowerBoundMap();
  assert(lbMap.getNumDims() == map.getNumDims() &&
         lbMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  (void)lbMap;
  (*this)->setAttr(getLowerBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setUpperBoundMap(AffineMap map) {
  auto ubMap = getUpperBoundMap();
  assert(ubMap.getNumDims() == map.getNumDims() &&
         ubMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  (void)ubMap;
  (*this)->setAttr(getUpperBoundAttrName(), AffineMapAttr::get(map));
}

bool AffineForOp::hasConstantLowerBound() {
  return getLowerBoundMap().isSingleConstant();
}

bool AffineForOp::hasConstantUpperBound() {
  return getUpperBoundMap().isSingleConstant();
}

int64_t AffineForOp::getConstantLowerBound() {
  return getLowerBoundMap().getSingleConstantResult();
}

int64_t AffineForOp::getConstantUpperBound() {
  return getUpperBoundMap().getSingleConstantResult();
}

void AffineForOp::setConstantLowerBound(int64_t value) {
  setLowerBound({}, AffineMap::getConstantMap(value, getContext()));
}

void AffineForOp::setConstantUpperBound(int64_t value) {
  setUpperBound({}, AffineMap::getConstantMap(value, getContext()));
}

AffineForOp::operand_range AffineForOp::getLowerBoundOperands() {
  return {operand_begin(), operand_begin() + getLowerBoundMap().getNumInputs()};
}

AffineForOp::operand_range AffineForOp::getUpperBoundOperands() {
  return {operand_begin() + getLowerBoundMap().getNumInputs(),
          operand_begin() + getLowerBoundMap().getNumInputs() +
              getUpperBoundMap().getNumInputs()};
}

bool AffineForOp::matchingBoundOperandList() {
  auto lbMap = getLowerBoundMap();
  auto ubMap = getUpperBoundMap();
  if (lbMap.getNumDims() != ubMap.getNumDims() ||
      lbMap.getNumSymbols() != ubMap.getNumSymbols())
    return false;

  unsigned numOperands = lbMap.getNumInputs();
  for (unsigned i = 0, e = lbMap.getNumInputs(); i < e; i++) {
    // Compare Value 's.
    if (getOperand(i) != getOperand(numOperands + i))
      return false;
  }
  return true;
}

Region &AffineForOp::getLoopBody() { return region(); }

bool AffineForOp::isDefinedOutsideOfLoop(Value value) {
  return !region().isAncestor(value.getParentRegion());
}

LogicalResult AffineForOp::moveOutOfLoop(ArrayRef<Operation *> ops) {
  for (auto *op : ops)
    op->moveBefore(*this);
  return success();
}

/// Returns true if the provided value is the induction variable of a
/// AffineForOp.
bool mlir::isForInductionVar(Value val) {
  return getForInductionVarOwner(val) != AffineForOp();
}

/// Returns the loop parent of an induction variable. If the provided value is
/// not an induction variable, then return nullptr.
AffineForOp mlir::getForInductionVarOwner(Value val) {
  auto ivArg = val.dyn_cast<BlockArgument>();
  if (!ivArg || !ivArg.getOwner())
    return AffineForOp();
  auto *containingInst = ivArg.getOwner()->getParent()->getParentOp();
  return dyn_cast<AffineForOp>(containingInst);
}

/// Extracts the induction variables from a list of AffineForOps and returns
/// them.
void mlir::extractForInductionVars(ArrayRef<AffineForOp> forInsts,
                                   SmallVectorImpl<Value> *ivs) {
  ivs->reserve(forInsts.size());
  for (auto forInst : forInsts)
    ivs->push_back(forInst.getInductionVar());
}

/// Builds an affine loop nest, using "loopCreatorFn" to create individual loop
/// operations.
template <typename BoundListTy, typename LoopCreatorTy>
static void buildAffineLoopNestImpl(
    OpBuilder &builder, Location loc, BoundListTy lbs, BoundListTy ubs,
    ArrayRef<int64_t> steps,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn,
    LoopCreatorTy &&loopCreatorFn) {
  assert(lbs.size() == ubs.size() && "Mismatch in number of arguments");
  assert(lbs.size() == steps.size() && "Mismatch in number of arguments");

  // If there are no loops to be constructed, construct the body anyway.
  OpBuilder::InsertionGuard guard(builder);
  if (lbs.empty()) {
    if (bodyBuilderFn)
      bodyBuilderFn(builder, loc, ValueRange());
    return;
  }

  // Create the loops iteratively and store the induction variables.
  SmallVector<Value, 4> ivs;
  ivs.reserve(lbs.size());
  for (unsigned i = 0, e = lbs.size(); i < e; ++i) {
    // Callback for creating the loop body, always creates the terminator.
    auto loopBody = [&](OpBuilder &nestedBuilder, Location nestedLoc, Value iv,
                        ValueRange iterArgs) {
      ivs.push_back(iv);
      // In the innermost loop, call the body builder.
      if (i == e - 1 && bodyBuilderFn) {
        OpBuilder::InsertionGuard nestedGuard(nestedBuilder);
        bodyBuilderFn(nestedBuilder, nestedLoc, ivs);
      }
      nestedBuilder.create<AffineYieldOp>(nestedLoc);
    };

    // Delegate actual loop creation to the callback in order to dispatch
    // between constant- and variable-bound loops.
    auto loop = loopCreatorFn(builder, loc, lbs[i], ubs[i], steps[i], loopBody);
    builder.setInsertionPointToStart(loop.getBody());
  }
}

/// Creates an affine loop from the bounds known to be constants.
static AffineForOp
buildAffineLoopFromConstants(OpBuilder &builder, Location loc, int64_t lb,
                             int64_t ub, int64_t step,
                             AffineForOp::BodyBuilderFn bodyBuilderFn) {
  return builder.create<AffineForOp>(loc, lb, ub, step, /*iterArgs=*/llvm::None,
                                     bodyBuilderFn);
}

/// Creates an affine loop from the bounds that may or may not be constants.
static AffineForOp
buildAffineLoopFromValues(OpBuilder &builder, Location loc, Value lb, Value ub,
                          int64_t step,
                          AffineForOp::BodyBuilderFn bodyBuilderFn) {
  auto lbConst = lb.getDefiningOp<ConstantIndexOp>();
  auto ubConst = ub.getDefiningOp<ConstantIndexOp>();
  if (lbConst && ubConst)
    return buildAffineLoopFromConstants(builder, loc, lbConst.getValue(),
                                        ubConst.getValue(), step,
                                        bodyBuilderFn);
  return builder.create<AffineForOp>(loc, lb, builder.getDimIdentityMap(), ub,
                                     builder.getDimIdentityMap(), step,
                                     /*iterArgs=*/llvm::None, bodyBuilderFn);
}

void mlir::buildAffineLoopNest(
    OpBuilder &builder, Location loc, ArrayRef<int64_t> lbs,
    ArrayRef<int64_t> ubs, ArrayRef<int64_t> steps,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn) {
  buildAffineLoopNestImpl(builder, loc, lbs, ubs, steps, bodyBuilderFn,
                          buildAffineLoopFromConstants);
}

void mlir::buildAffineLoopNest(
    OpBuilder &builder, Location loc, ValueRange lbs, ValueRange ubs,
    ArrayRef<int64_t> steps,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn) {
  buildAffineLoopNestImpl(builder, loc, lbs, ubs, steps, bodyBuilderFn,
                          buildAffineLoopFromValues);
}

//===----------------------------------------------------------------------===//
// AffineIfOp
//===----------------------------------------------------------------------===//

namespace {
/// Remove else blocks that have nothing other than a zero value yield.
struct SimplifyDeadElse : public OpRewritePattern<AffineIfOp> {
  using OpRewritePattern<AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AffineIfOp ifOp,
                                PatternRewriter &rewriter) const override {
    if (ifOp.elseRegion().empty() ||
        !llvm::hasSingleElement(*ifOp.getElseBlock()) || ifOp.getNumResults())
      return failure();

    rewriter.startRootUpdate(ifOp);
    rewriter.eraseBlock(ifOp.getElseBlock());
    rewriter.finalizeRootUpdate(ifOp);
    return success();
  }
};
} // end anonymous namespace.

static LogicalResult verify(AffineIfOp op) {
  // Verify that we have a condition attribute.
  auto conditionAttr =
      op->getAttrOfType<IntegerSetAttr>(op.getConditionAttrName());
  if (!conditionAttr)
    return op.emitOpError(
        "requires an integer set attribute named 'condition'");

  // Verify that there are enough operands for the condition.
  IntegerSet condition = conditionAttr.getValue();
  if (op.getNumOperands() != condition.getNumInputs())
    return op.emitOpError(
        "operand count and condition integer set dimension and "
        "symbol count must match");

  // Verify that the operands are valid dimension/symbols.
  if (failed(verifyDimAndSymbolIdentifiers(op, op.getOperands(),
                                           condition.getNumDims())))
    return failure();

  return success();
}

static ParseResult parseAffineIfOp(OpAsmParser &parser,
                                   OperationState &result) {
  // Parse the condition attribute set.
  IntegerSetAttr conditionAttr;
  unsigned numDims;
  if (parser.parseAttribute(conditionAttr, AffineIfOp::getConditionAttrName(),
                            result.attributes) ||
      parseDimAndSymbolList(parser, result.operands, numDims))
    return failure();

  // Verify the condition operands.
  auto set = conditionAttr.getValue();
  if (set.getNumDims() != numDims)
    return parser.emitError(
        parser.getNameLoc(),
        "dim operand count and integer set dim count must match");
  if (numDims + set.getNumSymbols() != result.operands.size())
    return parser.emitError(
        parser.getNameLoc(),
        "symbol operand count and integer set symbol count must match");

  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Create the regions for 'then' and 'else'.  The latter must be created even
  // if it remains empty for the validity of the operation.
  result.regions.reserve(2);
  Region *thenRegion = result.addRegion();
  Region *elseRegion = result.addRegion();

  // Parse the 'then' region.
  if (parser.parseRegion(*thenRegion, {}, {}))
    return failure();
  AffineIfOp::ensureTerminator(*thenRegion, parser.getBuilder(),
                               result.location);

  // If we find an 'else' keyword then parse the 'else' region.
  if (!parser.parseOptionalKeyword("else")) {
    if (parser.parseRegion(*elseRegion, {}, {}))
      return failure();
    AffineIfOp::ensureTerminator(*elseRegion, parser.getBuilder(),
                                 result.location);
  }

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

static void print(OpAsmPrinter &p, AffineIfOp op) {
  auto conditionAttr =
      op->getAttrOfType<IntegerSetAttr>(op.getConditionAttrName());
  p << "affine.if " << conditionAttr;
  printDimAndSymbolList(op.operand_begin(), op.operand_end(),
                        conditionAttr.getValue().getNumDims(), p);
  p.printOptionalArrowTypeList(op.getResultTypes());
  p.printRegion(op.thenRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/op.getNumResults());

  // Print the 'else' regions if it has any blocks.
  auto &elseRegion = op.elseRegion();
  if (!elseRegion.empty()) {
    p << " else";
    p.printRegion(elseRegion,
                  /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/op.getNumResults());
  }

  // Print the attribute list.
  p.printOptionalAttrDict(op.getAttrs(),
                          /*elidedAttrs=*/op.getConditionAttrName());
}

IntegerSet AffineIfOp::getIntegerSet() {
  return (*this)
      ->getAttrOfType<IntegerSetAttr>(getConditionAttrName())
      .getValue();
}
void AffineIfOp::setIntegerSet(IntegerSet newSet) {
  (*this)->setAttr(getConditionAttrName(), IntegerSetAttr::get(newSet));
}

void AffineIfOp::setConditional(IntegerSet set, ValueRange operands) {
  setIntegerSet(set);
  (*this)->setOperands(operands);
}

void AffineIfOp::build(OpBuilder &builder, OperationState &result,
                       TypeRange resultTypes, IntegerSet set, ValueRange args,
                       bool withElseRegion) {
  assert(resultTypes.empty() || withElseRegion);
  result.addTypes(resultTypes);
  result.addOperands(args);
  result.addAttribute(getConditionAttrName(), IntegerSetAttr::get(set));

  Region *thenRegion = result.addRegion();
  thenRegion->push_back(new Block());
  if (resultTypes.empty())
    AffineIfOp::ensureTerminator(*thenRegion, builder, result.location);

  Region *elseRegion = result.addRegion();
  if (withElseRegion) {
    elseRegion->push_back(new Block());
    if (resultTypes.empty())
      AffineIfOp::ensureTerminator(*elseRegion, builder, result.location);
  }
}

void AffineIfOp::build(OpBuilder &builder, OperationState &result,
                       IntegerSet set, ValueRange args, bool withElseRegion) {
  AffineIfOp::build(builder, result, /*resultTypes=*/{}, set, args,
                    withElseRegion);
}

/// Canonicalize an affine if op's conditional (integer set + operands).
LogicalResult AffineIfOp::fold(ArrayRef<Attribute>,
                               SmallVectorImpl<OpFoldResult> &) {
  auto set = getIntegerSet();
  SmallVector<Value, 4> operands(getOperands());
  canonicalizeSetAndOperands(&set, &operands);

  // Any canonicalization change always leads to either a reduction in the
  // number of operands or a change in the number of symbolic operands
  // (promotion of dims to symbols).
  if (operands.size() < getIntegerSet().getNumInputs() ||
      set.getNumSymbols() > getIntegerSet().getNumSymbols()) {
    setConditional(set, operands);
    return success();
  }

  return failure();
}

void AffineIfOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                             MLIRContext *context) {
  results.insert<SimplifyDeadElse>(context);
}

//===----------------------------------------------------------------------===//
// AffineLoadOp
//===----------------------------------------------------------------------===//

void AffineLoadOp::build(OpBuilder &builder, OperationState &result,
                         AffineMap map, ValueRange operands) {
  assert(operands.size() == 1 + map.getNumInputs() && "inconsistent operands");
  result.addOperands(operands);
  if (map)
    result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
  auto memrefType = operands[0].getType().cast<MemRefType>();
  result.types.push_back(memrefType.getElementType());
}

void AffineLoadOp::build(OpBuilder &builder, OperationState &result,
                         Value memref, AffineMap map, ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(memref);
  result.addOperands(mapOperands);
  auto memrefType = memref.getType().cast<MemRefType>();
  result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
  result.types.push_back(memrefType.getElementType());
}

void AffineLoadOp::build(OpBuilder &builder, OperationState &result,
                         Value memref, ValueRange indices) {
  auto memrefType = memref.getType().cast<MemRefType>();
  int64_t rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map =
      rank ? builder.getMultiDimIdentityMap(rank) : builder.getEmptyAffineMap();
  build(builder, result, memref, map, indices);
}

static ParseResult parseAffineLoadOp(OpAsmParser &parser,
                                     OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  MemRefType type;
  OpAsmParser::OperandType memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  return failure(
      parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffineLoadOp::getMapAttrName(),
                                    result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(memrefInfo, type, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands) ||
      parser.addTypeToList(type.getElementType(), result.types));
}

static void print(OpAsmPrinter &p, AffineLoadOp op) {
  p << "affine.load " << op.getMemRef() << '[';
  if (AffineMapAttr mapAttr =
          op->getAttrOfType<AffineMapAttr>(op.getMapAttrName()))
    p.printAffineMapOfSSAIds(mapAttr, op.getMapOperands());
  p << ']';
  p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/{op.getMapAttrName()});
  p << " : " << op.getMemRefType();
}

/// Verify common indexing invariants of affine.load, affine.store,
/// affine.vector_load and affine.vector_store.
template <typename AffineMemOpTy>
static LogicalResult
verifyMemoryOpIndexing(AffineMemOpTy op, AffineMapAttr mapAttr,
                       Operation::operand_range mapOperands,
                       MemRefType memrefType, unsigned numIndexOperands) {
  unsigned numDims;
  if (mapAttr) {
    AffineMap map = mapAttr.getValue();
    if (map.getNumResults() != memrefType.getRank())
      return op.emitOpError("affine map num results must equal memref rank");
    if (map.getNumInputs() != numIndexOperands)
      return op.emitOpError("expects as many subscripts as affine map inputs");
    numDims = map.getNumDims();
  } else {
    if (memrefType.getRank() != numIndexOperands)
      return op.emitOpError(
          "expects the number of subscripts to be equal to memref rank");
    numDims = op.getNumOperands() - 1;
  }

  for (auto idx : mapOperands) {
    if (!idx.getType().isIndex())
      return op.emitOpError("index to load must have 'index' type");
  }
  verifyDimAndSymbolIdentifiers(op, op.getMapOperands(), numDims);

  return success();
}

LogicalResult verify(AffineLoadOp op) {
  auto memrefType = op.getMemRefType();
  if (op.getType() != memrefType.getElementType())
    return op.emitOpError("result type must match element type of memref");

  if (failed(verifyMemoryOpIndexing(
          op,//op.getOperation(),
          op->getAttrOfType<AffineMapAttr>(op.getMapAttrName()),
          op.getMapOperands(), memrefType,
          /*numIndexOperands=*/op.getNumOperands() - 1)))
    return failure();

  return success();
}

void AffineLoadOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<SimplifyAffineOp<AffineLoadOp>>(context);
}

OpFoldResult AffineLoadOp::fold(ArrayRef<Attribute> cstOperands) {
  /// load(memrefcast) -> load
  if (succeeded(foldMemRefCast(*this)))
    return getResult();
  return OpFoldResult();
}

//===----------------------------------------------------------------------===//
// AffineStoreOp
//===----------------------------------------------------------------------===//

void AffineStoreOp::build(OpBuilder &builder, OperationState &result,
                          Value valueToStore, Value memref, AffineMap map,
                          ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(valueToStore);
  result.addOperands(memref);
  result.addOperands(mapOperands);
  result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
}

// Use identity map.
void AffineStoreOp::build(OpBuilder &builder, OperationState &result,
                          Value valueToStore, Value memref,
                          ValueRange indices) {
  auto memrefType = memref.getType().cast<MemRefType>();
  int64_t rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map =
      rank ? builder.getMultiDimIdentityMap(rank) : builder.getEmptyAffineMap();
  build(builder, result, valueToStore, memref, map, indices);
}

static ParseResult parseAffineStoreOp(OpAsmParser &parser,
                                      OperationState &result) {
  auto indexTy = parser.getBuilder().getIndexType();

  MemRefType type;
  OpAsmParser::OperandType storeValueInfo;
  OpAsmParser::OperandType memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  return failure(parser.parseOperand(storeValueInfo) || parser.parseComma() ||
                 parser.parseOperand(memrefInfo) ||
                 parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                               AffineStoreOp::getMapAttrName(),
                                               result.attributes) ||
                 parser.parseOptionalAttrDict(result.attributes) ||
                 parser.parseColonType(type) ||
                 parser.resolveOperand(storeValueInfo, type.getElementType(),
                                       result.operands) ||
                 parser.resolveOperand(memrefInfo, type, result.operands) ||
                 parser.resolveOperands(mapOperands, indexTy, result.operands));
}

static void print(OpAsmPrinter &p, AffineStoreOp op) {
  p << "affine.store " << op.getValueToStore();
  p << ", " << op.getMemRef() << '[';
  if (AffineMapAttr mapAttr =
          op->getAttrOfType<AffineMapAttr>(op.getMapAttrName()))
    p.printAffineMapOfSSAIds(mapAttr, op.getMapOperands());
  p << ']';
  p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/{op.getMapAttrName()});
  p << " : " << op.getMemRefType();
}

LogicalResult verify(AffineStoreOp op) {
  // First operand must have same type as memref element type.
  auto memrefType = op.getMemRefType();
  if (op.getValueToStore().getType() != memrefType.getElementType())
    return op.emitOpError(
        "first operand must have same type memref element type");

  if (failed(verifyMemoryOpIndexing(
          op,//op.getOperation(),
          op->getAttrOfType<AffineMapAttr>(op.getMapAttrName()),
          op.getMapOperands(), memrefType,
          /*numIndexOperands=*/op.getNumOperands() - 2)))
    return failure();

  return success();
}

void AffineStoreOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<SimplifyAffineOp<AffineStoreOp>>(context);
}

LogicalResult AffineStoreOp::fold(ArrayRef<Attribute> cstOperands,
                                  SmallVectorImpl<OpFoldResult> &results) {
  /// store(memrefcast) -> store
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// AffineMinMaxOpBase
//===----------------------------------------------------------------------===//

template <typename T> static LogicalResult verifyAffineMinMaxOp(T op) {
  // Verify that operand count matches affine map dimension and symbol count.
  if (op.getNumOperands() != op.map().getNumDims() + op.map().getNumSymbols())
    return op.emitOpError(
        "operand count and affine map dimension and symbol count must match");
  return success();
}

template <typename T> static void printAffineMinMaxOp(OpAsmPrinter &p, T op) {
  p << op.getOperationName() << ' ' << op->getAttr(T::getMapAttrName());
  auto operands = op.getOperands();
  unsigned numDims = op.map().getNumDims();
  p << '(' << operands.take_front(numDims) << ')';

  if (operands.size() != numDims)
    p << '[' << operands.drop_front(numDims) << ']';
  p.printOptionalAttrDict(op.getAttrs(),
                          /*elidedAttrs=*/{T::getMapAttrName()});
}

template <typename T>
static ParseResult parseAffineMinMaxOp(OpAsmParser &parser,
                                       OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexType = builder.getIndexType();
  SmallVector<OpAsmParser::OperandType, 8> dim_infos;
  SmallVector<OpAsmParser::OperandType, 8> sym_infos;
  AffineMapAttr mapAttr;
  return failure(
      parser.parseAttribute(mapAttr, T::getMapAttrName(), result.attributes) ||
      parser.parseOperandList(dim_infos, OpAsmParser::Delimiter::Paren) ||
      parser.parseOperandList(sym_infos,
                              OpAsmParser::Delimiter::OptionalSquare) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.resolveOperands(dim_infos, indexType, result.operands) ||
      parser.resolveOperands(sym_infos, indexType, result.operands) ||
      parser.addTypeToList(indexType, result.types));
}

/// Fold an affine min or max operation with the given operands. The operand
/// list may contain nulls, which are interpreted as the operand not being a
/// constant.
template <typename T>
static OpFoldResult foldMinMaxOp(T op, ArrayRef<Attribute> operands) {
  static_assert(llvm::is_one_of<T, AffineMinOp, AffineMaxOp>::value,
                "expected affine min or max op");

  // Fold the affine map.
  // TODO: Fold more cases:
  // min(some_affine, some_affine + constant, ...), etc.
  SmallVector<int64_t, 2> results;
  auto foldedMap = op.map().partialConstantFold(operands, &results);

  // If some of the map results are not constant, try changing the map in-place.
  if (results.empty()) {
    // If the map is the same, report that folding did not happen.
    if (foldedMap == op.map())
      return {};
    op->setAttr("map", AffineMapAttr::get(foldedMap));
    return op.getResult();
  }

  // Otherwise, completely fold the op into a constant.
  auto resultIt = std::is_same<T, AffineMinOp>::value
                      ? std::min_element(results.begin(), results.end())
                      : std::max_element(results.begin(), results.end());
  if (resultIt == results.end())
    return {};
  return IntegerAttr::get(IndexType::get(op.getContext()), *resultIt);
}

//===----------------------------------------------------------------------===//
// AffineMinOp
//===----------------------------------------------------------------------===//
//
//   %0 = affine.min (d0) -> (1000, d0 + 512) (%i0)
//

OpFoldResult AffineMinOp::fold(ArrayRef<Attribute> operands) {
  return foldMinMaxOp(*this, operands);
}

void AffineMinOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  patterns.insert<SimplifyAffineOp<AffineMinOp>>(context);
}

//===----------------------------------------------------------------------===//
// AffineMaxOp
//===----------------------------------------------------------------------===//
//
//   %0 = affine.max (d0) -> (1000, d0 + 512) (%i0)
//

OpFoldResult AffineMaxOp::fold(ArrayRef<Attribute> operands) {
  return foldMinMaxOp(*this, operands);
}

void AffineMaxOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  patterns.insert<SimplifyAffineOp<AffineMaxOp>>(context);
}

//===----------------------------------------------------------------------===//
// AffinePrefetchOp
//===----------------------------------------------------------------------===//

//
// affine.prefetch %0[%i, %j + 5], read, locality<3>, data : memref<400x400xi32>
//
static ParseResult parseAffinePrefetchOp(OpAsmParser &parser,
                                         OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  MemRefType type;
  OpAsmParser::OperandType memrefInfo;
  IntegerAttr hintInfo;
  auto i32Type = parser.getBuilder().getIntegerType(32);
  StringRef readOrWrite, cacheType;

  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  if (parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffinePrefetchOp::getMapAttrName(),
                                    result.attributes) ||
      parser.parseComma() || parser.parseKeyword(&readOrWrite) ||
      parser.parseComma() || parser.parseKeyword("locality") ||
      parser.parseLess() ||
      parser.parseAttribute(hintInfo, i32Type,
                            AffinePrefetchOp::getLocalityHintAttrName(),
                            result.attributes) ||
      parser.parseGreater() || parser.parseComma() ||
      parser.parseKeyword(&cacheType) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(memrefInfo, type, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands))
    return failure();

  if (!readOrWrite.equals("read") && !readOrWrite.equals("write"))
    return parser.emitError(parser.getNameLoc(),
                            "rw specifier has to be 'read' or 'write'");
  result.addAttribute(
      AffinePrefetchOp::getIsWriteAttrName(),
      parser.getBuilder().getBoolAttr(readOrWrite.equals("write")));

  if (!cacheType.equals("data") && !cacheType.equals("instr"))
    return parser.emitError(parser.getNameLoc(),
                            "cache type has to be 'data' or 'instr'");

  result.addAttribute(
      AffinePrefetchOp::getIsDataCacheAttrName(),
      parser.getBuilder().getBoolAttr(cacheType.equals("data")));

  return success();
}

static void print(OpAsmPrinter &p, AffinePrefetchOp op) {
  p << AffinePrefetchOp::getOperationName() << " " << op.memref() << '[';
  AffineMapAttr mapAttr = op->getAttrOfType<AffineMapAttr>(op.getMapAttrName());
  if (mapAttr) {
    SmallVector<Value, 2> operands(op.getMapOperands());
    p.printAffineMapOfSSAIds(mapAttr, operands);
  }
  p << ']' << ", " << (op.isWrite() ? "write" : "read") << ", "
    << "locality<" << op.localityHint() << ">, "
    << (op.isDataCache() ? "data" : "instr");
  p.printOptionalAttrDict(
      op.getAttrs(),
      /*elidedAttrs=*/{op.getMapAttrName(), op.getLocalityHintAttrName(),
                       op.getIsDataCacheAttrName(), op.getIsWriteAttrName()});
  p << " : " << op.getMemRefType();
}

static LogicalResult verify(AffinePrefetchOp op) {
  auto mapAttr = op->getAttrOfType<AffineMapAttr>(op.getMapAttrName());
  if (mapAttr) {
    AffineMap map = mapAttr.getValue();
    if (map.getNumResults() != op.getMemRefType().getRank())
      return op.emitOpError("affine.prefetch affine map num results must equal"
                            " memref rank");
    if (map.getNumInputs() + 1 != op.getNumOperands())
      return op.emitOpError("too few operands");
  } else {
    if (op.getNumOperands() != 1)
      return op.emitOpError("too few operands");
  }

  Region *scope = getAffineScope(op);
  for (auto idx : op.getMapOperands()) {
    if (!isValidAffineIndexOperand(idx, scope))
      return op.emitOpError("index must be a dimension or symbol identifier");
  }
  return success();
}

void AffinePrefetchOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // prefetch(memrefcast) -> prefetch
  results.insert<SimplifyAffineOp<AffinePrefetchOp>>(context);
}

LogicalResult AffinePrefetchOp::fold(ArrayRef<Attribute> cstOperands,
                                     SmallVectorImpl<OpFoldResult> &results) {
  /// prefetch(memrefcast) -> prefetch
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// AffineExecuteRegionOp
//===----------------------------------------------------------------------===//
//

// TODO: missing region body.
void AffineExecuteRegionOp::build(OpBuilder &builder, OperationState &result,
                                  ValueRange memrefs) {
  // Create a region and an empty entry block. The arguments of the region are
  // the supplied memrefs.
  Region *region = result.addRegion();
  Block *body = new Block();
  region->push_back(body);
  body->addArguments(memrefs.getTypes());
}

static LogicalResult verify(AffineExecuteRegionOp op) {
  // All memref uses in the execute_region region should be explicitly captured.
  // FIXME: change this walk to an affine walk that doesn't walk inner
  // execute_regions.
  DenseSet<Value> memrefsUsed;
  op.region().walk([&](Operation *innerOp) {
    for (auto v : innerOp->getOperands())
      if (v.getType().isa<MemRefType>())
        memrefsUsed.insert(v);
  });

  // For each memref use, ensure either an execute_region argument or a local
  // def.
  for (auto memref : memrefsUsed) {
    if (auto arg = memref.dyn_cast<BlockArgument>())
      if (arg.getOwner()->getParent()->getParentOp() == op)
        continue;
    if (auto *defOp = memref.getDefiningOp())
      // FIXME: this will only work if the memrefs collected above didn't
      // include any from inner execute_regions.
      if (defOp->getParentOfType<AffineExecuteRegionOp>() == op)
        continue;
    return op.emitOpError("incoming memref not explicitly captured");
  }

  // Verify that the region arguments match operands.
  auto &entryBlock = op.region().front();
  if (entryBlock.getNumArguments() != op.getNumOperands())
    return op.emitOpError("region argument count does not match operand count");

  for (auto argEn : llvm::enumerate(entryBlock.getArguments())) {
    if (op.getOperand(argEn.index()).getType() != argEn.value().getType())
      return op.emitOpError("region argument ")
             << argEn.index() << " does not match corresponding operand";
  }

  return success();
}

// Custom form syntax.
//
// (ssa-id `=`)? `affine.execute_region` `[` memref-region-arg-list `]`
//                                   `=` `(` memref-use-list `)`
//                  `:` memref-type-list-parens `->` function-result-type `{`
//    block+
// `}`
//
// Ex:
//
//  affine.execute_region [%rI, %rM] = (%I, %M)
//        : (memref<128xi32>, memref<1024xf32>) -> () {
//      %idx = affine.load %rI[%i] : memref<128xi32>
//      %index = index_cast %idx : i32 to index
//      affine.load %rM[%index]: memref<1024xf32>
//      return
//    }
//
static ParseResult parseAffineExecuteRegionOp(OpAsmParser &parser,
                                              OperationState &result) {
  // Memref operands.
  SmallVector<OpAsmParser::OperandType, 4> memrefs;

  // Region arguments to be created.
  SmallVector<OpAsmParser::OperandType, 4> regionMemRefs;

  // The execute_region op has the same type signature as a function.
  FunctionType opType;

  // Parse the memref assignments.
  auto argLoc = parser.getCurrentLocation();
  if (parser.parseRegionArgumentList(regionMemRefs,
                                     OpAsmParser::Delimiter::Square) ||
      parser.parseEqual() ||
      parser.parseOperandList(memrefs, OpAsmParser::Delimiter::Paren))
    return failure();

  if (memrefs.size() != regionMemRefs.size())
    return parser.emitError(parser.getNameLoc(),
                            "incorrect number of memref captures");

  if (parser.parseColonType(opType) ||
      parser.addTypesToList(opType.getResults(), result.types))
    return failure();

  auto memrefTypes = opType.getInputs();
  if (parser.resolveOperands(memrefs, memrefTypes, argLoc, result.operands))
    return failure();

  // Introduce the body region and parse it.
  Region *body = result.addRegion();
  if (parser.parseRegion(*body, regionMemRefs, memrefTypes) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

static void print(OpAsmPrinter &p, AffineExecuteRegionOp op) {
  p << AffineExecuteRegionOp::getOperationName() << " [";
  // TODO: consider shadowing region arguments.
  p.printOperands(op.region().front().getArguments());
  p << "] = (";
  auto operands = op.getOperands();
  p.printOperands(operands);
  p << ") ";

  SmallVector<Type, 4> argTypes(op.getOperandTypes());
  p << " : "
    << FunctionType::get(op.getOperation()->getContext(),argTypes, op.getResultTypes());

  p.printRegion(op.region(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/true);

  p.printOptionalAttrDict(op.getAttrs());
}

//===----------------------------------------------------------------------===//
// AffineParallelOp
//===----------------------------------------------------------------------===//

void AffineParallelOp::build(OpBuilder &builder, OperationState &result,
                             TypeRange resultTypes,
                             ArrayRef<AtomicRMWKind> reductions,
                             ArrayRef<int64_t> ranges) {
  SmallVector<AffineExpr, 8> lbExprs(ranges.size(),
                                     builder.getAffineConstantExpr(0));
  auto lbMap = AffineMap::get(0, 0, lbExprs, builder.getContext());
  SmallVector<AffineExpr, 8> ubExprs;
  for (int64_t range : ranges)
    ubExprs.push_back(builder.getAffineConstantExpr(range));
  auto ubMap = AffineMap::get(0, 0, ubExprs, builder.getContext());
  build(builder, result, resultTypes, reductions, lbMap, /*lbArgs=*/{}, ubMap,
        /*ubArgs=*/{});
}

void AffineParallelOp::build(OpBuilder &builder, OperationState &result,
                             TypeRange resultTypes,
                             ArrayRef<AtomicRMWKind> reductions,
                             AffineMap lbMap, ValueRange lbArgs,
                             AffineMap ubMap, ValueRange ubArgs) {
  auto numDims = lbMap.getNumResults();
  // Verify that the dimensionality of both maps are the same.
  assert(numDims == ubMap.getNumResults() &&
         "num dims and num results mismatch");
  // Make default step sizes of 1.
  SmallVector<int64_t, 8> steps(numDims, 1);
  build(builder, result, resultTypes, reductions, lbMap, lbArgs, ubMap, ubArgs,
        steps);
}

void AffineParallelOp::build(OpBuilder &builder, OperationState &result,
                             TypeRange resultTypes,
                             ArrayRef<AtomicRMWKind> reductions,
                             AffineMap lbMap, ValueRange lbArgs,
                             AffineMap ubMap, ValueRange ubArgs,
                             ArrayRef<int64_t> steps) {
  auto numDims = lbMap.getNumResults();
  // Verify that the dimensionality of the maps matches the number of steps.
  assert(numDims == ubMap.getNumResults() &&
         "num dims and num results mismatch");
  assert(numDims == steps.size() && "num dims and num steps mismatch");

  result.addTypes(resultTypes);
  // Convert the reductions to integer attributes.
  SmallVector<Attribute, 4> reductionAttrs;
  for (AtomicRMWKind reduction : reductions)
    reductionAttrs.push_back(
        builder.getI64IntegerAttr(static_cast<int64_t>(reduction)));
  result.addAttribute(getReductionsAttrName(),
                      builder.getArrayAttr(reductionAttrs));
  result.addAttribute(getLowerBoundsMapAttrName(), AffineMapAttr::get(lbMap));
  result.addAttribute(getUpperBoundsMapAttrName(), AffineMapAttr::get(ubMap));
  result.addAttribute(getStepsAttrName(), builder.getI64ArrayAttr(steps));
  result.addOperands(lbArgs);
  result.addOperands(ubArgs);
  // Create a region and a block for the body.
  auto *bodyRegion = result.addRegion();
  auto *body = new Block();
  // Add all the block arguments.
  for (unsigned i = 0; i < numDims; ++i)
    body->addArgument(IndexType::get(builder.getContext()));
  bodyRegion->push_back(body);
  if (resultTypes.empty())
    ensureTerminator(*bodyRegion, builder, result.location);
}

Region &AffineParallelOp::getLoopBody() { return region(); }

bool AffineParallelOp::isDefinedOutsideOfLoop(Value value) {
  return !region().isAncestor(value.getParentRegion());
}

LogicalResult AffineParallelOp::moveOutOfLoop(ArrayRef<Operation *> ops) {
  for (Operation *op : ops)
    op->moveBefore(*this);
  return success();
}

unsigned AffineParallelOp::getNumDims() { return steps().size(); }

AffineParallelOp::operand_range AffineParallelOp::getLowerBoundsOperands() {
  return getOperands().take_front(lowerBoundsMap().getNumInputs());
}

AffineParallelOp::operand_range AffineParallelOp::getUpperBoundsOperands() {
  return getOperands().drop_front(lowerBoundsMap().getNumInputs());
}

AffineValueMap AffineParallelOp::getLowerBoundsValueMap() {
  return AffineValueMap(lowerBoundsMap(), getLowerBoundsOperands());
}

AffineValueMap AffineParallelOp::getUpperBoundsValueMap() {
  return AffineValueMap(upperBoundsMap(), getUpperBoundsOperands());
}

AffineValueMap AffineParallelOp::getRangesValueMap() {
  AffineValueMap out;
  AffineValueMap::difference(getUpperBoundsValueMap(), getLowerBoundsValueMap(),
                             &out);
  return out;
}

Optional<SmallVector<int64_t, 8>> AffineParallelOp::getConstantRanges() {
  // Try to convert all the ranges to constant expressions.
  SmallVector<int64_t, 8> out;
  AffineValueMap rangesValueMap = getRangesValueMap();
  out.reserve(rangesValueMap.getNumResults());
  for (unsigned i = 0, e = rangesValueMap.getNumResults(); i < e; ++i) {
    auto expr = rangesValueMap.getResult(i);
    auto cst = expr.dyn_cast<AffineConstantExpr>();
    if (!cst)
      return llvm::None;
    out.push_back(cst.getValue());
  }
  return out;
}

Block *AffineParallelOp::getBody() { return &region().front(); }

OpBuilder AffineParallelOp::getBodyBuilder() {
  return OpBuilder(getBody(), std::prev(getBody()->end()));
}

void AffineParallelOp::setLowerBounds(ValueRange lbOperands, AffineMap map) {
  assert(lbOperands.size() == map.getNumInputs() &&
         "operands to map must match number of inputs");
  assert(map.getNumResults() >= 1 && "bounds map has at least one result");

  auto ubOperands = getUpperBoundsOperands();

  SmallVector<Value, 4> newOperands(lbOperands);
  newOperands.append(ubOperands.begin(), ubOperands.end());
  (*this)->setOperands(newOperands);

  lowerBoundsMapAttr(AffineMapAttr::get(map));
}

void AffineParallelOp::setUpperBounds(ValueRange ubOperands, AffineMap map) {
  assert(ubOperands.size() == map.getNumInputs() &&
         "operands to map must match number of inputs");
  assert(map.getNumResults() >= 1 && "bounds map has at least one result");

  SmallVector<Value, 4> newOperands(getLowerBoundsOperands());
  newOperands.append(ubOperands.begin(), ubOperands.end());
  (*this)->setOperands(newOperands);

  upperBoundsMapAttr(AffineMapAttr::get(map));
}

void AffineParallelOp::setLowerBoundsMap(AffineMap map) {
  AffineMap lbMap = lowerBoundsMap();
  assert(lbMap.getNumDims() == map.getNumDims() &&
         lbMap.getNumSymbols() == map.getNumSymbols());
  (void)lbMap;
  lowerBoundsMapAttr(AffineMapAttr::get(map));
}

void AffineParallelOp::setUpperBoundsMap(AffineMap map) {
  AffineMap ubMap = upperBoundsMap();
  assert(ubMap.getNumDims() == map.getNumDims() &&
         ubMap.getNumSymbols() == map.getNumSymbols());
  (void)ubMap;
  upperBoundsMapAttr(AffineMapAttr::get(map));
}

SmallVector<int64_t, 8> AffineParallelOp::getSteps() {
  SmallVector<int64_t, 8> result;
  for (Attribute attr : steps()) {
    result.push_back(attr.cast<IntegerAttr>().getInt());
  }
  return result;
}

void AffineParallelOp::setSteps(ArrayRef<int64_t> newSteps) {
  stepsAttr(getBodyBuilder().getI64ArrayAttr(newSteps));
}

static LogicalResult verify(AffineParallelOp op) {
  auto numDims = op.getNumDims();
  if (op.lowerBoundsMap().getNumResults() != numDims ||
      op.upperBoundsMap().getNumResults() != numDims ||
      op.steps().size() != numDims ||
      op.getBody()->getNumArguments() != numDims)
    return op.emitOpError("region argument count and num results of upper "
                          "bounds, lower bounds, and steps must all match");

  if (op.reductions().size() != op.getNumResults())
    return op.emitOpError("a reduction must be specified for each output");

  // Verify reduction  ops are all valid
  for (Attribute attr : op.reductions()) {
    auto intAttr = attr.dyn_cast<IntegerAttr>();
    if (!intAttr || !symbolizeAtomicRMWKind(intAttr.getInt()))
      return op.emitOpError("invalid reduction attribute");
  }

  // Verify that the bound operands are valid dimension/symbols.
  /// Lower bounds.
  if (failed(verifyDimAndSymbolIdentifiers(op, op.getLowerBoundsOperands(),
                                           op.lowerBoundsMap().getNumDims())))
    return failure();
  /// Upper bounds.
  if (failed(verifyDimAndSymbolIdentifiers(op, op.getUpperBoundsOperands(),
                                           op.upperBoundsMap().getNumDims())))
    return failure();
  return success();
}

LogicalResult AffineValueMap::canonicalize() {
  SmallVector<Value, 4> newOperands{operands};
  auto newMap = getAffineMap();
  composeAffineMapAndOperands(&newMap, &newOperands);
  if (newMap == getAffineMap() && newOperands == operands)
    return failure();
  reset(newMap, newOperands);
  return success();
}

/// Canonicalize the bounds of the given loop.
static LogicalResult canonicalizeLoopBounds(AffineParallelOp op) {
  AffineValueMap lb = op.getLowerBoundsValueMap();
  bool lbCanonicalized = succeeded(lb.canonicalize());

  AffineValueMap ub = op.getUpperBoundsValueMap();
  bool ubCanonicalized = succeeded(ub.canonicalize());

  // Any canonicalization change always leads to updated map(s).
  if (!lbCanonicalized && !ubCanonicalized)
    return failure();

  if (lbCanonicalized)
    op.setLowerBounds(lb.getOperands(), lb.getAffineMap());
  if (ubCanonicalized)
    op.setUpperBounds(ub.getOperands(), ub.getAffineMap());

  return success();
}

LogicalResult AffineParallelOp::fold(ArrayRef<Attribute> operands,
                                     SmallVectorImpl<OpFoldResult> &results) {
  return canonicalizeLoopBounds(*this);
}

static void print(OpAsmPrinter &p, AffineParallelOp op) {
  p << op.getOperationName() << " (" << op.getBody()->getArguments() << ") = (";
  p.printAffineMapOfSSAIds(op.lowerBoundsMapAttr(),
                           op.getLowerBoundsOperands());
  p << ") to (";
  p.printAffineMapOfSSAIds(op.upperBoundsMapAttr(),
                           op.getUpperBoundsOperands());
  p << ')';
  SmallVector<int64_t, 8> steps = op.getSteps();
  bool elideSteps = llvm::all_of(steps, [](int64_t step) { return step == 1; });
  if (!elideSteps) {
    p << " step (";
    llvm::interleaveComma(steps, p);
    p << ')';
  }
  if (op.getNumResults()) {
    p << " reduce (";
    llvm::interleaveComma(op.reductions(), p, [&](auto &attr) {
      AtomicRMWKind sym =
          *symbolizeAtomicRMWKind(attr.template cast<IntegerAttr>().getInt());
      p << "\"" << stringifyAtomicRMWKind(sym) << "\"";
    });
    p << ") -> (" << op.getResultTypes() << ")";
  }

  p.printRegion(op.region(), /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/op.getNumResults());
  p.printOptionalAttrDict(
      op.getAttrs(),
      /*elidedAttrs=*/{AffineParallelOp::getReductionsAttrName(),
                       AffineParallelOp::getLowerBoundsMapAttrName(),
                       AffineParallelOp::getUpperBoundsMapAttrName(),
                       AffineParallelOp::getStepsAttrName()});
}

//
// operation ::= `affine.parallel` `(` ssa-ids `)` `=` `(` map-of-ssa-ids `)`
//               `to` `(` map-of-ssa-ids `)` steps? region attr-dict?
// steps     ::= `steps` `(` integer-literals `)`
//
static ParseResult parseAffineParallelOp(OpAsmParser &parser,
                                         OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexType = builder.getIndexType();
  AffineMapAttr lowerBoundsAttr, upperBoundsAttr;
  SmallVector<OpAsmParser::OperandType, 4> ivs;
  SmallVector<OpAsmParser::OperandType, 4> lowerBoundsMapOperands;
  SmallVector<OpAsmParser::OperandType, 4> upperBoundsMapOperands;
  if (parser.parseRegionArgumentList(ivs, /*requiredOperandCount=*/-1,
                                     OpAsmParser::Delimiter::Paren) ||
      parser.parseEqual() ||
      parser.parseAffineMapOfSSAIds(
          lowerBoundsMapOperands, lowerBoundsAttr,
          AffineParallelOp::getLowerBoundsMapAttrName(), result.attributes,
          OpAsmParser::Delimiter::Paren) ||
      parser.resolveOperands(lowerBoundsMapOperands, indexType,
                             result.operands) ||
      parser.parseKeyword("to") ||
      parser.parseAffineMapOfSSAIds(
          upperBoundsMapOperands, upperBoundsAttr,
          AffineParallelOp::getUpperBoundsMapAttrName(), result.attributes,
          OpAsmParser::Delimiter::Paren) ||
      parser.resolveOperands(upperBoundsMapOperands, indexType,
                             result.operands))
    return failure();

  AffineMapAttr stepsMapAttr;
  NamedAttrList stepsAttrs;
  SmallVector<OpAsmParser::OperandType, 4> stepsMapOperands;
  if (failed(parser.parseOptionalKeyword("step"))) {
    SmallVector<int64_t, 4> steps(ivs.size(), 1);
    result.addAttribute(AffineParallelOp::getStepsAttrName(),
                        builder.getI64ArrayAttr(steps));
  } else {
    if (parser.parseAffineMapOfSSAIds(stepsMapOperands, stepsMapAttr,
                                      AffineParallelOp::getStepsAttrName(),
                                      stepsAttrs,
                                      OpAsmParser::Delimiter::Paren))
      return failure();

    // Convert steps from an AffineMap into an I64ArrayAttr.
    SmallVector<int64_t, 4> steps;
    auto stepsMap = stepsMapAttr.getValue();
    for (const auto &result : stepsMap.getResults()) {
      auto constExpr = result.dyn_cast<AffineConstantExpr>();
      if (!constExpr)
        return parser.emitError(parser.getNameLoc(),
                                "steps must be constant integers");
      steps.push_back(constExpr.getValue());
    }
    result.addAttribute(AffineParallelOp::getStepsAttrName(),
                        builder.getI64ArrayAttr(steps));
  }

  // Parse optional clause of the form: `reduce ("addf", "maxf")`, where the
  // quoted strings are a member of the enum AtomicRMWKind.
  SmallVector<Attribute, 4> reductions;
  if (succeeded(parser.parseOptionalKeyword("reduce"))) {
    if (parser.parseLParen())
      return failure();
    do {
      // Parse a single quoted string via the attribute parsing, and then
      // verify it is a member of the enum and convert to it's integer
      // representation.
      StringAttr attrVal;
      NamedAttrList attrStorage;
      auto loc = parser.getCurrentLocation();
      if (parser.parseAttribute(attrVal, builder.getNoneType(), "reduce",
                                attrStorage))
        return failure();
      llvm::Optional<AtomicRMWKind> reduction =
          symbolizeAtomicRMWKind(attrVal.getValue());
      if (!reduction)
        return parser.emitError(loc, "invalid reduction value: ") << attrVal;
      reductions.push_back(builder.getI64IntegerAttr(
          static_cast<int64_t>(reduction.getValue())));
      // While we keep getting commas, keep parsing.
    } while (succeeded(parser.parseOptionalComma()));
    if (parser.parseRParen())
      return failure();
  }
  result.addAttribute(AffineParallelOp::getReductionsAttrName(),
                      builder.getArrayAttr(reductions));

  // Parse return types of reductions (if any)
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Now parse the body.
  Region *body = result.addRegion();
  SmallVector<Type, 4> types(ivs.size(), indexType);
  if (parser.parseRegion(*body, ivs, types) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();

  // Add a terminator if none was parsed.
  AffineParallelOp::ensureTerminator(*body, builder, result.location);
  return success();
}

//===----------------------------------------------------------------------===//
// AffineYieldOp
//===----------------------------------------------------------------------===//

static LogicalResult verify(AffineYieldOp op) {
  auto *parentOp = op->getParentOp();
  auto results = parentOp->getResults();
  auto operands = op.getOperands();

  if (!isa<AffineParallelOp, AffineIfOp, AffineForOp>(parentOp))
    return op.emitOpError() << "only terminates affine.if/for/parallel regions";
  if (parentOp->getNumResults() != op.getNumOperands())
    return op.emitOpError() << "parent of yield must have same number of "
                               "results as the yield operands";
  for (auto it : llvm::zip(results, operands)) {
    if (std::get<0>(it).getType() != std::get<1>(it).getType())
      return op.emitOpError()
             << "types mismatch between yield op and its parent";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// AffineVectorLoadOp
//===----------------------------------------------------------------------===//

void AffineVectorLoadOp::build(OpBuilder &builder, OperationState &result,
                               VectorType resultType, AffineMap map,
                               ValueRange operands) {
  assert(operands.size() == 1 + map.getNumInputs() && "inconsistent operands");
  result.addOperands(operands);
  if (map)
    result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
  result.types.push_back(resultType);
}

void AffineVectorLoadOp::build(OpBuilder &builder, OperationState &result,
                               VectorType resultType, Value memref,
                               AffineMap map, ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(memref);
  result.addOperands(mapOperands);
  result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
  result.types.push_back(resultType);
}

void AffineVectorLoadOp::build(OpBuilder &builder, OperationState &result,
                               VectorType resultType, Value memref,
                               ValueRange indices) {
  auto memrefType = memref.getType().cast<MemRefType>();
  int64_t rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map =
      rank ? builder.getMultiDimIdentityMap(rank) : builder.getEmptyAffineMap();
  build(builder, result, resultType, memref, map, indices);
}

static ParseResult parseAffineVectorLoadOp(OpAsmParser &parser,
                                           OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  MemRefType memrefType;
  VectorType resultType;
  OpAsmParser::OperandType memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  return failure(
      parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffineVectorLoadOp::getMapAttrName(),
                                    result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(memrefType) || parser.parseComma() ||
      parser.parseType(resultType) ||
      parser.resolveOperand(memrefInfo, memrefType, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands) ||
      parser.addTypeToList(resultType, result.types));
}

static void print(OpAsmPrinter &p, AffineVectorLoadOp op) {
  p << "affine.vector_load " << op.getMemRef() << '[';
  if (AffineMapAttr mapAttr =
          op->getAttrOfType<AffineMapAttr>(op.getMapAttrName()))
    p.printAffineMapOfSSAIds(mapAttr, op.getMapOperands());
  p << ']';
  p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/{op.getMapAttrName()});
  p << " : " << op.getMemRefType() << ", " << op.getType();
}

/// Verify common invariants of affine.vector_load and affine.vector_store.
static LogicalResult verifyVectorMemoryOp(Operation *op, MemRefType memrefType,
                                          VectorType vectorType) {
  // Check that memref and vector element types match.
  if (memrefType.getElementType() != vectorType.getElementType())
    return op->emitOpError(
        "requires memref and vector types of the same elemental type");
  return success();
}

static LogicalResult verify(AffineVectorLoadOp op) {
  MemRefType memrefType = op.getMemRefType();
  if (failed(verifyMemoryOpIndexing(
          op,//op.getOperation(),
          op->getAttrOfType<AffineMapAttr>(op.getMapAttrName()),
          op.getMapOperands(), memrefType,
          /*numIndexOperands=*/op.getNumOperands() - 1)))
    return failure();

  if (failed(verifyVectorMemoryOp(op.getOperation(), memrefType,
                                  op.getVectorType())))
    return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// AffineVectorStoreOp
//===----------------------------------------------------------------------===//

void AffineVectorStoreOp::build(OpBuilder &builder, OperationState &result,
                                Value valueToStore, Value memref, AffineMap map,
                                ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(valueToStore);
  result.addOperands(memref);
  result.addOperands(mapOperands);
  result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
}

// Use identity map.
void AffineVectorStoreOp::build(OpBuilder &builder, OperationState &result,
                                Value valueToStore, Value memref,
                                ValueRange indices) {
  auto memrefType = memref.getType().cast<MemRefType>();
  int64_t rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map =
      rank ? builder.getMultiDimIdentityMap(rank) : builder.getEmptyAffineMap();
  build(builder, result, valueToStore, memref, map, indices);
}

static ParseResult parseAffineVectorStoreOp(OpAsmParser &parser,
                                            OperationState &result) {
  auto indexTy = parser.getBuilder().getIndexType();

  MemRefType memrefType;
  VectorType resultType;
  OpAsmParser::OperandType storeValueInfo;
  OpAsmParser::OperandType memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  return failure(
      parser.parseOperand(storeValueInfo) || parser.parseComma() ||
      parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffineVectorStoreOp::getMapAttrName(),
                                    result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(memrefType) || parser.parseComma() ||
      parser.parseType(resultType) ||
      parser.resolveOperand(storeValueInfo, resultType, result.operands) ||
      parser.resolveOperand(memrefInfo, memrefType, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands));
}

static void print(OpAsmPrinter &p, AffineVectorStoreOp op) {
  p << "affine.vector_store " << op.getValueToStore();
  p << ", " << op.getMemRef() << '[';
  if (AffineMapAttr mapAttr =
          op->getAttrOfType<AffineMapAttr>(op.getMapAttrName()))
    p.printAffineMapOfSSAIds(mapAttr, op.getMapOperands());
  p << ']';
  p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/{op.getMapAttrName()});
  p << " : " << op.getMemRefType() << ", " << op.getValueToStore().getType();
}

static LogicalResult verify(AffineVectorStoreOp op) {
  MemRefType memrefType = op.getMemRefType();
  if (failed(verifyMemoryOpIndexing(
          op,//op.getOperation(),
          op->getAttrOfType<AffineMapAttr>(op.getMapAttrName()),
          op.getMapOperands(), memrefType,
          /*numIndexOperands=*/op.getNumOperands() - 2)))
    return failure();

  if (failed(verifyVectorMemoryOp(op.getOperation(), memrefType,
                                  op.getVectorType())))
    return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/Dialect/Affine/IR/AffineOps.cpp.inc"
