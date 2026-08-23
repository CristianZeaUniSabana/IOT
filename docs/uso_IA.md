# Uso Transparente de Inteligencia Artificial — AQUASENSE

## Herramientas utilizadas
- Google Gemini
- Claude (Anthropic)

## Prompts utilizados

### Prompt 1 — Diseño de arquitectura (Gemini)
> *¿Cómo podría ser la arquitectura de un sistema de monitoreo en tiempo real para un tanque/fuente hídrica utilizando un microcontrolador ESP32 DevKit V1?*

**Resultado generado:** Diagrama de arquitectura por capas, tabla de justificación de componentes, diagrama de señales, flujo de procesamiento Edge y definición de interfaz de actuación.

### Prompt 2 — Revisión de código y checklist de requisitos (Claude)
Se utilizó Claude para verificar que el firmware (`aquasense.ino`) cumpliera con los Requisitos Funcionales (RF-01 a RF-08) definidos en la Wiki e identificar brechas técnicas.

**Resultado:** Se identificó la ausencia del sensor UV, el cálculo explícito de la tasa de descenso, la fusión completa del IRH y el patrón intermitente del buzzer para el estado de Advertencia.

**Validación del equipo:** Se priorizaron e implementaron manualmente los ajustes necesarios, corroborando su funcionamiento en Wokwi antes de su integración definitiva.

### Prompt 3 — Sincronización Técnica de Hardware/Firmware y Redacción (Gemini)
Se adjuntaron capturas de pantalla del esquemático, diagramas de bloques previamente elaborados y fragmentos de código/simulación (`sketch.ino` y `diagram.json` de Wokwi) para contrastar y alinear la documentación de los Módulos 05 al 08 con el prototipo real simulado.

**Resultado:** Corrección del pinout del ESP32 (LCD en modo paralelo, GPIOs de LEDs/Buzzer), actualización de la matriz de componentes (reemplazo del BME280 por el sensor BMP180 simulado), ajuste de diagramas ASCII por capas, redacción de la arquitectura de software (Módulo 07) y síntesis de conclusiones y trabajo futuro (Módulo 08).

## Protocolo de validación
Todo contenido generado o refinado mediante herramientas de Inteligencia Artificial fue revisado, corregido y contrastado técnicamente por el equipo de trabajo contra:
1. Las hojas de datos (*datasheets*) y restricciones físicas de los componentes reales.
2. El comportamiento observado y validado en la simulación interactiva de Wokwi.
3. Los requisitos funcionales, no funcionales y de entrega establecidos en la guía oficial de la asignatura.
