//===- LoopAnalysis.cpp - Misc loop analysis routines //-------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements miscellaneous loop analysis routines.
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/LoopAnalysis.h"

#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Analysis/NestedMatcher.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "mlir/Support/MathExtras.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include <type_traits>

using namespace mlir;

/// Returns the trip count of the loop as an affine expression if the latter is
/// expressible as an affine expression, and nullptr otherwise. The trip count
/// expression is simplified before returning. This method only utilizes map
/// composition to construct lower and upper bounds before computing the trip
/// count expressions.
void mlir::buildTripCountMapAndOperands(
    AffineForOp forOp, AffineMap *tripCountMap,
    SmallVectorImpl<Value> *tripCountOperands) {
  int64_t loopSpan;

  int64_t step = forOp.getStep();
  OpBuilder b(forOp.getOperation());

  if (forOp.hasConstantBounds()) {
    int64_t lb = forOp.getConstantLowerBound();
    int64_t ub = forOp.getConstantUpperBound();
    loopSpan = ub - lb;
    if (loopSpan < 0)
      loopSpan = 0;
    *tripCountMap = b.getConstantAffineMap(ceilDiv(loopSpan, step));
    tripCountOperands->clear();
    return;
  }
  auto lbMap = forOp.getLowerBoundMap();
  auto ubMap = forOp.getUpperBoundMap();
  if (lbMap.getNumResults() != 1) {
    *tripCountMap = AffineMap();
    return;
  }

  // Difference of each upper bound expression from the single lower bound
  // expression (divided by the step) provides the expressions for the trip
  // count map.
  AffineValueMap ubValueMap(ubMap, forOp.getUpperBoundOperands());

  SmallVector<AffineExpr, 4> lbSplatExpr(ubValueMap.getNumResults(),
                                         lbMap.getResult(0));
  auto lbMapSplat = AffineMap::get(lbMap.getNumDims(), lbMap.getNumSymbols(),
                                   lbSplatExpr, b.getContext());
  AffineValueMap lbSplatValueMap(lbMapSplat, forOp.getLowerBoundOperands());

  AffineValueMap tripCountValueMap;
  AffineValueMap::difference(ubValueMap, lbSplatValueMap, &tripCountValueMap);
  for (unsigned i = 0, e = tripCountValueMap.getNumResults(); i < e; ++i)
    tripCountValueMap.setResult(i,
                                tripCountValueMap.getResult(i).ceilDiv(step));

  *tripCountMap = tripCountValueMap.getAffineMap();
  tripCountOperands->assign(tripCountValueMap.getOperands().begin(),
                            tripCountValueMap.getOperands().end());
}

/// Returns the trip count of the loop if it's a constant, None otherwise. This
/// method uses affine expression analysis (in turn using getTripCount) and is
/// able to determine constant trip count in non-trivial cases.
// FIXME(mlir-team): this is really relying on buildTripCountMapAndOperands;
// being an analysis utility, it shouldn't. Replace with a version that just
// works with analysis structures (FlatAffineConstraints) and thus doesn't
// update the IR.
Optional<uint64_t> mlir::getConstantTripCount(AffineForOp forOp) {
  SmallVector<Value, 4> operands;
  AffineMap map;
  buildTripCountMapAndOperands(forOp, &map, &operands);

  if (!map)
    return None;

  // Take the min if all trip counts are constant.
  Optional<uint64_t> tripCount;
  for (auto resultExpr : map.getResults()) {
    if (auto constExpr = resultExpr.dyn_cast<AffineConstantExpr>()) {
      if (tripCount.hasValue())
        tripCount = std::min(tripCount.getValue(),
                             static_cast<uint64_t>(constExpr.getValue()));
      else
        tripCount = constExpr.getValue();
    } else
      return None;
  }
  return tripCount;
}

/// Returns the greatest known integral divisor of the trip count. Affine
/// expression analysis is used (indirectly through getTripCount), and
/// this method is thus able to determine non-trivial divisors.
uint64_t mlir::getLargestDivisorOfTripCount(AffineForOp forOp) {
  SmallVector<Value, 4> operands;
  AffineMap map;
  buildTripCountMapAndOperands(forOp, &map, &operands);

  if (!map)
    return 1;

  // The largest divisor of the trip count is the GCD of the individual largest
  // divisors.
  assert(map.getNumResults() >= 1 && "expected one or more results");
  Optional<uint64_t> gcd;
  for (auto resultExpr : map.getResults()) {
    uint64_t thisGcd;
    if (auto constExpr = resultExpr.dyn_cast<AffineConstantExpr>()) {
      uint64_t tripCount = constExpr.getValue();
      // 0 iteration loops (greatest divisor is 2^64 - 1).
      if (tripCount == 0)
        thisGcd = std::numeric_limits<uint64_t>::max();
      else
        // The greatest divisor is the trip count.
        thisGcd = tripCount;
    } else {
      // Trip count is not a known constant; return its largest known divisor.
      thisGcd = resultExpr.getLargestKnownDivisor();
    }
    if (gcd.hasValue())
      gcd = llvm::GreatestCommonDivisor64(gcd.getValue(), thisGcd);
    else
      gcd = thisGcd;
  }
  assert(gcd.hasValue() && "value expected per above logic");
  return gcd.getValue();
}

/// Given an induction variable `iv` of type AffineForOp and an access `index`
/// of type index, returns `true` if `index` is independent of `iv` and
/// false otherwise. The determination supports composition with at most one
/// AffineApplyOp. The 'at most one AffineApplyOp' comes from the fact that
/// the composition of AffineApplyOp needs to be canonicalized by construction
/// to avoid writing code that composes arbitrary numbers of AffineApplyOps
/// everywhere. To achieve this, at the very least, the compose-affine-apply
/// pass must have been run.
///
/// Prerequisites:
///   1. `iv` and `index` of the proper type;
///   2. at most one reachable AffineApplyOp from index;
///
/// Returns false in cases with more than one AffineApplyOp, this is
/// conservative.
static bool isAccessIndexInvariant(Value iv, Value index) {
  assert(isForInductionVar(iv) && "iv must be a AffineForOp");
  assert(index.getType().isa<IndexType>() && "index must be of IndexType");
  SmallVector<Operation *, 4> affineApplyOps;
  getReachableAffineApplyOps({index}, affineApplyOps);

  if (affineApplyOps.empty()) {
    // Pointer equality test because of Value pointer semantics.
    return index != iv;
  }

  if (affineApplyOps.size() > 1) {
    affineApplyOps[0]->emitRemark(
        "CompositionAffineMapsPass must have been run: there should be at most "
        "one AffineApplyOp, returning false conservatively.");
    return false;
  }

  auto composeOp = cast<AffineApplyOp>(affineApplyOps[0]);
  // We need yet another level of indirection because the `dim` index of the
  // access may not correspond to the `dim` index of composeOp.
  return !composeOp.getAffineValueMap().isFunctionOf(0, iv);
}

/// Checks if an affine load or store access depends on `forOp'. This also
/// transitively checks if the load/store is dependent on another loop IV which
/// in turn uses `forOp` is its loop bounds.
//
/// Pre-requisite: Loop bounds should be in canonical form.
template <typename LoadOrStoreOp>
bool mlir::isInvariantAccess(LoadOrStoreOp memOp, AffineForOp forOp) {

  for (auto operand : memOp.getMapOperands()) {
    if (!isAccessIndexInvariant(forOp.getInductionVar(), operand)) {
      return false;
    }
  }
  // Check whether other for op that use this IV as a bound operand impact the
  // access.
  DenseSet<Operation *> depForOps;
  for (auto &use : forOp.getInductionVar().getUses()) {
    if (auto depForOp = dyn_cast<AffineForOp>(use.getOwner()))
      depForOps.insert(depForOp.getOperation());
  }

  // TODO/FIXME: assert if affine for op bounds are in canonical form.
  for (auto depForOp : depForOps) {
    if (!isInvariantAccess(memOp, cast<AffineForOp>(depForOp)))
      return false;
  }

  return true;
}

// Explicitly instantiate the template so that the compiler knows we need them.
template bool mlir::isInvariantAccess(AffineLoadOp loadOp, AffineForOp);
template bool mlir::isInvariantAccess(AffineStoreOp loadOp, AffineForOp);

DenseSet<Value> mlir::getInvariantAccesses(Value iv, ArrayRef<Value> indices) {
  DenseSet<Value> res;
  for (unsigned idx = 0, n = indices.size(); idx < n; ++idx) {
    auto val = indices[idx];
    if (isAccessIndexInvariant(iv, val)) {
      res.insert(val);
    }
  }
  return res;
}

/// Given:
///   1. an induction variable `iv` of type AffineForOp;
///   2. a `memoryOp` of type const LoadOp& or const StoreOp&;
/// determines whether `memoryOp` has a contiguous access along `iv`. Contiguous
/// is defined as either invariant or varying only along a unique MemRef dim.
/// Upon success, the unique MemRef dim is written in `memRefDim` (or -1 to
/// convey the memRef access is invariant along `iv`).
///
/// Prerequisites:
///   1. `memRefDim` ~= nullptr;
///   2. `iv` of the proper type;
///   3. the MemRef accessed by `memoryOp` has no layout map or at most an
///      identity layout map.
///
/// Currently only supports no layoutMap or identity layoutMap in the MemRef.
/// Returns false if the MemRef has a non-identity layoutMap or more than 1
/// layoutMap. This is conservative.
///
// TODO: check strides.
template <typename LoadOrStoreOp>
bool mlir::isContiguousAccess(Value iv, LoadOrStoreOp memoryOp,
                              int *memRefDim) {
  static_assert(std::is_same<LoadOrStoreOp, AffineLoadOp>::value ||
                std::is_same<LoadOrStoreOp, AffineStoreOp>::value,
                "Must be called on either const LoadOp & or const StoreOp &");
  assert(memRefDim && "memRefDim == nullptr");
  auto memRefType = memoryOp.getMemRefType();

  auto layoutMap = memRefType.getAffineMaps();
  // TODO: remove dependence on Builder once we support non-identity layout map.
  Builder b(memoryOp.getContext());
  if (layoutMap.size() >= 2 ||
      (layoutMap.size() == 1 &&
       !(layoutMap[0] ==
         b.getMultiDimIdentityMap(layoutMap[0].getNumDims())))) {
    return memoryOp.emitError("NYI: non-trivial layoutMap"), false;
  }

  int uniqueVaryingIndexAlongIv = -1;
  auto accessMap = memoryOp.getAffineMap();
  for (unsigned i = 0, e = memRefType.getRank(); i < e; ++i) {
    // Gather map operands used result expr 'i' in 'exprOperands'.
    SmallVector<Value, 4> exprOperands;
    auto resultExpr = accessMap.getResult(i);

    SmallVector<int64_t, 4> flat;
    getFlattenedAffineExpr(resultExpr, accessMap.getNumDims(),
                           accessMap.getNumSymbols(), &flat);

    unsigned j = 0;
    int pos = -1;
    for (auto mapOperand : memoryOp.getMapOperands()) {
      assert((isValidSymbol(mapOperand) || isForInductionVar(mapOperand)) &&
             "loadOp not canonicalized");
      if (mapOperand == iv)
        pos = j;
      j++;
    }

    if (pos != -1 && flat[pos] > 0) {
      if (uniqueVaryingIndexAlongIv != -1) {
        // 2+ varying indices -> do not vectorize along iv.
        return false;
      }
      if (flat[pos] > 1)
        // High stride - not contiguous.
        return false;
      uniqueVaryingIndexAlongIv = i;
    }
  }

  if (uniqueVaryingIndexAlongIv == -1)
    *memRefDim = -1;
  else
    *memRefDim = memRefType.getRank() - (uniqueVaryingIndexAlongIv + 1);

  // Check whether other for op that use this IV as a bound operand impact the
  // access.
  DenseSet<Operation *> depForOps;
  for (auto &use : iv.getUses()) {
    if (auto depForOp = dyn_cast<AffineForOp>(use.getOwner()))
      depForOps.insert(depForOp.getOperation());
  }
  // FIXME: check whether affine for ops' bounds are canonicalized.
  for (auto depForOp : depForOps) {
    int depMemRefDim;
    if (!isContiguousAccess(cast<AffineForOp>(depForOp).getInductionVar(),
                            memoryOp, &depMemRefDim))
      return false;
    if (*memRefDim == -1)
      *memRefDim = depMemRefDim;
    else if (*memRefDim != depMemRefDim)
      return false;
  }

  return true;
}

template <typename LoadOrStoreOp>
static bool isVectorElement(LoadOrStoreOp memoryOp) {
  auto memRefType = memoryOp.getMemRefType();
  return memRefType.getElementType().template isa<VectorType>();
}

using VectorizableOpFun = std::function<bool(AffineForOp, Operation &)>;

static bool
isVectorizableLoopBodyWithOpCond(AffineForOp loop,
                                 VectorizableOpFun isVectorizableOp,
                                 NestedPattern &vectorTransferMatcher) {
  auto *forOp = loop.getOperation();

  // No vectorization across conditionals for now.
  auto conditionals = matcher::If();
  SmallVector<NestedMatch, 8> conditionalsMatched;
  conditionals.match(forOp, &conditionalsMatched);
  if (!conditionalsMatched.empty()) {
    return false;
  }

  // No vectorization across unknown regions.
  auto regions = matcher::Op([](Operation &op) -> bool {
    return op.getNumRegions() != 0 && !isa<AffineIfOp, AffineForOp>(op);
  });
  SmallVector<NestedMatch, 8> regionsMatched;
  regions.match(forOp, &regionsMatched);
  if (!regionsMatched.empty()) {
    return false;
  }

  SmallVector<NestedMatch, 8> vectorTransfersMatched;
  vectorTransferMatcher.match(forOp, &vectorTransfersMatched);
  if (!vectorTransfersMatched.empty()) {
    return false;
  }

  auto loadAndStores = matcher::Op(matcher::isLoadOrStore);
  SmallVector<NestedMatch, 8> loadAndStoresMatched;
  loadAndStores.match(forOp, &loadAndStoresMatched);
  for (auto ls : loadAndStoresMatched) {
    auto *op = ls.getMatchedOperation();
    auto load = dyn_cast<AffineLoadOp>(op);
    auto store = dyn_cast<AffineStoreOp>(op);
    // Only scalar types are considered vectorizable, all load/store must be
    // vectorizable for a loop to qualify as vectorizable.
    // TODO: ponder whether we want to be more general here.
    bool vector = load ? isVectorElement(load) : isVectorElement(store);
    if (vector) {
      return false;
    }
    if (isVectorizableOp && !isVectorizableOp(loop, *op)) {
      return false;
    }
  }
  return true;
}

bool mlir::isVectorizableLoopBody(AffineForOp loop, int *memRefDim,
                                  NestedPattern &vectorTransferMatcher) {
  *memRefDim = -1;
  VectorizableOpFun fun([memRefDim](AffineForOp loop, Operation &op) {
    auto load = dyn_cast<AffineLoadOp>(op);
    auto store = dyn_cast<AffineStoreOp>(op);
    int thisOpMemRefDim = -1;
    bool isContiguous = load ? isContiguousAccess(loop.getInductionVar(), load,
                                                  &thisOpMemRefDim)
                             : isContiguousAccess(loop.getInductionVar(), store,
                                                  &thisOpMemRefDim);
    if (thisOpMemRefDim != -1) {
      // If memory accesses vary across different dimensions then the loop is
      // not vectorizable.
      if (*memRefDim != -1 && *memRefDim != thisOpMemRefDim)
        return false;
      *memRefDim = thisOpMemRefDim;
    }
    return isContiguous;
  });
  return isVectorizableLoopBodyWithOpCond(loop, fun, vectorTransferMatcher);
}

bool mlir::isVectorizableLoopBody(AffineForOp loop,
                                  NestedPattern &vectorTransferMatcher) {
  return isVectorizableLoopBodyWithOpCond(loop, nullptr, vectorTransferMatcher);
}

/// Checks whether SSA dominance would be violated if a for op's body
/// operations are shifted by the specified shifts. This method checks if a
/// 'def' and all its uses have the same shift factor.
// TODO: extend this to check for memory-based dependence violation when we have
// the support.
bool mlir::isOpwiseShiftValid(AffineForOp forOp, ArrayRef<uint64_t> shifts) {
  auto *forBody = forOp.getBody();
  assert(shifts.size() == forBody->getOperations().size());

  // Work backwards over the body of the block so that the shift of a use's
  // ancestor operation in the block gets recorded before it's looked up.
  DenseMap<Operation *, uint64_t> forBodyShift;
  for (auto it : llvm::enumerate(llvm::reverse(forBody->getOperations()))) {
    auto &op = it.value();

    // Get the index of the current operation, note that we are iterating in
    // reverse so we need to fix it up.
    size_t index = shifts.size() - it.index() - 1;

    // Remember the shift of this operation.
    uint64_t shift = shifts[index];
    forBodyShift.try_emplace(&op, shift);

    // Validate the results of this operation if it were to be shifted.
    for (unsigned i = 0, e = op.getNumResults(); i < e; ++i) {
      Value result = op.getResult(i);
      for (auto *user : result.getUsers()) {
        // If an ancestor operation doesn't lie in the block of forOp,
        // there is no shift to check.
        if (auto *ancOp = forBody->findAncestorOpInBlock(*user)) {
          assert(forBodyShift.count(ancOp) > 0 && "ancestor expected in map");
          if (shift != forBodyShift[ancOp])
            return false;
        }
      }
    }
  }
  return true;
}
