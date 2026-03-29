# Data 目录详细分析

## 目录概述

`Data` 目录实现了数据管理功能，包括实时行情数据管理、合约信息管理、数据记录和回放等功能。这是系统数据层的核心模块。

## 文件结构

```
Data/
├── datamanager.h/cpp    # 数据管理器
├── tickwriter.h         # 行情数据写入器
└── tickreader.h         # 行情数据读取器
```

## 1. 数据管理器 (datamanager.h/cpp)

### 功能概述

`DataManager` 负责管理系统的数据，包括：
1. 实时行情数据（OrderBook）
2. 合约信息（Security）
3. 合约符号映射（CTP符号 ↔ 完整符号）

### 核心代码逻辑

#### 类定义

```cpp
class DataManager {
public:
    static DataManager* pinstance_;
    static mutex instancelock_;
    static DataManager& instance();
    
    TickWriter recorder_;                                  // 行情记录器
    uint64_t count_ = 0;                                    // 数据计数
    bool contractUpdated_ = false;                         // 合约是否已更新
    bool saveSecurityFile_ = false;                        // 是否保存合约文件
    
    // 数据存储
    std::map<std::string, Security> securityDetails_;      // CTP符号 -> 合约信息
    std::map<string, Tick> orderBook_;                     // 合约符号 -> 最新行情
    std::map<string, string> ctp2Full_;                    // CTP符号 -> 完整符号
    std::map<string, string> full2Ctp_;                     // 完整符号 -> CTP符号
    
    DataManager();
    ~DataManager();
    
    void reset();                                          // 重置
    void rebuild();                                        // 重建
    
    // 数据更新
    void updateOrderBook(const Tick& k);                  // 更新行情
    void updateOrderBook(const Fill& fill);               // 根据成交更新行情
    
    // 合约管理
    void saveSecurityToFile();                             // 保存合约到文件
    void loadSecurityFile();                               // 从文件加载合约
};
```

#### 单例模式实现

```cpp
DataManager* DataManager::pinstance_ = nullptr;
mutex DataManager::instancelock_;

DataManager& DataManager::instance() {
    if (pinstance_ == nullptr) {
        lock_guard<mutex> g(instancelock_);
        if (pinstance_ == nullptr) {
            pinstance_ = new DataManager();
        }
    }
    return *pinstance_;
}
```

#### 构造函数

```cpp
DataManager::DataManager() : count_(0) {
    loadSecurityFile();  // 加载合约信息
}
```

#### 更新行情数据

```cpp
void DataManager::updateOrderBook(const Tick& k) {
    // 直接更新或插入行情数据
    orderBook_[k.fullSymbol_] = k;
    count_++;
}

void DataManager::updateOrderBook(const Fill& fill) {
    // 根据成交更新行情（假设只有价格变化）
    if (orderBook_.find(fill.fullSymbol_) != orderBook_.end()) {
        orderBook_[fill.fullSymbol_].price_ = fill.tradePrice_;
        orderBook_[fill.fullSymbol_].size_ = fill.tradeSize_;
    } else {
        // 创建新的行情数据
        Tick newk;
        newk.depth_ = 0;
        newk.price_ = fill.tradePrice_;
        newk.size_ = fill.tradeSize_;
        newk.fullSymbol_ = fill.fullSymbol_;
        orderBook_[fill.fullSymbol_] = newk;
    }
}
```

**使用场景：**
- `updateOrderBook(const Tick& k)`: 行情引擎收到行情数据时调用
- `updateOrderBook(const Fill& fill)`: 交易引擎收到成交回报时调用（用于模拟交易）

#### 加载合约信息

```cpp
void DataManager::loadSecurityFile() {
    try {
        // 1. 读取合约配置文件
        string contractpath = boost::filesystem::current_path().string() 
            + "/etc/ctpcontract.yaml";
        YAML::Node contractinfo = YAML::LoadFile(contractpath);
        
        // 2. 解析每个合约
        for (YAML::const_iterator symsec = contractinfo.begin();
             symsec != contractinfo.end();
             symsec++) {
            
            auto sym = symsec->first.as<std::string>();  // CTP符号
            auto securities = symsec->second;
            
            // 3. 创建Security对象
            Security sec;
            sec.symbol_ = securities["symbol"].as<std::string>();
            sec.exchange_ = securities["exchange"].as<std::string>();
            sec.securityType_ = securities["product"].as<char>();
            sec.multiplier_ = securities["size"].as<int32_t>();
            sec.localName_ = securities["name"].as<std::string>();
            sec.ticksize_ = securities["pricetick"].as<double>();
            sec.postype_ = securities["positiontype"].as<char>();
            sec.longMarginRatio_ = securities["long_margin_ratio"].as<double>();
            sec.shortMarginRatio_ = securities["short_margin_ratio"].as<double>();
            
            // 期权相关字段
            sec.underlyingSymbol_ = securities["option_underlying"].as<std::string>();
            sec.optionType_ = securities["option_type"].as<char>();
            sec.strikePrice_ = securities["option_strike"].as<double>();
            sec.expiryDate_ = securities["option_expiry"].as<std::string>();
            sec.fullSymbol_ = securities["full_symbol"].as<std::string>();
            
            // 4. 保存到映射表
            securityDetails_[sym] = sec;
            ctp2Full_[sym] = sec.fullSymbol_;
            full2Ctp_[sec.fullSymbol_] = sym;
        }
        
        // 5. 备份配置文件
        std::ofstream fout("etc/ctpcontract.yaml.bak");
        fout << contractinfo;
        
    } catch(exception &e) {
        fmt::print("Read contract exception:{}.", e.what());
    } catch(...) {
        fmt::print("Read contract error!");
    }
}
```

**合约配置文件格式（YAML）：**

```yaml
cu2305:
  symbol: cu2305
  exchange: SHFE
  product: '1'  # 1=期货, 2=期权, 3=组合
  size: 5
  name: 沪铜2305
  pricetick: 10.0
  positiontype: '2'
  long_margin_ratio: 0.1
  short_margin_ratio: 0.1
  option_underlying: ""
  option_type: ""
  option_strike: 0.0
  option_expiry: ""
  full_symbol: "SHFE F CU 2305"
```

#### 保存合约信息

```cpp
void DataManager::saveSecurityToFile() {
    try {
        YAML::Node securities;
        
        // 1. 遍历所有合约
        for (auto iterator = securityDetails_.begin();
             iterator != securityDetails_.end();
             ++iterator) {
            
            auto sym = iterator->first;
            auto sec = iterator->second;
            
            // 2. 构建YAML节点
            securities[sym]["symbol"] = sec.symbol_;
            securities[sym]["exchange"] = sec.exchange_;
            securities[sym]["product"] = sec.securityType_;
            securities[sym]["size"] = sec.multiplier_;
            securities[sym]["name"] = sec.localName_;
            securities[sym]["pricetick"] = sec.ticksize_;
            securities[sym]["positiontype"] = sec.postype_;
            securities[sym]["long_margin_ratio"] = sec.longMarginRatio_;
            securities[sym]["short_margin_ratio"] = sec.shortMarginRatio_;
            securities[sym]["option_underlying"] = sec.underlyingSymbol_;
            securities[sym]["option_type"] = sec.optionType_;
            securities[sym]["option_strike"] = sec.strikePrice_;
            securities[sym]["option_expiry"] = sec.expiryDate_;
            
            // 3. 构建完整符号
            string fullsym;
            string type;
            string product;
            string contracno;
            
            if (sec.securityType_ == '1' || sec.securityType_ == '2') {
                // 期货或期权
                int32_t i;
                for (i = 0; i < sym.size(); i++) {
                    if (isdigit(sym[i]))
                        break;
                }
                product = sym.substr(0, i);
                contracno = sym.substr(i);
                type = (sec.securityType_ == '1' ? "F" : "O");
                fullsym = sec.exchange_ + " " + type + " " 
                    + boost::to_upper_copy(product) + " " + contracno;
            } else if (sec.securityType_ == '3') {
                // 组合合约
                // ... 处理组合合约逻辑
            }
            
            securities[sym]["full_symbol"] = fullsym;
        }
        
        // 4. 写入文件
        std::ofstream fout("etc/ctpcontract.yaml");
        fout << securities;
        
    } catch(exception &e) {
        fmt::print("Save contract exception:{}.", e.what());
    }
}
```

## 2. 行情数据写入器 (tickwriter.h)

### 功能概述

`TickWriter` 负责将行情数据写入文件或数据库，支持文件写入和MongoDB存储。

### 核心代码逻辑

#### 结构定义

```cpp
struct TickWriter {
    int32_t bufSize;                                       // 缓冲区大小
    FILE* fp = nullptr;                                    // 文件指针
    int32_t count = 0;                                     // 缓冲区已用长度
    char* head = nullptr;                                  // 缓冲区指针
    
    // MongoDB相关
    bson_error_t error;
    mongoc_client_pool_t *pool;                            // 连接池
    mongoc_uri_t *uri;                                     // URI
    
    TickWriter();
    ~TickWriter();
    
    void put(const string& _str);                          // 写入文件
    void insertdb(const string& _str);                     // 插入数据库（字符串）
    void insertdb(const Tick& k);                          // 插入数据库（Tick对象）
};
```

#### 构造函数

```cpp
TickWriter::TickWriter() {
    bufSize = 1024;
    head = new char[bufSize];
    
    // 初始化MongoDB
    mongoc_init();
    uri = mongoc_uri_new("mongodb://localhost:27017");
    pool = mongoc_client_pool_new(uri);
}
```

#### 写入文件

```cpp
void TickWriter::put(const string& _str) {
    if (!_str.empty()) {
        // 1. 构建带时间戳的字符串
        char tmp[512] = {};
        sprintf(tmp, "%s @%s\n", ymdhmsf().c_str(), _str.c_str());
        uint32_t strsize = strlen(tmp);
        uint32_t required_buffer_len = count + strsize;
        
        // 2. 检查缓冲区是否足够
        if (required_buffer_len > bufSize) {
            // 3. 缓冲区满，写入文件
            size_t r = fwrite(head, sizeof(char), count, fp);
            if (r == count) {
                // 4. 清空缓冲区，写入新数据
                memcpy(head, tmp, strsize * sizeof(char));
                count = strsize;
                fflush(fp);
                return;
            } else {
                // 写入失败处理
            }
        }
        
        // 5. 追加到缓冲区
        memcpy(head + count, tmp, strsize * sizeof(char));
        count = required_buffer_len;
    }
}
```

**设计要点：**
- 使用缓冲区减少文件I/O次数
- 缓冲区满时批量写入
- 提高写入性能

#### 插入数据库（字符串格式）

```cpp
void TickWriter::insertdb(const string& _str) {
    if (!_str.empty()) {
        // 1. 解析消息字符串
        vector<string> vs = stringsplit(_str, SERIALIZATION_SEPARATOR);
        
        if ((MSG_TYPE)(atoi(vs[0].c_str())) == MSG_TYPE::MSG_TYPE_TICK_L1) {
            // 2. 解析完整符号
            vector<string> fullsym = stringsplit(vs[1], ' ');
            string collectionname = fullsym[2];  // 使用品种名作为集合名
            
            // 3. 获取MongoDB客户端和集合
            mongoc_client_t *client = mongoc_client_pool_pop(pool);
            mongoc_collection_t *collection = mongoc_client_get_collection(
                client, "findata", collectionname.c_str()
            );
            
            // 4. 构建BSON文档
            bson_t *doc = bson_new();
            BSON_APPEND_UTF8(doc, "contractno", fullsym[3].c_str());
            BSON_APPEND_DATE_TIME(doc, "datetime", 
                string2unixtimems(vs[2]) + 8*3600000);  // 转换为UTC+8
            BSON_APPEND_DOUBLE(doc, "price", atof(vs[3].c_str()));
            BSON_APPEND_INT32(doc, "size", atoi(vs[4].c_str()));
            BSON_APPEND_DOUBLE(doc, "bidprice1", atof(vs[5].c_str()));
            BSON_APPEND_INT32(doc, "bidsize1", atoi(vs[6].c_str()));
            BSON_APPEND_DOUBLE(doc, "askprice1", atof(vs[7].c_str()));
            BSON_APPEND_INT32(doc, "asksize1", atoi(vs[8].c_str()));
            BSON_APPEND_INT32(doc, "openinterest", atoi(vs[9].c_str()));
            BSON_APPEND_INT32(doc, "dominant", 0);
            
            // 5. 插入文档
            if (!mongoc_collection_insert(collection, MONGOC_INSERT_NONE, 
                doc, NULL, &error)) {
                fprintf(stderr, "Insert failed: %s\n", error.message);
            }
            
            // 6. 清理资源
            bson_destroy(doc);
            mongoc_collection_destroy(collection);
            mongoc_client_pool_push(pool, client);
        }
    }
}
```

**数据库结构：**
- 数据库名：`findata`
- 集合名：品种名（如：CU、AL等）
- 文档字段：合约号、时间、价格、成交量、买卖盘等

#### 插入数据库（Tick对象）

```cpp
void TickWriter::insertdb(const Tick& k) {
    // 1. 解析完整符号
    vector<string> fullsym = stringsplit(k.fullSymbol_, ' ');
    string collectionname = fullsym[2];
    
    // 2. 获取MongoDB客户端和集合
    mongoc_client_t *client = mongoc_client_pool_pop(pool);
    mongoc_collection_t *collection = mongoc_client_get_collection(
        client, "findata", collectionname.c_str()
    );
    
    // 3. 构建BSON文档
    bson_t *doc = bson_new();
    BSON_APPEND_UTF8(doc, "contractno", fullsym[3].c_str());
    BSON_APPEND_DATE_TIME(doc, "datetime", 
        string2unixtimems(k.time_) + 8*3600000);
    BSON_APPEND_DOUBLE(doc, "price", k.price_);
    BSON_APPEND_INT32(doc, "size", k.size_);
    BSON_APPEND_DOUBLE(doc, "bidprice1", k.bidPrice_[0]);
    BSON_APPEND_INT32(doc, "bidsize1", k.bidSize_[0]);
    BSON_APPEND_DOUBLE(doc, "askprice1", k.askPrice_[0]);
    BSON_APPEND_INT32(doc, "asksize1", k.askSize_[0]);
    BSON_APPEND_INT32(doc, "openinterest", k.openInterest_);
    BSON_APPEND_INT32(doc, "dominant", 0);
    
    // 4. 插入文档
    if (!mongoc_collection_insert(collection, MONGOC_INSERT_NONE, 
        doc, NULL, &error)) {
        cout << "insert mongodb failed, errormsg = " << error.message;
    }
    
    // 5. 清理资源
    bson_destroy(doc);
    mongoc_collection_destroy(collection);
    mongoc_client_pool_push(pool, client);
}
```

#### 析构函数

```cpp
TickWriter::~TickWriter() {
    // 1. 将缓冲区剩余数据写入文件
    if (fp) {
        fwrite(head, sizeof(char), count, fp);
        fflush(fp);
        fclose(fp);
    }
    
    // 2. 释放缓冲区
    delete[] head;
    
    // 3. 清理MongoDB资源
    mongoc_client_pool_destroy(pool);
    mongoc_uri_destroy(uri);
    mongoc_cleanup();
}
```

## 3. 行情数据读取器 (tickreader.h)

### 功能概述

`TickReader` 负责从文件读取历史行情数据，用于数据回放。

### 核心代码逻辑

#### 读取回放文件

```cpp
vector<string> readreplayfile(const string& filetoreplay) {
    // 1. 打开文件
    ifstream f(filetoreplay);
    vector<string> lines;
    string x;
    
    // 2. 逐行读取
    while (f.is_open() && f.good()) {
        getline(f, x);
        if (!x.empty()) {
            // 3. 解析格式：时间戳@消息内容
            vector<string> tmp = stringsplit(x, '@');
            if (tmp.size() == 2) {
                string tam = tmp[1];
                tam.erase(0, 1);  // 去除前导空格
                lines.push_back(tam);
            }
        }
    }
    
    return lines;
}
```

**文件格式：**
```
2023-05-01 09:00:00.000 @MSG_TYPE|symbol|time|price|size|...
2023-05-01 09:00:00.100 @MSG_TYPE|symbol|time|price|size|...
```

**使用场景：**
- 策略回测
- 数据回放
- 历史数据分析

## 4. 数据流向

### 实时数据流

```
行情引擎 → Tick数据 → DataManager::updateOrderBook() → orderBook_
                                                      ↓
策略/服务 ← 查询orderBook_ ← DataManager
```

### 数据记录流

```
行情引擎 → Tick数据 → TickWriter::put() → 文件
                ↓
        TickWriter::insertdb() → MongoDB
```

### 数据回放流

```
文件 → TickReader::readreplayfile() → 字符串数组 → 解析 → Tick数据 → 发布到消息队列
```

## 5. 设计模式总结

### 1. 单例模式
- `DataManager` 采用单例模式
- 确保全局唯一的数据管理实例

### 2. 管理器模式
- `DataManager` 统一管理所有数据
- 提供统一的数据访问接口

### 3. 适配器模式
- `TickWriter` 适配不同的存储后端（文件/MongoDB）
- 统一的写入接口，不同的实现

## 总结

`Data` 目录实现了系统的数据层功能：

1. **DataManager**: 统一的数据管理，包括行情数据和合约信息
2. **TickWriter**: 高效的数据写入，支持文件和数据库存储
3. **TickReader**: 数据回放功能，支持策略回测

数据层为上层模块（策略、引擎等）提供了统一的数据访问接口，同时支持数据的持久化存储和历史数据回放。
