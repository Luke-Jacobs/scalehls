get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)

add_llvm_tool(scalehls-clang
  scalehls-clang.cpp
  )

llvm_update_compile_flags(scalehls-clang)

target_link_libraries(scalehls-clang
  PRIVATE
  ${dialect_libs}

  clangFrontend
  clangTooling
  clangBasic
  )
