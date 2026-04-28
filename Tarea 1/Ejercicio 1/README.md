# Tarea N°1 — Comunicaciones Cableadas
## Bus Paralelo de 8 Bits entre Dos Arduino Uno
**Comunicación de Datos | 4to año — Ingeniería en Computación**  
**Autor:** Francisco Bevilacqua | **Versión firmware:** v4.0.0 | **Fecha:** 2026-04-26

---
# Ejercicio 1: Comunicación Paralela de 8 bits Unidireccional

Este directorio contiene el código fuente y la documentación correspondientes al **Ejercicio 1** de la Tarea N°1 de la materia Comunicación de Datos. El objetivo principal de esta práctica es diseñar, desarrollar y establecer el intercambio de información paralela entre dos dispositivos digitales (microcontroladores).

##  Consigna del Ejercicio

Mediante la utilización de dos dispositivos programables, establecer una comunicación de 8 bits unidireccional de tipo paralelo entre ellos cumpliendo con los siguientes requisitos:

* **Dispositivo 1 (Emisor):** Dispondrá de dos pulsadores. Uno será para *encender/apagar* y el otro tendrá la función de *cambiar*.
* **Dispositivo 2 (Receptor):** En sus salidas PWM se deberá conectar un LED RGB.
* **Acción de Encendido/Apagado:** Al presionar el botón *encender/apagar*, el dispositivo 1 deberá indicarle al dispositivo 2 que debe prender o apagar el LED, dependiendo del estado en el que se encuentre.
* **Acción de Cambio de Color:** Al presionar el botón *cambiar*, el dispositivo 1 creará un color RGB (3 intensidades de 0 a 255) de forma aleatoria e indicará al dispositivo 2 que, si se encuentra encendido, el LED deberá cambiar al color creado.
* **Manejo del Canal:** El intercambio de información debe darse solo cuando se presiona uno de los botones, enviándose la trama una sola vez y luego dejando el canal sin uso (es decir, todas las líneas físicas del bus en estado lógico cero).

---
## 1. Descripción del Sistema

El sistema establece una comunicación digital **unidireccional de 8 bits en modo paralelo** entre dos Arduino Uno. El **Dispositivo 1 (D1)** actúa como emisor y el **Dispositivo 2 (D2)** como receptor. El usuario controla el LED RGB conectado a D2 mediante dos pulsadores en D1.

---

## 2. Esquema de Bloques del Sistema de Comunicación

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        SISTEMA DE COMUNICACIÓN PARALELA                     │
├────────────────┬──────────────────────────────┬─────────────────────────────┤
│   FUENTE DE    │        TRANSMISOR             │       CANAL DE              │
│   INFORMACIÓN  │      (Dispositivo 1)          │    TRANSMISIÓN              │
│                │                               │                             │
│  Usuario       │  ┌─────────┐  ┌────────────┐ │  ┌─────────────────────┐   │
│  (presiona     │  │Detección│  │Constructor │ │  │ Bus paralelo 8 bits │   │
│   botones)     │→ │botones  │→ │de tramas   │ │  │ + señal STR         │   │
│                │  │debounce │  │+ checksum  │ │  │ (9 cables)          │   │
│                │  └─────────┘  └────────────┘ │  └─────────────────────┘   │
│                │                               │                             │
│                │  CODIFICACIÓN DEL MENSAJE:    │  MEDIO FÍSICO:              │
│                │  • T0: Control (CMD+STATE)    │  • Cables directos          │
│                │  • T1: Rojo (0-255)           │  • Distancia: 0-50 cm       │
│                │  • T2: Verde (0-255)          │  • Nivel lógico: 0V/5V      │
│                │  • T3: Azul (0-255)           │  • Velocidad: ~1 trama/36ms │
│                │  • T4: CHK = XOR(T0..T3)      │                             │
└────────────────┴──────────────────────────────┴─────────────────────────────┘
                                                          │
                                                          ↓
┌─────────────────────────────────────────────────────────────────────────────┐
│       RECEPTOR (Dispositivo 2)                │     DESTINO DE              │
│                                               │    INFORMACIÓN              │
│  ┌──────────────┐  ┌──────────┐  ┌────────┐  │                             │
│  │Detección STR │  │Verificac.│  │Actuador│  │  LED RGB                    │
│  │(flanco↑)     │→ │checksum  │→ │PWM LED │→ │  (respuesta visual)         │
│  │+ leerBus()   │  │XOR local │  │R/G/B   │  │                             │
│  └──────────────┘  └──────────┘  └────────┘  │                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Funcionalidad de cada bloque

| Bloque | Dispositivo | Función |
|--------|-------------|---------|
| **Fuente de información** | D1 | Usuario que presiona los pulsadores físicos |
| **Codificador / Constructor de trama** | D1 | Arma la trama de 5 bytes y calcula el checksum XOR |
| **Transmisor físico** | D1 | Escribe cada byte en los 8 pines GPIO del bus y genera el pulso STR |
| **Canal de transmisión** | Cables | 8 cables de datos + 1 cable STR + 1 cable GND (10 cables totales) |
| **Receptor físico** | D2 | Detecta el flanco ascendente de STR y lee los 8 pines del bus |
| **Decodificador / Verificador** | D2 | Verifica el checksum XOR y descarta tramas corruptas |
| **Destino de información** | D2 → LED | LED RGB que refleja el estado y color indicado por D1 |

---

## 3. Protocolo de Comunicación

### 3.1 Estructura de Trama

Cada transmisión consiste en **5 transferencias de 8 bits** secuenciales, señalizadas individualmente por el strobe (STR):

```
 Transmisión completa (una presión de botón):
 ┌──────────┬──────────┬──────────┬──────────┬──────────┐
 │   T0     │   T1     │   T2     │   T3     │   T4     │
 │ Control  │  Rojo    │  Verde   │  Azul    │ Checksum │
 │ (1 byte) │ (1 byte) │ (1 byte) │ (1 byte) │ (1 byte) │
 └──────────┴──────────┴──────────┴──────────┴──────────┘
```

**T0 — Trama de control:**
```
 Bit: 7    6    5    4    3    2    1    0
      CMD  0    0    0    0    0    0  STATE
      │                                 │
      │                                 └── 1=ENCENDER | 0=APAGAR (solo si CMD=0)
      └── 0=ON/OFF | 1=COLOR
```

**T4 — Checksum:**
```
 CHK = T0 XOR T1 XOR T2 XOR T3
```

### 3.2 Justificación del Checksum XOR

El checksum XOR fue elegido porque:
- **Detecta errores de bit único** en cualquier posición del byte.
- **Detecta errores en múltiples bits** si afectan a bytes distintos.
- **Costo computacional mínimo** en el ATmega328P (una instrucción EOR por byte).
- **Simétrico**: la misma operación sirve para calcular y verificar.

Limitación: no detecta errores donde el mismo bit falla en dos bytes distintos (se cancelan en el XOR). Para esta aplicación con cables cortos, el XOR es suficiente.

### 3.3 Timing por Trama (Cronograma)

```
            D1 escribe         D1 activa STR           D1 limpia bus
            byte en bus        (D2 debe leer)
                │                   │                       │
 Bus (D0–D7): ──┤ DATO ESTABLE ─────│───────────────────────│── 0x00 ──
                │                   │                       │
 STR:         ──┘LOW                │HIGH          LOW      │
                │                   │    ╔══════╗           │
                │                   │    ║  D2  ║           │
                │                   │    ║ lee  ║           │
                │                   │    ║ bus  ║           │
                │                   │    ╚══════╝           │
                │<-- 15 ms Setup -->│<-- 8 ms STR_HIGH ---->│<-- 8 ms Hold -->│<-- 5 ms Gap -->
                                                        (STR baja aquí)

 Duración total por trama: 15 + 8 + 8 + 5 = 36 ms
 Transmisión completa (5 tramas): ~180 ms
```

**Criterio de diseño para evitar lecturas erróneas durante transiciones:**

D2 solo lee el bus cuando detecta el **flanco ascendente** de STR (LOW→HIGH). En ese momento, D1 ya esperó 15 ms desde que escribió el bus (setup time), garantizando que todos los pines estabilizaron su nivel lógico antes de habilitar la lectura. La señal STR actúa como señal de habilitación (*enable*): sin ella, D2 ignora completamente el estado del bus. Esto elimina lecturas en transición.

---

## 4. Causa Raíz del Error y Solución (v3 → v4)

### 4.1 Síntoma Observado en Logs

```
 Diferencia siempre = 0x06 (bits D1 y D2)
 Error solo en T4 (CHK), nunca en T0–T3
 Error solo en tramas COLOR, no en ON/OFF
 Timing inter-trama regular → no es problema de timing
```

### 4.2 Causa Raíz: Crosstalk Capacitivo PWM → Bus

El **pin 3 del Arduino** es `OC2B` (salida del Timer2). Este mismo timer controla el PWM del LED RGB (pin 11 = `OC2A`). Cuando D2 ejecuta `analogWrite()` después de recibir un COLOR, las corrientes de conmutación del Timer2 a ~490 Hz generan pulsos de ruido que se acoplan capacitivamente a los cables adyacentes del protoboard, afectando los pines 3 y 4 del bus (D1 y D2).

### 4.3 Solución: Reasignación de Pines

| Señal de bus | v3 (con error) | v4 (corregido) | Razón |
|---|---|---|---|
| **D1** | pin 3 (OC2B/Timer2 ⚠️) | **pin A2** (GPIO puro ✅) | Eliminación de interferencia PWM |
| **D2** | pin 4 (adyacente a OC2B ⚠️) | **pin A3** (GPIO puro ✅) | Separación física del Timer2 |

### 4.4 Mejoras Complementarias en v4

- **Doble lectura de verificación** en `leerBus()`: lee dos veces con 50 µs de intervalo; si los valores difieren, reintenta.
- **Verificación de escritura** en `escribirByte()`: lee de vuelta cada pin después de escribirlo; reintenta si hay discrepancia.
- **Hold time** aumentado de 5 ms a 8 ms.
- **Inter-frame** aumentado de 3 ms a 5 ms.

---

## 5. Conexión Eléctrica

### 5.1 Diagrama de Conexiones

```
 DISPOSITIVO 1 (Arduino Uno — Emisor)        DISPOSITIVO 2 (Arduino Uno — Receptor)
 ┌───────────────────────────────────┐        ┌────────────────────────────────────────┐
 │                                   │        │                                        │
 │   pin 2  (D0) ────────────────────┼────────┼──── pin 2  (D0)                       │
 │   pin A2 (D1) ────────────────────┼────────┼──── pin A2 (D1) ← v4: era pin3        │
 │   pin A3 (D2) ────────────────────┼────────┼──── pin A3 (D2) ← v4: era pin4        │
 │   pin 5  (D3) ────────────────────┼────────┼──── pin 5  (D3)                       │
 │   pin 6  (D4) ────────────────────┼────────┼──── pin 6  (D4)                       │
 │   pin 7  (D5) ────────────────────┼────────┼──── pin 7  (D5)                       │
 │   pin 8  (D6) ────────────────────┼────────┼──── pin 8  (D6)                       │
 │   pin A0 (D7) ────────────────────┼────────┼──── pin A0 (D7)                       │
 │   pin A1 (STR)────────────────────┼────────┼──── pin A1 (STR)                      │
 │   GND ────────────────────────────┼────────┼──── GND  ← ¡OBLIGATORIO!              │
 │                                   │        │                                        │
 │   pin 11 ─── [BTN_ONOFF] ─── GND │        │   pin 9  ─── [R 220Ω] ─── LED_R      │
 │   pin 12 ─── [BTN_CAMBIAR] ─ GND │        │   pin 10 ─── [R 220Ω] ─── LED_G      │
 │                                   │        │   pin 11 ─── [R 220Ω] ─── LED_B      │
 │   (pull-up interno habilitado)    │        │   GND ────────────────── LED_GND      │
 └───────────────────────────────────┘        └────────────────────────────────────────┘
```

### 5.2 Tabla de Conexiones Completa

| Cable | D1 (pin) | D2 (pin) | Descripción |
|-------|----------|----------|-------------|
| 1 | 2 | 2 | Bit 0 del bus (D0) |
| 2 | **A2** | **A2** | Bit 1 del bus (D1) ← v4 nuevo |
| 3 | **A3** | **A3** | Bit 2 del bus (D2) ← v4 nuevo |
| 4 | 5 | 5 | Bit 3 del bus (D3) |
| 5 | 6 | 6 | Bit 4 del bus (D4) |
| 6 | 7 | 7 | Bit 5 del bus (D5) |
| 7 | 8 | 8 | Bit 6 del bus (D6) |
| 8 | A0 | A0 | Bit 7 del bus (D7) |
| 9 | A1 | A1 | Strobe (STR) |
| 10 | GND | GND | Referencia común (**crítico**) |

### 5.3 LED RGB en Dispositivo 2

```
 Arduino D2           LED RGB (cátodo común)
 pin 9  ──── 220Ω ──── R (ánodo)
 pin 10 ──── 220Ω ──── G (ánodo)
 pin 11 ──── 220Ω ──── B (ánodo)
 GND   ──────────────── C (cátodo común)
```

> **Nota:** Si el LED es de **ánodo común**, conectar el ánodo común a 5V y negar los valores PWM en el firmware (`analogWrite(pin, 255 - valor)`).

### 5.4 Pulsadores en Dispositivo 1

```
 Arduino D1           Pulsador
 pin 11 ──────────────┤ BTN_ONOFF ├──── GND
                       (INPUT_PULLUP habilitado — sin resistencia externa)

 pin 12 ──────────────┤ BTN_CAMBIAR├─── GND
                       (INPUT_PULLUP habilitado — sin resistencia externa)
```

---

## 6. Manejo de Errores

### 6.1 Checksum Incorrecto
- D2 **descarta la trama completa** y no modifica el estado del LED.
- Imprime los bits erróneos identificados por posición (D0..D7).
- Incrementa `erroresChk` para estadísticas de sesión.

### 6.2 Timeout entre Tramas
- Si D2 no detecta el flanco de STR en 1000 ms, cancela la recepción.
- Imprime diagnóstico con la trama T0 recibida antes del fallo.
- Incrementa `erroresTimeout`.

### 6.3 Fallo de Escritura en Bus (D1)
- `escribirByte()` verifica cada pin con lectura de vuelta.
- Si tras 3 reintentos el bus no es consistente, loguea el error y continúa.
- El checksum en T4 detectará la inconsistencia en D2.

### 6.4 Lectura Inestable en Bus (D2)
- `leerBus()` realiza doble lectura con 50 µs de intervalo.
- Si ambas lecturas difieren, reintenta hasta 3 veces.
- Si persiste, usa la última lectura y registra el evento en `contadorReintentos`.

---

## 7. Archivos del Proyecto

| Archivo | Descripción |
|---------|-------------|
| `dispositivo1_paralelo_v4.ino` | Firmware emisor (D1) — control de botones y transmisión |
| `dispositivo2_paralelo_v4.ino` | Firmware receptor (D2) — recepción, verificación y LED RGB |
| `README.md` | Este documento |

---

## 8. Instrucciones de Instalación y Verificación

1. **Actualizar firmware:** grabar `dispositivo1_paralelo_v4.ino` en D1 y `dispositivo2_paralelo_v4.ino` en D2.
2. **Reconectar bus:** mover los cables que antes iban a pin3/pin4 a **pinA2/pinA3** en **ambos** dispositivos.
3. **Verificar GND común:** confirmar que hay un cable GND de D1 a D2.
4. **Abrir monitores seriales** de ambos dispositivos a 115200 bps.
5. **Presionar BTN_ONOFF** en D1: el LED de D2 debe encenderse con el último color.
6. **Presionar BTN_CAMBIAR** en D1: el LED de D2 debe cambiar de color aleatoriamente.
7. **Verificar en monitor D2** que todos los RX muestran `[CHK] OK` sin errores.
