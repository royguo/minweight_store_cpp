# minpatricia Go UT 覆盖矩阵

源码基线：

```text
github.com/JimChengLin/minpatricia
commit 2d1ddf70ce818228302912b81220b634218176b4
```

本文档记录 Go 版 `*_test.go` 的测试/benchmark 在 C++20 版中的对应覆盖点。

## index_test.go

```text
Go 测试                                      C++ 覆盖
------------------------------------------  ----------------------------------------------
TestNodeLayout                              TestNodeLayoutAndDiffs
TestDiffPrefixSemantics                     TestNodeLayoutAndDiffs, golden_diff_cases.tsv
TestHeapNodeStoreReusesFreedNodeIDs         TestHeapStoresAndConstructors
TestHeapRecordStore                         TestHeapStoresAndConstructors
TestNewHeapReturnsOwnedRecordStore          TestHeapStoresAndConstructors
TestDeleteUsesNodeStoreRoot                 TestOpenWithNodesAndNonZeroRoot
TestRootReadErrorIsReturned                 TestRootAndCorruptChildErrors
TestPutGetDeleteVisit                       TestPutGetDeleteVisit
TestPutSkipsNewPositionKey                  TestProbeAndRetarget
TestProbeDoesNotVerifyRecordKey             TestProbeAndRetarget
TestRetargetOnlyWalksIndexNodes             TestProbeAndRetarget
TestRetargetUpdatesBoundaryCaches           TestBoundaryReplaceAndRetarget
TestIteratorAPI                             TestIteratorAPI
TestOpenWithNodesUsesExistingNodes          TestOpenWithNodesAndNonZeroRoot
TestIteratorRangeMultiNode                  TestIteratorRangeMultiNode
TestCartesianRouteAgainstSortedMap          TestCartesianRouteAgainstSortedMap
TestMultiNodeAgainstMap                     TestMultiNodeAgainstMap
TestPutReplaceUpdatesBoundaryCaches         TestBoundaryReplaceAndRetarget
TestDeleteAllMaintainsRoutes                TestDeleteAllMaintainsRoutes
TestDeleteHeavyKeepsLookupConsistent        TestDeleteHeavyKeepsLookupConsistent
```

`AssertIndexMatchesMap` 的 C++ 等价实现会同时验证 `Len`、`Get`、`Probe`、`Ascend`、`Descend` 和 expected map 一致性。C++ 版还额外通过 `AssertAllRoutesValid` 递归校验每个 `NodePage::RouteDiffs()` 与根据 record key 重新计算的 diff 序列一致。

## coverage_test.go

```text
Go 测试                                      C++ 覆盖
------------------------------------------  ----------------------------------------------
TestRecordStoreFuncAndHeapRecordAccessors   TestHeapStoresAndConstructors
TestConstructorsAndPublicErrorPaths         TestErrorPaths, TestRootAndCorruptChildErrors
TestDiffAndRouteErrorPaths                  TestNodeLayoutAndDiffs, TestErrorPaths, golden_route_cases.tsv
TestInternalBuildAndNodePaths               AssertAllRoutesValid, TestRootAndCorruptChildErrors, TestCartesianRouteAgainstSortedMap
TestSplitAndCountInternalPaths              TestCartesianRouteAgainstSortedMap, TestDeleteHeavyKeepsLookupConsistent
TestEmptyIteratorsAndStoreErrorPaths        TestIteratorAPI, TestErrorPaths
TestCorruptOpenAndChildDeletePaths          TestRootAndCorruptChildErrors
TestIterPathOverflowAndCurrentRecordErrors  IterPath is private in C++; equivalent overflow and current-record behavior is exercised through deep multi-node range/seek tests
```

C++ 默认不暴露 Go 版内部 helper，例如 `rebuildNodeWithDiffs`、`insertFullAt`、`currentRecord`。这些内部路径通过 public API、route validation、corrupt node setup 和 large multi-node scenarios 覆盖。

## index_fuzz_test.go

```text
Go 测试                                      C++ 覆盖
------------------------------------------  ----------------------------------------------
FuzzIndexAgainstMap                          TestGoFuzzSeedsAgainstMap, TestDeterministicModelOps
```

C++ 版回放了 Go fuzz 中的三个 seed：

- alpha/bravo 操作序列。
- empty/NUL-byte key 操作序列。
- `fuzzSplitSeed(MaxNodeReps + 1)`。

此外 `TestDeterministicModelOps` 使用固定随机源执行 3000 步 map model 对照，覆盖 put/delete/get/seek/range。

## bench_test.go

```text
Go benchmark                                 C++ 覆盖
------------------------------------------  ----------------------------------------------
BenchmarkGet                                minpatricia_bench: Get/1K, Get/10K, Get/100K
BenchmarkPutReplace                         minpatricia_bench: PutReplace/1K, PutReplace/10K, PutReplace/100K
BenchmarkPutInsert                          minpatricia_bench: PutInsert/1K, PutInsert/10K, PutInsert/100K
BenchmarkVisitFullSetOrdered                minpatricia_bench: VisitFullSetOrdered/1K, VisitFullSetOrdered/10K, VisitFullSetOrdered/100K
BenchmarkVisitFullSetReverse                minpatricia_bench: VisitFullSetReverse/1K, VisitFullSetReverse/10K, VisitFullSetReverse/100K
BenchmarkSeek                               minpatricia_bench: Seek/1K, Seek/10K, Seek/100K
BenchmarkReverseSeek                        minpatricia_bench: ReverseSeek/1K, ReverseSeek/10K, ReverseSeek/100K
BenchmarkDeleteHeavy                        minpatricia_bench: DeleteHeavy/10K, DeleteHeavy/100K
BenchmarkFootprint                          minpatricia_bench: Footprint/1K, Footprint/10K, Footprint/100K
BenchmarkDeleteHeavyFootprint               minpatricia_bench: DeleteHeavyFootprint/10K, DeleteHeavyFootprint/100K
```

当前 Go/C++ 原始 benchmark 输出归档在：

```text
.runtime/benchmarks/2026-06-05/go_minpatricia_bench_large.txt
.runtime/benchmarks/2026-06-05/cpp_minpatricia_bench_large.txt
```

benchmark 使用 `src/minpatricia/testdata/bench_keys_{1K,10K,100K}.tsv`。这些 fixture 由 `gen_bench_fixtures.go` 生成，和 Go benchmark 的 `newBenchData(n)` 使用同源 `math/rand.New(rand.NewSource(int64(n)))` key 序列。

注意：`.runtime/` 是本地归档目录，不进入 git。
