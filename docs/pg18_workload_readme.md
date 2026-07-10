# PostgreSQL 18 Workload README

本文档记录 `codex/pg18-workload` 分支针对 PostgreSQL 18 增加和强化的压测能力，以及建议的本地验证方式。

## 目标

本轮适配的目标是让 `pstress-pg` 能覆盖 PostgreSQL 18 的新 SQL 能力，同时保持参数化、可关闭、失败后可继续运行，避免某个特性不满足环境条件时导致整个工具卡死。

## 新增和强化的参数

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| `--generated-column-kind` | `random` | 生成列类型，支持 `random`、`virtual`、`stored`；`virtual` 需要 PostgreSQL 18+。 |
| `--pg18-vacuum-analyze-only` | `5` | 对 `VACUUM` 或 `ANALYZE` 随机添加 PostgreSQL 18 的 `ONLY` 语义。 |
| `--pg18-not-null-constraint` | `5` | 覆盖 PostgreSQL 18 命名 `NOT NULL` 约束的 DDL。 |
| `--pg18-temporal-constraints` | `5` | 覆盖 temporal `WITHOUT OVERLAPS` / `PERIOD` 相关 DDL。 |
| `--pg18-partition-fk-not-valid` | `5` | 覆盖分区表 `FOREIGN KEY ... NOT VALID` 场景。 |
| `--returning-old-new` | `5` | 在 DML 中随机加入 PostgreSQL 18 `RETURNING old/new`。 |
| `--pg18-merge` | `5` | 覆盖 PostgreSQL 18 `MERGE`，包含 `RETURNING old/new`。 |
| `--pg18-partition-ops` | `5` | 覆盖分区表相关的 PostgreSQL 18 操作。 |
| `--pg18-copy` | `5` | 覆盖 PostgreSQL 18 `COPY` 能力。 |
| `--pg18-copy-mode` | `random` | `COPY` 模式，支持 `random`、`from-stdin`、`to-stdout`、`query-to-stdout`、`matview-to-stdout`。 |
| `--pg18-copy-reject-limit` | `10` | `COPY FROM` 使用 `ON_ERROR ignore` 时的 `REJECT_LIMIT`。 |
| `--pg18-copy-log-verbosity` | `random` | `COPY` 日志详细度，支持 `random`、`default`、`verbose`、`silent`。 |
| `--pg18-explain` | `5` | 覆盖 PostgreSQL 18 `EXPLAIN` 选项，如 `MEMORY`、`SERIALIZE`、`WAL`。 |
| `--pg18-functions` | `5` | 覆盖 PostgreSQL 18 新增或增强的内置函数、统计函数和系统视图。 |
| `--gist-index` | `3` | 对适合 GiST 的列生成 `USING gist` 索引。 |

已有维护类参数也参与本轮测试组合：

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| `--vacuum` | `3` | 对普通表或分区子表执行 `VACUUM`。 |
| `--vacuum-full` | `3` | 对普通表或分区子表执行 `VACUUM FULL`。 |
| `--checkpoint` | `1` | 执行 `CHECKPOINT`，要求连接用户具备相应权限。 |
| `--create-matview` | `5` | 基于随机表创建物化视图。 |
| `--refresh-matview-concurrently` | `5` | 刷新物化视图，必要时回退到非 concurrently。 |
| `--select-matview` | `5` | 查询物化视图。 |
| `--drop-matview` | `2` | 删除随机物化视图。 |
| `--prepared-tx-stress` | `5` | 覆盖 2PC：`BEGIN`、DML、`PREPARE TRANSACTION`，随后随机 `COMMIT PREPARED` 或 `ROLLBACK PREPARED`。 |

## 覆盖点说明

- **虚拟生成列**：默认 `random`，会在 PostgreSQL 18+ 上随机生成 `virtual` 或 `stored`；低版本会自动关闭 virtual 路径。
- **NOT NULL 约束**：覆盖 PostgreSQL 18 支持的命名 NOT NULL 约束 DDL。
- **Temporal 约束**：覆盖 `WITHOUT OVERLAPS` 和 `PERIOD` 相关语法，受表结构和索引条件限制时会跳过或继续。
- **分区外键 NOT VALID**：在 `run_some_query` 阶段按概率触发，不在建表前置阶段触发；目标是覆盖分区表外键的延迟校验路径。
- **MERGE / RETURNING old-new**：覆盖 PostgreSQL 18 DML 返回 old/new 行版本的路径。
- **COPY**：同时覆盖 libpq copy 协议和 SQL `COPY TO STDOUT` 变体，支持物化视图 `COPY`。
- **EXPLAIN**：覆盖 PostgreSQL 18 新增 explain 选项组合。
- **系统函数**：`--pg18-functions` 覆盖 PostgreSQL 18 新函数；`grammar.sql` 额外增加了跨版本可用的系统函数和系统视图查询。
- **GiST 索引**：只在适合 GiST opclass 的类型上尝试建索引，避免对任意列硬建导致高频语法失败。

## 环境注意事项

- 2PC 测试依赖 `max_prepared_transactions > 0`。如果数据库参数为 `0`，`PREPARE TRANSACTION` 会失败，工具应记录错误并继续运行，但不会实际覆盖 prepared transaction 成功路径。
- `CHECKPOINT` 通常要求超级用户或具备足够权限；权限不足时会表现为 SQL 执行失败，不应导致工具退出或卡死。
- PG18 专属 workload 都有服务端版本门禁；连接到低版本 PostgreSQL 时，对应概率会被置为 `0`。
- `COPY FROM ... ON_ERROR ignore` 的效果会受到输入数据、表结构和 PostgreSQL 版本支持情况影响。

## 推荐验证命令

建议每轮使用独立 `logdir`，并打开完整 SQL 日志和耗时日志：

```bash
./build/src/pstress-pg \
  --address=127.0.0.1 \
  --port=5433 \
  --user=$(whoami) \
  --database=postgres \
  --threads=6 \
  --tables=8 \
  --columns=8 \
  --records=80 \
  --seconds=600 \
  --logdir=./log/pg18_round_1 \
  --log-all-queries \
  --log-query-duration \
  --log-query-numbers \
  --pg18-not-null-constraint=10 \
  --pg18-temporal-constraints=10 \
  --pg18-partition-fk-not-valid=10 \
  --returning-old-new=20 \
  --pg18-merge=10 \
  --pg18-partition-ops=10 \
  --pg18-copy=10 \
  --pg18-explain=10 \
  --pg18-functions=10 \
  --gist-index=8 \
  --vacuum=6 \
  --vacuum-full=2 \
  --checkpoint=2 \
  --create-matview=8 \
  --refresh-matview-concurrently=8 \
  --select-matview=8 \
  --drop-matview=3 \
  --prepared-tx-stress=8
```

## 卡死判断方法

本轮关注的卡死定义是：`pstress-pg` 进程和数据库会话仍存在，但线程不再向 PostgreSQL 发送 SQL。

验证时可以同时观察：

- `pstress-pg` 是否在 `--seconds` 到期后自行退出。
- `logdir` 下 `*_thread-*.sql` 文件是否持续增加。
- 数据库侧 `pg_stat_activity` 中该工具连接的 `query_start`、`state_change` 是否长期无变化。
- `run.log` 是否停在等待建表、等待线程结束、等待元数据或连接异常处理路径。

如果进程在 `--seconds` 到期后正常退出，且线程 SQL 日志在测试期间持续更新，则不符合上述卡死现象。
