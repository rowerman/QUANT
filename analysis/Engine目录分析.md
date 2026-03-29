# Engine 目录详细分析

## 目录概述

`Engine` 目录实现了交易系统的核心引擎模块，负责与各种交易接口（CTP、TAP、XTP等）进行交互。引擎分为两类：**行情引擎（MD Engine）**和**交易引擎（TD Engine）**。

## 文件结构

```
Engine/
├── IEngine.h/cpp           # 引擎基类接口
├── CtpMDEngine.h/cpp       # CTP行情引擎
├── CtpTDEngine.h/cpp       # CTP交易引擎
├── TapMDEngine.h/cpp       # TAP行情引擎
├── TapTDEngine.h/cpp       # TAP交易引擎
└── PaperTDEngine.h/cpp     # 模拟交易引擎
```

## 1. 引擎基类 (IEngine.h/cpp)

### 功能概述

`IEngine` 是所有引擎的抽象基类，定义了引擎的基本接口和通用功能。

### 核心代码逻辑

#### 引擎状态枚举

```cpp
enum EState : int32_t {
    DISCONNECTED = 0,      // 初始状态：未连接
    CONNECTING,            // 连接中
    CONNECT_ACK,           // 连接确认（CTP：前置连接成功；TAP：登录成功）
    AUTHENTICATING,        // 认证中
    AUTHENTICATE_ACK,      // 认证成功（CTP交易认证）
    LOGINING,              // 登录中
    LOGIN_ACK,             // 登录成功（CTP登录成功；TAP API就绪）
    LOGOUTING,             // 登出中
    STOP                   // 停止状态
};
```

#### 基类定义

```cpp
class IEngine {
public:
    std::atomic<EState> estate_;              // 引擎状态（原子变量，线程安全）
    std::unique_ptr<IMessenger> messenger_;    // 消息传递器
    
    IEngine();
    virtual ~IEngine();
    
    virtual void init();                       // 初始化
    virtual void start();                     // 启动引擎
    virtual void stop();                      // 停止引擎
    virtual bool connect() = 0;               // 连接（纯虚函数）
    virtual bool disconnect() = 0;            // 断开连接（纯虚函数）
    
protected:
    std::shared_ptr<SQLogger> logger;         // 日志记录器
};
```

**设计要点：**
- 使用原子变量 `estate_` 保证状态访问的线程安全
- 每个引擎实例拥有独立的消息传递器
- 采用模板方法模式，子类实现具体的连接/断开逻辑

## 2. CTP行情引擎 (CtpMDEngine.h/cpp)

### 功能概述

`CtpMDEngine` 负责连接CTP行情接口，接收实时行情数据并转发给系统其他模块。

### 核心代码逻辑

#### 类定义

```cpp
class CtpMDEngine : public IEngine, public CThostFtdcMdSpi {
public:
    string name_;                    // 引擎名称（"CTP.MD"）
    Gateway ctpacc_;                 // CTP账户配置信息
    
    // 订阅/取消订阅
    void subscribe(const vector<string>& symbols, SymbolType st = ST_Ctp);
    void unsubscribe(const vector<string>& symbols, SymbolType st = ST_Ctp);
    
    // CTP回调接口实现
    virtual void OnFrontConnected();
    virtual void OnFrontDisconnected(int32_t nReason);
    virtual void OnRspUserLogin(...);
    virtual void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField *pDepthMarketData);
    // ... 更多回调
};
```

**继承关系：**
- `IEngine`: 提供引擎基础功能
- `CThostFtdcMdSpi`: CTP行情接口回调类

#### 初始化流程

```cpp
void CtpMDEngine::init() {
    // 1. 创建日志记录器
    logger = SQLogger::getLogger("MDEngine.CTP");
    
    // 2. 创建消息传递器（SUB模式，订阅服务器消息）
    messenger_ = std::make_unique<CMsgqEMessenger>(
        name_, 
        CConfig::instance().SERVERSUB_URL
    );
    
    // 3. 获取网关配置
    ctpacc_ = CConfig::instance()._gatewaymap[name_];
    
    // 4. 创建CTP API对象
    string path = CConfig::instance().logDir() + "/ctp/md";
    this->api_ = CThostFtdcMdApi::CreateFtdcMdApi(path.c_str());
    this->api_->RegisterSpi(this);  // 注册回调接口
    
    // 5. 设置初始状态
    estate_ = DISCONNECTED;
    
    // 6. 发送状态消息
    auto pmsgs = make_shared<InfoMsg>(
        DESTINATION_ALL, name_,
        MSG_TYPE_ENGINE_STATUS,
        to_string(estate_)
    );
    messenger_->send(pmsgs);
}
```

#### 启动流程（消息循环）

```cpp
void CtpMDEngine::start() {
    while (estate_ != EState::STOP) {
        // 1. 接收消息（非阻塞）
        auto pmsgin = messenger_->recv(msgqMode_);
        
        // 2. 检查消息是否应该处理
        bool processmsg = (
            (pmsgin != nullptr) &&
            (startwith(pmsgin->destination_, DESTINATION_ALL) || 
             (pmsgin->destination_ == name_))
        );
        
        if (processmsg) {
            switch (pmsgin->msgtype_) {
                case MSG_TYPE_ENGINE_CONNECT:
                    if (connect()) {
                        // 发送连接成功消息
                    }
                    break;
                    
                case MSG_TYPE_SUBSCRIBE_MARKET_DATA:
                    if (estate_ == LOGIN_ACK) {
                        auto pmsgin2 = static_pointer_cast<SubscribeMsg>(pmsgin);
                        subscribe(pmsgin2->data_, pmsgin2->symtype_);
                    }
                    break;
                    
                case MSG_TYPE_UNSUBSCRIBE:
                    if (estate_ == LOGIN_ACK) {
                        auto pmsgin2 = static_pointer_cast<UnSubscribeMsg>(pmsgin);
                        unsubscribe(pmsgin2->data_, pmsgin2->symtype_);
                    }
                    break;
                    
                case MSG_TYPE_TIMER:
                    timertask();  // 定时任务
                    break;
                    
                // ... 更多消息处理
            }
        }
        
        // 3. 处理缓冲区数据
        processbuf();
    }
}
```

#### 连接流程

```cpp
bool CtpMDEngine::connect() {
    if (inconnectaction_) return false;
    inconnectaction_ = true;
    
    // 1. 注册前置地址
    for (auto addr : ctpacc_.md_address) {
        api_->RegisterFront(const_cast<char*>(addr.c_str()));
    }
    
    // 2. 初始化API
    api_->Init();
    apiinited_ = true;
    estate_ = CONNECTING;
    
    inconnectaction_ = false;
    return true;
}
```

#### CTP回调处理

**1. 前置连接回调**

```cpp
void CtpMDEngine::OnFrontConnected() {
    LOG_INFO(logger, name_ << " OnFrontConnected");
    estate_ = CONNECT_ACK;
    
    // 发送登录请求
    CThostFtdcReqUserLoginField req;
    strcpy(req.BrokerID, ctpacc_.brokerid.c_str());
    strcpy(req.UserID, ctpacc_.userid.c_str());
    strcpy(req.Password, ctpacc_.password.c_str());
    
    int ret = api_->ReqUserLogin(&req, ++loginReqId_);
    if (ret != 0) {
        LOG_ERROR(logger, "Send login request failed: " << ret);
    }
}
```

**2. 登录响应回调**

```cpp
void CtpMDEngine::OnRspUserLogin(
    CThostFtdcRspUserLoginField *pRspUserLogin,
    CThostFtdcRspInfoField *pRspInfo,
    int32_t nRequestID,
    bool bIsLast
) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR(logger, "Login failed: " << pRspInfo->ErrorMsg);
        estate_ = DISCONNECTED;
        return;
    }
    
    LOG_INFO(logger, name_ << " Login success");
    estate_ = LOGIN_ACK;
    
    // 自动订阅之前的订阅列表
    if (!lastsubs_.empty()) {
        subscribe(lastsubs_, ST_Ctp);
    }
}
```

**3. 行情数据回调**

```cpp
void CtpMDEngine::OnRtnDepthMarketData(
    CThostFtdcDepthMarketDataField *pDepthMarketData
) {
    // 1. 转换CTP数据结构为系统Tick结构
    Tick tick;
    tick.fullSymbol_ = CtpSymbolToSecurityFullName(pDepthMarketData->InstrumentID);
    tick.time_ = pDepthMarketData->UpdateTime;
    tick.price_ = pDepthMarketData->LastPrice;
    tick.size_ = pDepthMarketData->Volume;
    tick.bidPrice_[0] = pDepthMarketData->BidPrice1;
    tick.bidSize_[0] = pDepthMarketData->BidVolume1;
    tick.askPrice_[0] = pDepthMarketData->AskPrice1;
    tick.askSize_[0] = pDepthMarketData->AskVolume1;
    // ... 填充更多字段
    
    // 2. 创建行情消息
    auto ptickmsg = make_shared<TickMsg>(
        DESTINATION_ALL,
        name_,
        tick
    );
    
    // 3. 发送行情消息
    messenger_->send(ptickmsg);
    
    // 4. 更新数据管理器
    DataManager::instance().updateOrderBook(tick);
}
```

#### 订阅/取消订阅

```cpp
void CtpMDEngine::subscribe(const vector<string>& symbols, SymbolType st) {
    vector<char*> instruments;
    
    for (const auto& symbol : symbols) {
        string ctpsymbol;
        if (st == ST_Ctp) {
            ctpsymbol = symbol;
        } else {
            ctpsymbol = SecurityFullNameToCtpSymbol(symbol);
        }
        
        char* inst = new char[ctpsymbol.length() + 1];
        strcpy(inst, ctpsymbol.c_str());
        instruments.push_back(inst);
    }
    
    // 调用CTP订阅接口
    int ret = api_->SubscribeMarketData(
        instruments.data(),
        instruments.size()
    );
    
    if (ret == 0) {
        lastsubs_ = symbols;  // 保存订阅列表
        LOG_INFO(logger, "Subscribe " << symbols.size() << " instruments");
    }
    
    // 释放内存
    for (auto inst : instruments) {
        delete[] inst;
    }
}
```

## 3. CTP交易引擎 (CtpTDEngine.h/cpp)

### 功能概述

`CtpTDEngine` 负责连接CTP交易接口，处理订单提交、撤单、查询等交易相关操作。

### 核心代码逻辑

#### 类定义

```cpp
class CtpTDEngine : public IEngine, public CThostFtdcTraderSpi {
public:
    string name_;
    bool needauthentication_;      // 是否需要认证
    bool needsettlementconfirm_;    // 是否需要结算确认
    bool issettleconfirmed_;        // 是否已确认结算
    Gateway ctpacc_;
    
    // 订单操作
    void insertOrder(shared_ptr<CtpOrderMsg> pmsg);
    void cancelOrder(shared_ptr<OrderActionMsg> pmsg);
    void cancelAll(shared_ptr<CancelAllMsg> pmsg);
    
    // 查询操作
    void queryAccount(shared_ptr<MsgHeader> pmsg);
    void queryPosition(shared_ptr<MsgHeader> pmsg);
    void queryContract(shared_ptr<QryContractMsg> pmsg);
    void queryOrder(shared_ptr<MsgHeader> pmsg);
    void queryTrade(shared_ptr<MsgHeader> pmsg);
    
    // CTP回调接口实现
    virtual void OnFrontConnected();
    virtual void OnRspAuthenticate(...);
    virtual void OnRspUserLogin(...);
    virtual void OnRspOrderInsert(...);
    virtual void OnRtnOrder(CThostFtdcOrderField *pOrder);
    virtual void OnRtnTrade(CThostFtdcTradeField *pTrade);
    // ... 更多回调
};
```

#### 初始化流程

```cpp
void CtpTDEngine::init() {
    // 1. 创建日志记录器
    logger = SQLogger::getLogger("TDEngine.CTP");
    
    // 2. 创建消息传递器
    messenger_ = std::make_unique<CMsgqEMessenger>(
        name_,
        CConfig::instance().SERVERSUB_URL
    );
    
    // 3. 获取网关配置
    string acc = accAddress(name_);
    ctpacc_ = CConfig::instance()._gatewaymap[name_];
    
    // 4. 创建CTP交易API对象
    string path = CConfig::instance().logDir() + "/ctp/td/" + acc;
    this->api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(path.c_str());
    this->api_->RegisterSpi(this);
    
    // 5. 判断是否需要认证
    if (ctpacc_.auth_code == "NA" || ctpacc_.auth_code.empty()) {
        needauthentication_ = false;
    } else {
        needauthentication_ = true;
    }
    
    // 6. 初始化状态
    estate_ = DISCONNECTED;
    reqId_ = 0;
    orderRef_ = 0;
    lastQryTime_ = getMicroTime();
}
```

#### 连接和登录流程

CTP交易接口的连接流程比行情接口复杂，需要经过：连接 → 认证（可选）→ 登录 → 结算确认

```cpp
bool CtpTDEngine::connect() {
    if (inconnectaction_) return false;
    inconnectaction_ = true;
    
    // 1. 注册前置地址
    for (auto addr : ctpacc_.td_address) {
        api_->RegisterFront(const_cast<char*>(addr.c_str()));
    }
    
    // 2. 设置订阅流类型
    api_->SubscribePublicTopic(THOST_TERT_RESTART);
    api_->SubscribePrivateTopic(THOST_TERT_RESTART);
    
    // 3. 初始化API
    api_->Init();
    apiinited_ = true;
    estate_ = CONNECTING;
    
    inconnectaction_ = false;
    return true;
}

// 前置连接回调
void CtpTDEngine::OnFrontConnected() {
    estate_ = CONNECT_ACK;
    
    if (needauthentication_) {
        // 发送认证请求
        CThostFtdcReqAuthenticateField req;
        strcpy(req.BrokerID, ctpacc_.brokerid.c_str());
        strcpy(req.UserID, ctpacc_.userid.c_str());
        strcpy(req.AuthCode, ctpacc_.auth_code.c_str());
        strcpy(req.AppID, ctpacc_.appid.c_str());
        
        api_->ReqAuthenticate(&req, ++reqId_);
        estate_ = AUTHENTICATING;
    } else {
        // 直接发送登录请求
        sendLoginRequest();
    }
}

// 认证成功回调
void CtpTDEngine::OnRspAuthenticate(...) {
    if (pRspInfo->ErrorID == 0) {
        estate_ = AUTHENTICATE_ACK;
        sendLoginRequest();
    }
}

// 发送登录请求
void CtpTDEngine::sendLoginRequest() {
    CThostFtdcReqUserLoginField req;
    strcpy(req.BrokerID, ctpacc_.brokerid.c_str());
    strcpy(req.UserID, ctpacc_.userid.c_str());
    strcpy(req.Password, ctpacc_.password.c_str());
    
    api_->ReqUserLogin(&req, ++reqId_);
    estate_ = LOGINING;
}

// 登录成功回调
void CtpTDEngine::OnRspUserLogin(...) {
    if (pRspInfo->ErrorID == 0) {
        frontID_ = pRspUserLogin->FrontID;
        sessionID_ = pRspUserLogin->SessionID;
        orderRef_ = atoi(pRspUserLogin->MaxOrderRef);
        
        estate_ = LOGIN_ACK;
        
        // 如果需要结算确认，发送结算确认请求
        if (needsettlementconfirm_) {
            CThostFtdcSettlementInfoConfirmField req;
            strcpy(req.BrokerID, ctpacc_.brokerid.c_str());
            strcpy(req.InvestorID, ctpacc_.userid.c_str());
            api_->ReqSettlementInfoConfirm(&req, ++reqId_);
        }
        
        // 自动查询账户和持仓
        if (autoqry_) {
            queryAccount(nullptr);
            queryPosition(nullptr);
        }
    }
}
```

#### 下单流程

```cpp
void CtpTDEngine::insertOrder(shared_ptr<CtpOrderMsg> pmsg) {
    // 1. 风险检查
    if (CConfig::instance().riskcheck) {
        if (!RiskManager::instance().passOrder(pmsg->data_)) {
            // 发送错误消息
            return;
        }
    }
    
    // 2. 创建CTP订单结构
    CThostFtdcInputOrderField order;
    memset(&order, 0, sizeof(order));
    
    // 3. 填充订单字段
    strcpy(order.BrokerID, ctpacc_.brokerid.c_str());
    strcpy(order.InvestorID, ctpacc_.userid.c_str());
    strcpy(order.InstrumentID, 
           SecurityFullNameToCtpSymbol(pmsg->data_.fullSymbol_).c_str());
    
    order.OrderRef = ++orderRef_;
    order.OrderPriceType = OrderTypeToCtpPriceType(pmsg->data_.orderType_);
    order.Direction = OrderSideToCtpDirection(pmsg->data_.side_);
    order.CombOffsetFlag[0] = OrderFlagToCtpComboOffsetFlag(pmsg->data_.flag_);
    order.LimitPrice = pmsg->data_.price_;
    order.VolumeTotalOriginal = pmsg->data_.size_;
    order.TimeCondition = THOST_FTDC_TC_GFD;  // 当日有效
    order.VolumeCondition = THOST_FTDC_VC_AV;  // 任意数量
    order.MinVolume = 1;
    order.ContingentCondition = THOST_FTDC_CC_Immediately;
    order.StopPrice = 0;
    order.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    order.IsAutoSuspend = 0;
    order.UserForceClose = 0;
    
    // 4. 生成服务器订单ID并跟踪
    int64_t serverOrderId = OrderManager::instance().generateOrderId();
    OrderManager::instance().trackOrder(pmsg->data_);
    
    // 5. 发送订单请求
    int ret = api_->ReqOrderInsert(&order, ++reqId_);
    
    if (ret != 0) {
        LOG_ERROR(logger, "Insert order failed: " << ret);
        // 发送错误消息
    } else {
        LOG_INFO(logger, "Insert order: " << pmsg->data_.fullSymbol_ 
                 << " " << pmsg->data_.side_ << " " << pmsg->data_.size_);
    }
}
```

#### 订单回报处理

```cpp
void CtpTDEngine::OnRtnOrder(CThostFtdcOrderField *pOrder) {
    // 1. 查找对应的订单
    string acc = accAddress(name_);
    auto order = OrderManager::instance().retrieveOrderFromAccAndBrokerOrderId(
        acc, pOrder->OrderRef
    );
    
    if (!order) {
        LOG_WARN(logger, "Order not found: " << pOrder->OrderRef);
        return;
    }
    
    // 2. 转换CTP订单状态
    OrderStatus status = CtpOrderStatusToOrderStatus(pOrder->OrderStatus);
    order->status_ = status;
    
    // 3. 更新订单信息
    order->filledSize_ = pOrder->VolumeTraded;
    order->avgFillPrice_ = pOrder->VolumeTraded > 0 ? 
        pOrder->VolumeTraded * pOrder->LimitPrice / pOrder->VolumeTraded : 0;
    
    // 4. 通知订单管理器
    if (status == OS_Acknowledged) {
        OrderManager::instance().gotOrder(order->serverOrderId_);
    }
    
    // 5. 发送订单回报消息
    auto pordermsg = make_shared<OrderMsg>(
        DESTINATION_ALL,
        name_,
        *order
    );
    messenger_->send(pordermsg);
}
```

#### 成交回报处理

```cpp
void CtpTDEngine::OnRtnTrade(CThostFtdcTradeField *pTrade) {
    // 1. 查找对应的订单
    string acc = accAddress(name_);
    auto order = OrderManager::instance().retrieveOrderFromAccAndBrokerOrderId(
        acc, pTrade->OrderRef
    );
    
    if (!order) {
        LOG_WARN(logger, "Order not found for trade: " << pTrade->OrderRef);
        return;
    }
    
    // 2. 创建成交对象
    Fill fill;
    fill.serverOrderId_ = order->serverOrderId_;
    fill.fullSymbol_ = CtpSymbolToSecurityFullName(pTrade->InstrumentID);
    fill.price_ = pTrade->Price;
    fill.size_ = pTrade->Volume;
    fill.time_ = string(pTrade->TradeTime);
    fill.exchange_ = string(pTrade->ExchangeID);
    fill.tradeID_ = string(pTrade->TradeID);
    
    // 3. 通知订单管理器
    OrderManager::instance().gotFill(fill);
    
    // 4. 更新投资组合
    PortfolioManager::instance().Adjust(fill);
    
    // 5. 发送成交回报消息
    auto pfillmsg = make_shared<FillMsg>(
        DESTINATION_ALL,
        name_,
        fill
    );
    messenger_->send(pfillmsg);
}
```

#### 查询操作

查询操作使用队列缓冲，避免查询频率过高：

```cpp
void CtpTDEngine::processbuf() {
    // 检查查询队列
    if (!qryBuffer_.empty()) {
        uint64_t now = getMicroTime();
        if (now - lastQryTime_ > 1000000) {  // 间隔1秒
            auto pmsg = qryBuffer_.front();
            qryBuffer_.pop();
            lastQryTime_ = now;
            
            switch (pmsg->msgtype_) {
                case MSG_TYPE_QRY_ACCOUNT:
                    queryAccount(pmsg);
                    break;
                case MSG_TYPE_QRY_POS:
                    queryPosition(pmsg);
                    break;
                // ... 更多查询类型
            }
        }
    }
}

void CtpTDEngine::queryAccount(shared_ptr<MsgHeader> pmsg) {
    CThostFtdcQryTradingAccountField req;
    memset(&req, 0, sizeof(req));
    strcpy(req.BrokerID, ctpacc_.brokerid.c_str());
    strcpy(req.InvestorID, ctpacc_.userid.c_str());
    
    int ret = api_->ReqQryTradingAccount(&req, ++reqId_);
    // ...
}

void CtpTDEngine::OnRspQryTradingAccount(...) {
    if (pRspInfo->ErrorID == 0 && pTradingAccount) {
        // 转换账户信息并发送
        AccountInfo acc;
        acc.accountID_ = accAddress(name_);
        acc.availableFunds_ = pTradingAccount->Available;
        acc.netLiquidation_ = pTradingAccount->Balance;
        // ... 填充更多字段
        
        auto paccmsg = make_shared<AccMsg>(
            pmsg ? pmsg->source_ : DESTINATION_ALL,
            name_,
            acc
        );
        messenger_->send(paccmsg);
    }
}
```

## 4. 模拟交易引擎 (PaperTDEngine.h/cpp)

### 功能概述

`PaperTDEngine` 是一个模拟交易引擎，不连接真实交易接口，用于策略回测和模拟交易。

### 核心特点

1. **无需真实连接**：不连接任何交易接口
2. **即时成交**：订单立即成交（按当前价格）
3. **本地管理**：持仓和账户信息在本地维护
4. **用于回测**：主要用于策略回测和模拟交易

### 核心代码逻辑

```cpp
void PaperTDEngine::insertOrder(shared_ptr<PaperOrderMsg> pmsg) {
    // 1. 风险检查
    if (!RiskManager::instance().passOrder(pmsg->data_)) {
        return;
    }
    
    // 2. 获取当前价格（从DataManager）
    Tick tick = DataManager::instance().orderBook_[pmsg->data_.fullSymbol_];
    double fillPrice = tick.price_;
    
    // 3. 立即成交
    Fill fill;
    fill.serverOrderId_ = pmsg->data_.serverOrderId_;
    fill.fullSymbol_ = pmsg->data_.fullSymbol_;
    fill.price_ = fillPrice;
    fill.size_ = pmsg->data_.size_;
    fill.time_ = nowMS();
    
    // 4. 更新订单状态
    pmsg->data_.status_ = OS_Filled;
    OrderManager::instance().gotFill(fill);
    
    // 5. 更新投资组合
    PortfolioManager::instance().Adjust(fill);
    
    // 6. 发送成交回报
    auto pfillmsg = make_shared<FillMsg>(
        DESTINATION_ALL,
        name_,
        fill
    );
    messenger_->send(pfillmsg);
}
```

## 5. 引擎状态管理

### 状态转换图

```
DISCONNECTED → CONNECTING → CONNECT_ACK → [AUTHENTICATING → AUTHENTICATE_ACK] → LOGINING → LOGIN_ACK
                                                                                            ↓
                                                                                          STOP
```

### 状态检查

引擎在处理各种操作前都会检查状态：

```cpp
if (estate_ == LOGIN_ACK) {
    // 执行操作
} else {
    // 发送错误消息：引擎未连接
}
```

## 6. 错误处理

### 连接错误

- 前置连接失败：`OnFrontDisconnected()` 回调
- 登录失败：`OnRspUserLogin()` 中检查 `pRspInfo->ErrorID`
- 认证失败：`OnRspAuthenticate()` 中检查错误码

### 操作错误

- 下单失败：`OnRspOrderInsert()` 回调
- 撤单失败：`OnRspOrderAction()` 回调
- 查询失败：各种查询响应回调中检查错误码

### 错误消息发送

```cpp
auto pmsgout = make_shared<ErrorMsg>(
    pmsgin->source_,
    name_,
    MSG_TYPE_ERROR_INSERTORDER,
    "Insert order failed: " + errorMsg
);
messenger_->send(pmsgout);
```

## 7. 定时任务

每个引擎都有定时任务功能：

```cpp
void CtpMDEngine::timertask() {
    timercount_++;
    
    // 每60秒检查一次连接状态
    if (timercount_ % 60 == 0) {
        if (estate_ == LOGIN_ACK) {
            // 发送心跳或检查连接
        }
    }
}
```

## 8. 设计模式总结

### 1. 模板方法模式
- `IEngine` 定义算法骨架
- 子类实现具体步骤（connect、disconnect）

### 2. 策略模式
- 不同的引擎实现不同的交易接口
- 统一的接口，不同的实现

### 3. 观察者模式
- CTP回调接口（Spi）
- 引擎监听CTP事件并响应

### 4. 适配器模式
- CTP数据结构 → 系统数据结构
- 不同接口的统一抽象

## 总结

`Engine` 目录实现了系统的核心交易接口适配层：

1. **统一接口**：`IEngine` 提供统一的引擎接口
2. **多接口支持**：支持CTP、TAP、XTP等多种交易接口
3. **状态管理**：完善的状态机管理连接和登录流程
4. **消息通信**：通过消息队列与系统其他模块通信
5. **错误处理**：完善的错误处理和消息反馈机制
6. **模拟交易**：提供模拟交易引擎用于回测

引擎模块是连接外部交易接口和内部系统的桥梁，实现了交易接口的抽象和统一。
