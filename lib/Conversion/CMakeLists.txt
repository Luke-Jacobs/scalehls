file(GLOB globbed *.cpp)

add_mlir_library(MLIRScaleHLSConversion
  ${globbed}

  DEPENDS
  MLIRScaleHLSConversionIncGen
  )
