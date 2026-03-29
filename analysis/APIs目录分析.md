# APIs 目录详细分析

## 目录概述

`APIs` 目录包含了各种交易接口的头文件定义，这些是第三方交易接口的SDK头文件。系统通过这些接口与不同的交易平台进行交互。

## 文件结构

```
APIs/
├── Ctp/                    # CTP接口（上期技术）
│   ├── ThostFtdcMdApi.h          # 行情API
│   ├── ThostFtdcTraderApi.h      # 交易API
│   ├── ThostFtdcUserApiDataType.h
│   ├── ThostFtdcUserApiStruct.h
│   └── DataCollect.h
├── Tap/                    # TAP接口（易盛）
│   ├── TapQuoteAPI.h             # 行情API
│   ├── TapTradeAPI.h             # 交易API
│   └── TapAPI*.h
├── ITap/                   # ITap接口（易盛国际版）
└── Xtp/                    # XTP接口（迅投）
    ├── xtp_quote_api.h
    └── xtp_trader_api.h
```

## 1. CTP接口 (Ctp/)

### 功能概述

CTP（Comprehensive Transaction Platform）是上海期货信息技术有限公司开发的综合交易平台接口，是中国期货市场最主流的交易接口。

### 核心文件

#### ThostFtdcMdApi.h（行情API）

**主要类：**

```cpp
// 行情API类
class CThostFtdcMdApi {
public:
    // 创建API实例
    static CThostFtdcMdApi* CreateFtdcMdApi(const char* pszFlowPath);
    
    // 注册回调接口
    void RegisterSpi(CThostFtdcMdSpi* pSpi);
    
    // 注册前置地址
    void RegisterFront(char* pszFrontAddress);
    
    // 初始化
    void Init();
    
    // 订阅行情
    int ReqUserLogin(CThostFtdcReqUserLoginField* pReqUserLogin, int nRequestID);
    int SubscribeMarketData(char* ppInstrumentID[], int nCount);
    
    // 获取API版本
    static const char* GetApiVersion();
    
    // 释放API
    void Release();
};
```

**回调接口（Spi）：**

```cpp
class CThostFtdcMdSpi {
public:
    // 连接回调
    virtual void OnFrontConnected();
    virtual void OnFrontDisconnected(int nReason);
    
    // 登录回调
    virtual void OnRspUserLogin(
        CThostFtdcRspUserLoginField* pRspUserLogin,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast
    );
    
    // 订阅回调
    virtual void OnRspSubMarketData(
        CThostFtdcSpecificInstrumentField* pSpecificInstrument,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast
    );
    
    // 行情数据回调
    virtual void OnRtnDepthMarketData(
        CThostFtdcDepthMarketDataField* pDepthMarketData
    );
};
```

#### ThostFtdcTraderApi.h（交易API）

**主要类：**

```cpp
// 交易API类
class CThostFtdcTraderApi {
public:
    // 创建API实例
    static CThostFtdcTraderApi* CreateFtdcTraderApi(const char* pszFlowPath);
    
    // 注册回调接口
    void RegisterSpi(CThostFtdcTraderSpi* pSpi);
    
    // 注册前置地址
    void RegisterFront(char* pszFrontAddress);
    
    // 订阅流类型
    void SubscribePublicTopic(THOST_TE_RESUME_TYPE nResumeType);
    void SubscribePrivateTopic(THOST_TE_RESUME_TYPE nResumeType);
    
    // 初始化
    void Init();
    
    // 认证和登录
    int ReqAuthenticate(CThostFtdcReqAuthenticateField* pReqAuthenticateField, int nRequestID);
    int ReqUserLogin(CThostFtdcReqUserLoginField* pReqUserLogin, int nRequestID);
    
    // 订单操作
    int ReqOrderInsert(CThostFtdcInputOrderField* pInputOrder, int nRequestID);
    int ReqOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction, int nRequestID);
    
    // 查询操作
    int ReqQryTradingAccount(CThostFtdcQryTradingAccountField* pQryTradingAccount, int nRequestID);
    int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField* pQryInvestorPosition, int nRequestID);
    int ReqQryInstrument(CThostFtdcQryInstrumentField* pQryInstrument, int nRequestID);
    
    // 释放API
    void Release();
};
```

**回调接口（Spi）：**

```cpp
class CThostFtdcTraderSpi {
public:
    // 连接回调
    virtual void OnFrontConnected();
    virtual void OnFrontDisconnected(int nReason);
    
    // 认证回调
    virtual void OnRspAuthenticate(
        CThostFtdcRspAuthenticateField* pRspAuthenticateField,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast
    );
    
    // 登录回调
    virtual void OnRspUserLogin(
        CThostFtdcRspUserLoginField* pRspUserLogin,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast
    );
    
    // 订单回调
    virtual void OnRspOrderInsert(
        CThostFtdcInputOrderField* pInputOrder,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast
    );
    
    virtual void OnRtnOrder(CThostFtdcOrderField* pOrder);
    virtual void OnRtnTrade(CThostFtdcTradeField* pTrade);
    
    // 查询回调
    virtual void OnRspQryTradingAccount(
        CThostFtdcTradingAccountField* pTradingAccount,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast
    );
    
    virtual void OnRspQryInvestorPosition(
        CThostFtdcInvestorPositionField* pInvestorPosition,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast
    );
};
```

#### 数据结构

**订单结构：**

```cpp
struct CThostFtdcInputOrderField {
    char BrokerID[11];              // 经纪公司代码
    char InvestorID[13];             // 投资者代码
    char InstrumentID[31];           // 合约代码
    char OrderRef[13];               // 报单引用
    char UserID[16];                // 用户代码
    char OrderPriceType;             // 报单价格条件
    char Direction;                  // 买卖方向
    char CombOffsetFlag[5];          // 组合开平标志
    char CombHedgeFlag[5];          // 组合投机套保标志
    double LimitPrice;               // 价格
    int VolumeTotalOriginal;         // 数量
    char TimeCondition;              // 有效期类型
    char VolumeCondition;            // 成交量类型
    int MinVolume;                   // 最小成交量
    char ContingentCondition;        // 触发条件
    double StopPrice;                // 止损价
    char ForceCloseReason;           // 强平原因
    int IsAutoSuspend;               // 自动挂起标志
    char UserForceClose;             // 用户强评标志
};
```

**行情数据结构：**

```cpp
struct CThostFtdcDepthMarketDataField {
    char TradingDay[9];              // 交易日
    char InstrumentID[31];           // 合约代码
    char ExchangeID[9];              // 交易所代码
    double LastPrice;                 // 最新价
    double PreSettlementPrice;       // 上次结算价
    double PreClosePrice;             // 昨收盘
    double PreOpenInterest;           // 昨持仓量
    double OpenPrice;                 // 今开盘
    double HighestPrice;              // 最高价
    double LowestPrice;               // 最低价
    int Volume;                       // 数量
    double Turnover;                  // 成交金额
    double OpenInterest;              // 持仓量
    double ClosePrice;                // 今收盘
    double SettlementPrice;           // 本次结算价
    double UpperLimitPrice;           // 涨停板价
    double LowerLimitPrice;           // 跌停板价
    double PreDelta;                  // 昨虚实度
    double CurrDelta;                 // 今虚实度
    char UpdateTime[9];               // 最后修改时间
    int UpdateMillisec;               // 最后修改毫秒
    double BidPrice1;                 // 申买价一
    int BidVolume1;                   // 申买量一
    double AskPrice1;                 // 申卖价一
    int AskVolume1;                   // 申卖量一
    // ... 更多买卖盘数据
};
```

### CTP接口使用流程

1. **创建API实例**
   ```cpp
   CThostFtdcMdApi* api = CThostFtdcMdApi::CreateFtdcMdApi("./flow");
   ```

2. **注册回调**
   ```cpp
   api->RegisterSpi(this);  // this实现CThostFtdcMdSpi接口
   ```

3. **注册前置地址**
   ```cpp
   api->RegisterFront("tcp://180.168.146.187:10010");
   ```

4. **初始化**
   ```cpp
   api->Init();
   ```

5. **等待连接回调**
   ```cpp
   void OnFrontConnected() {
       // 发送登录请求
   }
   ```

6. **登录**
   ```cpp
   CThostFtdcReqUserLoginField req;
   strcpy(req.BrokerID, "9999");
   strcpy(req.UserID, "user");
   strcpy(req.Password, "pass");
   api->ReqUserLogin(&req, 1);
   ```

7. **订阅行情**
   ```cpp
   char* instruments[] = {"cu2305", "al2305"};
   api->SubscribeMarketData(instruments, 2);
   ```

8. **接收行情**
   ```cpp
   void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) {
       // 处理行情数据
   }
   ```

## 2. TAP接口 (Tap/)

### 功能概述

TAP（Trade API Platform）是易盛信息技术有限公司开发的交易接口，主要用于期货交易。

### 核心特点

- 支持多账户管理
- 支持组合合约
- 提供完整的交易和行情功能

### 主要文件

- `TapQuoteAPI.h`: 行情API
- `TapTradeAPI.h`: 交易API
- `TapAPICommDef.h`: 通用定义
- `TapAPIError.h`: 错误码定义

## 3. XTP接口 (Xtp/)

### 功能概述

XTP（迅投）是A股市场的交易接口，主要用于股票交易。

### 核心特点

- 支持A股交易
- 低延迟
- 支持多种订单类型

### 主要文件

- `xtp_quote_api.h`: 行情API
- `xtp_trader_api.h`: 交易API
- `xtp_api_struct.h`: 数据结构定义

## 4. 接口适配

### 统一抽象

系统通过 `IEngine` 基类统一抽象不同的交易接口：

```cpp
class IEngine {
    // 统一的引擎接口
    virtual bool connect() = 0;
    virtual bool disconnect() = 0;
};

class CtpMDEngine : public IEngine, public CThostFtdcMdSpi {
    // CTP行情引擎实现
};

class TapMDEngine : public IEngine, public TapQuoteAPI {
    // TAP行情引擎实现
};
```

### 数据转换

系统定义了统一的数据结构（`Tick`、`Order`等），引擎负责将接口特定的数据结构转换为系统统一格式：

```cpp
// CTP数据转换为系统Tick
Tick CtpToTick(CThostFtdcDepthMarketDataField* ctpData) {
    Tick tick;
    tick.fullSymbol_ = CtpSymbolToSecurityFullName(ctpData->InstrumentID);
    tick.price_ = ctpData->LastPrice;
    tick.size_ = ctpData->Volume;
    // ... 更多字段转换
    return tick;
}
```

## 5. 接口选择

系统根据配置选择使用哪个接口：

```cpp
// 在tradingengine.cpp中
for (auto iter = CConfig::instance()._gatewaymap.begin();
     iter != CConfig::instance()._gatewaymap.end();
     iter++) {
    
    if (iter->second.api == "CTP.TD") {
        // 创建CTP交易引擎
    } else if (iter->second.api == "CTP.MD") {
        // 创建CTP行情引擎
    } else if (iter->second.api == "TAP.TD") {
        // 创建TAP交易引擎
    }
    // ...
}
```

## 6. 接口版本管理

每个接口目录下可能包含版本信息文件：

- `Ctp/version6.3.15`: CTP版本信息
- `Xtp/version1_1_18.txt`: XTP版本信息

## 总结

`APIs` 目录包含了各种交易接口的SDK头文件：

1. **CTP接口**：中国期货市场主流接口
2. **TAP接口**：易盛交易接口
3. **XTP接口**：A股交易接口
4. **统一抽象**：通过引擎层统一不同接口
5. **数据转换**：将接口特定数据转换为系统统一格式

这些接口文件是第三方SDK，系统通过继承和实现这些接口的回调类来与交易平台交互。引擎层负责适配这些接口，提供统一的抽象给上层使用。
