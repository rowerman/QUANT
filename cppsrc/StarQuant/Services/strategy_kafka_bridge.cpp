#include <Services/strategy_kafka_bridge.h>
#include <Common/config.h>
#include <Common/msgq.h>
#include <Common/datastruct.h>
#include <atomic>

namespace StarQuant {

extern std::atomic<bool> gShutdown;

void StrategyKafkaBridgeService() {
    if (CConfig::instance()._msgq != MSGQ::KAFKA)
        return;
    const std::string topic = CConfig::instance().kafka_topic_strategy_to_broker;
    std::unique_ptr<CMsgq> consumer =
        MakeMsgqSub(topic, std::string("sq-strategy-bridge"));
    while (!gShutdown) {
        std::string m = consumer->recmsg(0);
        if (m.empty())
            continue;
        if (CMsgqRMessenger::msgq_send_ != nullptr) {
            std::lock_guard<std::mutex> g(CMsgqRMessenger::sendlock_);
            CMsgqRMessenger::msgq_send_->sendmsg(m);
        }
    }
}

}  // namespace StarQuant
