# pstress-pg Metadata 和 Step 文件说明

本文档记录 `pstress-pg` 什么时候写元数据文件、`--step` 如何控制元数据加载和保存，以及如何使用上一轮 run 的元数据继续下一轮 run。

## 元数据文件名

`pstress-pg` 会把进程内存里的表元数据保存成 JSON 文件，文件名格式是：

```text
step_<step>.dll
```

例如：

```text
step_1.dll
step_2.dll
```

注意：当前代码里后缀是 `.dll`，不是 `.ddl`。

## 元数据写入目录

元数据文件的写入目录按下面的优先级选择：

1. 如果设置了 `--metadata-path`，写到 `--metadata-path` 指定的目录。
2. 如果没有设置 `--metadata-path`，写到 `--logdir` 指定的目录。

例如：

```bash
./build/src/pstress-pg \
  --database=postgres \
  --user=gongliangbiao \
  --logdir=/tmp/pstress-run1 \
  --metadata-path=/tmp/pstress-meta \
  --step=1 \
  --seconds=60
```

会写入：

```text
/tmp/pstress-meta/step_1.dll
```

如果不传 `--metadata-path`，则会写入：

```text
/tmp/pstress-run1/step_1.dll
```

## 什么时候会写元数据文件

当前 `pstress-pg` 的元数据只在进程正常收尾时写一次。主流程大致是：

```text
启动 worker
等待所有 worker 结束
save_metadata_to_file()
clean_up_at_end()
打印 COMPLETED
```

也就是说，只有进程正常走到收尾逻辑时，才会执行 `save_metadata_to_file()`。

正常写入时，控制台会看到类似输出：

```text
Saving metadata to file /tmp/pstress-meta/step_1.dll
COMPLETED
```

## 哪些场景不会写元数据文件

下面这些场景不会生成当前 step 的元数据文件：

- 进程没有正常结束，例如被 `kill`、Ctrl-C、系统终止、nohup 外部清理等。
- 进程崩溃，例如异常退出、段错误、abort。
- 连接数据库失败。`tryConnect()` 失败时会直接 `exit(EXIT_FAILURE)`，不会走到保存元数据逻辑。
- 启动连接阶段数据库已经挂掉或不可连接。这属于连接数据库失败，不会写当前 step 文件。
- 使用 `--test-connection`。连接成功后会直接 `exit(EXIT_SUCCESS)`，不会保存元数据。
- 参数错误、非法参数、配置文件解析失败等早期退出。
- 使用 `--help` 或帮助类参数，打印帮助后直接退出。
- 进程卡住，没有走到 `COMPLETED`。例如卡在等待所有表初始化完成时，不会写当前 step 文件。
- `--metadata-path` 指定的目录不存在或不可写。当前代码不会自动创建 `--metadata-path`，写入失败会抛异常。
- 多轮都使用同一个目录且 `--step` 不变。严格说这不是“不写”，而是反复覆盖同一个 `step_1.dll`，容易误以为没有新文件。

判断是否真正写入过，最直接的依据是日志或控制台是否出现：

```text
Saving metadata to file ...
COMPLETED
```

如果没有看到 `COMPLETED`，通常不要预期当前 step 文件已经生成。

## 数据库挂掉时是否会写元数据

数据库挂掉要分两个阶段看：

### 启动连接阶段数据库不可用

如果 `pstress-pg` 启动时数据库已经不可连接，连接检查会直接失败并退出：

```text
tryConnect()
exit(EXIT_FAILURE)
```

这种场景不会走到 `save_metadata_to_file()`，因此不会写 `step_N.dll`。

### 压测运行过程中数据库挂掉

如果数据库是在 workload 已经开始之后挂掉，SQL 执行失败时 `execute_sql()` 会检查连接状态和 SQLSTATE。如果判断为连接丢失，会设置：

```text
run_query_failed = true
```

之后 workload loop 会退出，worker 线程结束，主流程在 join 完 worker 后仍然会进入正常收尾路径：

```text
save_metadata_to_file()
clean_up_at_end()
COMPLETED
```

所以这种“运行中数据库挂掉，但 `pstress-pg` 自己正常感知并收尾退出”的场景，当前逻辑通常会写 `step_N.dll`。

需要注意的是，这个 `step_N.dll` 只代表 `pstress-pg` 退出时内存里的工具元数据快照，不保证数据库恢复后的真实 schema 一定和 metadata 完全一致。数据库崩溃、恢复、最后几条 DDL/DML 失败或事务回滚，都可能导致数据库真实状态和工具元数据存在差异。

简化判断：

```text
启动前数据库不可连接 -> 不写 dll
运行中数据库挂掉，pstress-pg 正常收尾 -> 通常会写 dll
运行中数据库挂掉，同时 pstress-pg 被 kill/崩溃/卡死 -> 不写 dll
```

## Prepare 模式

`--prepare` 会生成新的随机元数据，创建表并插入初始数据，然后通过正常收尾路径退出。因此 `--prepare` 正常结束时会写 metadata 文件。

示例：

```bash
rm -rf /tmp/pstress-meta /tmp/pstress-prepare
mkdir -p /tmp/pstress-meta /tmp/pstress-prepare

./build/src/pstress-pg \
  --database=postgres \
  --user=gongliangbiao \
  --logdir=/tmp/pstress-prepare \
  --metadata-path=/tmp/pstress-meta \
  --threads=1 \
  --tables=2 \
  --records=1 \
  --seconds=60 \
  --step=1 \
  --prepare \
  --log-all-queries \
  --log-query-duration
```

预期生成：

```text
/tmp/pstress-meta/step_1.dll
```

## 普通 Run 模式

普通 run 正常结束时也会写 metadata 文件。

示例：

```bash
rm -rf /tmp/pstress-meta /tmp/pstress-run1
mkdir -p /tmp/pstress-meta /tmp/pstress-run1

./build/src/pstress-pg \
  --database=postgres \
  --user=gongliangbiao \
  --logdir=/tmp/pstress-run1 \
  --metadata-path=/tmp/pstress-meta \
  --threads=1 \
  --tables=2 \
  --records=1 \
  --seconds=60 \
  --step=1 \
  --log-all-queries \
  --log-query-duration
```

预期生成：

```text
/tmp/pstress-meta/step_1.dll
```

如果不传 `--metadata-path`，则预期生成：

```text
/tmp/pstress-run1/step_1.dll
```

## 使用上一轮 Run 的元数据接力

如果想让第二轮 run 读取第一轮 run 的元数据，需要复用同一个 `--metadata-path`，并把 `--step` 加 1。

当 `--step=2` 且没有设置 `--prepare` 时，`pstress-pg` 启动时会读取：

```text
<metadata-path>/step_1.dll
```

第二轮正常结束时会写入：

```text
<metadata-path>/step_2.dll
```

第一轮 run：

```bash
rm -rf /tmp/pstress-meta /tmp/pstress-run1
mkdir -p /tmp/pstress-meta /tmp/pstress-run1

./build/src/pstress-pg \
  --database=postgres \
  --user=gongliangbiao \
  --logdir=/tmp/pstress-run1 \
  --metadata-path=/tmp/pstress-meta \
  --threads=1 \
  --tables=2 \
  --records=1 \
  --seconds=60 \
  --step=1 \
  --log-all-queries \
  --log-query-duration
```

第二轮接力 run：

```bash
mkdir -p /tmp/pstress-run2

./build/src/pstress-pg \
  --database=postgres \
  --user=gongliangbiao \
  --logdir=/tmp/pstress-run2 \
  --metadata-path=/tmp/pstress-meta \
  --threads=1 \
  --tables=2 \
  --records=1 \
  --seconds=60 \
  --step=2 \
  --log-all-queries \
  --log-query-duration
```

第二轮会读取：

```text
/tmp/pstress-meta/step_1.dll
```

并在正常结束后写入：

```text
/tmp/pstress-meta/step_2.dll
```

## 接力 Run 的注意事项

- `step > 1` 且没有 `--prepare` 时，会读取上一个 step 的 metadata 文件。
- `step=2` 读取 `step_1.dll`。
- `step=3` 读取 `step_2.dll`。
- 第二轮 run 不会重新创建第一轮的表，只会加载上一轮工具元数据并继续压测。
- 数据库里的 `pstress` schema 和表必须还保持上一轮结束时的状态。
- 不要在接力 run 中间 drop schema、清库、换库，或者手动大幅修改表结构，否则 metadata 和数据库真实状态会不一致。
- metadata 文件写入成功只代表进程走到了正常收尾，不代表本轮所有 SQL 都成功。

## 排查命令

查找某个目录下的 step 文件：

```bash
find /tmp/pstress-meta -name 'step_*.dll' -print
```

如果不确定文件写到了 `--metadata-path` 还是 `--logdir`，可以两个目录都查：

```bash
find /tmp/pstress-meta /tmp/pstress-run1 -name 'step_*.dll' -print
```

确认某次 run 是否真正走到元数据保存：

```bash
grep -R "Saving metadata to file\\|COMPLETED" /tmp/pstress-run1
```

## 源码位置

- 元数据保存函数：`src/random_test.cpp`，`save_metadata_to_file()`。
- 元数据加载函数：`src/random_test.cpp`，`load_metadata_from_file()`。
- 主流程保存调用：`src/pstress.cpp`，worker/node 完成之后。
- `--step` 参数定义：`src/help.cpp`。
- `--metadata-path` 参数定义：`src/help.cpp`。
