#ifndef CPPSRC_STARQUANT_SERVICES_MESSAGE_BUS_H_
#define CPPSRC_STARQUANT_SERVICES_MESSAGE_BUS_H_

namespace StarQuant {

/** Initialize static PUB producers used by engines and relay (nanomsg or Kafka). */
void InitTradingEngineMessengers();

}  // namespace StarQuant

#endif
