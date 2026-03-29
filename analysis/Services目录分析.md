# Services 目录详细分析

## 目录概述

`Services` 目录实现了系统的服务层，是系统的核心协调模块。它负责管理引擎、处理消息中继、提供数据服务等功能。主要包括交易引擎服务、数据服务、策略服务等。

## 文件结构

```
Services/
├── tradingengine.h/cpp      # 交易引擎服务（核心）
├── sqserver.cpp             # 服务器入口程序
├── dataservice.h/cpp        # 数据服务
├── strategyservice.h/cpp    # 策略服务
└── starquant_.cpp           # Python接口
```

## 1. 交易引擎服务 (tradingengine.h/cpp)

### 功能概述

`tradingengine` 类是系统的核心服务类，负责：
1. 初始化和管理所有引擎（MD/TD引擎）
2. 消息中继和路由
3. 定时任务和系统维护
4. 系统生命周期管理

### 核心代码逻辑

#### 类定义

```cpp
class tradingengine {
private:
    RUN_MODE mode = RUN_MODE::TRADE_MODE;     // 运行模式
    BROKERS _broker = BROKERS::PAPER;          // 经纪商类型
    vector<std::thread*> threads_;              // 引擎线程列表
    vector<std::shared_ptr<IEngine>> pengines_; // 引擎实例列表
    std::unique_ptr<IMessenger> msg_relay_;    // 消息中继器
    std::shared_ptr<SQLogger> logger;          // 日志记录器
    
public:
    tradingengine();
    ~tradingengine();
    
    int32_t run();           // 运行主循环
    int32_t cronjobs(bool force = true);  // 定时任务
    bool live() const;       // 检查是否运行中
};
```

#### 构造函数：系统初始化

```cpp
tradingengine::tradingengine() {
    // 1. 初始化单例管理器
    CConfig::instance();           // 配置管理器
    DataManager::instance();       // 数据管理器
    OrderManager::instance();       // 订单管理器
    PortfolioManager::instance();  // 投资组合管理器
    RiskManager::instance();       // 风险管理器
    
    // 2. 获取配置
    _broker = CConfig::instance()._broker;
    mode = CConfig::instance()._mode;
    
    // 3. 初始化日志
    if (logger == nullptr) {
        logger = SQLogger::getLogger("SYS");
    }
    
    // 4. 创建引擎消息发送队列（PUB模式，所有引擎共享）
    if (CMsgqEMessenger::msgq_send_ == nullptr) {
        CMsgqEMessenger::msgq_send_ = std::make_unique<CMsgqNanomsg>(
            MSGQ_PROTOCOL::PUB,
            CConfig::instance().SERVERPUB_URL
        );
        msleep(100);  // 等待绑定完成
    }
    
    // 5. 创建中继消息发送队列（PUB模式，转发到引擎）
    if (CMsgqRMessenger::msgq_send_ == nullptr) {
        CMsgqRMessenger::msgq_send_ = std::make_unique<CMsgqNanomsg>(
            MSGQ_PROTOCOL::PUB,
            CConfig::instance().SERVERSUB_URL
        );
        msleep(100);
    }
    
    // 6. 创建消息中继器（PULL模式，接收客户端请求）
    msg_relay_ = std::make_unique<CMsgqRMessenger>(
        CConfig::instance().SERVERPULL_URL
    );
    msleep(100);
}
```

**初始化顺序说明：**
1. 单例管理器必须在最前面初始化
2. 消息队列需要先创建发送端，再创建接收端
3. 每个步骤之间有短暂延迟，确保资源就绪

#### 运行主循环：run()

```cpp
int32_t tradingengine::run() {
    if (gShutdown) return 1;
    
    // 1. 设置异常处理
    g_except_stack.flag = sigsetjmp(g_except_stack.env, 1);
    if (!g_except_stack.isDef()) {
        signal(SIGSEGV, recvSignal);  // 注册段错误处理
        
        try {
            // 2. 启动定时任务线程（异步执行）
            auto fu1 = async(
                launch::async,
                std::bind(&tradingengine::cronjobs, this, std::placeholders::_1),
                true
            );
            
            // 3. 根据运行模式启动不同的服务
            if (mode == RUN_MODE::RECORD_MODE) {
                LOG_INFO(logger, "RECORD_MODE");
                // 启动数据记录服务
                // threads_.push_back(new thread(TickRecordingService));
                
            } else if (mode == RUN_MODE::REPLAY_MODE) {
                LOG_INFO(logger, "REPLAY_MODE");
                // 启动数据回放服务
                // threads_.push_back(new thread(
                //     TickReplayService,
                //     CConfig::instance().filetoreplay,
                //     CConfig::instance()._tickinterval
                // ));
                
            } else if (mode == RUN_MODE::TRADE_MODE) {
                LOG_INFO(logger, "TRADE_MODE");
                
                // 4. 根据配置创建并启动引擎
                for (auto iter = CConfig::instance()._gatewaymap.begin();
                     iter != CConfig::instance()._gatewaymap.end();
                     iter++) {
                    
                    if (iter->second.api == "CTP.TD") {
                        // 创建CTP交易引擎
                        std::shared_ptr<IEngine> ctptdengine = 
                            make_shared<CtpTDEngine>(iter->first);
                        threads_.push_back(new std::thread(startengine, ctptdengine));
                        pengines_.push_back(ctptdengine);
                        
                    } else if (iter->second.api == "CTP.MD") {
                        // 创建CTP行情引擎
                        std::shared_ptr<IEngine> ctpmdengine = 
                            make_shared<CtpMDEngine>();
                        pengines_.push_back(ctpmdengine);
                        threads_.push_back(new std::thread(startengine, ctpmdengine));
                        
                    } else if (iter->second.api == "PAPER.TD") {
                        // 创建模拟交易引擎
                        std::shared_ptr<IEngine> papertdengine = 
                            make_shared<PaperTDEngine>();
                        threads_.push_back(new std::thread(startengine, papertdengine));
                        pengines_.push_back(papertdengine);
                        
                    } else if (iter->second.api == "TAP.TD") {
                        // TODO: TAP交易引擎
                    } else if (iter->second.api == "TAP.MD") {
                        // TODO: TAP行情引擎
                    } else {
                        LOG_INFO(logger, "API not supported, ignore it!");
                    }
                }
            } else {
                LOG_ERROR(logger, "Mode doesn't exist, exit.");
                return 1;
            }
            
            // 5. 设置CPU亲和性（可选）
            if (CConfig::instance().cpuaffinity) {
                int32_t num_cpus = std::thread::hardware_concurrency();
                for (int32_t i = 0; i < threads_.size(); i++) {
                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    CPU_SET(i % num_cpus, &cpuset);
                    pthread_setaffinity_np(
                        threads_[i]->native_handle(),
                        sizeof(cpu_set_t),
                        &cpuset
                    );
                }
                // 主线程也设置CPU亲和性
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(threads_.size() % num_cpus, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
            }
            
            // 6. 主循环：消息中继
            while (!gShutdown) {
                msg_relay_->relay();  // 持续中继消息
            }
            
            // 7. 等待定时任务线程结束
            fu1.get();
            
        } catch (exception& e) {
            LOG_INFO(logger, e.what());
        } catch (...) {
            LOG_ERROR(logger, "StarQuant terminated in error!");
        }
    } else {
        signal(SIGSEGV, SIG_IGN);
        LOG_ERROR(logger, "StarQuant terminated by SEGSEGV!");
        exit(0);
    }
    
    return 0;
}
```

**关键流程说明：**

1. **引擎创建**：根据配置文件中的网关信息，创建对应的引擎实例
2. **线程管理**：每个引擎运行在独立线程中，通过 `startengine()` 函数启动
3. **消息中继**：主线程持续调用 `relay()` 方法，转发客户端消息到引擎
4. **异常处理**：使用 `sigsetjmp/siglongjmp` 捕获段错误

#### 定时任务：cronjobs()

```cpp
int32_t tradingengine::cronjobs(bool force) {
    // 1. 设置控制台信号处理
    signal(SIGINT, ConsoleControlHandler);   // Ctrl-C
    signal(SIGPWR, ConsoleControlHandler);   // 电源信号
    
    time_t timer;
    struct tm tm_info;
    
    // 2. 定时任务循环
    while (!gShutdown) {
        msleep(1 * 1000);  // 每秒检查一次
        
        time(&timer);
        localtime_r(&timer, &tm_info);
        
        // 3. 周末不执行任务
        if (tm_info.tm_wday == 0) {  // 0=Sunday
            continue;
        }
        
        // 4. 发送定时消息到所有引擎
        std::shared_ptr<MsgHeader> pmsg = make_shared<MsgHeader>(
            DESTINATION_ALL,
            "0",
            MSG_TYPE_TIMER
        );
        msg_relay_->send(pmsg);
        
        // 5. 重置流量计数（每秒）
        RiskManager::instance().resetflow();
        
        // 6. 自动重置（每天2:35）
        if (tm_info.tm_hour == 2 && tm_info.tm_min == 35 && tm_info.tm_sec == 0) {
            std::shared_ptr<MsgHeader> pmsg = make_shared<MsgHeader>(
                DESTINATION_ALL,
                "0",
                MSG_TYPE_ENGINE_RESET
            );
            msg_relay_->send(pmsg);
        }
        
        // 7. 自动重置（工作日16:00）
        if (tm_info.tm_wday != 6) {  // 6=Saturday
            if (tm_info.tm_hour == 16 && tm_info.tm_min == 0 && tm_info.tm_sec == 0) {
                std::shared_ptr<MsgHeader> pmsg = make_shared<MsgHeader>(
                    DESTINATION_ALL,
                    "0",
                    MSG_TYPE_ENGINE_RESET
                );
                msg_relay_->send(pmsg);
            }
            
            // 8. 切换交易日（每天20:30）
            if (tm_info.tm_hour == 20 && tm_info.tm_min == 30 && tm_info.tm_sec == 0) {
                std::shared_ptr<MsgHeader> pmsg = make_shared<MsgHeader>(
                    DESTINATION_ALL,
                    "0",
                    MSG_TYPE_SWITCH_TRADING_DAY
                );
                msg_relay_->send(pmsg);
                RiskManager::instance().switchday();  // 重置风险管理器
            }
            
            // 9. 自动连接（8:45, 13:15, 20:45）
            if ((tm_info.tm_hour == 8 && tm_info.tm_min == 45) ||
                (tm_info.tm_hour == 13 && tm_info.tm_min == 15) ||
                (tm_info.tm_hour == 20 && tm_info.tm_min == 45)) {
                if (tm_info.tm_sec == 0) {
                    std::shared_ptr<MsgHeader> pmsg = make_shared<MsgHeader>(
                        DESTINATION_ALL,
                        "0",
                        MSG_TYPE_ENGINE_CONNECT
                    );
                    msg_relay_->send(pmsg);
                }
            }
        }
    }
    
    // 10. Ctrl-C触发关闭
    if (force) {
        throw runtime_error("ctrl-c triggered shutdown");
    }
    
    return 0;
}
```

**定时任务说明：**

- **每秒任务**：发送定时消息、重置流量计数
- **2:35**：自动重置引擎（夜盘结束后）
- **16:00**：自动重置引擎（日盘结束后）
- **20:30**：切换交易日（重置风险管理器）
- **8:45/13:15/20:45**：自动连接引擎（交易时段开始前）

#### 析构函数：资源清理

```cpp
tradingengine::~tradingengine() {
    // 1. 停止所有引擎
    for (auto& e : pengines_) {
        while ((e != nullptr) && (e->estate_ != STOP)) {
            e->stop();
            msleep(100);
        }
    }
    
    // 2. 等待所有微服务结束
    while (MICRO_SERVICE_NUMBER > 0) {
        msleep(100);
    }
    
    // 3. 清理nanomsg资源
    if (CConfig::instance()._msgq == MSGQ::NANOMSG) {
        nn_term();
    }
    
    // 4. 等待所有线程结束
    for (thread* t : threads_) {
        if (t->joinable()) {
            t->join();
            delete t;
        }
    }
    
    LOG_DEBUG(logger, "Exit trading engine");
}
```

#### 消息中继机制

消息中继是系统的核心通信机制，负责在客户端和引擎之间转发消息：

```cpp
// 在CMsgqRMessenger::relay()中实现
void CMsgqRMessenger::relay() {
    // 1. 从PULL队列接收客户端消息
    string msgpull = msgq_recv_->recmsg(0);
    if (msgpull.empty()) return;
    
    // 2. 检查消息类型
    if (msgpull[0] == RELAY_DESTINATION) {
        // 特殊标志 '@'：消息需要返回给策略进程
        lock_guard<std::mutex> g(CMsgqEMessenger::sendlock_);
        CMsgqEMessenger::msgq_send_->sendmsg(msgpull);
    } else {
        // 普通消息：转发到各个引擎
        lock_guard<std::mutex> g(CMsgqRMessenger::sendlock_);
        CMsgqRMessenger::msgq_send_->sendmsg(msgpull);
    }
}
```

**消息流向：**

```
客户端 → PULL队列 → relay() → PUB队列 → 引擎(SUB)
                                    ↓
策略进程 ← PUB队列 ← 引擎(发送) ← 引擎处理
```

## 2. 服务器入口 (sqserver.cpp)

### 功能概述

`sqserver.cpp` 是系统的入口程序，创建并运行交易引擎服务。

### 核心代码

```cpp
int32_t main(int32_t argc, char** argv) {
    std::cout << "StarQuant Server.\n"
              << "Version: 1.0beta,"
              << "Build:" << __DATE__ << "  " << __TIME__ << endl;
    
    tradingengine engine;
    std::cout << "Type Ctrl-C to exit\n\n";
    engine.run();
    
    return 0;
}
```

**执行流程：**
1. 创建 `tradingengine` 实例（初始化所有资源）
2. 调用 `run()` 方法启动主循环
3. 等待用户按 Ctrl-C 退出

## 3. 数据服务 (dataservice.h/cpp)

### 功能概述

数据服务提供数据记录、回放、聚合等功能。

### 主要服务函数

#### DataBoardService（数据看板服务）

```cpp
void DataBoardService() {
    // 订阅行情数据
    std::unique_ptr<CMsgq> msgq_sub_ = std::make_unique<CMsgqNanomsg>(
        MSGQ_PROTOCOL::SUB,
        CConfig::instance().SERVERPUB_URL,
        false
    );
    
    while (!gShutdown) {
        string msg = msgq_sub_->recmsg(0);
        if (!msg.empty()) {
            // 解析消息
            vector<string> vs = stringsplit(msg, SERIALIZATION_SEPARATOR);
            
            // 更新数据管理器
            if ((MSG_TYPE)(atoi(vs[0].c_str())) == MSG_TYPE::MSG_TYPE_Trade) {
                Tick k;
                k.fullSymbol_ = vs[1];
                k.time_ = vs[2];
                k.price_ = atof(vs[3].c_str());
                k.size_ = atoi(vs[4].c_str());
                
                DataManager::instance().updateOrderBook(k);
            }
        }
    }
}
```

#### TickRecordingService（行情记录服务）

```cpp
void TickRecordingService() {
    TickWriter writer;
    
    std::unique_ptr<CMsgq> msgq_sub_ = std::make_unique<CMsgqNanomsg>(
        MSGQ_PROTOCOL::SUB,
        CConfig::instance().SERVERPUB_URL,
        false
    );
    
    while (!gShutdown) {
        string msg = msgq_sub_->recmsg(0);
        if (!msg.empty()) {
            // 解析并写入文件
            Tick tick;
            // ... 解析消息
            writer.write(tick);
        }
    }
}
```

#### TickReplayService（行情回放服务）

```cpp
void TickReplayService(const std::string& filetoreplay, int32_t tickinterval = 0) {
    TickReader reader(filetoreplay);
    
    std::unique_ptr<CMsgq> msgq_pub_ = std::make_unique<CMsgqNanomsg>(
        MSGQ_PROTOCOL::PUB,
        CConfig::instance().SERVERPUB_URL
    );
    
    Tick tick;
    while (reader.read(tick)) {
        // 创建行情消息
        TickMsg msg(DESTINATION_ALL, "REPLAY", tick);
        
        // 发布消息
        msgq_pub_->sendmsg(msg.serialize());
        
        // 按间隔延迟
        if (tickinterval > 0) {
            msleep(tickinterval);
        }
    }
}
```

## 4. 策略服务 (strategyservice.h/cpp)

### 功能概述

策略服务负责管理策略实例，处理策略与系统的交互。

### 核心代码逻辑

```cpp
void StrategyManagerService() {
    // 1. 创建消息队列
    std::unique_ptr<CMsgq> msgq_sub_ = std::make_unique<CMsgqNanomsg>(
        MSGQ_PROTOCOL::SUB,
        CConfig::instance().SERVERPUB_URL,
        false
    );
    
    std::unique_ptr<CMsgq> msgq_pair_ = std::make_unique<CMsgqNanomsg>(
        MSGQ_PROTOCOL::PAIR,
        CConfig::instance().BROKERAGE_PAIR_PORT,
        false
    );
    
    // 2. 加载策略
    auto strategy = make_unique<SmaCross>();
    strategy->initialize();
    
    // 3. 主循环
    while (!gShutdown) {
        // 接收行情数据
        string msg = msgq_sub_->recmsg(0);
        if (!msg.empty()) {
            vector<string> vs = stringsplit(msg, SERIALIZATION_SEPARATOR);
            if (vs.size() == 6 || vs.size() == 33) {
                Tick k;
                k.fullSymbol_ = vs[0];
                k.time_ = vs[1];
                k.msgtype_ = (MSG_TYPE)(atoi(vs[2].c_str()));
                k.price_ = atof(vs[3].c_str());
                k.size_ = atoi(vs[4].c_str());
                k.depth_ = atoi(vs[5].c_str());
                
                // 调用策略的OnTick方法
                strategy->OnTick(k);
            }
        }
        
        // 发送策略产生的订单
        while (!strategy->msgstobrokerage.empty()) {
            string smsg = strategy->msgstobrokerage.front();
            msgq_pair_->sendmsg(smsg);
            strategy->msgstobrokerage.pop();
        }
        
        // 接收订单回报
        msg = msgq_pair_->recmsg(1);
        if (!msg.empty()) {
            strategy->OnGeneralMessage(msg);
        }
    }
}
```

## 5. 系统架构总结

### 消息流向图

```
                    ┌─────────────┐
                    │  客户端     │
                    └──────┬──────┘
                           │ PULL
                           ↓
              ┌──────────────────────┐
              │  tradingengine       │
              │  (消息中继)          │
              └──────┬───────────┬───┘
                     │ PUB       │ PUB
                     ↓           ↓
            ┌────────┴──┐   ┌───┴────────┐
            │ CTP.MD    │   │ CTP.TD     │
            │ Engine    │   │ Engine     │
            └─────┬─────┘   └─────┬──────┘
                  │ SUB           │ SUB
                  └───────┬───────┘
                          │ PUB
                          ↓
              ┌──────────────────────┐
              │  策略/数据服务       │
              │  (SUB模式)           │
              └──────────────────────┘
```

### 线程模型

```
主线程 (tradingengine::run)
├── 定时任务线程 (cronjobs)
├── CTP.MD引擎线程
├── CTP.TD引擎线程
├── PaperTD引擎线程
└── 其他服务线程（可选）
```

### 设计模式

1. **单例模式**：各种管理器（Config、DataManager等）
2. **工厂模式**：引擎创建
3. **观察者模式**：消息订阅/发布
4. **策略模式**：不同的运行模式（TRADE_MODE、REPLAY_MODE等）

## 总结

`Services` 目录是系统的核心协调层：

1. **tradingengine**：系统的核心服务，管理所有引擎和消息路由
2. **消息中继**：实现客户端与引擎之间的消息转发
3. **定时任务**：自动化的系统维护和操作
4. **数据服务**：提供数据记录、回放等功能
5. **策略服务**：管理策略实例和交互

整个服务层采用事件驱动架构，通过消息队列实现模块间的解耦和异步通信。
