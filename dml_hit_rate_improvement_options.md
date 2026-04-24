# DML Hit Rate Improvement Options

## Background

当前 `pstress-pg` 的不少 `UPDATE`、`DELETE`、`SELECT ... WHERE` 实际命中率较低。
根因是 `WHERE` 条件大多基于完全随机生成的值，而不是基于当前表中真实存在的数据。
这会导致很多 SQL 虽然执行成功，但只是在空跑，无法充分覆盖真实行上的锁竞争、索引访问、
行更新、行删除等场景。

本轮目标不是直接改代码，而是先给出一组可选方案，并明确推荐方案，作为后续实现依据。

## Goals

- 提高 `WHERE` 条件命中真实数据行的概率
- 尽量保留随机压测的特征，不把工具完全变成回放器
- 控制实现复杂度，避免大幅破坏现有 DML 生成框架
- 保持较好的吞吐，避免每条 DML 都引入额外查询开销

## Candidate Options

### Option 1: recent successful row cache

为每张表维护一个轻量级真实值缓存，记录最近成功命中的主键、唯一键、FK 值以及少量普通列值。
后续 `UPDATE`、`DELETE`、`SELECT ... WHERE` 优先从缓存中抽取真实值构造谓词。

特点：

- 命中率提升明显
- 不需要每次额外发一条预查询
- 对现有框架侵入适中
- 缓存可能包含已删除值，但这类误差可接受

### Option 2: known primary-key pool

按表维护一个“已知存在的主键集合”，后续 DML 优先按主键命中。
插入成功后加入集合，删除成功后从集合中移除。

特点：

- 实现简单
- 命中率较高
- 对 `UPDATE/DELETE` 很有效
- 会让 workload 更偏主键访问，对非主键列覆盖不足

### Option 3: distribution-aware random values

不直接复用真实行，而是让随机值更贴近现有数据分布。
例如整数在已有区间内生成，时间在已有时间窗口内生成，字符串优先复用有限候选池。

特点：

- 保留随机性更好
- 不像回放
- 需要为不同类型维护分布信息
- 实现复杂度明显更高

### Option 4: hit-oriented WHERE plus random SET

将“命中行”和“修改内容”解耦。
`WHERE` 尽量命中真实数据行，`SET` 部分仍保持完全随机。

特点：

- 很适合 `UPDATE`
- 能提升有效写入比例
- 对 `DELETE/SELECT` 帮助有限
- 仍然需要一个真实值来源，通常要结合 Option 1 或 Option 2

### Option 5: mixed workload ratio

引入双模式混合策略：
一部分 DML 使用高命中真实值，另一部分 DML 继续保持完全随机。
例如 `70%` 命中型，`30%` 探索型。

特点：

- 平衡性最好
- 能同时保留命中型场景和空命中/边界场景
- 适合作为全局控制策略
- 需要依赖一种真实值来源机制

### Option 6: pre-select then DML

先查一条真实记录，再基于返回结果构造后续 DML。
这是目前已经考虑过的方法。

特点：

- 命中率最高
- 逻辑最直观
- 每次 DML 可能变成两条 SQL
- 会改变吞吐与锁竞争模型，更像“查询驱动回放”

## Comparison

| Option | Core idea | Hit-rate improvement | Implementation difficulty | Code change scope | Runtime overhead | Workload realism | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Option 1 | 维护最近成功行缓存 | High | Medium | Medium | Low | Good | 推荐作为真实值来源 |
| Option 2 | 维护主键池 | Medium-High | Low | Small | Low | Medium | 更偏主键路径 |
| Option 3 | 按数据分布生成随机值 | Medium | High | Large | Low-Medium | Very good | 设计和维护成本最高 |
| Option 4 | `WHERE` 命中，`SET` 随机 | High | Medium | Medium | Low | Good | 需要结合 1/2 使用 |
| Option 5 | 命中型与随机型混合 | High | Medium | Medium | Low | Very good | 推荐作为全局调度策略 |
| Option 6 | 先查再改 | Very high | Low-Medium | Small-Medium | High | Medium-Low | 命中率高，但吞吐模型变化大 |

## Recommended Choice

**最终推荐方案：Option 1 + Option 5**

即：

- 用 `Option 1` 作为真实值来源
- 用 `Option 5` 控制命中型与随机型 DML 的混合比例

推荐理由：

- 相比 `Option 6`，不需要给每条 DML 增加一次额外查询，运行成本更低
- 相比 `Option 2`，不只依赖主键，未来可以逐步扩展到唯一键、FK、部分普通列
- 相比 `Option 3`，实现复杂度更可控，收益来得更快
- 相比只做 `Option 1`，加入 `Option 5` 后可以避免 workload 变成“几乎全命中”的单一模式
- 能在提高命中率的同时，继续保留一部分随机探索能力

## Proposed Behavior

推荐的初始行为：

- 每张表维护一个小型 ring buffer 或 bounded cache
- 缓存内容优先记录：
  - 主键值
  - FK 列值
  - 唯一键值
  - 少量简单标量列值
- `UPDATE/DELETE/SELECT ... WHERE` 默认优先使用真实值缓存
- 同时保留一部分纯随机谓词路径

建议初始比例：

- `70%` 命中型 DML
- `30%` 随机型 DML

建议初始命中列优先级：

1. `PK`
2. `FK / referenced unique key`
3. 普通标量列

## Non-goals For First Iteration

第一轮不建议立刻支持以下内容：

- `JSON`、`JSONB`、数组、range、几何类型的命中型复杂谓词
- 基于真实行的复杂多列组合谓词
- 每条 DML 之前都做实时查询
- 分布建模和直方图式随机生成

## Suggested Next Step

后续实现时，建议先做一个最小可用版本：

- 为每表引入轻量真实值缓存
- 先只支持 `PK / FK / unique / simple scalar` 的命中型谓词
- 新增一个全局概率参数，用于控制命中型 DML 比例
- 先在 `UPDATE`、`DELETE`、`SELECT ... WHERE` 上落地
- 实现后对比：
  - 命中真实行比例
  - `rows affected`
  - `SELECT` 非空结果比例
  - 总吞吐变化

这版如果效果稳定，再考虑把命中型支持扩展到更复杂的类型和谓词。

## Implementation Status

2026-04-23 已完成第一版落地，当前实现状态如下：

- 已实现：
  - 每表轻量真实值缓存
  - 成功 `bulk insert` / `random insert` 后回填缓存
  - `UPDATE / DELETE / SELECT ... WHERE` 的 `70%` 命中型 +
    `30%` 随机型混合策略
  - 命中列优先级：`PK` -> `referenced unique/FK-supporting key` ->
    普通简单标量列
  - `TRUNCATE`、`DELETE ALL`、`DROP/CREATE`、列重命名、列删除后的
    缓存维护
- 当前暂未实现：
  - 命令行参数化的命中比例配置
  - `JSON/JSONB`、数组、range、几何类型的命中型谓词
  - 基于实时查询结果的两阶段 DML
  - 多列组合命中谓词

第一版的目标是先显著降低空打概率，同时保持 workload 仍然保留一部分
随机探索能力，而不是把工具改成“每次先查再改”的回放模式。
