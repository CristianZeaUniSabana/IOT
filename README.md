# AQUASENSE - Firmware ESP32 + servicio de IA por MQTT

## Qué contiene

- `ESP32/AQUASENSE_ESP32.ino`: copia del firmware que enviaste, con extensión `.ino` para abrirlo en Arduino IDE.
- `ESP32/CodigoESP32_original.txt`: respaldo idéntico del archivo recibido.
- `IA_Python/servicio_ia_mqtt.py`: servicio de detección de anomalías que se ejecuta en un computador o Raspberry Pi.
- `IA_Python/requirements.txt`: dependencias de Python.
- `Diagnostico/diagnostico_error.txt`: explicación del error de arranque que compartiste.

**El firmware se conserva sin modificaciones.** La IA no se ejecuta dentro del ESP32: recibe los datos que el ESP32 ya publica por MQTT y envía sus resultados a otro tópico MQTT.

## Cómo funciona la IA

1. Escucha `aquasense/nodo1/telemetria`.
2. Ignora lecturas donde `sensor_us` no sea `true` o las variables estén fuera de rango.
3. Guarda las muestras válidas en `IA_Python/historial_ia.csv`.
4. Aprende el comportamiento inicial con al menos 50 muestras válidas.
5. Usa `IsolationForest` para marcar lecturas como `NORMAL` o `ANOMALIA`.
6. Publica el resultado en `aquasense/nodo1/ia`.

Estados posibles: `EN_APRENDIZAJE`, `MODELO_LISTO`, `NORMAL`, `ANOMALIA` y `LECTURA_INVALIDA`.

La salida de IA no modifica LEDs, buzzer, LCD ni el dashboard web actual. Puedes observar el tópico de salida con MQTT Explorer o con un suscriptor MQTT. Para mostrar el resultado en el dashboard habría que añadir esa integración en otra etapa.

## Requisitos

- Python 3.10 o superior recomendado.
- Acceso de red desde el PC al mismo broker MQTT que usa el ESP32.
- Broker MQTT activo y credenciales válidas.
- El ESP32 debe estar publicando telemetría en `aquasense/nodo1/telemetria`.

## Instalación en Windows

Abre PowerShell dentro de la carpeta `IA_Python`:

```powershell
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
```

Si PowerShell bloquea la activación del entorno, ejecuta esta línea en esa misma ventana y vuelve a activar:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

Configura la conexión MQTT en la misma ventana de PowerShell. La IP predeterminada coincide con la que aparece en el firmware compartido; cámbiala si tu broker tiene otra IP:

```powershell
$env:AQUASENSE_MQTT_HOST="10.77.207.50"
$env:AQUASENSE_MQTT_PORT="1883"
$env:AQUASENSE_MQTT_USER="aquasense"
$env:AQUASENSE_MQTT_PASSWORD="ESCRIBE_AQUI_LA_CLAVE_DEL_BROKER"
```

Ejecuta:

```powershell
python servicio_ia_mqtt.py
```

Deja esa ventana abierta mientras quieras que la IA analice datos. Para detenerla, usa `Ctrl + C`.

## Comprobar la comunicación

- Entrada que debe llegar desde el ESP32: `aquasense/nodo1/telemetria`
- Salida publicada por la IA: `aquasense/nodo1/ia`

En MQTT Explorer, suscríbete a `aquasense/nodo1/#` para ver ambos tópicos. Si no llegan datos, comprueba primero que el ESP32 y el PC alcanzan el mismo broker, que la IP del broker no haya cambiado y que las credenciales sean correctas.

## Diagnóstico del error de arranque compartido

El registro adjunto contiene:

- `flash_parts: partition 0 invalid magic number 0x0`
- `Failed to verify partition table`
- `load partition table error!`
- reinicios posteriores del ESP32.

Esto apunta a un problema al leer la tabla de particiones/contenido de la memoria flash o a una configuración de flash incompatible. No es el mismo tipo de error que un fallo de compilación de C++ y no lo causa el servicio de IA Python.

Revisa en Arduino IDE, según la capacidad real de tu placa:

- Placa: `DOIT ESP32 DEVKIT V1`.
- Flash Mode: `DIO`.
- Flash Frequency: `40 MHz`.
- Flash Size: la capacidad real del módulo (confirma si es 4 MB).
- Partition Scheme: una opción compatible con esa capacidad, por ejemplo `Default 4MB with spiffs` si realmente tiene 4 MB.
- Upload Speed: `115200`.

Si el error persiste, considera `Erase All Flash Before Sketch Upload` (si tu versión del IDE ofrece esa opción). **Esto borra los datos guardados en la flash**, así que úsalo solo si estás de acuerdo y después vuelve a cargar el firmware. No se ha borrado ni probado físicamente la placa desde este paquete.

## Seguridad

El archivo original del firmware contiene credenciales de Wi-Fi, MQTT y del dashboard web escritas directamente en el código. Se han conservado para no alterar tu firmware, pero **no compartas este ZIP públicamente**. Como esas credenciales ya quedaron incluidas en un archivo compartido, es recomendable cambiarlas en los servicios correspondientes y actualizar el firmware de forma controlada. La contraseña del servicio Python se solicita mediante una variable de entorno y no está escrita en el script.

## Limitaciones importantes

- `IsolationForest` detecta patrones inusuales; no certifica potabilidad ni calidad sanitaria del agua.
- El modelo necesita datos representativos y lecturas correctas. Las primeras 50 muestras válidas se usan como base de aprendizaje.
- Si el comportamiento normal cambia mucho (por ejemplo, cambia el montaje o el tanque), conviene revisar y reiniciar el historial para volver a entrenar.
- El servicio no envía comandos al ESP32 ni cambia las alarmas locales.
