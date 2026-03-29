# StarQuant：nanomsg 通道与 Kafka Topic 映射

本文档对应服务端 `msgq: kafka` 时的拓扑（见 `etc/config_server.yaml`）。

## Topic 一览（默认名，可在配置中修改）

| Topic 配置键 | 默认值 | 对应原 nanomsg | 说明 |
|-------------|--------|----------------|------|
| `kafka_topic_server_pub` | `sq.server.pub` | `SERVERPUB_URL` PUB/SUB | 行情与回报广播；GUI/Streamlit/数据服务各自独立 **consumer group** |
| `kafka_topic_engine_commands` | `sq.engine.commands` | `SERVERSUB_URL` PUB → 引擎 SUB | 客户端经 relay 转发的指令；**每个引擎实例**使用唯一 `group.id`（如 `sq-engine-<网关名>`）以保证多订阅语义 |
| `kafka_topic_client_requests` | `sq.client.requests` | `SERVERPULL_URL` PULL + 客户端 PUSH | 客户端请求队列；**单 consumer group**（如 `sq-relay`），仅 tradingengine 主循环 relay 消费 |
| `kafka_topic_strategy_to_broker` | `sq.strategy.to_broker` | PAIR 半双工 | 策略进程 → 服务端（下单等） |
| `kafka_topic_broker_to_strategy` | `sq.strategy.from_broker` | PAIR 半双工 | 服务端 → 策略进程（回报等） |

## `relay()` 与 RELAY_DESTINATION

[`msgq.cpp`](../cppsrc/StarQuant/Common/msgq.cpp) 中 `CMsgqRMessenger::relay()`：

- 消息首字节为 `RELAY_DESTINATION`（`@`）时：produce 到 **server.pub** topic（策略等特殊回传路径）。
- 否则：produce 到 **engine.commands** topic，由各引擎消费。

Kafka 实现保持该分支逻辑不变，仅将 nanomsg send 替换为向对应 topic 的 `produce`。

## Consumer group 约定

- **Relay（PULL）**：`kafka_group_relay`（默认 `sq-relay`）。
- **引擎 SUB**：`kafka_group_prefix` + `-` + 引擎名（如 `sq-engine-ctp_td_1`）；`CtpMDEngine` 使用 `sq-engine-md`。
- **数据录制 / 策略行情 SUB**：`sq-dataservice-record`、`sq-strategy-md` 等独立 group，避免互抢分区。

## 策略 PAIR 与 `sq-strategy-bridge`

- 策略进程通过 `MakeMsgqStrategyPair()` 向 `kafka_topic_strategy_to_broker` 写、从 `kafka_topic_broker_to_strategy` 读。
- 服务端 `StrategyKafkaBridgeService`（`kafka_strategy_bridge: true` 时由 `tradingengine` 启动）消费 `strategy_to_broker` 并 **原样转发** 到 `engine.commands`（经 `CMsgqRMessenger::msgq_send_`），替代 nanomsg 下服务端对 PAIR 的 bind。
- 若需 **仅走 PAIR 的回报** 进入 `broker_to_strategy`，还需在对应成交/回报路径上 produce 到该 topic（当前主链路仍以 `server.pub` 广播为主）。

## 创建 Topic

Redpanda 通常自动创建 topic。若使用正式 Kafka，请预先创建上述 topic，或开启 `auto.create.topics.enable`。
