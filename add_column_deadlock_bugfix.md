# Add Column Deadlock Bug Fix

## 背景

`pstress-pg` 在 PostgreSQL 随机压测中同时生成 DML、普通 DDL 和事务性 DDL。PostgreSQL 的 DDL 会参与事务，并且 `ALTER TABLE ... ADD COLUMN` 需要获取表级 `AccessExclusiveLock`。工具侧同时维护一份内存表元数据，并通过 `Table::table_mutex` 保护元数据读写。

## Bug 现象

在以下并发场景中，工具可能卡住：

1. 事务 A 执行 DML，获取目标表上的部分行锁，事务暂未提交。
2. 事务 B 执行 `ALTER TABLE ... ADD COLUMN`。
3. PostgreSQL 侧，事务 B 等待目标表的数据库锁。
4. 工具侧，事务 B 同时持有目标表的 `table_mutex`。
5. 事务 A 继续生成同一张表上的 DML，需要读取工具内存元数据，因此等待 `table_mutex`。

最终形成等待环：

```text
事务 A 持有 PostgreSQL 行锁
事务 A 等待工具 table_mutex

事务 B 持有工具 table_mutex
事务 B 等待 PostgreSQL AccessExclusiveLock
```

表现为数据库锁等待和工具元数据锁互相等待，压测进程无法继续推进。

## 根因

普通 `Table::AddColumn()` 路径已经在执行 SQL 前释放了 `table_mutex`，因此它不是主要触发点。

主要风险在事务性 DDL 路径：

- `run_transactional_ddl_block()` 会在事务开始前为目标表加 `target->table_mutex`。
- 该锁会一直持有到事务 DDL 块完成、提交或回滚之后。
- 如果事务 DDL 块里执行 `ALTER TABLE ... ADD COLUMN`，并且该 SQL 在 PostgreSQL 侧等待其他事务的锁，那么工具侧 `table_mutex` 也会被长时间占用。
- 其他线程上的 DML 在继续构造 SQL 时需要读取同一张表的工具元数据，从而等待 `table_mutex`。

本质问题是工具锁和数据库锁存在反向等待顺序：

- DML 事务先持有数据库锁，再请求工具元数据锁。
- DDL 事务先持有工具元数据锁，再请求数据库锁。

## 修复方案

采用最小改动：不重构元数据锁模型，只给 `ADD COLUMN` 的数据库锁等待设置上限。

实现点：

1. 新增固定超时时间：

```cpp
const std::string k_add_column_lock_timeout = "1000ms";
```

2. 普通 `Table::AddColumn()` 改为通过 `execute_sql_with_lock_timeout()` 执行 `ALTER TABLE ... ADD COLUMN`。

3. `execute_sql_with_lock_timeout()` 在执行目标 SQL 前设置：

```sql
SET lock_timeout = '1000ms'
```

执行后再恢复：

```sql
RESET lock_timeout
```

并且恢复 `thd->success` 为目标 SQL 的真实结果，避免 `RESET lock_timeout` 成功覆盖 `ALTER TABLE` 失败状态。

4. 事务性 DDL 路径在 `START TRANSACTION` 后设置：

```sql
SET LOCAL lock_timeout = '1000ms'
```

`SET LOCAL` 只在当前事务内生效，提交或回滚后自动恢复，不影响后续 workload。

## 修复后的行为

当 `ALTER TABLE ... ADD COLUMN` 因其他事务持锁无法及时获取 `AccessExclusiveLock` 时：

- PostgreSQL 在约 1 秒后返回 lock timeout 错误。
- 普通 `AddColumn()` 不会更新工具内存元数据，并释放临时创建的 `Column` 对象。
- 事务性 DDL 块走已有失败处理逻辑，执行回滚并恢复事务前的工具元数据快照。
- 工具不会长期持有 `table_mutex` 等数据库锁，从而打破死锁等待环。

## 验证

已执行：

```bash
cmake --build build
```

构建通过。

本次还用静态回归检查覆盖了以下点：

- 存在 `k_add_column_lock_timeout`。
- 普通 `Table::AddColumn()` 使用 `execute_sql_with_lock_timeout(sql, thd)`。
- 事务性 DDL 中存在 `SET LOCAL lock_timeout`。
- `execute_sql_with_lock_timeout()` 会恢复 `thd->success` 为目标 SQL 的执行结果。

## 影响范围

- 主要影响 `ALTER TABLE ... ADD COLUMN`。
- 不改变随机 DML 的生成逻辑。
- 不改变元数据快照/回滚模型。
- 不新增用户参数，避免扩大配置面。

## 后续可选改进

- 将 `1000ms` 做成命令行参数，例如 `--add-column-lock-timeout-ms`。
- 为其他高风险 DDL 增加类似的 lock timeout。
- 长期上可以重构元数据锁策略，避免工具锁跨数据库事务持有。
