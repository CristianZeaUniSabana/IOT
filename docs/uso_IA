# Uso Transparente de Inteligencia Artificial — AQUASENSE

## Herramientas utilizadas
- Google Gemini
- Claude (Anthropic)

## Prompts utilizados

### Prompt 1 — Diseño de arquitectura (Gemini)
[pegar aquí el prompt completo que me compartiste]

**Resultado generado:** diagrama de arquitectura por capas, tabla de
justificación de componentes, diagrama de señales, flujo de
procesamiento Edge y definición de interfaz de actuación.

**Validación del equipo:** se revisó que los pines y protocolos
propuestos (I2C, GPIO, ADC) fueran compatibles con el ESP32 DevKit V1
real; se ajustó [lo que hayan cambiado, ej. "el nombre de las
variables" o "se corrigió que faltaba el sensor UV en el circuito
armado en Wokwi"].

### Prompt 2 — Revisión de código y checklist de requisitos (Claude)
Se usó Claude para verificar que el firmware (`aquasense.ino`)
cumpliera los requisitos funcionales (RF-01 a RF-08) definidos en la
Wiki, e identificar brechas.

**Resultado:** Claude identificó que faltaban el sensor UV, el
cálculo de tasa de descenso, la fusión completa de 6 variables en el
IRH, y un patrón de buzzer intermitente para el estado de Advertencia.

**Validación del equipo:** se implementaron/priorizaron los cambios
sugeridos de forma manual, verificando su correcto funcionamiento en
la simulación de Wokwi antes de integrarlos al firmware final.

## Protocolo de validación
Todo contenido generado por IA fue revisado y contrastado por el
equipo contra: (1) la hoja de datos de los componentes reales, (2) el
comportamiento observado en la simulación de Wokwi, y (3) los
requisitos funcionales y no funcionales definidos en la Wiki del
proyecto, antes de ser incorporado al entregable final.
