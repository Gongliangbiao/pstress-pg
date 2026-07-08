# PostgreSQL 维护类 Workload 参数说明

本文档记录本次同步到 `codex/pgstress-tool-setup` 分支的稳定功能。此次只同步通用 PostgreSQL 压测操作，不包含本地 `enhance/crash-repro` 分支里的显式崩溃复现、kill backend、并发 DDL flood 等高风险专项向量。

## 变更范围

新增或补齐以下参数，参数值仍沿用 pstress-pg 现有语义：整数值表示该操作在随机 workload 中的权重，设置为 `0` 表示关闭，值越大越容易被选中。

| 参数 | 默认值 | 含义 |
| --- | ---: | --- |
| `--vacuum` | `3` | 对随机表或分区子表执行 `VACUUM`。 |
| `--vacuum-full` | `3` | 对随机表或分区子表执行 `VACUUM FULL`，会触发表重写和较重锁等待。 |
| `--checkpoint` | `1` | 执行 `CHECKPOINT`，用于覆盖 WAL checkpoint 路径。 |
| `--create-index-concurrently` | `5` | 对随机表执行 `CREATE INDEX CONCURRENTLY`。 |
| `--reindex` | `3` | 随机执行 `REINDEX TABLE` 或 `REINDEX INDEX CONCURRENTLY`。 |
| `--cluster-table` | `2` | 使用随机索引执行 `CLUSTER table USING index`。 |
| `--brin-expression-index` | `3` | 随机创建 BRIN、表达式索引或 GIN 索引。 |
| `--create-matview` | `5` | 基于随机表创建物化视图。 |
| `--refresh-matview-concurrently` | `5` | 优先执行 `REFRESH MATERIALIZED VIEW CONCURRENTLY`，失败后降级为普通 refresh。 |
| `--select-matview` | `5` | 随机读取当前 schema 下的物化视图。 |
| `--drop-matview` | `2` | 随机删除当前 schema 下的物化视图。 |
| `--prepared-tx-stress` | `5` | 执行 2PC 流程：`BEGIN` + DML + `PREPARE TRANSACTION` + 随机 `COMMIT PREPARED` 或 `ROLLBACK PREPARED`。 |

## 使用样例

只开启本次新增操作，可以把常规 DML/DDL 权重降到较低或关闭，例如：

```bash
./build/src/pstress-pg \
  --database=postgres \
  --user=gongliangbiao \
  --logdir=/tmp/pstress-pg-maintenance \
  --threads=4 \
  --tables=8 \
  --records=100 \
  --seconds=300 \
  --vacuum=50 \
  --vacuum-full=10 \
  --checkpoint=20 \
  --create-index-concurrently=30 \
  --reindex=30 \
  --cluster-table=10 \
  --brin-expression-index=30 \
  --create-matview=30 \
  --refresh-matview-concurrently=30 \
  --select-matview=30 \
  --drop-matview=10 \
  --prepared-tx-stress=20 \
  --log-all-queries \
  --log-query-duration
```

如果需要关闭某一类操作，直接把对应权重设置为 `0`：

```bash
--checkpoint=0 --prepared-tx-stress=0
```

## 注意事项

`--prepared-tx-stress` 依赖 PostgreSQL 参数 `max_prepared_transactions`。如果该参数为 `0`，`PREPARE TRANSACTION` 会失败，工具会回滚当前事务并继续执行。要实际覆盖 2PC 路径，需要在数据库中设置大于 `0` 的值并重启数据库。

`--checkpoint` 通常要求执行用户具备足够权限。权限不足时 SQL 会失败并记录到日志，但不会影响参数解析。

`REFRESH MATERIALIZED VIEW CONCURRENTLY` 要求物化视图满足 PostgreSQL 对并发刷新的限制，例如需要可用的唯一索引。本实现会先尝试并发 refresh，失败后自动降级为普通 `REFRESH MATERIALIZED VIEW`。

`VACUUM FULL`、`CLUSTER`、`REINDEX` 这类操作可能持有较重锁。当前实现沿用工具已有的 DDL 串行互斥和随机事务前提交逻辑，避免工具内部元数据和数据库状态因为回滚而分叉。

随机脚本 `pstress/randomize_pstress_pg.sh` 已加入这些参数的随机生成逻辑。脚本仍固定使用传入的连接参数和 `--round-seconds`，不会随机化 PostgreSQL 连接端口或单轮运行时长。
