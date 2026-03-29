# Common 目录详细分析

## 目录概述

`Common` 目录包含了StarQuant C++服务端的核心通用组件，为整个系统提供基础功能支持。主要包括配置管理、日志系统、消息队列、数据结构和工具函数。

## 文件结构

```
Common/
├── config.h/cpp          # 配置管理
├── logger.h/cpp          # 日志系统
├── msgq.h/cpp            # 消息队列实现
├── datastruct.h/cpp      # 数据结构定义
└── util.h/cpp            # 工具函数
```

## 1. 配置管理 (config.h/cpp)

### 功能概述

`CConfig` 类采用单例模式，负责读取和管理系统配置。配置信息从YAML文件中读取，包括运行模式、网关信息、消息队列地址、风险控制参数等。

### 核心代码逻辑

#### 单例模式实现

```cpp
class CConfig {
    static CConfig* pinstance_;
    static mutex instancelock_;
    
    static CConfig& instance() {
        if (pinstance_ == nullptr) {
            std::lock_guard<mutex> g(instancelock_);
            if (pinstance_ == nullptr) {
                pinstance_ = new CConfig();
            }
        }
        return *pinstance_;
    }
};
```

**设计要点：**
- 使用双重检查锁定（Double-Checked Locking）确保线程安全
- 静态成员变量存储唯一实例
- 互斥锁保护实例创建过程

#### 配置读取逻辑

`readConfig()` 方法从 `etc/config_server.yaml` 文件读取配置：

1. **运行模式配置**
   - `TRADE_MODE`: 实盘交易模式
   - `RECORD_MODE`: 数据记录模式
   - `REPLAY_MODE`: 数据回放模式

2. **网关配置**
   - 支持多个网关（CTP、TAP、XTP等）
   - 每个网关包含：API类型、经纪商ID、用户ID、密码、认证码、前置地址等

3. **消息队列配置**
   - `SERVERPUB_URL`: 服务器发布地址（tcp://localhost:55555）
   - `SERVERSUB_URL`: 服务器订阅地址（tcp://localhost:55556）
   - `SERVERPULL_URL`: 服务器拉取地址（tcp://localhost:55557）

4. **风险控制配置**
   - 单笔订单限制（数量、金额）
   - 每日订单限制（数量、金额、总数量）
   - 每秒订单频率限制

5. **证券列表管理**
   - `securities`: 完整证券符号列表
   - `instrument2sec`: 合约ID到完整符号的映射
   - `sec2instrument`: 符号到CTP合约的映射

### 关键方法

- `readConfig()`: 从YAML文件读取配置
- `SecurityFullNameToCtpSymbol()`: 完整符号转换为CTP符号
- `CtpSymbolToSecurityFullName()`: CTP符号转换为完整符号
- `configDir()/logDir()/dataDir()`: 获取配置/日志/数据目录路径

## 2. 日志系统 (logger.h/cpp)

### 功能概述

日志系统提供了两套日志实现：
1. **logger类**: 简单的文件日志记录
2. **SQLogger类**: 基于log4cplus的完整日志框架

### 核心代码逻辑

#### logger类实现

```cpp
class logger {
    static logger* pinstance_;
    static mutex instancelock_;
    FILE* logfile;
    
    void Printf2File(const char *format, ...);
};
```

**特点：**
- 单例模式
- 使用互斥锁保证线程安全
- 日志文件按日期命名：`starquant-YYYYMMDD.txt`
- 无缓冲写入，确保日志及时刷新

#### SQLogger类实现

基于log4cplus库，提供更强大的日志功能：

```cpp
class SQLogger {
    static shared_ptr<SQLogger> getLogger(const string& name);
    void debug(const string& msg);
    void info(const string& msg);
    void warn(const string& msg);
    void error(const string& msg);
};
```

**功能：**
- 支持多个日志器实例（按名称区分）
- 可配置日志级别（DEBUG、INFO、WARN、ERROR）
- 支持文件和控制台输出
- 线程安全的日志记录

### 日志配置

日志配置从 `etc/config_log` 文件读取，支持：
- 日志级别设置
- 输出目标（文件/控制台）
- 日志格式配置
- 文件滚动策略

## 3. 消息队列 (msgq.h/cpp)

### 功能概述

消息队列模块是系统内部通信的核心，基于nanomsg库实现。支持多种通信模式：PUB/SUB、PUSH/PULL、PAIR等。

### 核心架构

#### 消息队列基类

```cpp
class CMsgq {
protected:
    MSGQ_PROTOCOL protocol_;  // 协议类型
    string url_;               // 连接地址
    
public:
    virtual void sendmsg(const string& str, int32_t dontwait = 1) = 0;
    virtual string recmsg(int32_t blockingflags = 1) = 0;
};
```

#### Nanomsg实现

```cpp
class CMsgqNanomsg : public CMsgq {
private:
    int32_t sock_;      // socket句柄
    int32_t eid_;       // endpoint ID
    string endpoint_;   // 端点地址
    
public:
    CMsgqNanomsg(MSGQ_PROTOCOL protocol, string url, bool binding = true);
    void sendmsg(const string& str, int32_t dontwait = 1);
    string recmsg(int32_t blockingflags = 1);
};
```

**支持的协议类型：**
- `PAIR`: 一对一通信
- `PUB/SUB`: 发布/订阅模式
- `PUSH/PULL`: 负载均衡模式

#### 消息传递器（Messenger）

系统定义了两种消息传递器：

**1. CMsgqEMessenger（引擎消息传递器）**

用于引擎（MD/TD）与系统其他部分的通信：

```cpp
class CMsgqEMessenger : public IMessenger {
private:
    std::unique_ptr<CMsgq> msgq_recv_;  // 接收队列
    
public:
    static mutex sendlock_;
    static std::unique_ptr<CMsgq> msgq_send_;  // 共享发送队列
    
    void send(shared_ptr<MsgHeader> pmsg, int32_t mode = 0);
    shared_ptr<MsgHeader> recv(int32_t mode = 0);
};
```

**工作流程：**
- 每个引擎实例有自己的接收队列（SUB模式）
- 所有引擎共享一个发送队列（PUB模式）
- 通过消息头中的destination字段路由消息

**2. CMsgqRMessenger（中继消息传递器）**

用于交易引擎的消息中继：

```cpp
class CMsgqRMessenger : public IMessenger {
private:
    std::unique_ptr<CMsgq> msgq_recv_;  // PULL模式接收
    
public:
    static mutex sendlock_;
    static std::unique_ptr<CMsgq> msgq_send_;  // PUB模式发送
    
    void relay();  // 消息中继逻辑
};
```

**中继逻辑：**

```cpp
void CMsgqRMessenger::relay() {
    string msgpull = msgq_recv_->recmsg(0);
    if (msgpull.empty()) return;
    
    // 特殊标志 '@' 表示消息需要返回给策略进程
    if (msgpull[0] == RELAY_DESTINATION) {
        lock_guard<std::mutex> g(CMsgqEMessenger::sendlock_);
        CMsgqEMessenger::msgq_send_->sendmsg(msgpull);
    } else {
        lock_guard<std::mutex> g(CMsgqRMessenger::sendlock_);
        // 转发消息到各个引擎
        CMsgqRMessenger::msgq_send_->sendmsg(msgpull);
    }
}
```

### 消息序列化

消息使用管道符 `|` 作为分隔符进行序列化：

```
destination|source|msgtype|data1|data2|...
```

### 消息路由

- `DESTINATION_ALL` ("*"): 广播到所有引擎
- `"CTP.MD"`: 路由到CTP行情引擎
- `"CTP.TD"`: 路由到CTP交易引擎
- `"@strategy"`: 特殊路由，返回给策略进程

## 4. 数据结构 (datastruct.h/cpp)

### 功能概述

定义了系统中使用的所有核心数据结构，包括消息类型、订单、持仓、账户、行情等。

### 核心数据结构

#### 消息类型枚举

系统定义了丰富的消息类型（MSG_TYPE），按功能分类：

- **10xx**: 行情数据（Tick、Bar等）
- **11xx**: 系统控制（引擎状态、连接、重置等）
- **12xx**: 策略相关
- **13xx**: 任务相关
- **20xx**: 引擎请求（订阅、查询、下单等）
- **25xx**: 引擎回调（订单回报、成交回报等）
- **31xx**: 信息类消息
- **34xx**: 错误类消息
- **40xx**: 测试消息

#### 消息基类

```cpp
class MsgHeader {
public:
    string destination_;  // 目标
    string source_;        // 源
    MSG_TYPE msgtype_;    // 消息类型
    
    virtual string serialize();
    virtual void deserialize(const string& msgin);
};
```

#### 核心数据结构

**1. Tick（行情数据）**

```cpp
class Tick {
    string fullSymbol_;      // 完整符号
    string time_;            // 时间戳
    double price_;           // 价格
    int32_t size_;           // 成交量
    int32_t depth_;          // 深度
    double bidPrice_[20];    // 买价（最多20档）
    int32_t bidSize_[20];    // 买量
    double askPrice_[20];    // 卖价
    int32_t askSize_[20];    // 卖量
    double open_;            // 开盘价
    double high_;            // 最高价
    double low_;             // 最低价
    double preClose_;        // 昨收价
    double upperLimitPrice_;  // 涨停价
    double lowerLimitPrice_; // 跌停价
};
```

**2. Order（订单）**

```cpp
class Order {
    int64_t serverOrderId_;  // 服务器订单ID
    string fullSymbol_;      // 完整符号
    OrderType orderType_;    // 订单类型
    OrderFlag flag_;         // 开平标志
    double price_;           // 价格
    int32_t size_;          // 数量
    OrderStatus status_;     // 状态
    string accountID_;       // 账户ID
    // ... 更多字段
};
```

**3. Fill（成交）**

```cpp
class Fill {
    int64_t serverOrderId_;  // 关联订单ID
    string fullSymbol_;      // 完整符号
    double price_;           // 成交价
    int32_t size_;          // 成交量
    string time_;            // 成交时间
    // ... 更多字段
};
```

**4. Position（持仓）**

```cpp
class Position {
    string fullSymbol_;      // 完整符号
    string accountID_;       // 账户ID
    int32_t size_;         // 持仓数量
    double avgPrice_;       // 平均价格
    double unrealizedPnL_;  // 未实现盈亏
    // ... 更多字段
};
```

**5. AccountInfo（账户信息）**

```cpp
class AccountInfo {
    string accountID_;
    double availableFunds_;      // 可用资金
    double netLiquidation_;      // 净值
    double equityWithLoanValue_;  // 含借贷权益
    double fullInitialMargin_;   // 初始保证金
    double fullMaintainanceMargin_; // 维持保证金
    double cashBalance_;          // 现金余额
    double realizedPnL_;         // 已实现盈亏
    double unrealizedPnL_;        // 未实现盈亏
};
```

**6. Security（合约信息）**

```cpp
class Security {
    string fullSymbol_;      // 完整符号
    string symbol_;          // 合约代码
    string exchange_;       // 交易所
    int32_t multiplier_;    // 合约乘数
    double ticksize_;       // 最小变动价位
    double longMarginRatio_;  // 多头保证金率
    double shortMarginRatio_; // 空头保证金率
    // ... 期权相关字段
};
```

### 消息类型定义

系统为每种数据类型定义了对应的消息类：

- `TickMsg`: 行情消息
- `OrderMsg`: 订单消息
- `FillMsg`: 成交消息
- `PositionMsg`: 持仓消息
- `AccMsg`: 账户消息
- `SecurityMsg`: 合约消息

每个消息类都继承自 `MsgHeader`，并实现 `serialize()` 和 `deserialize()` 方法。

### 枚举类型

**OrderType（订单类型）**
- `OT_Market`: 市价单
- `OT_Limit`: 限价单
- `OT_Stop`: 止损单
- `OT_StopLimit`: 止损限价单
- 等等

**OrderStatus（订单状态）**
- `OS_NewBorn`: 新建
- `OS_PendingSubmit`: 待提交
- `OS_Submitted`: 已提交
- `OS_PartiallyFilled`: 部分成交
- `OS_Filled`: 全部成交
- `OS_Canceled`: 已撤销
- 等等

**OrderFlag（开平标志）**
- `OF_OpenPosition`: 开仓
- `OF_ClosePosition`: 平仓
- `OF_CloseToday`: 平今
- `OF_CloseYesterday`: 平昨

## 5. 工具函数 (util.h/cpp)

### 功能概述

提供各种工具函数，包括字符串处理、时间处理、文件操作等。

### 主要功能

1. **字符串处理**
   - `stringsplit()`: 字符串分割
   - `startwith()`: 检查字符串前缀
   - `trim()`: 去除空白字符

2. **时间处理**
   - `now()`: 获取当前时间
   - `nowMS()`: 获取当前时间（毫秒）
   - `ymd()`: 获取年月日字符串
   - `ymdhmsf6()`: 获取完整时间戳

3. **文件操作**
   - `fileExists()`: 检查文件是否存在
   - `createDirectory()`: 创建目录

4. **系统相关**
   - `msleep()`: 毫秒级睡眠
   - `ConsoleControlHandler()`: 控制台信号处理

## 设计模式总结

### 1. 单例模式
- `CConfig`: 配置管理
- `logger`: 日志记录
- `DataManager`: 数据管理
- `OrderManager`: 订单管理
- `PortfolioManager`: 投资组合管理
- `RiskManager`: 风险管理

### 2. 工厂模式
- 消息队列工厂（支持nanomsg和zmq）
- 消息对象创建（根据消息类型）

### 3. 策略模式
- 不同的消息队列实现（nanomsg、zmq）
- 不同的日志实现（logger、SQLogger）

### 4. 观察者模式
- 消息订阅/发布机制

## 线程安全

所有共享资源都使用互斥锁保护：
- 配置读取：`CConfig::readlock_`
- 消息发送：`CMsgqEMessenger::sendlock_`、`CMsgqRMessenger::sendlock_`
- 日志写入：`logger::instancelock_`

## 总结

`Common` 目录是整个系统的基础设施层，提供了：
1. **配置管理**: 统一的配置读取和管理
2. **日志系统**: 完善的日志记录功能
3. **消息队列**: 高效的进程间通信机制
4. **数据结构**: 完整的数据模型定义
5. **工具函数**: 常用的辅助功能

这些组件为上层模块（Engine、Services、Trade等）提供了坚实的基础支持。
