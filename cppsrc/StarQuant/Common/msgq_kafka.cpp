/*****************************************************************************
 * Kafka-backed CMsgq (librdkafka). Active when SQ_HAVE_RDKAFKA is defined.
 *****************************************************************************/

#include <Common/msgq.h>
#include <Common/config.h>
#include <Common/logger.h>

#include <cstring>
#include <mutex>

#ifndef SQ_HAVE_RDKAFKA
#define SQ_HAVE_RDKAFKA 0
#endif

#if SQ_HAVE_RDKAFKA
#include <librdkafka/rdkafka.h>
#endif

namespace StarQuant {

#if SQ_HAVE_RDKAFKA

static std::string resolve_topic(const std::string& url) {
    const CConfig& c = CConfig::instance();
    if (url == c.SERVERPUB_URL)
        return c.kafka_topic_server_pub;
    if (url == c.SERVERSUB_URL)
        return c.kafka_topic_engine_commands;
    if (url == c.SERVERPULL_URL)
        return c.kafka_topic_client_requests;
    return url;
}

class CMsgqKafka : public CMsgq {
 public:
    CMsgqKafka(MSGQ_PROTOCOL protocol, const std::string& url, bool binding,
               const std::string& consumer_group)
        : CMsgq(protocol, url), rk_(nullptr), topic_(resolve_topic(url)), is_producer_(false) {
        (void)binding;
        char err[512] = {};
        rd_kafka_conf_t* conf = rd_kafka_conf_new();
        const std::string brokers = CConfig::instance().kafka_brokers;
        if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), err, sizeof(err)) !=
            RD_KAFKA_CONF_OK) {
            LOG_ERROR(logger, "Kafka conf bootstrap.servers: " << err);
            rd_kafka_conf_destroy(conf);
            return;
        }
        is_producer_ = (protocol == MSGQ_PROTOCOL::PUB || protocol == MSGQ_PROTOCOL::PUSH);
        if (is_producer_) {
            rd_kafka_conf_set(conf, "linger.ms", "1", err, sizeof(err));
            rk_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf, err, sizeof(err));
            if (!rk_) {
                LOG_ERROR(logger, "Kafka producer create failed: " << err);
                rd_kafka_conf_destroy(conf);
            }
        } else {
            std::string gid = consumer_group.empty() ? std::string("sq-default") : consumer_group;
            if (rd_kafka_conf_set(conf, "group.id", gid.c_str(), err, sizeof(err)) != RD_KAFKA_CONF_OK) {
                LOG_ERROR(logger, "Kafka conf group.id: " << err);
                rd_kafka_conf_destroy(conf);
                return;
            }
            rd_kafka_conf_set(conf, "enable.auto.commit", "true", err, sizeof(err));
            rd_kafka_conf_set(conf, "auto.offset.reset", "latest", err, sizeof(err));
            rk_ = rd_kafka_new(RD_KAFKA_CONSUMER, conf, err, sizeof(err));
            if (!rk_) {
                LOG_ERROR(logger, "Kafka consumer create failed: " << err);
                rd_kafka_conf_destroy(conf);
                return;
            }
            rd_kafka_poll_set_consumer(rk_);
            rd_kafka_topic_partition_list_t* topics = rd_kafka_topic_partition_list_new(1);
            rd_kafka_topic_partition_list_add(topics, topic_.c_str(), RD_KAFKA_PARTITION_UA);
            rd_kafka_resp_err_t serr = rd_kafka_subscribe(rk_, topics);
            rd_kafka_topic_partition_list_destroy(topics);
            if (serr) {
                LOG_ERROR(logger, "Kafka subscribe failed: " << rd_kafka_err2str(serr));
            }
        }
    }

    ~CMsgqKafka() override {
        if (rk_) {
            if (is_producer_)
                rd_kafka_flush(rk_, 5000);
            rd_kafka_destroy(rk_);
            rk_ = nullptr;
        }
    }

    void sendmsg(const std::string& str, int32_t dontwait) override {
        (void)dontwait;
        if (!rk_ || !is_producer_)
            return;
        const void* payload = str.data();
        size_t len = str.size();
    retry:
        rd_kafka_resp_err_t err =
            rd_kafka_producev(rk_, RD_KAFKA_V_TOPIC(topic_.c_str()), RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                              RD_KAFKA_V_VALUE(const_cast<void*>(payload), len), RD_KAFKA_V_END);
        if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
            rd_kafka_poll(rk_, 50);
            goto retry;
        }
        if (err)
            LOG_ERROR(logger, "Kafka produce failed: " << rd_kafka_err2str(err));
        rd_kafka_poll(rk_, 0);
    }

    void sendmsg(const char* str, int32_t dontwait) override {
        if (str)
            sendmsg(std::string(str), dontwait);
    }

    std::string recmsg(int32_t blockingflags) override {
        if (!rk_ || is_producer_)
            return {};
        int timeout_ms = (blockingflags == 0) ? 500 : 100;
        rd_kafka_message_t* rkm = rd_kafka_consumer_poll(rk_, timeout_ms);
        if (!rkm)
            return {};
        if (rkm->err) {
            rd_kafka_message_destroy(rkm);
            return {};
        }
        std::string out(static_cast<const char*>(rkm->payload),
                        static_cast<std::string::size_type>(rkm->len));
        rd_kafka_message_destroy(rkm);
        return out;
    }

 private:
    rd_kafka_t* rk_;
    std::string topic_;
    bool is_producer_;
};

class CMsgqKafkaPair : public CMsgq {
 public:
    CMsgqKafkaPair()
        : CMsgq(MSGQ_PROTOCOL::PAIR, CConfig::instance().BROKERAGE_PAIR_PORT),
          rk_prod_(nullptr),
          rk_cons_(nullptr),
          topic_out_(CConfig::instance().kafka_topic_strategy_to_broker),
          topic_in_(CConfig::instance().kafka_topic_broker_to_strategy) {
        char err[512] = {};
        const std::string brokers = CConfig::instance().kafka_brokers;
        rd_kafka_conf_t* c1 = rd_kafka_conf_new();
        rd_kafka_conf_set(c1, "bootstrap.servers", brokers.c_str(), err, sizeof(err));
        rd_kafka_conf_set(c1, "linger.ms", "1", err, sizeof(err));
        rk_prod_ = rd_kafka_new(RD_KAFKA_PRODUCER, c1, err, sizeof(err));
        if (!rk_prod_) {
            LOG_ERROR(logger, "Kafka pair producer: " << err);
            rd_kafka_conf_destroy(c1);
        }

        rd_kafka_conf_t* c2 = rd_kafka_conf_new();
        rd_kafka_conf_set(c2, "bootstrap.servers", brokers.c_str(), err, sizeof(err));
        std::string gid = CConfig::instance().kafka_group_strategy + "-pair";
        rd_kafka_conf_set(c2, "group.id", gid.c_str(), err, sizeof(err));
        rd_kafka_conf_set(c2, "enable.auto.commit", "true", err, sizeof(err));
        rd_kafka_conf_set(c2, "auto.offset.reset", "latest", err, sizeof(err));
        rk_cons_ = rd_kafka_new(RD_KAFKA_CONSUMER, c2, err, sizeof(err));
        if (!rk_cons_) {
            LOG_ERROR(logger, "Kafka pair consumer: " << err);
            rd_kafka_conf_destroy(c2);
        }
        if (rk_cons_) {
            rd_kafka_poll_set_consumer(rk_cons_);
            rd_kafka_topic_partition_list_t* topics = rd_kafka_topic_partition_list_new(1);
            rd_kafka_topic_partition_list_add(topics, topic_in_.c_str(), RD_KAFKA_PARTITION_UA);
            rd_kafka_subscribe(rk_cons_, topics);
            rd_kafka_topic_partition_list_destroy(topics);
        }
    }

    ~CMsgqKafkaPair() override {
        if (rk_prod_) {
            rd_kafka_flush(rk_prod_, 3000);
            rd_kafka_destroy(rk_prod_);
        }
        if (rk_cons_)
            rd_kafka_destroy(rk_cons_);
    }

    void sendmsg(const std::string& str, int32_t dontwait) override {
        (void)dontwait;
        if (!rk_prod_)
            return;
    retry:
        rd_kafka_resp_err_t err =
            rd_kafka_producev(rk_prod_, RD_KAFKA_V_TOPIC(topic_out_.c_str()),
                              RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                              RD_KAFKA_V_VALUE(const_cast<void*>(static_cast<const void*>(str.data())), str.size()),
                              RD_KAFKA_V_END);
        if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
            rd_kafka_poll(rk_prod_, 50);
            goto retry;
        }
        rd_kafka_poll(rk_prod_, 0);
    }

    void sendmsg(const char* str, int32_t dontwait) override {
        if (str)
            sendmsg(std::string(str), dontwait);
    }

    std::string recmsg(int32_t blockingflags) override {
        if (!rk_cons_)
            return {};
        int timeout_ms = (blockingflags == 0) ? 500 : 100;
        rd_kafka_message_t* rkm = rd_kafka_consumer_poll(rk_cons_, timeout_ms);
        if (!rkm)
            return {};
        if (rkm->err) {
            rd_kafka_message_destroy(rkm);
            return {};
        }
        std::string out(static_cast<const char*>(rkm->payload),
                        static_cast<std::string::size_type>(rkm->len));
        rd_kafka_message_destroy(rkm);
        return out;
    }

 private:
    rd_kafka_t* rk_prod_;
    rd_kafka_t* rk_cons_;
    std::string topic_out_;
    std::string topic_in_;
};

#endif  // SQ_HAVE_RDKAFKA

std::unique_ptr<CMsgq> MakeMsgqPub(const std::string& url) {
    if (CConfig::instance()._msgq != MSGQ::KAFKA)
        return std::make_unique<CMsgqNanomsg>(MSGQ_PROTOCOL::PUB, url);
#if SQ_HAVE_RDKAFKA
    return std::make_unique<CMsgqKafka>(MSGQ_PROTOCOL::PUB, url, true, "");
#else
    return std::make_unique<CMsgqNanomsg>(MSGQ_PROTOCOL::PUB, url);
#endif
}

std::unique_ptr<CMsgq> MakeMsgqSub(const std::string& url, const std::string& consumer_group) {
    if (CConfig::instance()._msgq != MSGQ::KAFKA)
        return std::make_unique<CMsgqNanomsg>(MSGQ_PROTOCOL::SUB, url, false);
#if SQ_HAVE_RDKAFKA
    return std::make_unique<CMsgqKafka>(MSGQ_PROTOCOL::SUB, url, false, consumer_group);
#else
    return std::make_unique<CMsgqNanomsg>(MSGQ_PROTOCOL::SUB, url, false);
#endif
}

std::unique_ptr<CMsgq> MakeMsgqPull(const std::string& url, const std::string& consumer_group) {
    if (CConfig::instance()._msgq != MSGQ::KAFKA)
        return std::make_unique<CMsgqNanomsg>(MSGQ_PROTOCOL::PULL, url);
#if SQ_HAVE_RDKAFKA
    return std::make_unique<CMsgqKafka>(MSGQ_PROTOCOL::PULL, url, true, consumer_group);
#else
    return std::make_unique<CMsgqNanomsg>(MSGQ_PROTOCOL::PULL, url);
#endif
}

std::unique_ptr<CMsgq> MakeMsgqStrategyPair() {
    if (CConfig::instance()._msgq != MSGQ::KAFKA)
        return std::make_unique<CMsgqNanomsg>(MSGQ_PROTOCOL::PAIR, CConfig::instance().BROKERAGE_PAIR_PORT, false);
#if SQ_HAVE_RDKAFKA
    return std::make_unique<CMsgqKafkaPair>();
#else
    return std::make_unique<CMsgqNanomsg>(MSGQ_PROTOCOL::PAIR, CConfig::instance().BROKERAGE_PAIR_PORT, false);
#endif
}

}  // namespace StarQuant
