#include <Services/message_bus.h>
#include <Common/config.h>
#include <Common/msgq.h>
#include <Common/util.h>

namespace StarQuant {

void InitTradingEngineMessengers() {
    if (CMsgqEMessenger::msgq_send_ == nullptr) {
        CMsgqEMessenger::msgq_send_ = MakeMsgqPub(CConfig::instance().SERVERPUB_URL);
        msleep(100);
    }
    if (CMsgqRMessenger::msgq_send_ == nullptr) {
        CMsgqRMessenger::msgq_send_ = MakeMsgqPub(CConfig::instance().SERVERSUB_URL);
        msleep(100);
    }
}

}  // namespace StarQuant
