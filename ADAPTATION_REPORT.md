# pstress-pg 适配进展报告

## 概述

本报告对比原始MySQL版本 pstress (https://github.com/Percona-QA/pstress) 与适配后的PostgreSQL版本 pstress-pg (https://github.com/Gongliangbiao/pstress-pg) 的功能差异。

---

## ✅ 已成功迁移的核心功能

### DDL操作（全部保留）

- ✅ create/drop table
- ✅ add/drop/rename column
- ✅ add/drop/rename index
- ✅ modify column
- ✅ analyze table
- ✅ check table（适配为PG轻量级检查）
- ✅ optimize table（映射为VACUUM ANALYZE）
- ✅ truncate table
- ✅ recreate table（drop+create）
- ✅ 分区表操作（RANGE, LIST, HASH, KEY）

### DML操作（全部保留）

- ✅ SELECT (select-all-rows, select-single-row)
- ✅ INSERT
- ✅ UPDATE
- ✅ DELETE (delete-all-rows, delete-with-cond)

### 表类型（全部保留）

- ✅ 普通表 (NORMAL)
- ✅ 分区表 (PARTITION)
- ✅ 临时表 (TEMPORARY)
- ✅ 外键表 (FK)

### 事务功能（全部保留+增强）

- ✅ commit/rollback
- ✅ savepoint
- ✅ 事务概率控制 (trx-prob-k, trx-size, commit-prob)
- ✅ **新增**：事务性DDL（PG支持DDL在事务中）

### 索引/约束（全部保留）

- ✅ 主键
- ✅ 外键约束
- ✅ 普通索引
- ✅ 降序索引（desc index）

---

## ✅ 新增功能（PG特有优势）

| 新增选项 | 说明 |
|----------|------|
| SELECT_WITH_JOIN | JOIN查询支持 |
| SELECT_WITH_CTE | CTE (WITH查询)支持 |
| TRX_DDL_PROB_K | DDL事务化概率 |
| TRX_DDL_SIZE | DDL事务大小 |

---

## ✅ 数据类型大幅扩展

| 类别 | MySQL版本 | PG版本 |
|------|----------|--------|
| 整数 | INTEGER, INT | SMALLINT, INTEGER, INT, BIGINT |
| 浮点 | FLOAT, DOUBLE | FLOAT, DOUBLE, NUMERIC |
| 字符 | CHAR, VARCHAR, BLOB | CHAR, VARCHAR, BYTEA, BLOB, JSON, JSONB |
| 时间 | 无 | DATE, TIME, TIMETZ, TIMESTAMP, TIMESTAMPTZ, INTERVAL |
| 网络 | 无 | INET, CIDR, MACADDR, MACADDR8 |
| 几何 | 无 | POINT, LINE, LSEG, BOX, PATH, POLYGON, CIRCLE |
| 数组 | 无 | INTARRAY, BIGINTARRAY, TEXTARRAY, BOOLARRAY等 |
| 范围 | 无 | INT4RANGE, INT8RANGE, NUMRANGE, TSRANGE等 |
| 其他 | 无 | BIT, VARBIT, MONEY, XML, TSVECTOR, UUID |

---

## ✅ 合理移除的MySQL特有功能

以下功能因PG不支持或机制不同而移除：

| 移除功能 | 原因 |
|----------|------|
| ENGINE选项 | PG无多存储引擎概念 |
| Undo Tablespace (undo-tbs-count, undo-tbs-sql) | PG使用不同MVCC机制 |
| General Tablespace数量 (tbs-count) | PG的tablespace概念不同 |
| 表压缩/列压缩 (no-table-compression, no-column-compression, alter-table-compress) | MySQL特有InnoDB压缩 |
| 表加密 (no-encryption, encryption-type, alter-table-encrypt) | PG无内置表加密 |
| Keyring组件 (reload-keyring) | MySQL特有 |
| Redo Log控制 (alter-redo-log, rotate-redo-log-key) | PG使用WAL机制，不能禁用 |
| GCache Master Key (rotate-gcache-key) | Galera/PXC特有 |
| Alter Engine (alter-table-engine) | PG无多引擎切换 |
| Discard/Import Tablespace (alt-discard-tbs) | MySQL特有 |
| Row Format (row-format) | PG无行格式概念 |
| 服务器选项文件 (--mso, --sof) | MySQL特有动态变量设置 |
| Alter算法/锁参数 (alter-algorithm, alter-lock) | PG忽略，内部自动选择 |
| 服务器变量动态设置 (set-variable) | PG语法不同 |
| Tablespace加密/重命名/数据库加密 | MySQL特有 |

---

## ✅ 构建系统适配

| 文件 | 变化 |
|------|------|
| CMakeLists.txt | MySQL → PostgreSQL |
| cmake/PQFindPostgreSQL.cmake | 新增，替代PQFindMySQLFork.cmake |
| src/CMakeLists.txt | 链接库从mysqlclient改为libpq |
| 所有源文件 | 头文件从mysql.h改为libpq-fe.h |

---

## ⚠️ 发现的遗留问题

**common.hpp** 中的 `Option::Opt` enum 仍保留了MySQL特有选项的定义，但help.cpp中没有注册这些选项。这些是死代码，建议清理：

```cpp
NUMBER_OF_GENERAL_TABLESPACE
NUMBER_OF_UNDO_TABLESPACE
UNDO_SQL
ENGINE
NO_ENCRYPTION
ENCRYPTION_TYPE
NO_COLUMN_COMPRESSION
NO_TABLE_COMPRESSION
NO_TABLESPACE
ALTER_TABLE_ENCRYPTION
ALTER_DISCARD_TABLESPACE
ALTER_ENGINE
ALTER_TABLE_COMPRESSION
ALTER_INSTANCE_RELOAD_KEYRING
ROW_FORMAT
SERVER_OPTION_FILE
SET_GLOBAL_VARIABLE
ALTER_MASTER_KEY
ALTER_ENCRYPTION_KEY
ALTER_GCACHE_MASTER_KEY
ALTER_REDO_LOGGING
ROTATE_REDO_LOG_KEY
ALTER_TABLESPACE_ENCRYPTION
ALTER_TABLESPACE_RENAME
ALTER_DATABASE_ENCRYPTION
MYSQLD_SERVER_OPTION
```

建议从common.hpp中删除这些enum值以达到100%完成度。

---

## 📊 适配完成度评估

| 方面 | 完成度 |
|------|--------|
| 核心DDL/DML功能 | **100%** |
| 表类型支持 | **100%** |
| 事务功能 | **100%+增强** |
| 数据类型支持 | **大幅增强** |
| 构建系统 | **100%** |
| 代码清理 | **95%**（有enum遗留） |

---

## 结论

适配工作已基本完成，核心功能完全迁移，并利用PG特性进行了增强（更多数据类型、JOIN/CTE查询、事务性DDL）。建议清理common.hpp中的遗留enum定义以达到100%完成度。
