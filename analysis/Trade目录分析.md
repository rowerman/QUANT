# Trade 目录详细分析

## 目录概述

`Trade` 目录实现了交易相关的核心功能，包括订单管理、投资组合管理、风险管理、保证金管理等。这些模块共同构成了交易系统的业务逻辑层。

## 文件结构

```
Trade/
├── ordermanager.h/cpp      # 订单管理器
├── portfoliomanager.h/cpp  # 投资组合管理器
├── riskmanager.h/cpp      # 风险管理器
├── marginmanager.h/cpp     # 保证金管理器
└── calc.h/cpp              # 计算工具
```

## 1. 订单管理器 (ordermanager.h/cpp)

### 功能概述

`OrderManager` 负责跟踪和管理所有订单，维护订单状态，处理订单确认、成交、撤销等事件。

### 核心代码逻辑

#### 类定义

```cpp
class OrderManager {
public:
    static OrderManager* pinstance_;
    static mutex instancelock_;
    static OrderManager& instance();
    
    std::shared_ptr<SQLogger> logger;
    int32_t _count = 0;
    
    // 订单存储
    std::map<int64_t, std::shared_ptr<Order>> orders_;      // 服务器订单ID -> 订单
    std::map<int64_t, int64_t> fills_;                       // 订单ID -> 已成交量
    std::map<int64_t, bool> cancels_;                       // 订单ID -> 是否已撤销
    mutex wlock;                                             // 写锁
    
    // 订单生命周期管理
    void trackOrder(std::shared_ptr<Order> o);              // 开始跟踪订单
    void gotOrder(int64_t oid);                              // 订单确认
    void gotFill(const Fill& fill);                          // 订单成交
    void gotCancel(int64_t oid);                            // 订单撤销
    
    // 订单查询
    std::shared_ptr<Order> retrieveOrderFromServerOrderId(int64_t oid);
    std::shared_ptr<Order> retrieveOrderFromSourceAndClientOrderId(int32_t source, int64_t oid);
    std::shared_ptr<Order> retrieveOrderFromOrderNo(const string& ono);
    std::shared_ptr<Order> retrieveOrderFromAccAndBrokerOrderId(const string& acc, int32_t oid);
    std::shared_ptr<Order> retrieveOrderFromAccAndLocalNo(const string& acc, const string& ono);
    vector<std::shared_ptr<Order>> retrieveOrder(const string& fullsymbol);
    vector<std::shared_ptr<Order>> retrieveNonFilledOrderPtr();
    vector<std::shared_ptr<Order>> retrieveNonFilledOrderPtr(const string& fullsymbol);
    
    // 状态检查
    bool isEmpty();
    bool isTracked(int64_t oid);
    bool isCompleted(int64_t oid);                          // 是否已完成（成交或撤销）
    bool hasPendingOrders();                                // 是否有待处理订单
    
    void reset();                                           // 重置
};
```

#### 单例模式实现

```cpp
OrderManager* OrderManager::pinstance_ = nullptr;
mutex OrderManager::instancelock_;

OrderManager& OrderManager::instance() {
    if (pinstance_ == nullptr) {
        lock_guard<mutex> g(instancelock_);
        if (pinstance_ == nullptr) {
            pinstance_ = new OrderManager();
        }
    }
    return *pinstance_;
}
```

#### 跟踪订单

```cpp
void OrderManager::trackOrder(std::shared_ptr<Order> o) {
    // 1. 检查订单是否已存在
    auto iter = orders_.find(o->serverOrderID_);
    if (iter != orders_.end()) {
        return;  // 订单已存在，不重复添加
    }
    
    // 2. 添加到订单映射
    orders_[o->serverOrderID_] = o;
    
    // 3. 初始化成交量和撤销状态
    cancels_[o->serverOrderID_] = false;
    fills_[o->serverOrderID_] = 0;
    
    LOG_INFO(logger, "Order is put under track. ServerOrderId=" << o->serverOrderID_);
}
```

**调用时机：**
- 引擎收到下单请求时
- 订单创建后立即调用

#### 订单确认处理

```cpp
void OrderManager::gotOrder(int64_t oid) {
    // 1. 检查订单是否被跟踪
    if (!isTracked(oid)) {
        LOG_ERROR(logger, "Order is not tracked. ServerOrderId= " << oid);
        return;
    }
    
    // 2. 更新订单状态
    lock_guard<mutex> g(orderStatus_mtx);
    if ((orders_[oid]->orderStatus_ == OrderStatus::OS_NewBorn) ||
        (orders_[oid]->orderStatus_ == OrderStatus::OS_Submitted)) {
        orders_[oid]->orderStatus_ = OrderStatus::OS_Acknowledged;
    }
}
```

**状态转换：**
- `OS_NewBorn` → `OS_Acknowledged`
- `OS_Submitted` → `OS_Acknowledged`

#### 订单成交处理

```cpp
void OrderManager::gotFill(const Fill& fill) {
    // 1. 检查订单是否被跟踪
    if (!isTracked(fill.serverOrderID_)) {
        LOG_ERROR(logger, "Order is not tracked. ServerOrderId= " << fill.serverOrderID_);
        return;
    }
    
    // 2. 更新订单状态和成交信息
    lock_guard<mutex> g(orderStatus_mtx);
    orders_[fill.serverOrderID_]->orderStatus_ = OrderStatus::OS_Filled;
    orders_[fill.serverOrderID_]->updateTime_ = ymdhmsf();
    
    // 3. 更新成交量
    fills_[fill.serverOrderID_] += fill.size_;
    
    // 4. 检查部分成交
    // TODO: 处理部分成交情况
    
    // 5. 更新投资组合
    PortfolioManager::instance().Adjust(fill);
    
    LOG_INFO(logger, "Order is filled. ServerOrderId=" << fill.serverOrderID_
             << " price=" << fill.tradePrice_);
}
```

**处理流程：**
1. 验证订单存在
2. 更新订单状态为已成交
3. 累计成交量
4. 通知投资组合管理器更新持仓

#### 订单撤销处理

```cpp
void OrderManager::gotCancel(int64_t oid) {
    if (isTracked(oid)) {
        lock_guard<mutex> g(orderStatus_mtx);
        orders_[oid]->orderStatus_ = OrderStatus::OS_Canceled;
        orders_[oid]->updateTime_ = ymdhmsf();
        cancels_[oid] = true;
    }
}
```

#### 订单查询方法

系统提供了多种订单查询方法，支持不同的查询条件：

```cpp
// 1. 通过服务器订单ID查询
std::shared_ptr<Order> OrderManager::retrieveOrderFromServerOrderId(int64_t oid) {
    if (orders_.count(oid)) {
        return orders_[oid];
    }
    return nullptr;
}

// 2. 通过客户端订单ID和来源查询
std::shared_ptr<Order> OrderManager::retrieveOrderFromSourceAndClientOrderId(
    int32_t source, int64_t oid
) {
    for (auto iterator = orders_.begin(); iterator != orders_.end(); ++iterator) {
        if ((iterator->second->clientOrderID_ == oid) &&
            (iterator->second->clientID_ == source)) {
            return iterator->second;
        }
    }
    return nullptr;
}

// 3. 通过账户和经纪商订单ID查询（用于CTP等接口）
std::shared_ptr<Order> OrderManager::retrieveOrderFromAccAndBrokerOrderId(
    const string& acc, int32_t oid
) {
    for (auto iterator = orders_.begin(); iterator != orders_.end(); ++iterator) {
        if ((iterator->second->brokerOrderID_ == oid) &&
            (iterator->second->account_ == acc)) {
            return iterator->second;
        }
    }
    return nullptr;
}

// 4. 查询指定合约的所有订单
vector<std::shared_ptr<Order>> OrderManager::retrieveOrder(const string& fullsymbol) {
    vector<std::shared_ptr<Order>> result;
    for (auto iterator = orders_.begin(); iterator != orders_.end(); ++iterator) {
        if (iterator->second->fullSymbol_ == fullsymbol) {
            result.push_back(iterator->second);
        }
    }
    return result;
}

// 5. 查询所有未完全成交的订单
vector<std::shared_ptr<Order>> OrderManager::retrieveNonFilledOrderPtr() {
    vector<std::shared_ptr<Order>> result;
    for (auto iterator = orders_.begin(); iterator != orders_.end(); ++iterator) {
        auto order = iterator->second;
        if (order->orderStatus_ != OrderStatus::OS_Filled &&
            order->orderStatus_ != OrderStatus::OS_Canceled) {
            result.push_back(order);
        }
    }
    return result;
}
```

#### 状态检查方法

```cpp
// 检查订单是否被跟踪
bool OrderManager::isTracked(int64_t oid) {
    return orders_.count(oid) > 0;
}

// 检查订单是否已完成（成交或撤销）
bool OrderManager::isCompleted(int64_t oid) {
    if (!isTracked(oid)) {
        return false;
    }
    auto order = orders_[oid];
    return (order->orderStatus_ == OrderStatus::OS_Filled ||
            order->orderStatus_ == OrderStatus::OS_Canceled);
}

// 检查是否有待处理订单
bool OrderManager::hasPendingOrders() {
    for (auto iterator = orders_.begin(); iterator != orders_.end(); ++iterator) {
        if (!isCompleted(iterator->first)) {
            return true;
        }
    }
    return false;
}
```

## 2. 投资组合管理器 (portfoliomanager.h/cpp)

### 功能概述

`PortfolioManager` 负责管理投资组合，包括账户信息、持仓信息等。当订单成交时，自动更新持仓和账户信息。

### 核心代码逻辑

#### 类定义

```cpp
class PortfolioManager {
public:
    static PortfolioManager* pinstance_;
    static mutex instancelock_;
    static PortfolioManager& instance();
    
    uint64_t _count = 0;
    
    // 账户和持仓信息
    AccountInfo account_;                                    // 账户信息
    map<string, AccountInfo> accinfomap_;                  // 账户名 -> 账户信息
    map<string, std::shared_ptr<Position>> positions_;     // 持仓键 -> 持仓
    double cash_;                                           // 现金余额
    
    // 操作方法
    void reset();                                           // 重置
    void rebuild();                                         // 重建
    void Add(std::shared_ptr<Position> ppos);              // 添加持仓
    double Adjust(const Fill& fill);                       // 根据成交调整持仓
    std::shared_ptr<Position> retrievePosition(const string& key);  // 查询持仓
};
```

#### 持仓调整

```cpp
double PortfolioManager::Adjust(const Fill& fill) {
    // 1. 构建持仓键（通常为：账户ID + 合约符号）
    string key = fill.accountID_ + "_" + fill.fullSymbol_;
    
    // 2. 查找或创建持仓
    auto it = positions_.find(key);
    std::shared_ptr<Position> pos;
    
    if (it == positions_.end()) {
        // 创建新持仓
        pos = make_shared<Position>();
        pos->key_ = key;
        pos->fullSymbol_ = fill.fullSymbol_;
        pos->accountID_ = fill.accountID_;
        pos->size_ = 0;
        pos->avgPrice_ = 0.0;
        positions_[key] = pos;
    } else {
        pos = it->second;
    }
    
    // 3. 更新持仓
    int32_t oldSize = pos->size_;
    double oldAvgPrice = pos->avgPrice_;
    
    // 计算新的持仓数量和平均价格
    if (fill.side_ == OrderSide::BUY) {
        // 买入：增加持仓
        pos->size_ += fill.size_;
    } else {
        // 卖出：减少持仓
        pos->size_ -= fill.size_;
    }
    
    // 计算新的平均价格
    if (oldSize == 0) {
        // 首次持仓
        pos->avgPrice_ = fill.price_;
    } else if ((oldSize > 0 && pos->size_ > 0) || (oldSize < 0 && pos->size_ < 0)) {
        // 同向持仓：加权平均
        pos->avgPrice_ = (oldSize * oldAvgPrice + fill.size_ * fill.price_) / pos->size_;
    } else {
        // 反向持仓：重置平均价格
        pos->avgPrice_ = fill.price_;
    }
    
    // 4. 更新账户现金
    if (fill.side_ == OrderSide::BUY) {
        cash_ -= fill.size_ * fill.price_;  // 买入：减少现金
    } else {
        cash_ += fill.size_ * fill.price_;  // 卖出：增加现金
    }
    
    // 5. 计算盈亏（需要当前价格，这里简化处理）
    // TODO: 根据当前市场价格计算未实现盈亏
    
    return 1.0;
}
```

**持仓更新逻辑：**
1. **买入成交**：持仓数量增加，现金减少
2. **卖出成交**：持仓数量减少，现金增加
3. **平均价格**：
   - 首次持仓：使用成交价格
   - 同向持仓：加权平均
   - 反向持仓：重置为新价格

#### 持仓查询

```cpp
std::shared_ptr<Position> PortfolioManager::retrievePosition(const string& key) {
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }
    return nullptr;
}
```

## 3. 风险管理器 (riskmanager.h/cpp)

### 功能概述

`RiskManager` 负责风险控制，在下单前检查订单是否符合风险限制条件。

### 核心代码逻辑

#### 类定义

```cpp
class RiskManager {
public:
    static RiskManager* pinstance_;
    static mutex instancelock_;
    static RiskManager& instance();
    
    bool alive_;                                            // 是否启用风险检查
    
    // 单笔订单限制
    int32_t limitSizePerOrder_ = 100;                      // 单笔订单数量限制
    double limitCashPerOrder_ = 100000;                     // 单笔订单金额限制
    
    // 每日总限制
    int32_t limitOrderCount_ = 100;                        // 每日订单数量限制
    int32_t limitCash_ = 100000;                           // 每日订单金额限制
    int32_t limitOrderSize_ = 100;                         // 每日订单总数量限制
    
    // 流量限制
    int32_t limitOrderCountPerSec_ = 10;                   // 每秒订单数量限制
    
    // 当前计数
    int32_t totalOrderCount_ = 0;                          // 今日订单总数
    double totalCash_ = 0.0;                               // 今日订单总金额
    int32_t totalOrderSize_ = 0;                           // 今日订单总数量
    int32_t orderCountPerSec_ = 0;                         // 当前秒订单数
    
    // 风险检查
    bool passOrder(std::shared_ptr<Order> o);              // 检查订单是否通过风险控制
    
    // 重置
    void reset();                                           // 重置（从配置读取）
    void switchday();                                       // 切换交易日（重置每日计数）
    void resetflow();                                       // 重置流量计数（每秒调用）
};
```

#### 风险检查

```cpp
bool RiskManager::passOrder(std::shared_ptr<Order> o) {
    // 1. 如果风险检查未启用，直接通过
    if (!alive_) {
        return true;
    }
    
    // 2. 更新计数
    totalOrderCount_ += 1;
    totalOrderSize_ += abs(o->quantity_);
    orderCountPerSec_ += 1;
    
    // 3. 计算订单金额
    double orderCash = abs(o->quantity_) * o->price_;
    totalCash_ += orderCash;
    
    // 4. 检查各项限制
    bool ocok = (totalOrderCount_ <= limitOrderCount_);           // 每日订单数量
    bool osok = (totalOrderSize_ <= limitOrderSize_);            // 每日订单总数量
    bool cashok = (totalCash_ <= limitCash_);                    // 每日订单总金额
    bool ospok = (abs(o->quantity_) <= limitSizePerOrder_);      // 单笔订单数量
    bool cashpok = (orderCash <= limitCashPerOrder_);            // 单笔订单金额
    bool ocpsok = (orderCountPerSec_ <= limitOrderCountPerSec_); // 每秒订单数量
    
    // 5. 所有条件都满足才通过
    if (ocok && osok && cashok && ospok && cashpok && ocpsok) {
        return true;
    }
    
    // 6. 记录拒绝原因
    fmt::printf("Risk check failed: totalcount:{}, totalsize:{}, "
                "sizeperorder:{}, countpersecond:{}",
                ocok, osok, ospok, ocpsok);
    
    return false;
}
```

**风险检查项：**
1. **单笔订单限制**：数量、金额
2. **每日总限制**：订单数量、总数量、总金额
3. **流量限制**：每秒订单数量

#### 重置方法

```cpp
void RiskManager::reset() {
    // 从配置读取风险参数
    alive_ = CConfig::instance().riskcheck;
    limitSizePerOrder_ = CConfig::instance().sizeperorderlimit;
    limitCashPerOrder_ = CConfig::instance().cashperorderlimit;
    limitOrderCount_ = CConfig::instance().ordercountlimit;
    limitCash_ = CConfig::instance().cashlimit;
    limitOrderSize_ = CConfig::instance().ordersizelimit;
    limitOrderCountPerSec_ = CConfig::instance().ordercountperseclimit;
    
    // 重置计数
    totalOrderCount_ = 0;
    totalCash_ = 0.0;
    totalOrderSize_ = 0;
    orderCountPerSec_ = 0;
}

void RiskManager::switchday() {
    // 切换交易日：重置每日计数
    totalOrderCount_ = 0;
    totalCash_ = 0.0;
    totalOrderSize_ = 0;
}

void RiskManager::resetflow() {
    // 重置流量计数（每秒调用）
    orderCountPerSec_ = 0;
}
```

**调用时机：**
- `reset()`: 系统启动时、配置更新时
- `switchday()`: 每天20:30切换交易日时
- `resetflow()`: 每秒定时任务调用

## 4. 保证金管理器 (marginmanager.h/cpp)

### 功能概述

`MarginManager` 负责计算和管理保证金，根据合约信息和持仓计算所需的保证金。

### 核心功能

（注：当前实现较简单，主要功能待完善）

## 5. 计算工具 (calc.h/cpp)

### 功能概述

提供各种计算工具函数，如盈亏计算、保证金计算等。

### 主要功能

（注：当前实现较简单，主要功能待完善）

## 6. 模块协作关系

### 订单流程中的协作

```
下单请求
  ↓
RiskManager::passOrder()  ← 风险检查
  ↓ (通过)
Engine::insertOrder()      ← 引擎处理
  ↓
OrderManager::trackOrder() ← 开始跟踪
  ↓
交易接口
  ↓
订单确认 → OrderManager::gotOrder()
  ↓
订单成交 → OrderManager::gotFill()
  ↓
PortfolioManager::Adjust() ← 更新持仓
```

### 数据流向

```
订单 → OrderManager → 状态跟踪
成交 → OrderManager → PortfolioManager → 持仓更新
查询 → OrderManager/PortfolioManager → 返回数据
```

## 7. 线程安全

所有管理器都使用互斥锁保护共享数据：

- `OrderManager::wlock`: 保护订单映射的写操作
- `orderStatus_mtx`: 保护订单状态更新
- `PortfolioManager::instancelock_`: 保护单例创建
- `RiskManager::instancelock_`: 保护单例创建

## 8. 设计模式总结

### 1. 单例模式
- 所有管理器都采用单例模式
- 确保全局唯一实例
- 线程安全的双重检查锁定

### 2. 管理器模式
- 每个管理器负责特定的业务领域
- 提供统一的接口和操作

### 3. 观察者模式
- 订单状态变化通知相关模块
- 成交事件触发持仓更新

## 总结

`Trade` 目录实现了交易系统的核心业务逻辑：

1. **OrderManager**: 完整的订单生命周期管理
2. **PortfolioManager**: 投资组合和持仓管理
3. **RiskManager**: 完善的风险控制机制
4. **模块协作**: 各模块紧密协作，共同完成交易功能

这些模块共同构成了交易系统的业务核心，确保了订单的正确处理、持仓的准确维护和风险的有效控制。
