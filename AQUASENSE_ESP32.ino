/*
  ============================================================
                  AQUASENSE - CHALLENGE 3
             Firmware ESP32 con dos tareas FreeRTOS
  ============================================================

  TAREA 1: TareaMedicion
  - Lee HC-SR04, GUVA-S12SD y BME280.
  - Calcula nivel, riesgo, VPD e indicador evaporativo.
  - Detecta cambios anormales del nivel.
  - Actualiza LCD, LEDs y buzzer.
  - Guarda el historial en memoria.
  - Publica la telemetria completa por MQTT (JSON) y atiende
    comandos MQTT (silenciar / activar alarma).

  TAREA 2: TareaWeb
  - Atiende el servidor web.
  - Solicita usuario y contraseña.
  - Muestra el dashboard y el historial.
  - Permite silenciar y reactivar la alarma.

  No se utiliza una ISR para leer sensores porque las lecturas
  ultrasónicas e I2C no deben ejecutarse dentro de una ISR.

  LIBRERIAS NECESARIAS (Administrar bibliotecas):
  - Adafruit BME280 Library (y Adafruit Unified Sensor)
  - PubSubClient (Nick O'Leary)
  - ArduinoJson (Benoit Blanchon), version 7

  TOPICOS MQTT:
  - Telemetria (ESP32 -> Pi): aquasense/nodo1/telemetria
  - Comandos   (Pi -> ESP32): aquasense/nodo1/cmd
      mensajes validos: "silenciar" o "activar"
*/

// ============================================================
// 1. LIBRERIAS
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LiquidCrystal.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <math.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

// ============================================================
// 2. CONFIGURACION WIFI, MQTT Y LOGIN
// ============================================================

// Coloca aquí el nombre y la contraseña de tu red Wi-Fi.
const char* WIFI_SSID = "Aquansense";
const char* WIFI_PASSWORD = "Uni269984";

// Configuración del Broker MQTT (Raspberry Pi).
// IMPORTANTE: si la IP de la Pi cambia en el hotspot, actualizarla aquí.
const char* MQTT_SERVER = "10.77.207.50";
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = "aquasense";
const char* MQTT_PASS   = "12345"; // La contraseña creada con mosquitto_passwd

// Tópicos MQTT.
const char* TOPIC_DATOS = "aquasense/nodo1/telemetria";
const char* TOPIC_CMD   = "aquasense/nodo1/cmd";

// Usuario y contraseña para entrar al dashboard.
const char* USUARIO_WEB = "admin";
const char* CLAVE_WEB = "AquaSense2026";

// ============================================================
// 3. PINES
// ============================================================

// HC-SR04
const int TRIG_PIN = 5;
const int ECHO_PIN = 4;

// GUVA-S12SD
const int UV_PIN = 34;

// BME280
const int SDA_PIN = 21;
const int SCL_PIN = 22;

// LEDs
const int LED_VERDE = 25;
const int LED_AMARILLO = 26;
const int LED_ROJO = 27;

// Buzzer
const int BUZZER_PIN = 32;

// LCD paralelo 16x2: RS, E, D4, D5, D6, D7
LiquidCrystal lcd(13, 14, 23, 19, 18, 17);

// ============================================================
// 4. CONFIGURACION DEL TANQUE
// ============================================================

// Sensor a 30 cm del fondo.
// Tanque de 20 cm de altura.
// Distancia al agua cuando está lleno: 10 cm.

const float DISTANCIA_VACIO = 30.0;
const float DISTANCIA_LLENO = 10.0;

// ============================================================
// 5. INTERVALOS
// ============================================================

const unsigned long INTERVALO_MEDICION = 5000;
const unsigned long INTERVALO_LCD = 2500;
const unsigned long INTERVALO_REINTENTO_MQTT = 5000;
const int MAX_HISTORIAL = 20;

// ============================================================
// 6. OBJETOS
// ============================================================

WebServer server(80);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

Adafruit_BME280 bme;

bool bmeDisponible = false;
bool alarmaSilenciada = false;

// Mutex para proteger los datos compartidos entre tareas.
SemaphoreHandle_t mutexDatos;

// Marca de tiempo del último intento de conexión MQTT.
unsigned long ultimoIntentoMQTT = 0;

// ============================================================
// 7. VARIABLES DEL SISTEMA
// ============================================================

float distancia = -1;
float nivel = 0;

int uvADC = 0;
float uvVoltaje = 0;

float temperatura = NAN;
float humedad = NAN;
float presion = NAN;

float vpd = NAN;
float gammaPsicrometrica = NAN;

float riesgo = 0;
float indiceEvaporacion = 0;

String estado = "INICIANDO";
String alerta = "Inicializando sistema";

bool sensorUSDisponible = false;
bool indiceDisponible = false;
bool anomaliaDetectada = false;

// Para comparar el nivel entre mediciones.
float nivelAnterior = -1;

// ============================================================
// 8. ESTRUCTURAS DE DATOS
// ============================================================

// Registro del historico en memoria.
struct Registro {

  unsigned long tiempo;

  float distancia;
  float nivel;

  int uv;

  float temperatura;
  float humedad;
  float presion;

  float vpd;
  float riesgo;
  float evaporacion;

  bool anomalia;
};

Registro historial[MAX_HISTORIAL];

int indiceHistorial = 0;
int cantidadHistorial = 0;

// Copia de los datos que se publican por MQTT. Se llena dentro
// del mutex para que el mensaje sea consistente.
struct DatosMQTT {

  float nivel;
  float distancia;

  float temperatura;
  float humedad;
  float presion;

  float vpd;
  float riesgo;
  float iep;

  int uv;

  String estado;

  bool anomalia;
  bool usOK;
  bool silenciada;
};

// ============================================================
// 9. LECTURA DEL SENSOR ULTRASONICO
// ============================================================

float leerDistancia() {

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  // Tiempo máximo de espera: 30 ms.
  unsigned long duracion = pulseIn(
    ECHO_PIN,
    HIGH,
    30000UL
  );

  if (duracion == 0) {
    return -1;
  }

  float distanciaCM = duracion * 0.0343 / 2.0;

  return distanciaCM;
}

// ============================================================
// 10. CALCULAR NIVEL DEL AGUA
// ============================================================

float calcularNivel(float d) {

  if (d < 0) {
    return nivel;
  }

  float porcentaje =
    ((DISTANCIA_VACIO - d) /
    (DISTANCIA_VACIO - DISTANCIA_LLENO)) * 100.0;

  if (porcentaje < 0) {
    porcentaje = 0;
  }

  if (porcentaje > 100) {
    porcentaje = 100;
  }

  return porcentaje;
}

// ============================================================
// 11. LEER SENSOR UV
// ============================================================

void leerUV() {

  uvADC = analogRead(UV_PIN);

  uvVoltaje = (uvADC / 4095.0) * 3.3;
}

// ============================================================
// 12. LEER SENSOR BME280
// ============================================================

void leerBME280() {

  if (!bmeDisponible) {

    temperatura = NAN;
    humedad = NAN;
    presion = NAN;

    return;
  }

  float nuevaTemperatura = bme.readTemperature();
  float nuevaHumedad = bme.readHumidity();
  float nuevaPresion = bme.readPressure() / 100.0F;

  // Validar que las lecturas estén dentro de rangos posibles.
  bool lecturaValida =
    !isnan(nuevaTemperatura) &&
    !isnan(nuevaHumedad) &&
    !isnan(nuevaPresion) &&
    nuevaTemperatura >= -40 &&
    nuevaTemperatura <= 85 &&
    nuevaHumedad >= 0 &&
    nuevaHumedad <= 100 &&
    nuevaPresion >= 300 &&
    nuevaPresion <= 1100;

  if (lecturaValida) {

    temperatura = nuevaTemperatura;
    humedad = nuevaHumedad;
    presion = nuevaPresion;

  } else {

    temperatura = NAN;
    humedad = NAN;
    presion = NAN;
  }
}

// ============================================================
// 13. MQTT: COMANDOS, RECONEXION Y PUBLICACION
// ============================================================

// Se ejecuta dentro de mqttClient.loop() (TareaMedicion), fuera
// del mutex, por lo que puede tomarlo sin riesgo de bloqueo.
void callbackMQTT(char* topic, byte* payload, unsigned int length) {

  String mensaje;

  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }

  mensaje.trim();

  Serial.print("MQTT comando recibido [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(mensaje);

  if (String(topic) != TOPIC_CMD) {
    return;
  }

  if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(500)) == pdTRUE) {

    if (mensaje == "silenciar") {

      alarmaSilenciada = true;

    } else if (mensaje == "activar") {

      alarmaSilenciada = false;
    }

    xSemaphoreGive(mutexDatos);
  }
}

// Mantiene la conexión MQTT sin bloquear la tarea de medición:
// reintenta cada 5 s (sin bucles de espera) y atiende mensajes.
void mantenerMQTT() {

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!mqttClient.connected()) {

    if (millis() - ultimoIntentoMQTT < INTERVALO_REINTENTO_MQTT) {
      return;
    }

    ultimoIntentoMQTT = millis();

    Serial.print("Intentando conexion MQTT...");

    if (mqttClient.connect("ESP32_AquaSense", MQTT_USER, MQTT_PASS)) {

      Serial.println(" conectado al broker MQTT");

      mqttClient.subscribe(TOPIC_CMD);

    } else {

      Serial.print(" fallo, rc=");
      Serial.println(mqttClient.state());
    }

    return;
  }

  mqttClient.loop();
}

// Asigna un float al JSON; si es NAN lo envia como null
// (NaN no es JSON valido).
void asignarFloat(JsonDocument &doc, const char* clave, float valor) {

  if (isnan(valor)) {

    doc[clave] = nullptr;

  } else {

    doc[clave] = valor;
  }
}

void publicarTelemetria(const DatosMQTT &m) {

  if (!mqttClient.connected()) {
    return;
  }

  JsonDocument doc;

  asignarFloat(doc, "nivel", m.nivel);
  asignarFloat(doc, "distancia", m.usOK ? m.distancia : (float)NAN);

  doc["uv"] = m.uv;

  asignarFloat(doc, "temperatura", m.temperatura);
  asignarFloat(doc, "humedad", m.humedad);
  asignarFloat(doc, "presion", m.presion);
  asignarFloat(doc, "vpd", m.vpd);
  asignarFloat(doc, "riesgo", m.riesgo);
  asignarFloat(doc, "iep", m.iep);

  doc["estado"] = m.estado;
  doc["anomalia"] = m.anomalia;
  doc["sensor_us"] = m.usOK;
  doc["alarma_silenciada"] = m.silenciada;

  char buffer[512];

  size_t longitud = serializeJson(doc, buffer, sizeof(buffer));

  if (longitud == 0 || longitud >= sizeof(buffer)) {
    Serial.println("ERROR: JSON demasiado grande para el buffer");
    return;
  }

  if (mqttClient.publish(TOPIC_DATOS, buffer)) {

    Serial.print("MQTT publicado: ");
    Serial.println(buffer);

  } else {

    Serial.println("ERROR: fallo la publicacion MQTT");
  }
}

// ============================================================
// 14. CALCULAR VPD Y CONSTANTE PSICROMETRICA
// ============================================================

void calcularVariablesAmbientales() {

  vpd = NAN;
  gammaPsicrometrica = NAN;
  indiceDisponible = false;

  if (isnan(temperatura) ||
      isnan(humedad) ||
      isnan(presion)) {

    return;
  }

  // Presión de vapor de saturación (kPa).
  float es = 0.6108 * exp(
    (17.27 * temperatura) /
    (temperatura + 237.3)
  );

  // Presión de vapor real (kPa).
  float ea = es * (humedad / 100.0);

  // Déficit de presión de vapor.
  vpd = es - ea;

  if (vpd < 0) {
    vpd = 0;
  }

  // Constante psicrométrica (kPa/°C).
  float presionKPa = presion / 10.0;

  gammaPsicrometrica = 0.000665 * presionKPa;
}

// ============================================================
// 15. INDICADOR EXPERIMENTAL DE DEMANDA EVAPORATIVA
// ============================================================

void calcularEvaporacion() {

  indiceDisponible = false;
  indiceEvaporacion = 0;

  if (isnan(vpd) || isnan(gammaPsicrometrica)) {
    return;
  }

  // Normalizar el déficit de presión de vapor.
  float vpdNormalizado = (vpd / 3.0) * 100.0;

  if (vpdNormalizado > 100) {
    vpdNormalizado = 100;
  }

  // Normalizar la lectura UV.
  float uvNormalizado = (uvADC / 4095.0) * 100.0;

  // Normalizar la constante psicrométrica.
  float gammaNormalizado =
    (gammaPsicrometrica / 0.08) * 100.0;

  if (gammaNormalizado > 100) {
    gammaNormalizado = 100;
  }

  // Variación del nivel entre mediciones.
  float perdida = 0;

  if (nivelAnterior >= 0) {

    perdida = nivelAnterior - nivel;

    // Filtrar variaciones pequeñas por ruido.
    if (perdida < 1.0) {
      perdida = 0;
    }

    if (perdida > 20.0) {
      perdida = 20.0;
    }
  }

  float perdidaNormalizada = (perdida / 20.0) * 100.0;

  // Indicador experimental de 0 a 100.
  indiceEvaporacion =
    (0.55 * vpdNormalizado) +
    (0.20 * uvNormalizado) +
    (0.15 * gammaNormalizado) +
    (0.10 * perdidaNormalizada);

  if (indiceEvaporacion > 100) {
    indiceEvaporacion = 100;
  }

  if (indiceEvaporacion < 0) {
    indiceEvaporacion = 0;
  }

  indiceDisponible = true;
}

// ============================================================
// 16. CALCULAR RIESGO Y ESTADO
// ============================================================

void calcularRiesgoYEstado() {

  float r = 0;

  // Riesgo según el nivel de agua.
  if (nivel < 20) {

    r += 60;

  } else if (nivel < 40) {

    r += 35;

  } else if (nivel < 60) {

    r += 0;

  } else if (nivel < 80) {

    r += 35;

  } else {

    r += 60;
  }

  // Riesgo según exposición UV.
  if (uvADC >= 3000) {

    r += 10;

  } else if (uvADC >= 2000) {

    r += 5;
  }

  // Integración de las variables del BME280.
  if (!isnan(temperatura) &&
      !isnan(humedad) &&
      !isnan(vpd)) {

    if (temperatura >= 30) {
      r += 5;
    }

    if (humedad <= 35) {
      r += 5;
    }

    if (vpd >= 1.5) {
      r += 10;
    }
  }

  if (r > 100) {
    r = 100;
  }

  riesgo = r;

  // Se conservan los cinco rangos de nivel originales.
  if (nivel < 20) {

    estado = "CRITICO";

  } else if (nivel < 40) {

    estado = "ADVERTENCIA";

  } else if (nivel < 60) {

    estado = "NORMAL";

  } else if (nivel < 80) {

    estado = "ADVERTENCIA";

  } else {

    estado = "CRITICO";
  }

  // Una condición ambiental elevada genera advertencia.
  if (estado == "NORMAL" && !isnan(vpd) && vpd >= 1.5) {
    estado = "ADVERTENCIA";
  }

  alerta = "Sin alertas";

  if (estado == "ADVERTENCIA") {
    alerta = "Condicion de vigilancia";
  }

  if (estado == "CRITICO") {
    alerta = "Nivel critico de agua";
  }
}

// ============================================================
// 17. DETECCION LOCAL DE ANOMALIAS
// ============================================================

void detectarAnomalias() {

  anomaliaDetectada = false;

  if (nivelAnterior >= 0) {

    float cambio = nivelAnterior - nivel;

    // Descenso de 10 puntos porcentuales entre dos lecturas.
    if (cambio >= 10.0) {

      anomaliaDetectada = true;

      alerta = "ANOMALIA: descenso rapido del nivel";

      if (estado == "NORMAL") {
        estado = "ADVERTENCIA";
      }
    }
  }
}

// ============================================================
// 18. ACTUALIZAR ACTUADORES
// ============================================================

void actualizarActuadores() {

  if (estado == "NORMAL") {

    digitalWrite(LED_VERDE, HIGH);
    digitalWrite(LED_AMARILLO, LOW);
    digitalWrite(LED_ROJO, LOW);

    noTone(BUZZER_PIN);
  }

  else if (estado == "ADVERTENCIA") {

    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_AMARILLO, HIGH);
    digitalWrite(LED_ROJO, LOW);

    static unsigned long ultimoCambio = 0;
    static bool pitando = false;

    if (alarmaSilenciada) {

      noTone(BUZZER_PIN);
      pitando = false;

    } else if (millis() - ultimoCambio >= 700) {

      ultimoCambio = millis();

      pitando = !pitando;

      if (pitando) {
        tone(BUZZER_PIN, 700);
      } else {
        noTone(BUZZER_PIN);
      }
    }
  }

  else {

    // CRITICO o ERROR SENSOR.
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_AMARILLO, LOW);
    digitalWrite(LED_ROJO, HIGH);

    if (alarmaSilenciada) {
      noTone(BUZZER_PIN);
    } else {
      tone(BUZZER_PIN, 1200);
    }
  }
}

// ============================================================
// 19. GUARDAR HISTORICO
// ============================================================

void guardarHistorial() {

  Registro &r = historial[indiceHistorial];

  r.tiempo = millis();

  r.distancia = distancia;
  r.nivel = nivel;

  r.uv = uvADC;

  r.temperatura = temperatura;
  r.humedad = humedad;
  r.presion = presion;

  r.vpd = vpd;
  r.riesgo = riesgo;
  r.evaporacion = indiceEvaporacion;

  r.anomalia = anomaliaDetectada;

  indiceHistorial++;

  if (indiceHistorial >= MAX_HISTORIAL) {
    indiceHistorial = 0;
  }

  if (cantidadHistorial < MAX_HISTORIAL) {
    cantidadHistorial++;
  }
}

// ============================================================
// 20. LCD CON PANTALLAS ROTATIVAS
// ============================================================

void actualizarLCD() {

  static unsigned long ultimaPantalla = 0;
  static int pantalla = 0;

  if (millis() - ultimaPantalla < INTERVALO_LCD) {
    return;
  }

  ultimaPantalla = millis();

  pantalla++;

  if (pantalla > 3) {
    pantalla = 0;
  }

  lcd.clear();

  // PANTALLA 1: NIVEL Y ESTADO.
  if (pantalla == 0) {

    lcd.setCursor(0, 0);
    lcd.print("Nivel: ");
    lcd.print(nivel, 0);
    lcd.print("%");

    lcd.setCursor(0, 1);
    lcd.print(estado);
    lcd.print(" R:");
    lcd.print(riesgo, 0);
  }

  // PANTALLA 2: TEMPERATURA Y HUMEDAD.
  else if (pantalla == 1) {

    lcd.setCursor(0, 0);
    lcd.print("Temp: ");

    if (!isnan(temperatura)) {
      lcd.print(temperatura, 1);
      lcd.print(" C");
    } else {
      lcd.print("No disp.");
    }

    lcd.setCursor(0, 1);
    lcd.print("Humedad: ");

    if (!isnan(humedad)) {
      lcd.print(humedad, 1);
      lcd.print("%");
    } else {
      lcd.print("--");
    }
  }

  // PANTALLA 3: PRESION.
  else if (pantalla == 2) {

    lcd.setCursor(0, 0);
    lcd.print("Presion:");

    lcd.setCursor(0, 1);

    if (!isnan(presion)) {
      lcd.print(presion, 1);
      lcd.print(" hPa");
    } else {
      lcd.print("BME no disp.");
    }
  }

  // PANTALLA 4: UV, VPD E INDICADOR.
  else {

    lcd.setCursor(0, 0);
    lcd.print("UV:");
    lcd.print(uvADC);

    lcd.print(" IEP:");

    if (indiceDisponible) {
      lcd.print(indiceEvaporacion, 0);
    } else {
      lcd.print("--");
    }

    lcd.setCursor(0, 1);
    lcd.print("VPD:");

    if (!isnan(vpd)) {
      lcd.print(vpd, 2);
      lcd.print(" kPa");
    } else {
      lcd.print("No disp.");
    }
  }
}

// ============================================================
// 21. AUTENTICACION Y ACCESO WLAN
// ============================================================

bool autenticarUsuario() {

  if (!server.authenticate(USUARIO_WEB, CLAVE_WEB)) {

    server.requestAuthentication(
      BASIC_AUTH,
      "AQUASENSE - Acceso autorizado"
    );

    return false;
  }

  return true;
}

bool accesoPermitido() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  IPAddress cliente = server.client().remoteIP();
  IPAddress local = WiFi.localIP();

  // Se conserva la restriccion de misma subred /24.
  for (int i = 0; i < 3; i++) {

    if (cliente[i] != local[i]) {
      return false;
    }
  }

  return true;
}

bool verificarAccesoCompleto() {

  if (!autenticarUsuario()) {
    return false;
  }

  if (!accesoPermitido()) {

    server.send(
      403,
      "text/plain; charset=utf-8",
      "Acceso denegado. Conectese a la WLAN autorizada."
    );

    return false;
  }

  return true;
}

// ============================================================
// 22. DASHBOARD PRINCIPAL
// ============================================================

void paginaPrincipal() {

  if (!verificarAccesoCompleto()) {
    return;
  }

  // Copia protegida de las variables para evitar conflictos
  // entre la tarea de medicion y la tarea web.
  if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(1000)) != pdTRUE) {

    server.send(503, "text/plain", "Datos ocupados. Intente de nuevo.");
    return;
  }

  float d = distancia;
  float n = nivel;
  int uv = uvADC;
  float voltajeUV = uvVoltaje;

  float t = temperatura;
  float h = humedad;
  float p = presion;

  float v = vpd;
  float gamma = gammaPsicrometrica;

  float r = riesgo;
  float iep = indiceEvaporacion;

  String e = estado;
  String mensaje = alerta;

  bool bmeOK = !isnan(t) && !isnan(h) && !isnan(p);
  bool iepOK = indiceDisponible;
  bool alarmaOFF = alarmaSilenciada;
  bool anomalia = anomaliaDetectada;
  bool sensorOK = sensorUSDisponible;

  Registro copiaHistorial[MAX_HISTORIAL];

  int cantidad = cantidadHistorial;
  int indice = indiceHistorial;

  for (int i = 0; i < MAX_HISTORIAL; i++) {
    copiaHistorial[i] = historial[i];
  }

  xSemaphoreGive(mutexDatos);

  // Construccion de la pagina.
  String html;
  html.reserve(12000);

  html += F("<!DOCTYPE html><html lang='es'><head>");
  html += F("<meta charset='UTF-8'>");
  html += F("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  html += F("<meta http-equiv='refresh' content='8'>");
  html += F("<title>AQUASENSE - Dashboard</title>");

  html += F("<style>");

  html += F("*{box-sizing:border-box}");
  html += F("body{margin:0;font-family:Arial,sans-serif;background:#edf3f6;color:#243746}");
  html += F("header{background:linear-gradient(120deg,#0b536d,#1686a0);color:white;padding:25px 5%}");
  html += F("header h1{margin:0;font-size:28px}");
  html += F("header p{margin:7px 0 0}");
  html += F("main{max-width:1200px;margin:25px auto;padding:0 16px}");
  html += F(".top{display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap}");
  html += F(".pill{background:#dcebf1;border-radius:20px;padding:8px 13px;font-size:13px}");
  html += F(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:15px;margin:18px 0}");
  html += F(".card{background:white;border-radius:14px;padding:19px;box-shadow:0 3px 12px #18374612;margin-bottom:16px}");
  html += F(".label{color:#687d89;font-size:13px}");
  html += F(".value{font-size:27px;font-weight:bold;color:#126782;margin-top:8px}");
  html += F(".state{font-size:21px;font-weight:bold;margin-top:8px}");
  html += F(".normal{color:#198754}.warning{color:#d99b16}.critical{color:#c43d4b}");
  html += F(".muted{font-size:13px;color:#73838c}");
  html += F(".bar{height:9px;background:#e5edf0;border-radius:20px;margin-top:10px;overflow:hidden}");
  html += F(".fill{height:100%;background:#1686a0}");
  html += F(".tablewrap{overflow-x:auto}");
  html += F("table{width:100%;min-width:900px;border-collapse:collapse;font-size:13px}");
  html += F("th,td{text-align:left;padding:10px;border-bottom:1px solid #e5ecef}");
  html += F("button{border:0;border-radius:9px;padding:12px 17px;background:#126782;color:white;font-weight:bold;cursor:pointer}");
  html += F(".danger{background:#b83b49}.success{background:#198754}");
  html += F("footer{text-align:center;color:#71838d;font-size:12px;padding:20px}");
  html += F("@media(max-width:600px){header h1{font-size:23px}.value{font-size:23px}}");

  html += F("</style></head><body>");

  html += F("<header><h1>AQUASENSE</h1>");
  html += F("<p>Sistema local de monitoreo y alerta hidrica</p></header>");

  html += F("<main>");

  html += F("<div class='top'>");
  html += F("<span class='pill'>Actualizacion automatica cada 8 segundos</span>");
  html += F("<span class='pill'>Usuario autenticado | Red local</span>");
  html += F("</div>");

  // Estado visual.
  String claseEstado = "normal";

  if (e == "ADVERTENCIA") {
    claseEstado = "warning";
  }

  if (e == "CRITICO" || e == "ERROR SENSOR") {
    claseEstado = "critical";
  }

  // TARJETAS DE VARIABLES.
  html += F("<section class='grid'>");

  html += F("<article class='card'><div class='label'>Nivel del tanque</div>");
  html += F("<div class='value'>");
  html += String(n, 1);
  html += F(" %</div><div class='bar'><div class='fill' style='width:");
  html += String(n, 1);
  html += F("%'></div></div><p class='muted'>Distancia: ");
  html += String(d, 1);
  html += F(" cm</p></article>");

  html += F("<article class='card'><div class='label'>Estado del sistema</div>");
  html += F("<div class='state ");
  html += claseEstado;
  html += F("'>");
  html += e;
  html += F("</div><p class='muted'>Riesgo: ");
  html += String(r, 0);
  html += F(" / 100</p></article>");

  html += F("<article class='card'><div class='label'>Temperatura</div><div class='value'>");
  html += bmeOK ? String(t, 1) + " &deg;C" : String("No disponible");
  html += F("</div><p class='muted'>BME280</p></article>");

  html += F("<article class='card'><div class='label'>Humedad relativa</div><div class='value'>");
  html += bmeOK ? String(h, 1) + " %" : String("No disponible");
  html += F("</div><p class='muted'>BME280</p></article>");

  html += F("<article class='card'><div class='label'>Presion atmosferica</div><div class='value'>");
  html += bmeOK ? String(p, 1) + " hPa" : String("No disponible");
  html += F("</div><p class='muted'>BME280</p></article>");

  html += F("<article class='card'><div class='label'>Radiacion UV (ADC)</div><div class='value'>");
  html += String(uv);
  html += F("</div><p class='muted'>Voltaje: ");
  html += String(voltajeUV, 2);
  html += F(" V</p></article>");

  html += F("<article class='card'><div class='label'>Deficit de presion de vapor</div><div class='value'>");
  html += !isnan(v) ? String(v, 2) + " kPa" : String("No disponible");
  html += F("</div><p class='muted'>VPD calculado con temperatura y humedad</p></article>");

  html += F("<article class='card'><div class='label'>Constante psicrometrica</div><div class='value'>");
  html += !isnan(gamma) ? String(gamma, 4) : String("No disponible");
  html += F("</div><p class='muted'>kPa por grado Celsius</p></article>");

  html += F("<article class='card'><div class='label'>Indicador evaporativo experimental</div><div class='value'>");
  html += iepOK ? String(iep, 1) + " / 100" : String("No disponible");
  html += F("</div><p class='muted'>No equivale a ET0 FAO-56</p></article>");

  html += F("</section>");

  // ALERTAS.
  html += F("<section class='card'><h2>Alertas y actuadores</h2><p>");
  html += mensaje;
  html += F("</p><p class='muted'>Sensor ultrasonico: ");
  html += sensorOK ? "Conectado" : "Sin lectura";
  html += F(" | BME280: ");
  html += bmeOK ? "Conectado" : "No disponible";
  html += F(" | Anomalia: ");
  html += anomalia ? "Detectada" : "No detectada";
  html += F("</p><p class='muted'>Alarma: ");
  html += alarmaOFF ? "Silenciada" : "Activa";
  html += F("</p>");

  html += F("<form action='/silenciar' method='POST' style='display:inline-block;margin-right:8px'>");
  html += F("<button class='danger' type='submit'>Silenciar alarma</button></form>");

  html += F("<form action='/activar' method='POST' style='display:inline-block'>");
  html += F("<button class='success' type='submit'>Reactivar alarma</button></form>");

  html += F("</section>");

  // HISTORICO.
  html += F("<section class='card'><h2>Historico de mediciones</h2>");
  html += F("<div class='tablewrap'><table><thead><tr>");
  html += F("<th>Tiempo (s)</th><th>Nivel (%)</th><th>Distancia (cm)</th>");
  html += F("<th>UV ADC</th><th>Temp. (C)</th><th>Humedad (%)</th>");
  html += F("<th>Presion (hPa)</th><th>VPD (kPa)</th><th>Riesgo</th>");
  html += F("<th>Indicador</th><th>Anomalia</th></tr></thead><tbody>");

  for (int i = 0; i < cantidad; i++) {

    int posicion =
      (indice - cantidad + i + MAX_HISTORIAL) % MAX_HISTORIAL;

    Registro &reg = copiaHistorial[posicion];

    html += F("<tr><td>");
    html += String(reg.tiempo / 1000);
    html += F("</td><td>");
    html += String(reg.nivel, 1);
    html += F("</td><td>");
    html += String(reg.distancia, 1);
    html += F("</td><td>");
    html += String(reg.uv);
    html += F("</td><td>");
    html += isnan(reg.temperatura) ? "--" : String(reg.temperatura, 1);
    html += F("</td><td>");
    html += isnan(reg.humedad) ? "--" : String(reg.humedad, 1);
    html += F("</td><td>");
    html += isnan(reg.presion) ? "--" : String(reg.presion, 1);
    html += F("</td><td>");
    html += isnan(reg.vpd) ? "--" : String(reg.vpd, 2);
    html += F("</td><td>");
    html += String(reg.riesgo, 0);
    html += F("</td><td>");
    html += isnan(reg.temperatura) ? "--" : String(reg.evaporacion, 1);
    html += F("</td><td>");
    html += reg.anomalia ? "SI" : "NO";
    html += F("</td></tr>");
  }

  html += F("</tbody></table></div>");
  html += F("<p class='muted'>Se conservan las ultimas 20 mediciones durante la ejecucion del ESP32.</p>");
  html += F("</section>");

  html += F("<footer>AQUASENSE | Prototipo academico IoT</footer>");
  html += F("</main></body></html>");

  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================
// 23. SILENCIAR Y REACTIVAR ALARMA (DASHBOARD LOCAL)
// ============================================================

void silenciarAlarma() {

  if (!verificarAccesoCompleto()) {
    return;
  }

  if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(1000)) == pdTRUE) {

    alarmaSilenciada = true;

    xSemaphoreGive(mutexDatos);
  }

  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Alarma silenciada");
}

void activarAlarma() {

  if (!verificarAccesoCompleto()) {
    return;
  }

  if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(1000)) == pdTRUE) {

    alarmaSilenciada = false;

    xSemaphoreGive(mutexDatos);
  }

  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Alarma reactivada");
}

// ============================================================
// 24. MEDICION COMPLETA
// ============================================================

void realizarMedicion() {

  distancia = leerDistancia();

  sensorUSDisponible = distancia >= 0;

  // Se leen los sensores ambientales aunque falle el ultrasónico.
  leerUV();
  leerBME280();

  if (!sensorUSDisponible) {

    estado = "ERROR SENSOR";
    alerta = "Revise conexiones TRIG y ECHO";

    anomaliaDetectada = false;

    actualizarActuadores();
    actualizarLCD();

    Serial.println("ERROR: no hay lectura del HC-SR04");

    return;
  }

  nivel = calcularNivel(distancia);

  calcularVariablesAmbientales();

  calcularEvaporacion();

  calcularRiesgoYEstado();

  detectarAnomalias();

  actualizarActuadores();

  guardarHistorial();

  actualizarLCD();

  // MONITOR SERIAL.
  Serial.println();
  Serial.println("========== AQUASENSE ==========");

  Serial.print("Sensor: HC-SR04");
  Serial.println();

  Serial.print("Distancia: ");
  Serial.print(distancia, 2);
  Serial.println(" cm");

  Serial.print("Nivel: ");
  Serial.print(nivel, 2);
  Serial.println(" %");

  Serial.print("UV ADC: ");
  Serial.print(uvADC);
  Serial.print(" | Voltaje: ");
  Serial.print(uvVoltaje, 2);
  Serial.println(" V");

  if (!isnan(temperatura)) {

    Serial.print("Temperatura: ");
    Serial.print(temperatura, 2);
    Serial.println(" C");

    Serial.print("Humedad: ");
    Serial.print(humedad, 2);
    Serial.println(" %");

    Serial.print("Presion: ");
    Serial.print(presion, 2);
    Serial.println(" hPa");

    Serial.print("VPD: ");
    Serial.print(vpd, 3);
    Serial.println(" kPa");

    Serial.print("Constante psicrometrica: ");
    Serial.print(gammaPsicrometrica, 4);
    Serial.println(" kPa/C");

    Serial.print("Indicador evaporativo experimental: ");
    Serial.println(indiceEvaporacion, 2);

  } else {

    Serial.println("BME280: no disponible");
  }

  Serial.print("Riesgo: ");
  Serial.println(riesgo, 2);

  Serial.print("Estado: ");
  Serial.println(estado);

  Serial.print("Alerta: ");
  Serial.println(alerta);

  Serial.println("===============================");
}

// ============================================================
// 25. TAREA 1: MEDICION, ACTUADORES Y MQTT
// ============================================================

void TareaMedicion(void *parameter) {

  unsigned long ultimaMedicion = 0;

  for (;;) {

    unsigned long ahora = millis();

    // Medir cada 5 segundos.
    if (ahora - ultimaMedicion >= INTERVALO_MEDICION) {

      ultimaMedicion = ahora;

      DatosMQTT m;
      bool medido = false;

      if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(1000)) == pdTRUE) {

        realizarMedicion();

        // Guardar el nivel actual para la siguiente comparación.
        if (sensorUSDisponible) {
          nivelAnterior = nivel;
        }

        // Copia de los datos dentro del mutex: el mensaje MQTT
        // queda consistente aunque otra tarea los modifique luego.
        m.nivel = nivel;
        m.distancia = distancia;
        m.temperatura = temperatura;
        m.humedad = humedad;
        m.presion = presion;
        m.vpd = vpd;
        m.riesgo = riesgo;
        m.iep = indiceDisponible ? indiceEvaporacion : (float)NAN;
        m.uv = uvADC;
        m.estado = estado;
        m.anomalia = anomaliaDetectada;
        m.usOK = sensorUSDisponible;
        m.silenciada = alarmaSilenciada;

        xSemaphoreGive(mutexDatos);

        medido = true;
      }

      // Publicación MQTT hacia la Raspberry Pi (fuera del mutex).
      if (medido) {
        publicarTelemetria(m);
      }
    }

    // Actualizar el buzzer sin esperar los siguientes 5 segundos.
    if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(100)) == pdTRUE) {

      actualizarActuadores();
      actualizarLCD();

      xSemaphoreGive(mutexDatos);
    }

    // Mantiene la conexión MQTT y atiende comandos entrantes.
    mantenerMQTT();

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================
// 26. TAREA 2: SERVIDOR WEB
// ============================================================

void TareaWeb(void *parameter) {

  for (;;) {

    server.handleClient();

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ============================================================
// 27. SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  // Pines de sensores.
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(UV_PIN, INPUT);

  // Pines de actuadores.
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  analogReadResolution(12);

  // Estado inicial de los actuadores.
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AMARILLO, LOW);
  digitalWrite(LED_ROJO, LOW);
  noTone(BUZZER_PIN);

  // LCD.
  lcd.begin(16, 2);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AQUASENSE");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");

  // Crear mutex.
  mutexDatos = xSemaphoreCreateMutex();

  if (mutexDatos == NULL) {

    Serial.println("ERROR: no se pudo crear el mutex");

    while (true) {
      delay(1000);
    }
  }

  // Iniciar comunicacion I2C.
  Wire.begin(SDA_PIN, SCL_PIN);

  // Buscar BME280 en sus dos direcciones habituales.
  if (bme.begin(0x76)) {

    bmeDisponible = true;
    Serial.println("BME280 detectado en 0x76");

  } else if (bme.begin(0x77)) {

    bmeDisponible = true;
    Serial.println("BME280 detectado en 0x77");

  } else {

    bmeDisponible = false;
    Serial.println("ADVERTENCIA: BME280 no detectado");
    Serial.println("El sistema seguira funcionando sin el BME280");
  }

  // Conectar Wi-Fi.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Conectando a Wi-Fi");

  unsigned long inicioWiFi = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicioWiFi < 25000) {

    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println();
    Serial.println("Wi-Fi conectado");

    Serial.print("Direccion IP: ");
    Serial.println(WiFi.localIP());

  } else {

    Serial.println();
    Serial.println("No se pudo conectar al Wi-Fi");
    Serial.println("El monitoreo local seguira funcionando");
  }

  // Configurar rutas del servidor.
  server.on("/", HTTP_GET, paginaPrincipal);
  server.on("/silenciar", HTTP_POST, silenciarAlarma);
  server.on("/activar", HTTP_POST, activarAlarma);

  server.begin();

  Serial.println("Servidor web iniciado");

  // Configuración del broker MQTT. La conexión la realiza
  // mantenerMQTT() dentro de TareaMedicion, sin bloquear el arranque.
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(768);
  mqttClient.setCallback(callbackMQTT);

  Serial.println("Configuracion MQTT establecida");

  // Mostrar direccion IP en el LCD.
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AQUASENSE listo");

  if (WiFi.status() == WL_CONNECTED) {

    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());

  } else {

    lcd.setCursor(0, 1);
    lcd.print("Sin Wi-Fi");
  }

  delay(1500);

  // ==========================================================
  // CREAR LOS DOS HILOS / TAREAS FREERTOS
  // ==========================================================

  // Tarea 1: sensores, calculos, LCD, alarmas y MQTT.
  BaseType_t resultadoMedicion = xTaskCreatePinnedToCore(
    TareaMedicion,
    "TareaMedicion",
    10240,
    NULL,
    2,
    NULL,
    1
  );

  // Tarea 2: servidor web y autenticacion.
  BaseType_t resultadoWeb = xTaskCreatePinnedToCore(
    TareaWeb,
    "TareaWeb",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  if (resultadoMedicion == pdPASS) {
    Serial.println("TareaMedicion creada correctamente");
  } else {
    Serial.println("ERROR al crear TareaMedicion");
  }

  if (resultadoWeb == pdPASS) {
    Serial.println("TareaWeb creada correctamente");
  } else {
    Serial.println("ERROR al crear TareaWeb");
  }

  Serial.println("--------------------------------");
  Serial.println("AQUASENSE INICIADO");
  Serial.println("Tarea 1: medicion, actuadores y MQTT");
  Serial.println("Tarea 2: servidor web y login");
  Serial.println("--------------------------------");
}

// ============================================================
// 28. LOOP PRINCIPAL
// ============================================================

void loop() {

  // Las dos tareas FreeRTOS realizan el trabajo principal.
  // El loop se mantiene libre para no bloquearlas.

  vTaskDelay(pdMS_TO_TICKS(1000));
}
