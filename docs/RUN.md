# MiniVectorDB 运行说明

## 1. 语言与依赖

| 项 | 值 |
|---|---|
| 语言 | C11（`gcc -std=gnu11`），无任何第三方依赖 |
| 构建镜像 | `gcc:13-bookworm`（编译阶段）→ `debian:bookworm-slim`（运行阶段） |
| 运行期依赖 | 仅 libc（glibc）；运行期不需要网络 |
| 平台 | Linux x86_64，容器内前台运行，通过 stdin/stdout 通信 |

## 2. 构建镜像

```bash
docker build -t minivectordb:round2 .
```

镜像由仓库根目录的 `Dockerfile` 构建：第一阶段用 `gcc:13-bookworm` 从 `src/` 编译，
第二阶段只拷贝二进制。构建过程只依赖基础镜像，不需要访问软件源。

## 3. 启动入口

```bash
docker run -i --network=none --cpus=2 --memory=2g --read-only \
  --mount type=volume,src=<data-volume>,dst=/data minivectordb:round2
```

- 容器的前台进程就是数据库（`ENTRYPOINT ["/usr/local/bin/minivectordb", "--data-dir", "/data"]`）；
- 使用 `-i` 保持 stdin 打开，**不要**加 `-t` 或 `-d`；
- 不需要 EXPOSE、端口映射或 host/port 参数；
- 程序启动后先向 stdout 输出一行 `{"type":"ready"}`，随后逐行读取请求、逐行返回响应；
- 日志与调试信息全部写 stderr，stdout 只有协议报文；
- 收到 `{"op":"shutdown"}` 后回一行 `{"id":N,"ok":true}` 并以退出码 0 结束。

数据目录的取址顺序：`--data-dir` 参数 > 环境变量 `VECTORDB_DATA_DIR`（且该路径确实存在）
> `/data`（存在时）> `./data`。
（评测驱动会把 `VECTORDB_DATA_DIR` 指向宿主临时目录，该路径在容器内并不存在，
因此容器里实际使用的是 `/data`；镜像的 ENTRYPOINT 也显式传了 `--data-dir /data`。）

## 4. 数据目录

程序只在数据目录下创建文件：

| 文件 | 内容 |
|---|---|
| `heap.log` | 事务日志（追加写） |
| `vec_<id>.dat` | 向量载荷池 |
| `idx_<id>.ivf` / `idx_<id>.log` | 向量索引的基线文件与增量日志 |

每个用例从**空的**数据目录开始；重启与崩溃恢复复用同一目录。

## 5. 支持的 SQL

```sql
CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(n), vec VECTOR(d));
CREATE VECTOR INDEX idx ON t(vec);
INSERT INTO t [(cols)] VALUES (...), (...);
SELECT [* | col | L2_DISTANCE(vec, '[...]') [AS alias], ...] FROM t
       [WHERE col op value [AND ...]] [ORDER BY col [ASC|DESC]] [ORDER BY alias]
       [LIMIT n];
UPDATE t SET col = value [, ...] [WHERE ...];
DELETE FROM t [WHERE ...];
BEGIN;  COMMIT;  ROLLBACK;
```

- 比较运算符：`=`、`<`、`<=`、`>`、`>=`，条件之间用 `AND` 连接；
- 排序并列项按主键升序；未指定 `ORDER BY` 的结果按集合比较；
- 向量字面量写成字符串或裸数组均可：`'[0,2]'` 与 `[0,2]` 等价；
- `L2_DISTANCE` 返回欧氏距离；`exact` 模式永远返回精确结果，`ann` 模式使用近似索引；
- `LIMIT` 在排序之后生效；过滤后的记录不足 K 条时全部返回。

### 错误处理

失败的语句返回 `{"ok":false,"error":{"code":"...","message":"..."}}`，**进程不会退出**，
后续语句照常执行。错误语义名：

| code | 含义 |
|---|---|
| `duplicate_key` | 主键重复 |
| `dimension_mismatch` | 向量维度与列定义不符 |
| `invalid_vector` | 向量元素为 NaN / Infinity / 超出 float32 表示范围 |
| `unsupported_sql` | 语句或语法不被支持（如 `ALTER TABLE`） |
| `duplicate_table` | 表已存在 |
| `no_such_table` / `no_such_column` | 表或列不存在 |
| `invalid_value` | 字面量类型与列类型不匹配 |

## 6. 从空数据目录开始的完整示例

下面每一条都是真正发给数据库的一行 JSON（这里为了可读性换行展示，协议要求一行一条）。

### 6.1 建表与写入

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2))","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (3, '乙', '三', '[0, 2]'), (1, '甲', '一', '[0, 0]')","search_mode":"exact"}
```

响应：

```json
{"id":1,"ok":true}
{"id":2,"ok":true,"affected_rows":2}
```

### 6.2 普通查询

```json
{"id":3,"op":"execute","sql":"SELECT id, content FROM docs WHERE category = '甲' ORDER BY id","search_mode":"exact"}
```

```json
{"id":3,"ok":true,"columns":["id","content"],"rows":[[1,"一"]]}
```

### 6.3 向量检索（精确与近似）

```json
{"id":4,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0, 0]') AS distance FROM docs ORDER BY distance LIMIT 2","search_mode":"exact"}
{"id":5,"op":"execute","sql":"CREATE VECTOR INDEX docs_vec ON docs(embedding)","search_mode":"exact"}
{"id":6,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0, 0]') AS distance FROM docs ORDER BY distance LIMIT 2","search_mode":"ann"}
```

```json
{"id":4,"ok":true,"columns":["id","distance"],"rows":[[1,0.0],[3,2.0]]}
{"id":5,"ok":true}
{"id":6,"ok":true,"columns":["id","distance"],"rows":[[1,0.0],[3,2.0]]}
```

### 6.4 提交与回滚

```json
{"id":7,"op":"execute","sql":"BEGIN","search_mode":"exact"}
{"id":8,"op":"execute","sql":"UPDATE docs SET content = '修改' WHERE id = 1","search_mode":"exact"}
{"id":9,"op":"execute","sql":"COMMIT","search_mode":"exact"}
{"id":10,"op":"execute","sql":"SELECT content FROM docs WHERE id = 1","search_mode":"exact"}
```

```json
{"id":7,"ok":true}
{"id":8,"ok":true,"affected_rows":1}
{"id":9,"ok":true}
{"id":10,"ok":true,"columns":["content"],"rows":[["修改"]]}
```

把第 9 条换成 `"ROLLBACK"` 即可看到回滚效果；未提交事务在进程关闭或被强杀时也会回滚。

### 6.5 重启

```json
{"id":11,"op":"shutdown"}
```

进程以退出码 0 结束。用**同一个数据目录**再次启动，已提交的数据仍在，
未提交事务的修改不在。

## 7. 资源与调优开关

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `MVDB_VEC_CACHE_MB` | 256 | 向量在内存中的缓存上限（MiB）。超过后该表改为从载荷池按需读取 |
| `MVDB_TIMING` | 未设置 | 设置后把每条语句的解析/执行耗时写到 stderr |
| `MVDB_DEBUG` | 未设置 | 设置后输出候选行数、扫描阶段耗时等调试信息 |

## 8. 本地自测

```bash
make            # 编译
make official   # 用题包自带的 protocol_check/run.py 跑全部公开用例
make handshake  # 只检查启动握手与关闭
make cases      # 公开题包 20 个用例（自研驱动，不需要 docker）
make edge       # 公开包未覆盖的语义（非法向量、维度、主键更新、大维度等）
make crash      # 强杀 / 恢复 / 原子性
make bench      # 性能基准
make recall     # 近似模式召回率
make spec       # STDIO_PROTOCOL.md 第 7 节的逐字节验收
make fuzz       # 近似/精确对拍、暴力真值对拍、索引维护一致性
```

题包自带的驱动也可以直接跑本地程序（`--command` 模式，不需要 docker，
断言语义与判题器一致）：

```bash
make official
```

有 docker 权限时，用镜像跑一遍（平台的做法）：

```bash
export VECTORDB_IMAGE='minivectordb:round2'
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --handshake-only --output "out/handshake"
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --output "out/public"
```
