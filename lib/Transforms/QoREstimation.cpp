//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#include "scalehls/Transforms/QoREstimation.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "scalehls/Transforms/Passes.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace std;
using namespace mlir;
using namespace scalehls;
using namespace hlscpp;

//===----------------------------------------------------------------------===//
// LoadOp and StoreOp Related Methods
//===----------------------------------------------------------------------===//

/// Calculate the overall partition index.
void ScaleHLSEstimator::getPartitionIndices(Operation *op) {
  auto builder = Builder(op);
  auto access = MemRefAccess(op);
  auto memrefType = access.memref.getType().cast<MemRefType>();

  // If the layout map does not exist, it means the memory is not partitioned.
  auto layoutMap = getLayoutMap(memrefType);
  if (!layoutMap) {
    auto partitionIndices = SmallVector<int64_t, 8>(memrefType.getRank(), 0);
    op->setAttr("partition_indices", builder.getI64ArrayAttr(partitionIndices));
    return;
  }

  AffineValueMap accessMap;
  access.getAccessMap(&accessMap);

  // Replace all dims in the memory access AffineMap with (step * dims). This
  // will ensure the "cyclic" array partition can be correctly detected.
  SmallVector<AffineExpr, 4> dimReplacements;
  SmallVector<AffineExpr, 4> symReplacements;

  unsigned operandIdx = 0;
  for (auto operand : accessMap.getOperands()) {
    if (operandIdx < accessMap.getNumDims()) {
      int64_t step = 1;
      if (isForInductionVar(operand))
        step = getForInductionVarOwner(operand).getStep();

      dimReplacements.push_back(step * builder.getAffineDimExpr(operandIdx));
    } else {
      symReplacements.push_back(
          builder.getAffineSymbolExpr(operandIdx - accessMap.getNumDims()));
    }
    ++operandIdx;
  }

  auto newMap = accessMap.getAffineMap().replaceDimsAndSymbols(
      dimReplacements, symReplacements, accessMap.getNumDims(),
      accessMap.getNumSymbols());

  // Compose the access map with the layout map.
  auto composeMap = layoutMap.compose(newMap);

  // Collect partition factors.
  SmallVector<int64_t, 8> factors;
  getPartitionFactors(memrefType, &factors);

  // Calculate the partition index of this load/store operation honoring the
  // partition strategy applied.
  SmallVector<int64_t, 8> partitionIndices;
  int64_t maxMuxSize = 1;
  bool hasUncertainIdx = false;

  for (int64_t dim = 0; dim < memrefType.getRank(); ++dim) {
    auto idxExpr = composeMap.getResult(dim);

    if (auto constExpr = idxExpr.dyn_cast<AffineConstantExpr>())
      partitionIndices.push_back(constExpr.getValue());
    else {
      partitionIndices.push_back(-1);
      maxMuxSize = max(maxMuxSize, factors[dim]);
      hasUncertainIdx = true;
    }
  }

  op->setAttr("partition_indices", builder.getI64ArrayAttr(partitionIndices));
  if (hasUncertainIdx)
    op->setAttr("max_mux_size", builder.getI64IntegerAttr(maxMuxSize));
}

/// Timing load/store operation honoring the memory ports number limitation.
void ScaleHLSEstimator::estimateLoadStore(Operation *op, int64_t begin) {
  auto access = MemRefAccess(op);
  auto memref = access.memref;
  auto memrefType = memref.getType().cast<MemRefType>();

  SmallVector<int64_t, 8> factors;
  auto partitionNum = getPartitionFactors(memrefType, &factors);
  auto storageType = MemoryKind(memrefType.getMemorySpaceAsInt());
  auto partitionIndices = getIntArrayAttrValue(op, "partition_indices");

  // Try to avoid memory port violation until a legal schedule is found. Since
  // an infinite length schedule cannot be generated, this while loop can be
  // proofed to have an end.
  int64_t resMinII = 1;
  for (;; ++begin) {
    auto &memPortInfos = memPortInfosMap[begin][memref];

    // If the memory has not been occupied by the current schedule level, it
    // should be initialized according to its storage type. Note that each
    // partition should have one PortInfo structure, where the default case is
    // BRAM_S2P.
    if (memPortInfos.empty())
      for (unsigned p = 0; p < partitionNum; ++p) {
        MemPortInfo info;

        if (storageType == MemoryKind::BRAM_1P)
          info.rdwrPort = 1;
        else if (storageType == MemoryKind::BRAM_T2P)
          info.rdwrPort = 2;
        else
          info.rdPort = 1, info.wrPort = 1;

        memPortInfos.push_back(info);
      }

    // Indicate whether the memory access operation is successfully scheduled in
    // the current schedule level.
    bool successFlag = true;

    // Walk through all partitions to check whether the current partition is
    // occupied and whether available memory ports are enough to schedule the
    // current memory access operation.
    for (int64_t idx = 0; idx < partitionNum; ++idx) {
      bool isOccupied = true;
      int64_t accumFactor = 1;

      for (int64_t dim = 0; dim < memrefType.getRank(); ++dim) {
        // If the index is -1, all ports in the current partition will be
        // occupied and a multiplexer will be generated in HLS.
        if (partitionIndices[dim] != -1 &&
            idx / accumFactor % factors[dim] != partitionIndices[dim]) {
          isOccupied = false;
          break;
        }
        accumFactor *= factors[dim];
      }

      if (isOccupied) {
        auto &info = memPortInfos[idx];
        if (isa<AffineReadOpInterface>(op)) {
          bool hasIdenticalAccess = false;
          // The rationale is as long as the current read operation has
          // identical memory access information with any scheduled read
          // operation, the schedule will success.
          for (auto rdAccess : info.rdAccesses) {
            if (access == rdAccess &&
                op->getBlock() == rdAccess.opInst->getBlock())
              hasIdenticalAccess = true;
          }

          if (hasIdenticalAccess)
            continue;

          if (info.rdPort > 0) {
            info.rdPort--;
            info.rdAccesses.push_back(access);
          } else if (info.rdwrPort > 0) {
            info.rdwrPort--;
            info.rdAccesses.push_back(access);
          } else {
            successFlag = false;
            break;
          }
        } else if (isa<AffineWriteOpInterface>(op)) {
          if (info.wrPort > 0)
            info.wrPort--;
          else if (info.rdwrPort > 0)
            info.rdwrPort--;
          else {
            successFlag = false;
            break;
          }
        }
      }
    }

    if (successFlag)
      break;
    ++resMinII;
  }

  if (isa<AffineReadOpInterface>(op))
    setTiming(op, begin, begin + 2, 2, 1);
  else
    setTiming(op, begin, begin + 1, 1, 1);
}

//===----------------------------------------------------------------------===//
// AffineForOp Related Methods
//===----------------------------------------------------------------------===//

static int64_t getMaxMuxSize(Operation *op) {
  if (auto maxMuxSize = op->getAttrOfType<IntegerAttr>("max_mux_size"))
    return maxMuxSize.getInt();
  else
    return 1;
}

static bool isNoTouch(Operation *op) {
  if (auto noTouch = op->getAttrOfType<BoolAttr>("no_touch"))
    if (noTouch.getValue())
      return true;

  return false;
}

int64_t ScaleHLSEstimator::getResMinII(int64_t begin, int64_t end,
                                       MemAccessesMap &map) {
  int64_t II = 1;
  for (auto &pair : map) {
    auto memref = pair.first;
    auto memrefType = memref.getType().cast<MemRefType>();
    auto partitionNum = getPartitionFactors(memrefType);
    auto storageType = MemoryKind(memrefType.getMemorySpaceAsInt());

    auto accessNum = SmallVector<int64_t, 16>(partitionNum, 0);
    // Prepare for BRAM_S1P memory kind.
    auto writeNum = SmallVector<int64_t, 16>(partitionNum, 0);

    // TODO: fine-tune for BRAM_T2P.
    for (int64_t level = begin; level < end; ++level)
      if (memPortInfosMap.count(level))
        if (memPortInfosMap[level].count(memref)) {
          auto &memPortInfos = memPortInfosMap[level][memref];

          for (int64_t idx = 0; idx < partitionNum; ++idx) {
            auto &info = memPortInfos[idx];
            if (storageType == MemoryKind::BRAM_1P && info.rdwrPort < 1)
              ++accessNum[idx];
            else if (storageType == MemoryKind::BRAM_T2P && info.rdwrPort < 2)
              ++accessNum[idx];
            else if (info.rdPort < 1)
              ++accessNum[idx];
            else if (info.wrPort < 1)
              ++writeNum[idx];
          }
        }

    II = max({II, *std::max_element(writeNum.begin(), writeNum.end()),
              *std::max_element(accessNum.begin(), accessNum.end())});
  }
  return II;
}

/// Calculate the minimum dependency II of function.
int64_t ScaleHLSEstimator::getDepMinII(int64_t II, FuncOp func,
                                       MemAccessesMap &map) {
  for (auto &pair : map) {
    auto loadStores = pair.second;

    // Walk through each pair of dependency source and destination.
    for (auto dstOp : loadStores)
      for (auto srcOp : loadStores) {
        if (dstOp == srcOp)
          continue;

        // If delay is smaller than the current II, stop and continue because
        // the minimum distance is one.
        auto delay = getTiming(dstOp).getEnd() - getTiming(srcOp).getBegin();
        if (delay <= II)
          continue;

        // Similar to getFuncDependencies() method, in some cases RAR is not
        // considered as dependency in Vivado HLS.
        auto srcMuxSize = getMaxMuxSize(srcOp);
        auto dstMuxSize = getMaxMuxSize(dstOp);
        if (isa<AffineReadOpInterface>(srcOp) && srcMuxSize <= 3 &&
            isa<AffineReadOpInterface>(dstOp) && dstMuxSize <= 3)
          continue;

        // Distance is always 1 thus the minimum II is equal to delay.
        // TODO: need more case study.
        if (MemRefAccess(srcOp) == MemRefAccess(dstOp))
          II = max(II, delay);
      }
  }
  return II;
}

/// Calculate the minimum dependency II of loop.
int64_t ScaleHLSEstimator::getDepMinII(int64_t II, AffineForOp forOp,
                                       MemAccessesMap &map) {
  AffineLoopBand band;
  getLoopIVs(forOp.front(), &band);

  // Find all loop levels whose dependency need to be checked.
  SmallVector<unsigned, 8> loopDepths;
  for (unsigned i = 1, e = band.size(); i <= e; ++i) {
    auto loop = band[i - 1];
    auto loopDirect = getLoopDirective(loop);
    if (!loopDirect)
      loop.emitError("loop directives missing on for loops");

    if (loopDirect.getFlatten() || loopDirect.getPipeline())
      if (!loopDirect.getParallel())
        loopDepths.push_back(i);
  }

  for (auto &pair : map) {
    auto loadStores = pair.second;

    // Walk through each pair of source and destination.
    unsigned dstIndex = 1;
    for (auto dstOp : loadStores) {
      auto srcOps = SmallVector<Operation *, 16>(
          llvm::drop_begin(loadStores, dstIndex++));

      for (auto it = srcOps.rbegin(); it != srcOps.rend(); ++it) {
        auto srcOp = *it;

        // If delay is smaller than the current II, stop and continue because
        // the minimum distance is one.
        auto delay = getTiming(dstOp).getEnd() - getTiming(srcOp).getBegin();
        if (delay <= II)
          continue;

        // Similar to getFuncDependencies() method, in some cases RAR is not
        // considered as dependency in Vivado HLS.
        auto srcMuxSize = getMaxMuxSize(srcOp);
        auto dstMuxSize = getMaxMuxSize(dstOp);
        if (isa<AffineReadOpInterface>(srcOp) && srcMuxSize <= 3 &&
            isa<AffineReadOpInterface>(dstOp) && dstMuxSize <= 3)
          continue;

        // Now we must check whether carried dependency exists and calculate the
        // dependency distance if required.
        MemRefAccess dstAccess(dstOp);
        MemRefAccess srcAccess(srcOp);

        // If depAnalysis is not set, only when the two memref accesses are
        // identical, we analyze their dependency.
        if (!depAnalysis && dstAccess != srcAccess)
          continue;

        for (auto depth : loopDepths) {
          FlatAffineValueConstraints depConstrs;
          SmallVector<DependenceComponent, 2> depComps;

          DependenceResult result = checkMemrefAccessDependence(
              srcAccess, dstAccess, depth, &depConstrs, &depComps,
              /*allowRAR=*/true);

          if (hasDependence(result)) {
            int64_t distance = 0;

            if (dstMuxSize > 3 || srcMuxSize > 3) {
              // If the two memory accesses are identical or one of them is
              // implemented as separate function call, the dependency exists.
              distance = 1;
            } else {
              SmallVector<int64_t, 8> accumTrips;
              accumTrips.push_back(1);

              // Calculate the distance of this dependency.
              for (auto i = depComps.rbegin(); i < depComps.rend(); ++i) {
                auto dep = *i;
                auto loop = cast<AffineForOp>(dep.op);

                auto ub = dep.ub.getValue();
                auto lb = dep.lb.getValue();

                // If ub is more than zero, calculate the minimum positive
                // disatance. Otherwise, set distance to negative and break.
                if (ub >= 0)
                  distance +=
                      accumTrips.back() * max(lb, (int64_t)0) / loop.getStep();
                else {
                  distance = -1;
                  break;
                }
                accumTrips.push_back(accumTrips.back() *
                                     getAverageTripCount(loop).getValue());
              }
            }

            // We will only consider intra-dependencies with positive distance.
            if (distance > 0) {
              int64_t minII = ceil((float)delay / distance);
              II = max(II, minII);
            }
          }
        }
      }
    }
  }
  return II;
}

bool ScaleHLSEstimator::visitOp(AffineForOp op, int64_t begin) {
  // If a loop is marked as no_touch, then directly infer the schedule_end with
  // the exist latency.
  if (isNoTouch(op)) {
    auto timing = getTiming(op);
    auto resource = getResource(op);

    if (timing && resource) {
      auto latency = timing.getLatency();
      setTiming(op, begin, begin + latency, latency, latency);
      return true;
    }
  }

  // Set an attribute indicating the trip count. For now, we assume all loops
  // have static loop bound.
  auto optionalTripCount = getAverageTripCount(op);
  if (!optionalTripCount)
    return false;
  auto tripCount = optionalTripCount.getValue();

  // Estimate the contained loop block.
  auto &loopBlock = *op.getBody();
  auto timing = estimateTiming(loopBlock, begin);
  if (!timing)
    return false;

  auto end = max(begin, timing.getEnd());
  begin = max(begin, timing.getBegin());

  // Handle pipelined or flattened loops.
  if (auto loopDirect = getLoopDirective(op)) {
    // If the current loop is annotated as pipelined loop, extra dependency and
    // resource aware II analysis will be executed.
    if (loopDirect.getPipeline()) {
      // Collect load and store operations in the loop block for solving
      // possible carried dependencies.
      // TODO: include CallOps, how? It seems dependencies always exist for all
      // CallOps not matter its access pattern.
      MemAccessesMap map;
      getMemAccessesMap(loopBlock, map);

      // Calculate initial interval.
      auto targetII = loopDirect.getTargetII();
      auto resII = getResMinII(begin, end, map);
      auto depII = getDepMinII(max(targetII, resII), op, map);
      auto II = max({targetII, resII, depII});

      // Calculate latency of each iteration and update loop information.
      auto iterLatency = end - begin;
      setLoopInfo(op, tripCount, iterLatency, II);

      // Entering and leaving a loop will consume extra 2 clock cycles.
      auto latency = iterLatency + II * (tripCount - 1) + 2;
      setTiming(op, begin, begin + latency, latency, latency);

      // Estimate the loop block resource utilization.
      setResource(op, estimateResource(loopBlock, II));
      return true;
    }

    // If the current loop is annotated as flatten, it will be flattened into
    // the child pipelined loop. This will increase the flattened loop trip
    // count without changing the iteration latency.
    else if (loopDirect.getFlatten()) {
      auto child = dyn_cast<AffineForOp>(op.getBody()->front());
      assert(child && "the first containing operation is not a loop");
      auto childLoopInfo = getLoopInfo(child);

      // Flattened loop share the same II with its child loop. Calculate latency
      // of each iteration and update loop information.
      auto iterLatency = childLoopInfo.getIterLatency();
      auto flattenTripCount = childLoopInfo.getFlattenTripCount() * tripCount;
      auto II = childLoopInfo.getMinII();
      setLoopInfo(op, flattenTripCount, iterLatency, II);

      auto latency = iterLatency + II * (flattenTripCount - 1) + 2;
      setTiming(op, begin, begin + latency, latency, latency);

      // The resource utilization of flattened loop is equal to its child's.
      setResource(op, getResource(child));
      return true;
    }
  }

  // Default case (not flattend or pipelined), calculate latency and resource
  // utilization accordingly.
  auto iterLatency = end - begin;
  setLoopInfo(op, tripCount, iterLatency, iterLatency);

  auto latency = iterLatency * tripCount + 2;
  setTiming(op, begin, begin + latency, latency, latency);
  setResource(op, estimateResource(loopBlock));
  return true;
}

//===----------------------------------------------------------------------===//
// Other Operation Handlers
//===----------------------------------------------------------------------===//

bool ScaleHLSEstimator::visitOp(AffineIfOp op, int64_t begin) {
  auto end = begin;
  auto thenBlock = op.getThenBlock();

  // Estimate then block.
  if (auto timing = estimateTiming(*thenBlock, begin))
    end = max(end, timing.getEnd());
  else
    return false;

  // Handle else block if required.
  if (op.hasElse()) {
    auto elseBlock = op.getElseBlock();

    if (auto timing = estimateTiming(*elseBlock, begin))
      end = max(end, timing.getEnd());
    else
      return false;
  }

  // In our assumption, AffineIfOp is completely transparent. Therefore, we
  // set a dummy schedule begin here.
  setTiming(op, end, end, 0, 0);
  return true;
}

bool ScaleHLSEstimator::visitOp(CallOp op, int64_t begin) {
  auto callee = SymbolTable::lookupNearestSymbolFrom(op, op.calleeAttr());
  auto subFunc = dyn_cast<FuncOp>(callee);
  assert(subFunc && "callable is not a function operation");

  ScaleHLSEstimator estimator(latencyMap, depAnalysis);
  estimator.estimateFunc(subFunc);

  // We assume enter and leave the subfunction require extra 2 clock cycles.
  if (auto timing = getTiming(subFunc)) {
    auto latency = timing.getLatency();
    setTiming(op, begin, begin + latency, latency, timing.getInterval());
    setResource(op, getResource(subFunc));
    return true;
  } else
    return false;
}

//===----------------------------------------------------------------------===//
// Block Scheduler and Estimator
//===----------------------------------------------------------------------===//

int64_t ScaleHLSEstimator::getDspAllocMap(Block &block,
                                          ResourceAllocMap &faddMap,
                                          ResourceAllocMap &fmulMap) {
  int64_t staticDspNum = 0;
  for (auto &op : block) {
    auto begin = getTiming(&op).getBegin();
    auto end = getTiming(&op).getEnd();

    // Accumulate the resource utilization of each operation.
    if (isa<arith::AddFOp, arith::SubFOp>(op))
      for (unsigned i = begin; i < end; ++i)
        ++faddMap[i];

    else if (isa<arith::MulFOp>(op))
      for (unsigned i = begin; i < end; ++i)
        ++fmulMap[i];

    else if (isa<AffineForOp, CallOp>(op))
      staticDspNum += getResource(&op).getDsp();

    else if (auto ifOp = dyn_cast<AffineIfOp>(op)) {
      // AffineIfOp is transparent during scheduling, thus here we recursively
      // enter each if block.
      staticDspNum += getDspAllocMap(*ifOp.getThenBlock(), faddMap, fmulMap);
      if (ifOp.hasElse())
        staticDspNum += getDspAllocMap(*ifOp.getElseBlock(), faddMap, fmulMap);
    }
  }
  return staticDspNum;
}

ResourceAttr ScaleHLSEstimator::estimateResource(Block &block, int64_t minII) {
  ResourceAllocMap faddMap;
  ResourceAllocMap fmulMap;
  auto staticDspNum = getDspAllocMap(block, faddMap, fmulMap);

  // Find the max resource utilization across all schedule levels.
  int64_t maxFadd = 0;
  for (auto level : faddMap)
    maxFadd = max(maxFadd, level.second);

  int64_t maxFmul = 0;
  for (auto level : fmulMap)
    maxFmul = max(maxFmul, level.second);

  // We assume the loop resource utilization cannot be shared. Therefore, the
  // overall resource utilization is loops' plus other operstions'. According
  // to profiling, floating-point add and muliply will consume 2 and 3 DSP
  // units, respectively.
  auto shareDspNum = maxFadd * 2 + maxFmul * 3;
  auto dsp = staticDspNum + shareDspNum;

  int64_t totalFadd = 0;
  int64_t totalFmul = 0;
  block.walk([&](Operation *op) {
    if (isa<arith::AddFOp, arith::SubFOp>(op))
      ++totalFadd;
    else if (isa<arith::MulFOp>(op))
      ++totalFmul;
  });

  auto nonShareDspNum = totalFadd * 2 + totalFmul * 3;

  // If the block is pipelined (minII is positive), the minimum resource
  // utilization is determined by minII.
  if (minII > 0)
    dsp = staticDspNum + nonShareDspNum / minII;

  // TODO: Estimate bram, ff, lut.
  int64_t lut = 0;
  int64_t bram = 0;
  return ResourceAttr::get(block.getParentOp()->getContext(), lut, dsp, bram,
                           nonShareDspNum);
}

// Get the pointer of the scrOp's parent loop, which should locat at the same
// level with dstOp's any parent loop.
static Operation *getSameLevelDstOp(Operation *srcOp, Operation *dstOp) {
  // If srcOp and dstOp are already at the same level, return the srcOp.
  if (checkSameLevel(srcOp, dstOp))
    return dstOp;

  // Helper to get all surrouding AffineForOps. AffineIfOps are skipped.
  auto getSurroundFors =
      ([&](Operation *op, SmallVector<Operation *, 4> &nests) {
        nests.push_back(op);
        auto currentOp = op;
        while (true) {
          if (auto parentOp = currentOp->getParentOfType<AffineForOp>()) {
            nests.push_back(parentOp);
            currentOp = parentOp;
          } else if (auto parentOp = currentOp->getParentOfType<AffineIfOp>())
            currentOp = parentOp;
          else
            break;
        }
      });

  SmallVector<Operation *, 4> srcNests;
  SmallVector<Operation *, 4> dstNests;

  getSurroundFors(srcOp, srcNests);
  getSurroundFors(dstOp, dstNests);

  // If any parent of srcOp (or itself) and any parent of dstOp (or itself) are
  // at the same level, return the pointer.
  for (auto src : srcNests)
    for (auto dst : dstNests)
      if (checkSameLevel(src, dst))
        return dst;

  return nullptr;
}

/// Estimate the latency of a block with ALAP scheduling strategy, return the
/// end level of schedule. Meanwhile, the input begin will also be updated if
/// required (typically happens in AffineForOps).
TimingAttr ScaleHLSEstimator::estimateTiming(Block &block, int64_t begin) {
  auto blockBegin = begin;
  auto blockEnd = begin;

  // Reversely walk through all operations in the block.
  for (auto i = block.rbegin(), e = block.rend(); i != e; ++i) {
    auto op = &*i;
    auto opBegin = begin;
    auto opEnd = begin;

    // Calculate the partition indices of memory load and store operations.
    if (isa<AffineLoadOp, AffineStoreOp>(op))
      getPartitionIndices(op);

    // Find the latest arrived successor depending on the current operation.
    for (auto user : op->getUsers()) {
      auto sameLevelUser = getSameLevelDstOp(op, user);
      opBegin = max(opBegin, getTiming(sameLevelUser).getEnd());
    }

    // Check other dependencies and update schedule level.
    for (auto dstOp : dependsMap[op]) {
      auto sameLevelDstOp = getSameLevelDstOp(op, dstOp);
      opBegin = max(opBegin, getTiming(sameLevelDstOp).getEnd());
    }

    // Check memory dependencies of the operation and update schedule level.
    for (auto operand : op->getOperands()) {
      if (operand.getType().isa<MemRefType>())
        // All users of the same memref value has the possibility to share
        // dependency with the current operation.
        for (auto depOp : operand.getUsers()) {
          // If the depOp has not been scheduled or its schedule level will not
          // impact the current operation's scheduling, stop and continue.
          auto depOpTiming = getTiming(depOp);
          if (!depOpTiming)
            continue;

          auto depOpEnd = depOpTiming.getEnd();
          if (depOpEnd <= opBegin)
            continue;

          // If either the depOp or the current operation is a function call,
          // dependency exists and the schedule level should be updated.
          if (isa<CallOp>(op) || isa<CallOp>(depOp)) {
            opBegin = max(opBegin, depOpEnd);
            continue;
          }

          // Now both of the depOp and the current operation must be memory
          // load/store operation.
          auto opMuxSize = getMaxMuxSize(op);
          auto depOpMuxSize = getMaxMuxSize(depOp);

          // In Vivado HLS, a memory access with undetermined partition index
          // will be implemented as a function call with a multiplexer if the
          // partition factor is larger than 3. Function call has dependency
          // with any load/store operation including RAR.
          if (isa<AffineReadOpInterface>(op) && opMuxSize <= 3 &&
              isa<AffineReadOpInterface>(depOp) && depOpMuxSize <= 3)
            continue;

          // Now we must check whether any dependency exists between the two
          // operations. If so, update the scheduling level.
          auto opAccess = MemRefAccess(op);
          auto depOpAccess = MemRefAccess(depOp);

          // If depAnalysis is not set, only when the two memref accesses are
          // identical, we analyze their dependency.
          if (!depAnalysis && opAccess != depOpAccess)
            continue;

          AffineLoopBand commonLoops;
          auto loopDepth = getCommonSurroundingLoops(op, depOp, &commonLoops);

          for (unsigned depth = 1; depth <= loopDepth + 1; ++depth) {
            // Skip all parallel loop level.
            if (depth != loopDepth + 1) {
              if (auto loopDirect = getLoopDirective(commonLoops[depth - 1]))
                if (loopDirect.getParallel())
                  continue;
            }

            FlatAffineValueConstraints dependConstrs;

            DependenceResult result = checkMemrefAccessDependence(
                opAccess, depOpAccess, depth, &dependConstrs,
                /*dependenceComponents=*/nullptr, /*allowRAR=*/true);

            if (hasDependence(result)) {
              opBegin = max(opBegin, depOpEnd);
              break;
            }
          }
        }
    }

    // Estimate the current operation.
    if (dispatchVisitor(op, opBegin))
      opEnd = max(opEnd, getTiming(op).getEnd());
    else {
      op->emitError("Failed to estimate op");
      return TimingAttr();
    }

    // Update the block schedule end and begin.
    if (i == block.rbegin())
      blockBegin = opBegin;
    else
      blockBegin = min(blockBegin, opBegin);

    blockEnd = max(blockEnd, opEnd);
  }

  return TimingAttr::get(block.getParentOp()->getContext(), blockBegin,
                         blockEnd, blockEnd - blockBegin,
                         blockEnd - blockBegin);
}

/// Get the innermost surrounding operation, either an AffineForOp or a FuncOp.
/// In this method, AffineIfOp is transparent as well.
static Operation *getSurroundingOp(Operation *op) {
  auto currentOp = op;
  while (true) {
    if (auto parentIfOp = currentOp->getParentOfType<AffineIfOp>())
      currentOp = parentIfOp;
    else if (auto parentForOp = currentOp->getParentOfType<AffineForOp>())
      return parentForOp;
    else if (auto parentFuncOp = currentOp->getParentOfType<FuncOp>())
      return parentFuncOp;
    else
      return nullptr;
  }
}

void ScaleHLSEstimator::reverseTiming(Block &block) {
  block.walk([&](Operation *op) {
    // Get schedule level.
    if (auto timing = getTiming(op)) {
      auto begin = timing.getBegin();
      auto end = timing.getEnd();
      auto latency = timing.getLatency();
      auto interval = timing.getInterval();

      // Reverse schedule level.
      if (auto srd = getSurroundingOp(op)) {
        if (isa<AffineForOp, scf::ForOp>(srd)) {
          auto srdBegin = getTiming(srd).getBegin();

          // Handle normal cases.
          auto iterLatency = getLoopInfo(srd).getIterLatency();
          setTiming(op, srdBegin + iterLatency - end,
                    srdBegin + iterLatency - begin, latency, interval);

          // Handle flattened surrounding loops.
          if (auto srdDirect = getLoopDirective(srd)) {
            if (srdDirect.getFlatten())
              setTiming(op, srdBegin, srdBegin + latency, latency, interval);
          }
        } else if (isa<FuncOp>(srd)) {
          auto srdLatency = getTiming(srd).getLatency() - 2;
          setTiming(op, srdLatency - end, srdLatency - begin, latency,
                    interval);
        } else
          op->emitError("unexpected surrounding operation");
      }
    }
  });
}

void ScaleHLSEstimator::initEstimator(Block &block) {
  // Clear global maps and scheduling information. Walk through all loops and
  // establish dependencies. The rationale here is in Vivado HLS, a loop will
  // always be blocked by other loops before it, even if no actual dependency
  // exists between them.
  dependsMap.clear();
  memPortInfosMap.clear();

  SmallVector<Operation *, 16> loops;
  block.walk([&](Operation *op) {
    if (!isNoTouch(op)) {
      op->removeAttr("resource");
      op->removeAttr("timing");
      op->removeAttr("loop_info");
    }

    if (isa<AffineForOp>(op))
      loops.push_back(op);
  });

  unsigned idx = 1;
  for (auto srcLoop : loops) {
    for (auto dstLoop : llvm::drop_begin(loops, idx))
      if (checkSameLevel(srcLoop, dstLoop))
        dependsMap[srcLoop].push_back(dstLoop);
    ++idx;
  }
}

void ScaleHLSEstimator::estimateFunc(FuncOp func) {
  initEstimator(func.front());

  // Collect all memory access operations for later use.
  MemAccessesMap map;
  getMemAccessesMap(func.front(), map);

  // Recursively estimate blocks in the function.
  auto timing = estimateTiming(func.front());
  if (!timing)
    return;

  auto latency = timing.getEnd() + 2;
  auto interval = latency;

  // Handle pipelined or dataflowed loops.
  if (auto funcDirect = getFuncDirective(func)) {
    if (funcDirect.getDataflow()) {
      interval = 1;
      for (auto callOp : func.getOps<CallOp>()) {
        auto subFuncLatency =
            getTiming(callOp).getEnd() - getTiming(callOp).getBegin();
        interval = max(interval, subFuncLatency);
      }

    } else if (funcDirect.getPipeline()) {
      // TODO: support CallOp inside of the function.
      auto targetInterval = funcDirect.getTargetInterval();
      auto resInterval = getResMinII(0, timing.getEnd(), map);
      auto depInterval =
          getDepMinII(max(targetInterval, resInterval), func, map);
      interval = max({targetInterval, resInterval, depInterval});
    }
  }

  // Estimate and set timing and resource attributes.
  setTiming(func, 0, latency, latency, interval);
  setResource(func, estimateResource(func.front(), interval));

  // Scheduled levels of all operations are reversed in this method, because
  // we have done the ALAP scheduling in a reverse order. Note that after
  // the reverse, the annotated scheduling level of each operation is a
  // relative level of the nearest surrounding AffineForOp or FuncOp.
  reverseTiming(func.front());

  // // Calculate the function memrefs BRAM utilization.
  // int64_t numBram = 0;
  // for (auto &pair : map) {
  //   auto memrefType = pair.first.getType().cast<MemRefType>();
  //   auto partitionNum = getPartitionFactors(memrefType);
  //   auto storageType = MemoryKind(memrefType.getMemorySpaceAsInt());

  //   if (storageType == MemoryKind::BRAM_1P ||
  //       storageType == MemoryKind::BRAM_S2P ||
  //       storageType == MemoryKind::BRAM_T2P) {
  //     // Multiply bit width of type.
  //     // TODO: handle index types.
  //     int64_t memrefSize =
  //         memrefType.getElementTypeBitWidth() * memrefType.getNumElements();
  //     numBram += ((memrefSize + 18000 - 1) / 18000) * partitionNum;
  //   }
  // }
  // resource.bram += numBram;
}

void ScaleHLSEstimator::estimateLoop(AffineForOp loop, FuncOp func) {
  initEstimator(func.getBody().front());
  dispatchVisitor(loop, 0);
}

//===----------------------------------------------------------------------===//
// Entry of scalehls-opt
//===----------------------------------------------------------------------===//

void scalehls::getLatencyMap(INIReader spec, LatencyMap &latencyMap) {
  auto freq = spec.Get("specification", "frequency", "100MHz");

  latencyMap["fadd"] = spec.GetInteger(freq, "fadd", 4);
  latencyMap["fmul"] = spec.GetInteger(freq, "fmul", 3);
  latencyMap["fdiv"] = spec.GetInteger(freq, "fdiv", 15);
  latencyMap["fcmp"] = spec.GetInteger(freq, "fcmp", 1);
}

namespace {
struct QoREstimation : public scalehls::QoREstimationBase<QoREstimation> {
  void runOnOperation() override {
    auto module = getOperation();

    // Read configuration file.
    INIReader spec(targetSpec);
    if (spec.ParseError())
      emitError(module.getLoc(), "target spec file parse fail\n");

    // Collect profiling latency data, where default values are based on Xilinx
    // PYNQ-Z1 board.
    LatencyMap latencyMap;
    getLatencyMap(spec, latencyMap);

    // Estimate performance and resource utilization. If any other functions are
    // called by the top function, it will be estimated in the procedure of
    // estimating the top function.
    for (auto func : module.getOps<FuncOp>())
      if (auto funcDirect = getFuncDirective(func))
        if (funcDirect.getTopFunc())
          ScaleHLSEstimator(latencyMap, depAnalysis).estimateFunc(func);
  }
};
} // namespace

std::unique_ptr<Pass> scalehls::createQoREstimationPass() {
  return std::make_unique<QoREstimation>();
}
