#!/usr/bin/env bash
set -e
sudo apt-get update
sudo apt-get install -y mosquitto mosquitto-clients python3-venv python3-pip sqlite3
sudo mosquitto_passwd -c -b /etc/mosquitto/passwd aquasense "${AQUASENSE_MQTT_PASSWORD:?Define AQUASENSE_MQTT_PASSWORD}"
sudo tee /etc/mosquitto/conf.d/aquasense.conf >/dev/null <<EOF
listener 1883 0.0.0.0
allow_anonymous false
password_file /etc/mosquitto/passwd
EOF
sudo systemctl restart mosquitto
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip paho-mqtt numpy scikit-learn
echo "Gateway listo. Ejecuta: python gateway.py y python servicio_ia_mqtt.py"
