//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#include "scalehls/Transforms/Utils.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/LoopUtils.h"
#include "mlir/Transforms/Passes.h"
#include "scalehls/Conversion/Passes.h"
#include "scalehls/Transforms/Passes.h"

using namespace mlir;
using namespace scalehls;

/// Fully unroll all loops insides of a block.
bool scalehls::applyFullyLoopUnrolling(Block &block) {
  // Try 8 iterations before exiting.
  for (auto i = 0; i < 8; ++i) {
    bool hasFullyUnrolled = true;
    block.walk([&](AffineForOp loop) {
      if (failed(loopUnrollFull(loop)))
        hasFullyUnrolled = false;
    });

    if (hasFullyUnrolled)
      break;

    if (i == 7)
      return false;
  }
  return true;
}

static void addPassPipeline(PassManager &pm) {
  // To factor out the redundant AffineApply/AffineIf operations.
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createSimplifyAffineIfPass());

  // To simplify the memory accessing. Note that the store forwarding is
  // non-trivial and has a worst case complexity of O(n^2).
  pm.addPass(createAffineStoreForwardPass());
  pm.addPass(createSimplifyMemrefAccessPass());

  // Generic common sub expression elimination.
  pm.addPass(createCSEPass());

  // Apply the best suitable array partition strategy to the function.
  pm.addPass(createArrayPartitionPass());
}

bool scalehls::applyFullyUnrollAndPartition(Block &block, FuncOp func) {
  applyFullyLoopUnrolling(block);

  // Apply general optimizations and array partition.
  PassManager optPM(func.getContext(), "func");
  addPassPipeline(optPM);
  if (failed(optPM.run(func)))
    return false;

  return true;
}

/// Apply optimization strategy to a loop band. The ancestor function is also
/// passed in because the post-tiling optimizations have to take function as
/// target, e.g. canonicalizer and array partition.
bool scalehls::applyOptStrategy(AffineLoopBand &band, FuncOp func,
                                TileList tileList, unsigned targetII) {
  // By design the input function must be the ancestor of the input loop band.
  if (!func->isProperAncestor(band.front()))
    return false;

  // Apply loop tiling.
  auto pipelineLoopLoc = applyLoopTiling(band, tileList);
  if (!pipelineLoopLoc)
    return false;

  // Apply LegalizeToHLSCpp conversion pass.
  PassManager convertPM(func.getContext(), "func");
  convertPM.addPass(createLegalizeToHLSCppPass());
  if (failed(convertPM.run(func)))
    return false;

  // Apply loop pipelining.
  if (!applyLoopPipelining(band, pipelineLoopLoc.getValue(), targetII))
    return false;

  // Apply general optimizations and array partition.
  PassManager optPM(func.getContext(), "func");
  addPassPipeline(optPM);
  if (failed(optPM.run(func)))
    return false;

  return true;
}

/// Apply optimization strategy to a function.
bool scalehls::applyOptStrategy(FuncOp func, ArrayRef<TileList> tileLists,
                                ArrayRef<unsigned> targetIIs) {
  AffineLoopBands bands;
  getLoopBands(func.front(), bands);

  // Apply loop tiling and pipelining to all loop bands.
  SmallVector<unsigned, 4> pipelineLoopLocs;
  for (unsigned i = 0, e = bands.size(); i < e; ++i) {
    auto pipelineLoopLoc = applyLoopTiling(bands[i], tileLists[i]);
    if (!pipelineLoopLoc)
      return false;
    pipelineLoopLocs.push_back(pipelineLoopLoc.getValue());
  }

  // Apply LegalizeToHLSCpp conversion pass.
  PassManager convertPM(func.getContext(), "func");
  convertPM.addPass(createLegalizeToHLSCppPass());
  if (failed(convertPM.run(func)))
    return false;

  for (unsigned i = 0, e = bands.size(); i < e; ++i) {
    if (!applyLoopPipelining(bands[i], pipelineLoopLocs[i], targetIIs[i]))
      return false;
  }

  // Apply general optimizations and array partition.
  PassManager optPM(func.getContext(), "func");
  addPassPipeline(optPM);
  if (failed(optPM.run(func)))
    return false;

  return true;
}
