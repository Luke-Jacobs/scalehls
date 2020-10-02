// RUN: scalehls-opt -qor-estimation %s | FileCheck %s

// CHECK-LABEL: func @test_for
func @test_for(%arg0: memref<16x4x4xindex>, %arg1: memref<16x4x4xindex>) {
  affine.for %i = 0 to 16 {
    "hlscpp.loop_pragma" () {II = 1 : ui32, off = true, enable_flush = false, rewind = false, factor = 4 : ui32, region = false, skip_exit_check = false} : () -> ()
    affine.for %j = 0 to 4 {
      "hlscpp.loop_pragma" () {II = 1 : ui32, off = false, enable_flush = false, rewind = false, factor = 1 : ui32, region = false, skip_exit_check = false} : () -> ()
      affine.for %k = 0 to 4 {
        "hlscpp.loop_pragma" () {II = 1 : ui32, off = true, enable_flush = false, rewind = false, factor = 4 : ui32, region = false, skip_exit_check = false} : () -> ()
        %0 = affine.load %arg0[%i, %j, %k] : memref<16x4x4xindex>
        %1 = affine.load %arg1[%i, %j, %k] : memref<16x4x4xindex>
        %2 = muli %0, %1 : index
        affine.store %2, %arg0[%i, %j, %k] : memref<16x4x4xindex>
      }
    }
  }
  return
}