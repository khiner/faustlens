add_executable(faustlens_cli Compile.cpp)
set_target_properties(faustlens_cli PROPERTIES OUTPUT_NAME faustlens-compile EXCLUDE_FROM_ALL "$<NOT:$<BOOL:${FAUSTLENS_CLI}>>")
target_link_libraries(faustlens_cli PRIVATE faustlens_compiler)
