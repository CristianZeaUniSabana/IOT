# AQUASENSE — Sistema IoT de Monitoreo y Alerta Temprana para Riesgo de Escasez Hídrica

**Asignatura:** Internet de las Cosas (IoT)
**Institución:** Universidad de La Sabana — Facultad de Ingeniería
**Semestre:** 2026-2
**Docente:** Alejandro Beltrán (@afbeltranp)

## Resumen

AQUASENSE es un prototipo funcional de bajo costo para el monitoreo en tiempo real de la disponibilidad y riesgo de escasez de agua en puntos críticos de almacenamiento de la región de Sabana Centro (Cundinamarca). A diferencia de los sistemas tradicionales basados en umbrales estáticos, AQUASENSE integra lógica de fusión de datos en el Edge (ESP32), combinando el nivel del reservorio, la tasa de descenso del recurso y variables meteorológicas (temperatura, humedad, presión y radiación UV) para calcular un Índice de Riesgo Hídrico (IRH) dinámico de 0 a 100.

El sistema opera de forma completamente autónoma, sin depender de redes de comunicación convencionales ni servicios en la nube, emitiendo alertas físicas in situ (LCD, semáforo de LEDs y buzzer) para comunidades rurales y autoridades locales.

Ver la [Wiki completa del proyecto](https://github.com/CristianZeaUniSabana/IOT/wiki/Sistema-IoT-de-Monitoreo-y-Alerta-Temprana-para-Riesgo-de-Escasez-H%C3%ADdrica) para el detalle de requisitos, restricciones y justificación de componentes.

## Simulación

🔗 **Simulación en Wokwi:** [wokwi.com/projects/472973110009658369](https://wokwi.com/projects/472973110009658369)

## Componentes principales

| Componente | Modelo | Función |
|---|---|---|
| Microcontrolador | ESP32 DevKit V1 | Procesamiento Edge y fusión de datos |
| Sensor de nivel | JSN-SR04T (IP67) | Medición ultrasónica del nivel de agua |
| Sensor climático | BME280 (I2C) | Temperatura, humedad y presión |
| Sensor UV | GUVA-S12SD | Índice de radiación solar |
| Pantalla | LCD 16x2 | Interfaz visual local |
| Indicador | Semáforo de LEDs (verde/amarillo/rojo) | Estado del riesgo hídrico |
| Alerta sonora | Buzzer piezoeléctrico | Alerta audible según gravedad |

## Estructura del repositorio

```
IOT/
├── firmware/
│   └── aquasense.ino       # Código fuente del ESP32
├── docs/
│   ├── diagrama-conexiones.png
│   └── uso-transparente-de-IA.md
└── README.md
```

## Cómo correr la simulación

1. Abre el link de Wokwi de arriba.
2. Dale click a ▶️ Start Simulation.
3. Observa el LCD, el semáforo de LEDs y el buzzer reaccionando a los cambios de nivel de agua y condiciones ambientales.
4. El código fuente también está disponible en [`firmware/aquasense.ino`](firmware/aquasense.ino) para revisión o modificación en el Arduino IDE.
