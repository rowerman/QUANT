#!/usr/bin/env python3
"""Minimal Streamlit UI: Kafka equivalent of source/common/client_mq.py (pub/sub + requests)."""
import os
import threading
import queue

import streamlit as st
import yaml

try:
    from confluent_kafka import Consumer, Producer
except ImportError:
    Consumer = None
    Producer = None


def load_cfg(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def main():
    st.set_page_config(page_title="StarQuant Kafka Client", layout="wide")
    st.title("StarQuant Kafka 客户端（演示）")

    if Consumer is None:
        st.error("请安装: pip install -r requirements.txt")
        return

    default_cfg = os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "etc", "config_server.yaml.example")
    )
    cfg_path = st.text_input("配置文件", value=default_cfg)
    if os.path.isfile(cfg_path):
        cfg = load_cfg(cfg_path)
        brokers = cfg.get("kafka_brokers", "localhost:9092")
        topic_pub = cfg.get("kafka_topic_server_pub", "sq.server.pub")
        topic_req = cfg.get("kafka_topic_client_requests", "sq.client.requests")
    else:
        st.warning("未找到配置文件，使用手动输入。")
        brokers = st.text_input("kafka_brokers", value="localhost:9092")
        topic_pub = st.text_input("server pub topic", value="sq.server.pub")
        topic_req = st.text_input("client requests topic", value="sq.client.requests")

    if "q" not in st.session_state:
        st.session_state.q = queue.Queue()
    if "stop" not in st.session_state:
        st.session_state.stop = threading.Event()
    if "consumer_started" not in st.session_state:
        st.session_state.consumer_started = False

    def consume_loop(bootstrap: str, topic: str, outq: queue.Queue, stop_flag: threading.Event):
        c = Consumer(
            {
                "bootstrap.servers": bootstrap,
                "group.id": "sq-streamlit-ui",
                "auto.offset.reset": "latest",
                "enable.auto.commit": True,
            }
        )
        c.subscribe([topic])
        while not stop_flag.is_set():
            msg = c.poll(0.5)
            if msg is None or msg.error():
                continue
            try:
                outq.put(msg.value().decode("utf-8", errors="replace"))
            except Exception:
                pass
        c.close()

    c1, c2 = st.columns(2)
    with c1:
        if st.button("启动订阅", disabled=st.session_state.consumer_started):
            st.session_state.stop.clear()
            t = threading.Thread(
                target=consume_loop,
                args=(brokers, topic_pub, st.session_state.q, st.session_state.stop),
                daemon=True,
            )
            t.start()
            st.session_state.consumer_started = True
            st.success("已订阅: " + topic_pub)
    with c2:
        if st.button("停止订阅", disabled=not st.session_state.consumer_started):
            st.session_state.stop.set()
            st.session_state.consumer_started = False
            st.info("已发送停止信号")

    if st.button("刷新消息缓冲"):
        pass
    lines = []
    try:
        while True:
            lines.append(st.session_state.q.get_nowait())
    except queue.Empty:
        pass
    st.subheader("最近收到的广播")
    st.text_area("", value="\n".join(lines[-100:]), height=240, disabled=True, label_visibility="collapsed")

    st.subheader("发送请求（等价 client PUSH → server PULL）")
    raw = st.text_area("消息体", height=80, placeholder="*.*.*|...")
    if st.button("发送到 relay topic"):
        if raw.strip():
            p = Producer({"bootstrap.servers": brokers})
            p.produce(topic_req, raw.strip().encode("utf-8"))
            p.flush(5)
            st.success("已写入: " + topic_req)


if __name__ == "__main__":
    main()
