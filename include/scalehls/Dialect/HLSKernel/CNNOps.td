//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#ifndef SCALEHLS_DIALECT_HLSKERNEL_CNNOPS_TD
#define SCALEHLS_DIALECT_HLSKERNEL_CNNOPS_TD

def DenseOp : HLSKernelOp<"dense", [HLSKernelOpInterface]> {
  let summary = "dense operation";
  let description = [{
    Dense layer (fully-connected layer).

    I: (N, C) or (N, C, H, W)
    K: (F, C) or (F, C, H, W)
    B: (F)
    O: (N, F)
  }];

  let arguments = (ins
    AnyShaped:$I,
    AnyShaped:$K,
    AnyShaped:$B,
    Optional<AnyMemRef>:$O
  );
  let results = (outs
    Optional<AnyRankedTensor>:$tensorO
  );

  let verifier = [{ return ::verify(*this); }];
}

def ConvOp : HLSKernelOp<"conv", [HLSKernelOpInterface]> {
  let summary = "convolution operation";
  let description = [{
    Convolutional layer.

    I: (N, C, H, W)
    K: (F, C, KH, KW)
    B: (F)
    O: (N, F, H, W)

    Strides and padding are integer attributes of the same rank as the number of
    window dimensions. The padding attribute specifies the amount of zero
    padding to be applied to the base area, which is a 4-d array of (top,
    bottom, left, right) padding.
  }];

  let arguments = (ins
    AnyShaped:$I,
    AnyShaped:$K,
    AnyShaped:$B,
    Optional<AnyMemRef>:$O,
    OptionalAttr<I64ArrayAttr>:$strides,
    OptionalAttr<I64ArrayAttr>:$padding
  );
  let results = (outs
    Optional<AnyRankedTensor>:$tensorO
  );

  let verifier = [{ return ::verify(*this); }];
}

def MaxPoolOp : HLSKernelOp<"maxpool", [HLSKernelOpInterface]> {
  let summary = "max pooling operation";
  let description = [{
    Max pooling layer. Only support 2D pooling, aka dimension of kernel_shape
    attribute is 2.

    I: (N, C, H, W)
    O: (N, C, H, W)
  }];

  let arguments = (ins
    AnyShaped:$I,
    Optional<AnyMemRef>:$O,
    OptionalAttr<I64ArrayAttr>:$kernel_shape,
    OptionalAttr<I64ArrayAttr>:$strides,
    OptionalAttr<I64ArrayAttr>:$padding
  );
  let results = (outs
    Optional<AnyRankedTensor>:$tensorO
  );

  let verifier = [{ return ::verify(*this); }];
}

def ReluOp : HLSKernelOp<"relu", [HLSKernelOpInterface]> {
  let summary = "relu operation";
  let description = [{
    ReLU activation layer.

    I: (N, C, H, W) or (N, C)
    O: (N, C, H, W) or (N, C)
  }];

  let arguments = (ins
    AnyShaped:$I,
    Optional<AnyMemRef>:$O
  );
  let results = (outs
    Optional<AnyRankedTensor>:$tensorO
  );

  let verifier = [{ return ::verify(*this); }];
}

def MergeOp : HLSKernelOp<"merge", [HLSKernelOpInterface]> {
  let summary = "merge operation";
  let description = [{
    Element-wisely add I0 with I1, where I0 and I1 should have identical shape.

    I0: (N, C, H, W)
    I1: (N, C, H, W)
    O: (N, C, H, W)
  }];

  let arguments = (ins
    AnyShaped:$I0,
    AnyShaped:$I1,
    Optional<AnyMemRef>:$O
  );
  let results = (outs
    Optional<AnyRankedTensor>:$tensorO
  );

  let verifier = [{ return ::verify(*this); }];
}

def CopyOp : HLSKernelOp<"copy", [HLSKernelOpInterface]> {
  let summary = "copy operation";
  let description = [{
    Copy I to O.

    I: (N, C, H, W)
    O: (N, C, H, W)
  }];

  let arguments = (ins
    AnyShaped:$I,
    Optional<AnyMemRef>:$O
  );
  let results = (outs
    Optional<AnyRankedTensor>:$tensorO
  );

  let verifier = [{ return ::verify(*this); }];
}

#endif // SCALEHLS_DIALECT_HLSKERNEL_CNNOPS_TD
