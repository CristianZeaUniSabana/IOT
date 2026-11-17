"""
AQUASENSE - Servicio de deteccion de anomalias y recomendaciones mediante IA.
Se ejecuta en un PC/Raspberry Pi, NO dentro del ESP32.

Entrada:  aquasense/nodo1/telemetria
Salida:   aquasense/nodo1/ia
"""

import csv
import json
import logging
import math
import os
import sys
import time
from collections import deque
from pathlib import Path

import numpy as np
import paho.mqtt.client as mqtt
from sklearn.ensemble import IsolationForest

BROKER_HOST = os.getenv("AQUASENSE_MQTT_HOST", "10.77.207.50")
BROKER_PORT = int(os.getenv("AQUASENSE_MQTT_PORT", "1883"))
MQTT_USER = os.getenv("AQUASENSE_MQTT_USER", "aquasense")
MQTT_PASSWORD = os.getenv("AQUASENSE_MQTT_PASSWORD", "")

TOPIC_IN = "aquasense/nodo1/telemetria"
TOPIC_OUT = "aquasense/nodo1/ia"

BASE_DIR = Path(__file__).resolve().parent
CSV_PATH = BASE_DIR / "historial_ia.csv"

FEATURES = ["nivel", "distancia", "uv", "vpd", "iep"]
MIN_SAMPLES = 50
CONTAMINATION = 0.05

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s | %(levelname)s | %(message)s",
)
log = logging.getLogger("aquasense-ia")

client = None
model = None
training_rows = []
recent_levels = deque(maxlen=12)  # Ultimo minuto (12 muestras cada 5 s)


def valid_number(value):
    try:
        if value is None:
            return False
        number = float(value)
        return math.isfinite(number)
    except (TypeError, ValueError):
        return False


def extract_features(payload):
    """Extrae features. Si BME280 no esta disponible (null), usa valores neutros."""
    if not all(valid_number(payload.get(k)) for k in ["nivel", "distancia", "uv"]):
        return None
    nivel = float(payload["nivel"])
    distancia = float(payload["distancia"])
    uv = float(payload["uv"])
    vpd = float(payload["vpd"]) if valid_number(payload.get("vpd")) else 0.8
    iep = float(payload["iep"]) if valid_number(payload.get("iep")) else 20.0
    return [nivel, distancia, uv, vpd, iep]


def load_history():
    rows = []
    if not CSV_PATH.exists():
        return rows
    try:
        with CSV_PATH.open("r", newline="", encoding="utf-8") as file:
            for row in csv.DictReader(file):
                feats = extract_features(row)
                if feats is not None:
                    rows.append(feats)
    except (OSError, csv.Error) as exc:
        log.warning("No se pudo leer el historial previo: %s", exc)
    return rows


def append_history(values, payload):
    new_file = not CSV_PATH.exists()
    with CSV_PATH.open("a", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        if new_file:
            writer.writerow(["fecha_pc", *FEATURES, "estado_esp32", "riesgo_esp32"])
        writer.writerow([
            time.strftime("%Y-%m-%d %H:%M:%S"),
            *values,
            payload.get("estado", ""),
            payload.get("riesgo", ""),
        ])


def generar_recomendacion(status, values, tasa_cambio_min):
    """Genera diagnostico y recomendacion operativa segun variables y tendencia."""
    nivel, distancia, uv, vpd, iep = values

    if status == "ANOMALIA":
        if tasa_cambio_min is not None and tasa_cambio_min <= -5.0:
            return (
                f"ANOMALIA CRITICA: Caida acelerada de nivel ({tasa_cambio_min:.1f} %/min). "
                "Recomendacion: Inspeccionar inmediatamente valvula de salida, tuberias o posibles fugas en el tanque."
            )
        if nivel >= 80.0:
            return (
                f"ANOMALIA POR SOBRELLENADO: Nivel en {nivel:.1f}% (distancia {distancia:.1f} cm). "
                "Recomendacion: Cerrar valvula de entrada para evitar desbordamiento."
            )
        if nivel <= 20.0:
            return (
                f"ANOMALIA POR DESABASTECIMIENTO: Nivel critico en {nivel:.1f}%. "
                "Recomendacion: Activar suministro de respaldo e iniciar racionamiento preventivo."
            )
        return (
            "ANOMALIA MULTIVARIABLE: Combinacion atipica de nivel y demanda atmosferica. "
            "Recomendacion: Verificar obstrucciones en el sensor HC-SR04 y revisar consumo de red."
        )

    # Estado NORMAL
    if vpd >= 1.5 or iep >= 60.0:
        return (
            f"Operacion normal con alta demanda evaporativa (VPD={vpd:.2f} kPa, IEP={iep:.0f}/100). "
            "Recomendacion: Cubrir o sombrear el reservorio y programar recarga en horas de menor radiacion."
        )
    if nivel < 40.0:
        return (
            f"Patron estable pero en franja de vigilancia ({nivel:.1f}%). "
            "Recomendacion: Planificar reabastecimiento preventivo antes de descender del 20%."
        )
    return (
        f"Sistema estable (nivel {nivel:.1f}%, VPD {vpd:.2f} kPa). "
        "Recomendacion: Mantener monitoreo rutinario sin intervenciones."
    )


def publish_result(status, message, recomendacion="", values=None, score=None, tasa=None):
    result = {
        "estado_ia": status,
        "mensaje": message,
        "recomendacion": recomendacion,
        "tasa_nivel_pct_min": round(tasa, 2) if tasa is not None else None,
        "muestras_entrenamiento": len(training_rows),
        "modelo_listo": model is not None,
        "timestamp_pc": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "variables": dict(zip(FEATURES, values)) if values else None,
        "anomalia": status == "ANOMALIA",
        "score": round(score, 4) if score is not None else None,
        "metodo": "IsolationForest-Multivariable",
    }
    if client is not None and client.is_connected():
        info = client.publish(TOPIC_OUT, json.dumps(result, ensure_ascii=False), qos=1, retain=True)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            log.warning("No se pudo publicar el resultado IA (rc=%s)", info.rc)
    log.info("IA [%s]: %s | %s", status, message, recomendacion)


def on_connect(mqtt_client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        log.info("Conectado al broker MQTT %s:%s", BROKER_HOST, BROKER_PORT)
        mqtt_client.subscribe(TOPIC_IN, qos=1)
        publish_result("EN_APRENDIZAJE", "Esperando telemetria valida del ESP32.")
    else:
        log.error("Conexion MQTT rechazada: %s", reason_code)


def on_disconnect(mqtt_client, userdata, disconnect_flags, reason_code, properties):
    if reason_code != 0:
        log.warning("Conexion MQTT interrumpida; el cliente intentara reconectar.")


def on_message(mqtt_client, userdata, message):
    global model, training_rows
    try:
        payload = json.loads(message.payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        log.warning("Mensaje MQTT ignorado: JSON invalido.")
        return

    if payload.get("sensor_us") is not True:
        publish_result(
            "LECTURA_INVALIDA",
            "Se descarto una lectura con sensor_us=false.",
            "Revisar cableado TRIG/ECHO del sensor ultrasonico HC-SR04.",
        )
        return

    values = extract_features(payload)
    if values is None:
        publish_result(
            "LECTURA_INVALIDA",
            "La telemetria no contiene nivel, distancia y UV validos.",
            "Verificar formato JSON emitido por el nodo ESP32.",
        )
        return

    if not (0.0 <= values[0] <= 100.0 and 0.0 <= values[1] <= 500.0 and 0 <= values[2] <= 4095):
        publish_result(
            "LECTURA_INVALIDA",
            "Una o mas variables estan fuera de rango fisico.",
            "Comprobar calibracion de tanque (DISTANCIA_VACIO / DISTANCIA_LLENO).",
        )
        return

    now = time.time()
    recent_levels.append((now, values[0]))
    tasa_cambio_min = None
    if len(recent_levels) >= 2:
        dt_min = (recent_levels[-1][0] - recent_levels[0][0]) / 60.0
        if dt_min > 0.05:
            tasa_cambio_min = (recent_levels[-1][1] - recent_levels[0][1]) / dt_min

    try:
        append_history(values, payload)
    except OSError as exc:
        log.error("No se pudo guardar historial CSV: %s", exc)

    if model is None:
        training_rows.append(values)
        if len(training_rows) < MIN_SAMPLES:
            publish_result(
                "EN_APRENDIZAJE",
                f"Recolectando datos: {len(training_rows)}/{MIN_SAMPLES} muestras validas.",
                "Mantener operacion normal para establecer linea base.",
                values,
                tasa=tasa_cambio_min,
            )
            return
        model = IsolationForest(n_estimators=200, contamination=CONTAMINATION, random_state=42)
        model.fit(np.asarray(training_rows, dtype=float))
        publish_result(
            "MODELO_LISTO",
            "Modelo entrenado con las primeras 50 muestras validas.",
            "Monitoreo inteligente activo.",
            values,
            tasa=tasa_cambio_min,
        )
        return

    prediction = int(model.predict(np.asarray([values], dtype=float))[0])
    score = float(model.decision_function(np.asarray([values], dtype=float))[0])
    status = "ANOMALIA" if prediction == -1 else "NORMAL"
    msg = (
        "La lectura se aleja del patron multivariable aprendido."
        if status == "ANOMALIA"
        else "Lectura dentro del patron aprendido."
    )
    rec = generar_recomendacion(status, values, tasa_cambio_min)
    publish_result(status, msg, rec, values, score, tasa_cambio_min)


def main():
    global client, model, training_rows
    if not MQTT_PASSWORD:
        log.error("Falta AQUASENSE_MQTT_PASSWORD. Configura la variable de entorno.")
        return 2

    training_rows = load_history()
    if len(training_rows) >= MIN_SAMPLES:
        model = IsolationForest(n_estimators=200, contamination=CONTAMINATION, random_state=42)
        model.fit(np.asarray(training_rows, dtype=float))
        log.info("Modelo restaurado desde CSV con %d muestras.", len(training_rows))

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="AQUASENSE_IA_PC",
        protocol=mqtt.MQTTv311,
    )
    client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    try:
        client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
        client.loop_forever()
    except KeyboardInterrupt:
        log.info("Servicio detenido por el usuario.")
    finally:
        if client is not None:
            client.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
