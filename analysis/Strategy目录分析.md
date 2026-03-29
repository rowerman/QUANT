# Strategy 目录详细分析

## 目录概述

`Strategy` 目录实现了策略框架，提供了策略基类、策略工厂、策略管理器等功能。策略是量化交易系统的核心，负责根据市场数据生成交易信号。

## 文件结构

```
Strategy/
├── strategybase.h/cpp        # 策略基类
├── strategyFactory.h/cpp     # 策略工厂
├── strategymanager.h/cpp     # 策略管理器
├── smacross.h/cpp            # 示例策略：均线交叉
└── mystrategy.cpp            # 示例策略
```

## 1. 策略基类 (strategybase.h/cpp)

### 功能概述

`StrategyBase` 是所有策略的基类，定义了策略的基本接口和通用功能。策略通过继承此类并实现虚函数来创建具体的交易策略。

### 核心代码逻辑

#### 类定义

```cpp
class StrategyBase {
public:
    bool initialized = false;              // 是否已初始化
    static int m_orderId;                  // 订单ID计数器
    queue<string> msgstobrokerage;         // 待发送到经纪商的消息队列
    
    StrategyBase();
    virtual ~StrategyBase();
    
    // 生命周期方法
    virtual void initialize();             // 初始化策略
    virtual void reset();                  // 重置策略
    virtual void teardown();               // 清理策略
    
    // ************  Incoming Message Handlers ********************//
    virtual void OnTick(Tick& k) {}                    // 行情回调
    virtual void OnBar(Bar& k) {}                        // K线回调
    virtual void OnOrderTicket(long oid) {}             // 订单确认回调
    virtual void OnOrderCancel(long oid) {}              // 订单撤销回调
    virtual void OnFill(Fill& f) {}                     // 成交回调
    virtual void OnPosition(Position& p) {}             // 持仓回调
    virtual void OnGeneralMessage(string& msg) {}        // 通用消息回调
    
    // ************  Outgoing Methods ********************//
    void SendOrder(std::shared_ptr<Order> o);           // 发送订单
    void SendOrderCancel(long oid);                     // 发送撤单
    void SendSubscriptionRequest();                     // 发送订阅请求
    void SendHistoricalBarRequest();                   // 发送历史数据请求
    void SendGeneralInformation();                     // 发送通用信息
    void SendLog();                                    // 发送日志
};
```

#### 构造函数

```cpp
StrategyBase::StrategyBase() {
    // 策略基类构造函数
    // 注意：消息队列的创建已移至策略服务层
    // 策略通过msgstobrokerage队列与系统通信
}
```

**设计说明：**
- 策略不直接持有消息队列，而是通过 `msgstobrokerage` 队列与系统通信
- 消息队列的创建和管理由策略服务层负责
- 这种设计实现了策略与系统的解耦

#### 初始化方法

```cpp
void StrategyBase::initialize() {
    msleep(2000);  // 等待2秒，确保系统就绪
    initialized = true;
}
```

**初始化时机：**
- 策略创建后立即调用
- 系统启动时调用
- 策略重置后调用

#### 发送订单

```cpp
void StrategyBase::SendOrder(std::shared_ptr<Order> o) {
    // 1. 加锁保护订单ID生成
    lock_guard<mutex> g(oid_mtx);
    
    // 2. 设置订单基本信息
    o->createTime = time(nullptr);
    o->orderStatus_ = OrderStatus::OS_NewBorn;
    o->clientOrderId = m_orderId;
    m_orderId++;  // 递增订单ID
    
    // 3. 构建订单消息字符串
    string s;
    string s_price = "0.0";
    
    // 根据订单类型设置价格
    if (o->orderType == OrderType::OT_Limit) {
        s_price = to_string(o->limitPrice);
    } else if (o->orderType == OrderType::OT_StopLimit) {
        s_price = to_string(o->stopPrice);
    }
    
    // 4. 序列化订单消息
    s = to_string(MSG_TYPE::MSG_TYPE_ORDER)
        + SERIALIZATION_SEPARATOR + CConfig::instance().account
        + SERIALIZATION_SEPARATOR + to_string(o->source)
        + SERIALIZATION_SEPARATOR + to_string(o->clientOrderId)
        + SERIALIZATION_SEPARATOR + to_string(static_cast<int>(o->orderType))
        + SERIALIZATION_SEPARATOR + o->fullSymbol
        + SERIALIZATION_SEPARATOR + to_string(o->orderSize)
        + SERIALIZATION_SEPARATOR + s_price
        + SERIALIZATION_SEPARATOR + to_string(static_cast<int>(o->orderFlag))
        + SERIALIZATION_SEPARATOR + o->tag;
    
    // 5. 将消息加入队列（由策略服务层处理）
    msgstobrokerage.push(s);
}
```

**消息格式：**
```
MSG_TYPE|account|source|clientOrderId|orderType|fullSymbol|orderSize|price|orderFlag|tag
```

#### 发送撤单

```cpp
void StrategyBase::SendOrderCancel(long oid) {
    // 构建撤单消息
    string msg = to_string(MSG_TYPE::MSG_TYPE_CANCEL_ORDER)
        + SERIALIZATION_SEPARATOR + std::to_string(oid);
    
    msgstobrokerage.push(msg);
}
```

### 策略生命周期

```
创建 → initialize() → 运行 → OnTick/OnBar/OnFill... → reset() → 运行 → teardown() → 销毁
```

## 2. 策略工厂 (strategyFactory.h/cpp)

### 功能概述

策略工厂负责根据策略名称创建对应的策略实例，实现了策略的动态加载。

### 核心代码逻辑

```cpp
std::unique_ptr<StrategyBase> strategyFactory(const string& _algo) {
    if (_algo == "SmaCross") {
        return std::make_unique<SmaCross>();
    } else if (_algo == "MyStrategy") {
        return std::make_unique<MyStrategy>();
    } else {
        // 返回默认策略或抛出异常
        return nullptr;
    }
}
```

**使用示例：**

```cpp
// 在策略服务中使用
auto strategy = strategyFactory("SmaCross");
strategy->initialize();
```

**设计模式：**
- **工厂模式**：通过字符串名称创建策略实例
- **多态**：返回基类指针，支持不同策略类型

## 3. 策略管理器 (strategymanager.h/cpp)

### 功能概述

策略管理器负责管理多个策略实例，提供策略的添加、删除、启动、停止等功能。

### 核心代码逻辑

```cpp
class StrategyManager {
    // 策略管理器类定义
    // 注意：当前实现较简单，主要功能在策略服务层
};
```

**当前状态：**
- 策略管理器类定义较简单
- 主要功能在 `strategyservice.cpp` 中实现
- 未来可以扩展为支持多策略管理

## 4. 示例策略：均线交叉 (smacross.h/cpp)

### 功能概述

`SmaCross` 是一个简单的均线交叉策略示例，展示了如何实现一个完整的策略。

### 核心代码逻辑

#### 类定义

```cpp
class SmaCross : public StrategyBase {
public:
    virtual void initialize();
    virtual void OnTick(Tick& k);
    virtual void OnGeneralMessage(string& msg);
    
private:
    int order_time;              // 订单时间
    bool buy_sell;              // 买卖标志
    vector<Tick> tickarray;     // 行情数组（用于计算均线）
    int ordercount;             // 订单计数
    int sid;                    // 策略ID
};
```

#### 初始化

```cpp
void SmaCross::initialize() {
    StrategyBase::initialize();
    
    // 初始化策略参数
    order_time = 0;
    buy_sell = false;
    tickarray.clear();
    ordercount = 0;
    sid = 0;
    
    // 订阅行情数据
    SendSubscriptionRequest();
}
```

#### 行情处理

```cpp
void SmaCross::OnTick(Tick& k) {
    // 1. 保存行情数据
    tickarray.push_back(k);
    
    // 限制数组大小（例如保留最近1000个tick）
    if (tickarray.size() > 1000) {
        tickarray.erase(tickarray.begin());
    }
    
    // 2. 计算均线（需要足够的数据）
    if (tickarray.size() < 50) {
        return;  // 数据不足，不交易
    }
    
    // 计算短期均线（例如5周期）
    double shortMA = 0.0;
    for (int i = tickarray.size() - 5; i < tickarray.size(); i++) {
        shortMA += tickarray[i].price_;
    }
    shortMA /= 5;
    
    // 计算长期均线（例如20周期）
    double longMA = 0.0;
    for (int i = tickarray.size() - 20; i < tickarray.size(); i++) {
        longMA += tickarray[i].price_;
    }
    longMA /= 20;
    
    // 3. 判断均线交叉
    if (tickarray.size() >= 2) {
        // 计算上一周期的均线值
        double prevShortMA = 0.0;
        for (int i = tickarray.size() - 6; i < tickarray.size() - 1; i++) {
            prevShortMA += tickarray[i].price_;
        }
        prevShortMA /= 5;
        
        double prevLongMA = 0.0;
        for (int i = tickarray.size() - 21; i < tickarray.size() - 1; i++) {
            prevLongMA += tickarray[i].price_;
        }
        prevLongMA /= 20;
        
        // 4. 检测金叉（短期均线上穿长期均线）
        if (prevShortMA <= prevLongMA && shortMA > longMA) {
            // 买入信号
            if (!buy_sell) {  // 当前没有持仓
                auto order = make_shared<Order>();
                order->fullSymbol = k.fullSymbol_;
                order->orderType = OrderType::OT_Market;
                order->orderSize = 1;  // 买入1手
                order->orderFlag = OrderFlag::OF_OpenPosition;
                order->side = OrderSide::BUY;
                
                SendOrder(order);
                buy_sell = true;
                ordercount++;
            }
        }
        
        // 5. 检测死叉（短期均线下穿长期均线）
        if (prevShortMA >= prevLongMA && shortMA < longMA) {
            // 卖出信号
            if (buy_sell) {  // 当前有持仓
                auto order = make_shared<Order>();
                order->fullSymbol = k.fullSymbol_;
                order->orderType = OrderType::OT_Market;
                order->orderSize = 1;  // 卖出1手
                order->orderFlag = OrderFlag::OF_ClosePosition;
                order->side = OrderSide::SELL;
                
                SendOrder(order);
                buy_sell = false;
                ordercount++;
            }
        }
    }
}
```

**策略逻辑说明：**
1. **数据收集**：保存最近的行情数据
2. **均线计算**：计算短期和长期移动平均线
3. **信号生成**：
   - 金叉（短期上穿长期）：买入信号
   - 死叉（短期下穿长期）：卖出信号
4. **订单发送**：根据信号生成并发送订单

#### 消息处理

```cpp
void SmaCross::OnGeneralMessage(string& msg) {
    // 处理来自系统的消息
    // 例如：订单确认、成交回报等
    
    vector<string> vs = stringsplit(msg, SERIALIZATION_SEPARATOR);
    if (vs.size() > 0) {
        MSG_TYPE msgtype = (MSG_TYPE)(atoi(vs[0].c_str()));
        
        switch (msgtype) {
            case MSG_TYPE_RTN_ORDER:
                // 处理订单回报
                break;
            case MSG_TYPE_RTN_TRADE:
                // 处理成交回报
                break;
            default:
                break;
        }
    }
}
```

## 5. 策略开发指南

### 创建新策略的步骤

1. **继承StrategyBase**

```cpp
class MyStrategy : public StrategyBase {
public:
    virtual void initialize();
    virtual void OnTick(Tick& k);
    virtual void OnBar(Bar& k);
    virtual void OnFill(Fill& f);
    // ... 实现其他回调方法
};
```

2. **实现初始化方法**

```cpp
void MyStrategy::initialize() {
    StrategyBase::initialize();
    // 初始化策略参数
    // 订阅行情数据
    SendSubscriptionRequest();
}
```

3. **实现行情处理方法**

```cpp
void MyStrategy::OnTick(Tick& k) {
    // 处理行情数据
    // 计算指标
    // 生成交易信号
    // 发送订单
}
```

4. **实现成交处理方法**

```cpp
void MyStrategy::OnFill(Fill& f) {
    // 处理成交回报
    // 更新持仓
    // 记录交易
}
```

5. **在工厂中注册**

```cpp
std::unique_ptr<StrategyBase> strategyFactory(const string& _algo) {
    if (_algo == "MyStrategy") {
        return std::make_unique<MyStrategy>();
    }
    // ...
}
```

### 策略开发最佳实践

1. **状态管理**
   - 使用成员变量保存策略状态
   - 在 `reset()` 中重置状态
   - 在 `teardown()` 中清理资源

2. **错误处理**
   - 检查数据有效性
   - 处理异常情况
   - 记录错误日志

3. **性能优化**
   - 避免不必要的计算
   - 使用高效的数据结构
   - 限制历史数据大小

4. **风险管理**
   - 设置止损止盈
   - 控制仓位大小
   - 避免过度交易

## 6. 策略与系统的交互

### 数据流向

```
行情数据 → 策略服务 → OnTick() → 策略逻辑 → SendOrder() → msgstobrokerage → 交易引擎
                                                                                    ↓
订单回报 ← 策略服务 ← OnGeneralMessage() ← 交易引擎 ← 订单处理 ← 交易接口
```

### 消息类型

策略需要处理的消息类型：
- `MSG_TYPE_TICK`: 行情数据
- `MSG_TYPE_BAR`: K线数据
- `MSG_TYPE_RTN_ORDER`: 订单回报
- `MSG_TYPE_RTN_TRADE`: 成交回报
- `MSG_TYPE_RSP_POS`: 持仓回报
- `MSG_TYPE_RSP_ACCOUNT`: 账户回报

## 7. 设计模式总结

### 1. 模板方法模式
- `StrategyBase` 定义策略框架
- 子类实现具体策略逻辑

### 2. 工厂模式
- `strategyFactory` 根据名称创建策略

### 3. 观察者模式
- 策略订阅行情数据
- 系统回调策略方法

### 4. 策略模式
- 不同的策略实现不同的交易逻辑
- 统一的接口，不同的实现

## 总结

`Strategy` 目录实现了完整的策略框架：

1. **策略基类**：提供统一的策略接口和基础功能
2. **策略工厂**：支持策略的动态创建
3. **示例策略**：展示如何实现具体策略
4. **消息通信**：通过消息队列与系统交互

策略开发遵循面向对象设计原则，通过继承和虚函数实现多态，使得策略开发简单而灵活。策略与系统解耦，可以独立开发和测试。
