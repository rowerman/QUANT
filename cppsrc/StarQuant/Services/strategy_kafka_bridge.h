#ifndef CPPSRC_STARQUANT_SERVICES_STRATEGY_KAFKA_BRIDGE_H_
#define CPPSRC_STARQUANT_SERVICES_STRATEGY_KAFKA_BRIDGE_H_

namespace StarQuant {

/** Forwards sq.strategy.to_broker -> engine command topic (replaces server-side PAIR bind). */
void StrategyKafkaBridgeService();

}  // namespace StarQuant

#endif
