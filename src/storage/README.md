# storage

本目录实现 C++ 版 KV engine。

职责边界：

- store public API：`Put`、`Get`、`Delete`、`Len`、`SeekGE`、`SeekLE`、`Scan`、`ScanRange`、`ReverseScan`、`ReverseScanRange`。
- WAL record encoding、CRC、segment rollover 和 replay policy。
- manifest append log、compact、startup validation。
- mmap node store、primary index、secondary checkpoint index。
- SST layer、flush、recovery、minor compaction 和 major compaction。

读取路径必须始终通过 primary index。flush 与 recovery 只同步 checkpoint state，不应把读路径切换到 checkpoint index。
