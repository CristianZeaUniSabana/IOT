"""
AQUASENSE - Gateway IoT (Raspberry Pi)
1. Suscribe a Mosquitto local: aquasense/+/telemetria y aquasense/+/ia
2. Almacena telemetria en SQLite (aquasense.db)
3. Reenvia variables y recomendacion de IA a Ubidots por MQTT
"""

import json
import logging
import os
import sqlite3
import sys
import time
from pathlib import Path
import paho.mqtt.client as mqtt

# Broker local (Mosquitto)
LOCAL_HOST = os.getenv("AQUASENSE_MQTT_HOST", "127.0.0.1")
LOCAL_PORT = int(os.getenv("AQUASENSE_MQTT_PORT", "1883"))
LOCAL_USER = os.getenv("AQUASENSE_MQTT_USER", "aquasense")
LOCAL_PASS = os.getenv("AQUASENSE_MQTT_PASSWORD", "")

# Broker Ubidots (Nube)
UBIDOTS_HOST = "industrial.api.ubidots.com"
UBIDOTS_PORT = 1883
UBIDOTS_TOKEN = os.getenv("UBIDOTS_TOKEN", "")
UBIDOTS_DEVICE = os.getenv("UBIDOTS_DEVICE_LABEL", "aquasense-nodo1")

DB_PATH = Path(__file__).resolve().parent / "aquasense.db"

logging.basicConfig(level=logging.INFO, format="%(asctime)s | %(levelname)s | %(message)s")
log = logging.getLogger("aquasense-gateway")

ubidots_client = None
ultimo_estado_ia = {"anomalia_ia": 0, "recomendacion": "Esperando analisis IA"}


def init_db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS lecturas (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts TEXT DEFAULT (datetime('now','localtime')),
            nodo TEXT,
            nivel REAL, distancia REAL, uv INTEGER,
            temperatura REAL, humedad REAL, presion REAL,
            vpd REAL, riesgo REAL, iep REAL,
            estado TEXT, anomalia INTEGER, sensor_us INTEGER
        )
    """)
    conn.commit()
    conn.close()


def guardar_sqlite(nodo, data):
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        INSERT INTO lecturas (
            nodo, nivel, distancia, uv, temperatura, humedad, presion,
            vpd, riesgo, iep, estado, anomalia, sensor_us
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        nodo,
        data.get("nivel"), data.get("distancia"), data.get("uv"),
        data.get("temperatura"), data.get("humedad"), data.get("presion"),
        data.get("vpd"), data.get("riesgo"), data.get("iep"),
        data.get("estado"),
        1 if data.get("anomalia") else 0,
        1 if data.get("sensor_us") else 0,
    ))
    conn.commit()
    conn.close()


def publicar_ubidots(data):
    if not UBIDOTS_TOKEN or ubidots_client is None or not ubidots_client.is_connected():
        return

    payload = {}
    for k in ["nivel", "distancia", "uv", "temperatura", "humedad", "presion", "vpd", "riesgo", "iep"]:
        if data.get(k) is not None:
            payload[k] = data[k]

    payload["anomalia_local"] = 1 if data.get("anomalia") else 0
    payload["anomalia_ia"] = ultimo_estado_ia["anomalia_ia"]
    payload["estado_sistema"] = {
        "value": data.get("riesgo", 0),
        "context": {
            "estado": data.get("estado", "DESCONOCIDO"),
            "recomendacion_ia": ultimo_estado_ia["recomendacion"],
        },
    }

    topic = f"/v1.6/devices/{UBIDOTS_DEVICE}"
    ubidots_client.publish(topic, json.dumps(payload, ensure_ascii=False), qos=1)
    log.info("Datos reenviados a Ubidots (%s)", topic)


def on_local_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        log.info("Gateway conectado a Mosquitto local")
        client.subscribe("aquasense/+/telemetria", qos=1)
        client.subscribe("aquasense/+/ia", qos=1)


def on_local_message(client, userdata, msg):
    global ultimo_estado_ia
    try:
        data = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        return

    partes = msg.topic.split("/")
    nodo = partes[1] if len(partes) >= 3 else "nodo1"

    if msg.topic.endswith("/ia"):
        ultimo_estado_ia = {
            "anomalia_ia": 1 if data.get("anomalia") else 0,
            "recomendacion": data.get("recomendacion", data.get("mensaje", "")),
        }
        return

    if msg.topic.endswith("/telemetria"):
        guardar_sqlite(nodo, data)
        log.info("Registro guardado en SQLite [%s] nivel=%s%%", nodo, data.get("nivel"))
        publicar_ubidots(data)


def main():
    global ubidots_client
    if not LOCAL_PASS:
        log.error("Falta AQUASENSE_MQTT_PASSWORD.")
        return 2

    init_db()

    if UBIDOTS_TOKEN:
        ubidots_client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="AQUASENSE_GW_UBIDOTS",
            protocol=mqtt.MQTTv311,
        )
        ubidots_client.username_pw_set(UBIDOTS_TOKEN, "")
        ubidots_client.connect(UBIDOTS_HOST, UBIDOTS_PORT, keepalive=60)
        ubidots_client.loop_start()
        log.info("Cliente MQTT Ubidots iniciado")
    else:
        log.warning("UBIDOTS_TOKEN no definido: solo se guardara en SQLite local.")

    local_client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="AQUASENSE_GATEWAY",
        protocol=mqtt.MQTTv311,
    )
    local_client.username_pw_set(LOCAL_USER, LOCAL_PASS)
    local_client.on_connect = on_local_connect
    local_client.on_message = on_local_message

    local_client.connect(LOCAL_HOST, LOCAL_PORT, keepalive=60)
    try:
        local_client.loop_forever()
    except KeyboardInterrupt:
        pass
    finally:
        local_client.disconnect()
        if ubidots_client:
            ubidots_client.loop_stop()
            ubidots_client.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
