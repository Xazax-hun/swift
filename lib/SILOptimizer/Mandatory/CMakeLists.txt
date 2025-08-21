set(LIFETIMES_FILE "${CMAKE_CURRENT_BINARY_DIR}/Lifetimes.cpp")
set(SOUFFLE_BIN "${CMAKE_BINARY_DIR}/bin/souffle")

file(TOUCH ${LIFETIMES_FILE})
add_custom_command(
  OUTPUT ${LIFETIMES_FILE}
  COMMAND ${SOUFFLE_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/Lifetimes.dl
          -g ${LIFETIMES_FILE}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Lifetimes.dl)

set_source_files_properties(${LIFETIMES_FILE} PROPERTIES GENERATED TRUE COMPILE_FLAGS "-frtti -fexceptions")
add_custom_target(generate_datalog DEPENDS "${LIFETIMES_FILE}")
add_swift_host_library(datalogLib OBJECT)
set_swift_llvm_is_available(datalogLib)
add_dependencies(datalogLib generate_datalog)
target_compile_options(datalogLib PRIVATE -frtti -fexceptions)
target_include_directories(datalogLib PRIVATE 
  ${souffle_SOURCE_DIR}/src/include/
)
target_sources(datalogLib PRIVATE ${LIFETIMES_FILE})
target_compile_definitions(datalogLib PRIVATE "__EMBEDDED_SOUFFLE__")

# Link the object library to your main target
target_link_libraries(swiftSILOptimizer PUBLIC datalogLib)

target_sources(swiftSILOptimizer PRIVATE
  AccessEnforcementSelection.cpp
  AccessMarkerElimination.cpp
  AlwaysEmitConformanceMetadataPreservation.cpp
  AddressLowering.cpp
  CapturePromotion.cpp
  ClosureLifetimeFixup.cpp
  ConsumeOperatorCopyableAddressesChecker.cpp
  ConsumeOperatorCopyableValuesChecker.cpp
  PhiStorageOptimizer.cpp
  ConstantPropagation.cpp
  DiagnoseUnnecessaryPreconcurrencyImports.cpp
  DatalogDiagnostics.cpp
  DebugInfoCanonicalizer.cpp
  DefiniteInitialization.cpp
  DIMemoryUseCollector.cpp
  DataflowDiagnostics.cpp
  DiagnoseInvalidEscapingCaptures.cpp
  DiagnoseLifetimeIssues.cpp
  DiagnoseStaticExclusivity.cpp
  DiagnoseUnreachable.cpp
  Differentiation.cpp
  FlowIsolation.cpp
  IRGenPrepare.cpp
  LexicalLifetimeEliminator.cpp
  LowerHopToActor.cpp
  MandatoryInlining.cpp
  MoveOnlyAddressCheckerTester.cpp
  MoveOnlyAddressCheckerUtils.cpp
  MoveOnlyBorrowToDestructureTester.cpp
  MoveOnlyBorrowToDestructureUtils.cpp
  MoveOnlyChecker.cpp
  MoveOnlyDiagnostics.cpp
  MoveOnlyObjectCheckerTester.cpp
  MoveOnlyObjectCheckerUtils.cpp
  MoveOnlyTempAllocationFromLetTester.cpp
  MoveOnlyTypeUtils.cpp
  MoveOnlyUtils.cpp
  MovedAsyncVarDebugInfoPropagator.cpp
  NestedSemanticFunctionCheck.cpp
  OptimizeHopToExecutor.cpp
  PerformanceDiagnostics.cpp
  PredictableMemOpt.cpp
  PMOMemoryUseCollector.cpp
  RawSILInstLowering.cpp
  ReferenceBindingTransform.cpp
  ThunkLowering.cpp
  SendNonSendable.cpp
  LowerTupleAddrConstructor.cpp
  SILGenCleanup.cpp
  YieldOnceCheck.cpp
  OSLogOptimization.cpp
  MoveOnlyWrappedTypeEliminator.cpp
  RegionAnalysisInvalidationTransform.cpp
  DiagnosticDeadFunctionElimination.cpp
  OwnershipModelEliminator.cpp)

target_include_directories(swiftSILOptimizer PRIVATE 
  ${souffle_SOURCE_DIR}/src/include/
)
