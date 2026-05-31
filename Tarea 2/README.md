# Sistema de Control Inalámbrico Infrarrojo Bidireccional

**Trabajo Práctico N.° 2 — Comunicaciones Inalámbricas**  
Universidad Nacional de Rafaela — Ingeniería en Computación  
Comunicación de Datos — 4.° año — 1.° Cuatrimestre 2026  
Bevilacqua Francisco - Clement Sebastian

> Sincronización de estado cromático RGB entre dos nodos Arduino UNO mediante protocolo NEC infrarrojo, con handshaking bidireccional por IR, control de errores por cabecera, persistencia en EEPROM y ARQ stop-and-wait.

---

## 📋 Tabla de contenidos

- [Descripción del proyecto](#descripción-del-proyecto)
- [Consigna del trabajo](#consigna-del-trabajo)
- [Protocolo de comunicación](#protocolo-de-comunicación)
  - [Elección del protocolo NEC](#elección-del-protocolo-nec)
  - [Estructura de la trama de 32 bits](#estructura-de-la-trama-de-32-bits)
  - [Trama de ACK](#trama-de-ack)
  - [Uso de sendNECRaw()](#uso-de-sendnecraw)
- [Arquitectura del sistema](#arquitectura-del-sistema)
- [Hardware y conexiones](#hardware-y-conexiones)
  - [Componentes utilizados](#componentes-utilizados)
  - [Mapa de pines — Esclavo](#mapa-de-pines--esclavo-dispositivo-1)
  - [Mapa de pines — Maestro](#mapa-de-pines--maestro-dispositivo-2)
- [Firmware](#firmware)
  - [Esclavo — flujo de ejecución](#esclavo--flujo-de-ejecución)
  - [Maestro — flujo de ejecución](#maestro--flujo-de-ejecución)
  - [Handshaking bidireccional](#handshaking-bidireccional)
  - [Gestión half-duplex del canal IR](#gestión-half-duplex-del-canal-ir)
- [Secuencia de colores](#secuencia-de-colores)
- [Persistencia en EEPROM](#persistencia-en-eeprom)
- [Escenarios de falla y resiliencia](#escenarios-de-falla-y-resiliencia)
- [Librerías utilizadas](#librerías-utilizadas)
- [Estructura del repositorio](#estructura-del-repositorio)
- [Instrucciones de uso](#instrucciones-de-uso)
- [Autores](#autores)

---

## Descripción del proyecto

El sistema implementa un enlace inalámbrico infrarrojo **bidireccional y completamente inalámbrico** entre dos nodos Arduino UNO. Emula la lógica de control de set-point de un sistema de climatización moderno: el **Maestro** (control remoto) y el **Esclavo** (unidad controlada) deben mantener coherencia de estado cromático en todo momento.

Cada vez que el usuario presiona el botón del Maestro, el sistema:

1. Calcula el siguiente color en la secuencia circular de 8 colores.
2. Empaqueta el color en una trama IR de 32 bits con cabecera de identificación `0xAA`.
3. Transmite la trama al Esclavo por infrarrojo (protocolo NEC, 38 kHz).
4. El Esclavo valida la trama, aplica el color a su LED, persiste el estado en EEPROM y responde con un **ACK infrarrojo** de 32 bits (`0xBB` + mismo RGB).
5. El Maestro verifica el ACK byte a byte. Solo si coincide exactamente actualiza su propio LED y persiste el índice. En caso contrario, reintenta hasta 3 veces antes de mantener el estado anterior.

El sistema es **puramente inalámbrico**: no hay ningún cable entre los dos nodos.

---

## Consigna del trabajo

El trabajo exigió:

- **Investigación de protocolos IR:** comparar NEC, RC-5, Sony SIRC, entre otros; seleccionar uno y justificar la elección.
- **Diseño de trama propia:** no se permite enviar una simple orden de "cambio"; la trama debe transportar los valores RGB específicos con un mecanismo de identificación.
- **Dispositivo 1 (Esclavo):** recibir el color, actualizar el LED, confirmar recepción al Maestro. Sin botones; estado dependiente exclusivamente de la comunicación.
- **Dispositivo 2 (Maestro):** ciclar 8 colores con pulsador; enviar la trama; verificar el ACK antes de actualizar su propio LED; mostrar el último color confirmado exitosamente.
- **Persistencia en EEPROM:** al energizarse, cada dispositivo recupera su último estado. El sistema debe poder sincronizarse ante divergencias de estado.
- **ARQ:** reintentar hasta 3 veces si no llega confirmación.

---

## Protocolo de comunicación

### Elección del protocolo NEC

Se evaluaron tres estándares IR antes de seleccionar NEC:

| Criterio | NEC | RC-5 (Philips) | Sony SIRC |
|---|---|---|---|
| Frecuencia portadora | **38 kHz** | 36 kHz | 40 kHz |
| Bits de datos | **32 bits** | 14 bits | 12–20 bits |
| Codificación | Pulse Distance (PDM) | Biphase (Manchester) | Pulse Width (PWM) |
| Duración trama | ~67,5 ms | ~24,9 ms | ~45 ms |
| Soporte `sendNECRaw()` | **Sí** | No aplica | No aplica |
| Capacidad `[H\|R\|G\|B]` | **Suficiente** | Insuficiente | Insuficiente |
| Módulo KY-022 | **Compatible (38 kHz)** | Incompatible | Incompatible |

**NEC fue seleccionado** por tres razones decisivas:
- Sus **32 bits de payload** permiten transportar un byte de cabecera + R + G + B en una única trama atómica, sin fragmentación.
- Opera a **38 kHz**, frecuencia exacta del módulo receptor KY-022 (TSOP38238).
- La función `sendNECRaw()` de IRremote v4 permite usar el frame NEC como transporte de datos arbitrarios de 32 bits, sin que la librería interprete ni trunque el contenido.

### Estructura de la trama de 32 bits

```
 Bit 31          24  23          16  15           8  7            0
┌────────────────────┬────────────────┬────────────────┬────────────────┐
│   CABECERA (8 b)   │    RED (8 b)   │   GREEN (8 b)  │   BLUE (8 b)   │
│      0xAA          │    0x00–0xFF   │   0x00–0xFF    │   0x00–0xFF    │
└────────────────────┴────────────────┴────────────────┴────────────────┘
```

**Justificación de `0xAA` como cabecera:**
- El patrón binario `10101010` garantiza que la trama nunca empieza con ceros en el byte más significativo, evitando comportamientos inesperados de `sendNECRaw()`.
- La alternancia de bits genera una firma espectral reconocible en el osciloscopio, facilitando la identificación visual del inicio de trama.
- Cualquier trama recibida con cabecera diferente de `0xAA` es descartada silenciosamente por el Esclavo, actuando como primer filtro contra interferencia ambiental.

**Ejemplo — color Amarillo (R=255, G=200, B=0):**

```
Trama = (0xAA << 24) | (255 << 16) | (200 << 8) | 0
      = 0xAAFFC800
      
Binario: 10101010 11111111 11001000 00000000
         [Cabec.]  [  R  ]  [  G  ]  [  B  ]
```

### Trama de ACK

El Esclavo responde con una trama de estructura idéntica pero con cabecera `0xBB`:

```
┌────────────────────┬────────────────┬────────────────┬────────────────┐
│      0xBB          │  R (recibido)  │  G (recibido)  │  B (recibido)  │
└────────────────────┴────────────────┴────────────────┴────────────────┘
```

El Maestro verifica **byte a byte** que los tres valores RGB del ACK coincidan exactamente con los enviados. Esto garantiza que el Esclavo aplicó el color correcto, y no una trama de otro ciclo o con datos parcialmente corruptos. La cabecera `0xBB` (`10111011`) es distinta de `0xAA`, lo que impide que el Maestro confunda el eco de su propio comando con un ACK válido.

### Uso de `sendNECRaw()`

La función estándar `sendNEC(address, command, repeats)` de IRremote v4 interpreta los parámetros según el protocolo NEC original: `address` de 16 bits y `command` de **8 bits**. Al pasarle una variable de 32 bits como `command`, la función la **trunca a 8 bits**, descartando los canales G, B y la cabecera.

La solución fue usar `sendNECRaw(uint32_t data, uint8_t repeats)`, que transmite los 32 bits como payload completo sin modificación. El receptor obtiene estos 32 bits íntegros mediante `IrReceiver.decodedIRData.decodedRawData`.

> ⚠️ **Nota crítica:** `IR_SEND_PIN` debe definirse **antes** del `#include <IRremote.hpp>`. Si se define después, la librería ya configuró su pin por defecto en tiempo de compilación y la definición tardía no tiene efecto.

---

## Arquitectura del sistema

```
  ┌─────────────────────────────┐          ┌─────────────────────────────┐
  │    DISPOSITIVO 2 — MAESTRO  │          │   DISPOSITIVO 1 — ESCLAVO   │
  │        Arduino UNO          │          │        Arduino UNO          │
  │                             │          │                             │
  │  [BTN D7]  [LED RGB D9/10/6]│          │         [LED RGB D9/10/6]   │
  │                             │          │                             │
  │  KY-005 (TX IR) ── D4       │────────▶│  D2 ── KY-022 (RX IR)       │
  │  KY-022 (RX IR) ── D2       │◀────────│  D3 ── KY-005 (TX IR)       │
  │                             │          │                             │
  │  EEPROM: índice color       │          │  EEPROM: R, G, B            │
  └─────────────────────────────┘          └─────────────────────────────┘
         ▲                                          ▲
         │ CMD [0xAA | R | G | B]                   │
         │ ──────────────────────────────────────▶ │
         │                                          │
         │ ACK [0xBB | R | G | B]                   │
         │ ◀────────────────────────────────────── │
```

**Canal de comunicación:** infrarrojo 940 nm, modulado a 38 kHz, protocolo NEC raw.  
**Modo dúplex:** half-duplex gestionado por software (`IrReceiver.stop()` / `IrReceiver.start()`).  
**Alcance operativo:** 1–2 metros con visión directa entre módulos.

---

## Hardware y conexiones

### Componentes utilizados

| Cantidad | Componente | Descripción |
|---|---|---|
| 2 | Arduino UNO | Microcontrolador ATmega328P |
| 2 | KY-005 | Módulo emisor IR — LED 940 nm |
| 2 | KY-022 | Módulo receptor IR — TSOP38238, 38 kHz |
| 1 | KY-016 | Módulo LED RGB cátodo común (Maestro) |
| 1 | LED RGB cátodo común | LED RGB 4 pines (Esclavo) |
| 3 | Resistencia 220 Ω | Para LED RGB del Esclavo |
| 1 | Pulsador (push button) | 4 pines, para el Maestro |
| 2 | Protoboard | Montaje del circuito |
| — | Cables Dupont | Macho-macho y macho-hembra |

### Mapa de pines — Esclavo (Dispositivo 1)

| Componente | Pin módulo | Pin Arduino | Recurso AVR | Notas |
|---|---|---|---|---|
| KY-022 Receptor IR | S (DATA) | **D2** | INT0 | IRremote requiere INT0 para captura por interrupción |
| KY-022 | V+ / GND | 5V / GND | — | Alimentación TSOP38238 |
| KY-005 Emisor IR | S (DATA) | **D3** | OC2B / Timer2 | `IR_SEND_PIN 3`; Timer2 genera portadora 38 kHz |
| KY-005 | V+ / GND | 5V / GND | — | Alimentación LED IR |
| LED RGB — Rojo | Ánodo | **D9** → 220Ω | OC1A / Timer1 | PWM; Timer1 no colisiona con Timer2 |
| LED RGB — Verde | Ánodo | **D10** → 220Ω | OC1B / Timer1 | PWM; Timer1 |
| LED RGB — Azul | Ánodo | **D6** → 220Ω | OC0A / Timer0 | PWM; Timer0 no colisiona con Timer2 |
| LED RGB — Cátodo | — | GND | — | LED cátodo común |

### Mapa de pines — Maestro (Dispositivo 2)

| Componente | Pin módulo | Pin Arduino | Recurso AVR | Notas |
|---|---|---|---|---|
| KY-005 Emisor IR | S (DATA) | **D4** | GPIO | `IR_SEND_PIN 4`; distinto de D3 para evitar colisión de Timer2 |
| KY-005 | V+ / GND | 5V / GND | — | — |
| KY-022 Receptor IR | S (DATA) | **D2** | INT0 | Captura del ACK del Esclavo |
| KY-022 | V+ / GND | 5V / GND | — | — |
| Pulsador | Terminal 1 | **D7** | GPIO | `INPUT_PULLUP`; activo en LOW al presionar |
| Pulsador | Terminal 2 | GND | — | Referencia de tierra |
| LED RGB — Rojo | Ánodo | **D9** → 220Ω | OC1A / Timer1 | PWM |
| LED RGB — Verde | Ánodo | **D10** → 220Ω | OC1B / Timer1 | PWM |
| LED RGB — Azul | Ánodo | **D6** → 220Ω | OC0A / Timer0 | PWM |
| LED RGB — Cátodo | — | GND | — | — |

> **¿Por qué el Maestro usa D4 para TX y el Esclavo usa D3?**  
> En la arquitectura AVR del ATmega328P, Timer2 es el recurso que IRremote utiliza para generar la portadora de 38 kHz. D3 es el pin canónico OC2B de Timer2. En el Esclavo, esta asignación no genera conflictos. En el Maestro, mover el TX a D4 libera Timer2 de posibles interferencias con los otros periféricos activos en ese nodo, garantizando la estabilidad de la portadora.

---

## Firmware

### Esclavo — flujo de ejecución

```
SETUP:
  1. Serial.begin(9600)
  2. Pines LED como OUTPUT
  3. IrReceiver.begin(D2, DISABLE_LED_FEEDBACK)
  4. Leer EEPROM[0] == 0xA5 ?
       SÍ → leer R=EEPROM[1], G=EEPROM[2], B=EEPROM[3] → setColor(R,G,B)
       NO → setColor(0,0,0)  [estado seguro]

LOOP:
  Si IrReceiver.decode():
    Si flag IRDATA_FLAGS_IS_REPEAT → resume() y descartar
    
    trama = decodedIRData.decodedRawData
    resume()
    
    cabecera = trama >> 24
    R = (trama >> 16) & 0xFF
    G = (trama >>  8) & 0xFF
    B =  trama        & 0xFF
    
    Si cabecera == 0xAA:
      setColor(R, G, B)
      guardarEEPROM(R, G, B)          ← persiste ANTES de responder
      IrReceiver.stop()
      delay(100)                       ← margen para que Maestro active RX
      IrSender.sendNECRaw(0xBB|R|G|B, 0)
      IrReceiver.start()
    
    Si cabecera != 0xAA:
      descartar silenciosamente        ← filtro de ruido ambiental
```

### Maestro — flujo de ejecución

```
SETUP:
  1. Serial.begin(9600)
  2. pinMode(D7, INPUT_PULLUP)
  3. Pines LED como OUTPUT
  4. IrReceiver.begin(D2, DISABLE_LED_FEEDBACK)
  5. Leer EEPROM[0] == 0xA5 ?
       SÍ → colorActual = EEPROM[1]; sanity check (>=8 → reset a 0)
       NO → colorActual = 0
  6. setColor(COLORES[colorActual])

LOOP:
  Si digitalRead(D7) == LOW:
    delay(50)                          ← antirrebote
    Si digitalRead(D7) == LOW:        ← confirmar pulsación real
      
      siguiente = (colorActual + 1) % 8
      R, G, B = COLORES[siguiente]
      
      ack = false
      Para intento en [0, MAX_REINTENTOS=3) mientras !ack:
        Si intento > 0: delay(300)    ← pausa entre reintentos
        ack = enviarColor(R, G, B)
      
      Si ack:
        colorActual = siguiente
        setColor(R, G, B)
        guardarEEPROM(colorActual)
      Si !ack:
        mantener estado actual
      
      delay(500)
      Esperar liberación del botón
```

### `enviarColor(R, G, B)` — handshaking completo

```
trama = (0xAA << 24) | (R << 16) | (G << 8) | B

IrReceiver.stop()
IrSender.sendNECRaw(trama, 0)
IrReceiver.start()

t = millis()
Mientras (millis() - t) < 2000:          ← timeout 2 segundos
  Si IrReceiver.decode():
    Si flag IS_REPEAT → resume(); continuar
    
    respuesta = decodedRawData
    cab = respuesta >> 24
    resume()
    
    Si cab==0xBB
       Y (respuesta>>16)&0xFF == R
       Y (respuesta>> 8)&0xFF == G
       Y  respuesta     &0xFF == B:
         return TRUE                       ← ACK válido

return FALSE                               ← timeout
```

### Handshaking bidireccional

```
MAESTRO                                         ESCLAVO
   │                                               │
   │  [usuario presiona botón]                     │
   │  calcula siguiente color                      │
   │  IrReceiver.stop()                            │
   │──── sendNECRaw([0xAA|R|G|B]) ───────────────▶│
   │  IrReceiver.start()                           │  decode() → true
   │  espera ACK (timeout 2000ms)                  │  filtra IS_REPEAT
   │                                               │  verifica cab == 0xAA
   │                                               │  setColor(R,G,B)
   │                                               │  guardarEEPROM(R,G,B)
   │                                               │  IrReceiver.stop()
   │                                               │  delay(100)
   │◀─── sendNECRaw([0xBB|R|G|B]) ───────────────│
   │                                               │  IrReceiver.start()
   │  decode() → true                              │
   │  verifica cab==0xBB y R,G,B byte a byte       │
   │  colorActual = siguiente                      │
   │  setColor(R,G,B)                              │
   │  guardarEEPROM(colorActual)                   │
```

### Gestión half-duplex del canal IR

El canal IR es un medio compartido: si ambos nodos emiten simultáneamente, las señales se superponen y el receptor no puede decodificar nada. La gestión del turno se implementa por software:

- **Antes de emitir:** `IrReceiver.stop()` inhabilita la interrupción INT0 del receptor propio, impidiendo que el KY-022 capture el eco del KY-005 local.
- **Después de emitir:** `IrReceiver.start()` reactiva el receptor.
- **Delay de 100 ms en el Esclavo:** da tiempo al Maestro de finalizar su transmisión y activar su receptor antes de que el ACK llegue. Este es el parámetro de timing más crítico del sistema.

---

## Secuencia de colores

| Índice | Color | R | G | B | Trama CMD |
|---|---|---|---|---|---|
| 0 | 🔴 Rojo | 255 | 0 | 0 | `0xAA000000` |
| 1 | 🟡 Amarillo | 255 | 200 | 0 | `0xAAFFC800` |
| 2 | 🟢 Verde | 0 | 255 | 0 | `0xAA00FF00` |
| 3 | 🩵 Celeste | 0 | 255 | 255 | `0xAA00FFFF` |
| 4 | 🔵 Azul | 0 | 0 | 255 | `0xAA0000FF` |
| 5 | 🟣 Lila | 180 | 0 | 255 | `0xAAB400FF` |
| 6 | ⚪ Blanco | 255 | 255 | 255 | `0xAAFFFFFF` |
| 7 | 🌸 Rosa | 255 | 105 | 180 | `0xAAFF69B4` |

La secuencia es circular: índice 7 → índice 0. El avance ocurre **únicamente** si el ACK del Esclavo es válido.

---

## Persistencia en EEPROM

### Esclavo — mapa EEPROM

| Dirección | Nombre | Valor | Descripción |
|---|---|---|---|
| `0x00` | `EEPROM_VALID` | `0xA5` (magic byte) | Indica que los datos en 0x01–0x03 son válidos. `0xFF` = EEPROM virgen |
| `0x01` | `EEPROM_R` | `uint8_t` 0–255 | Último canal Rojo confirmado |
| `0x02` | `EEPROM_G` | `uint8_t` 0–255 | Último canal Verde confirmado |
| `0x03` | `EEPROM_B` | `uint8_t` 0–255 | Último canal Azul confirmado |

### Maestro — mapa EEPROM

| Dirección | Nombre | Valor | Descripción |
|---|---|---|---|
| `0x00` | `EEPROM_VALID` | `0xA5` (magic byte) | Indica que el índice en 0x01 es válido |
| `0x01` | `EEPROM_INDEX` | `uint8_t` 0–7 | Índice del último color exitosamente confirmado |

**Estrategia del Maestro:** se persiste el índice (1 byte) en lugar de los tres valores RGB, porque el color completo se puede reconstruir desde la tabla `COLORES[]`. Esto reduce la escritura EEPROM por evento exitoso a 2 bytes, extendiendo la vida útil de las celdas (mínimo 100.000 ciclos garantizados por Atmel).

**Bandera mágica `0xA5`:** al encenderse, si el byte en la dirección de control no es exactamente `0xA5` (el valor por defecto de una EEPROM virgen es `0xFF`), los datos se consideran inválidos y se aplica el estado seguro por defecto: Esclavo → negro/apagado `(0,0,0)`; Maestro → índice 0 (Rojo).

---

## Escenarios de falla y resiliencia

| Escenario | Causa | Comportamiento del sistema |
|---|---|---|
| **Esclavo pierde energía** | Corte de alimentación | Al reconectarse: lee EEPROM → aplica último color. El Maestro mantiene su estado; al siguiente botón, envía el siguiente color normalmente. |
| **Maestro pierde energía** | Corte de alimentación | Al reconectarse: lee EEPROM → recupera índice; aplica color. El Esclavo mantiene su color. El sistema continúa desde donde quedó. |
| **Trama con cabecera inválida** | Interferencia IR ambiental, otro control remoto | El Esclavo verifica cabecera y descarta silenciosamente. No aplica color, no envía ACK. El Maestro detecta timeout y reintenta (ARQ). |
| **ACK perdido o corrupto** | Interferencia durante la respuesta del Esclavo | El Esclavo ya aplicó el color. El Maestro no recibe ACK → reintenta. Si el reintento llega al Esclavo, éste vuelve a aplicar el mismo color (idempotente) y responde. Si los 3 reintentos fallan, el Maestro mantiene su color anterior. |
| **Trama duplicada por ARQ** | El Maestro reenvía porque no recibió ACK | El Esclavo es **idempotente**: procesar la misma trama N veces produce siempre el mismo estado. No hay inconsistencias. |
| **EEPROM virgen o inválida** | Primer uso / escritura incompleta por corte de energía | Magic byte ausente → estado por defecto seguro: negro en Esclavo, índice 0 en Maestro. |

---

## Librerías utilizadas

### IRremote v4.x

- **Autor:** Armin Joachimsmeyer
- **Repositorio:** [Arduino-IRremote/Arduino-IRremote](https://github.com/Arduino-IRremote/Arduino-IRremote)
- **Instalación:** Arduino IDE → Library Manager → buscar "IRremote"

| Función | Descripción |
|---|---|
| `IrReceiver.begin(pin, feedback)` | Inicia el receptor en `pin`. `DISABLE_LED_FEEDBACK` evita parpadeo del pin 13. |
| `IrReceiver.decode()` | Retorna `true` si hay una trama completa en el buffer. |
| `IrReceiver.decodedIRData.decodedRawData` | Los 32 bits del payload NEC recibido (`uint32_t`). |
| `IrReceiver.decodedIRData.flags` | Flags de estado; `IRDATA_FLAGS_IS_REPEAT` indica repetición automática NEC. |
| `IrReceiver.resume()` | Libera el buffer para recibir la siguiente trama. **Debe llamarse después de leer el dato.** |
| `IrReceiver.stop()` | Pausa la interrupción del receptor; evita capturar el eco propio. |
| `IrReceiver.start()` | Reactiva el receptor tras una transmisión. |
| `IrSender.sendNECRaw(data, repeats)` | Transmite 32 bits como payload NEC íntegro. `repeats=0` = sin repeticiones. |

### EEPROM.h (built-in AVR Core)

| Función | Descripción |
|---|---|
| `EEPROM.read(address)` | Lee 1 byte de la dirección especificada. |
| `EEPROM.write(address, value)` | Escribe 1 byte. Consume 1 ciclo de escritura siempre. |

---

## Estructura del repositorio

```
Tarea 2/
│
├── Esclavo.ino                             — Firmware Dispositivo 1 (Esclavo / Receptor)
│
├── Maestro.ino                             — Firmware Dispositivo 2 (Maestro / Control Remoto)
│
├── Bevilacqua_Clement_CdD_T2_2026.pdf      — Informe técnico completo
│
└── README.md
```

---

## Instrucciones de uso

### Requisitos previos

- Arduino IDE 2.x
- Librería **IRremote ≥ 4.x** (Library Manager del IDE)
- 2× Arduino UNO con cable USB

### Carga del firmware

1. Abrir `Maestro/Maestro.ino` en Arduino IDE.
2. Seleccionar la placa **Arduino UNO** y el puerto COM correspondiente.
3. Compilar y cargar en el Arduino del Maestro.
4. Repetir con `Esclavo/Esclavo.ino` en el Arduino del Esclavo.

### Monitor serie

Ambos dispositivos envían logs a 9600 bps. Abrir el Monitor Serie del IDE para verificar el funcionamiento:

**Maestro:**
```
Maestro listo
Enviando -> R=255 G=0 B=0
Trama: 0xAA000000
Respuesta: 0xBB000000
ACK OK! Color actualizado.
```

**Esclavo:**
```
Esclavo listo
Trama: 0xAA000000 R=255 G=0 B=0
ACK enviado: 0xBB000000
```

### Operación

1. Encender ambos dispositivos (el orden no importa).
2. Cada uno muestra en su LED el último color guardado en EEPROM (o negro/apagado si es el primer uso).
3. Presionar el botón del Maestro para avanzar al siguiente color.
4. Si el LED de ambos dispositivos cambia simultáneamente → handshaking exitoso.
5. Si el LED del Maestro no cambia → no se recibió ACK; el sistema reintenta hasta 3 veces.

### Distancia y orientación

- Apuntar el KY-005 del Maestro hacia el KY-022 del Esclavo (y viceversa).
- Distancia operativa garantizada: **1–2 metros** con visión directa.
- Evitar fuentes de luz IR directa (luz solar intensa, lámparas halógenas).

---

## Autores

**Francisco Bevilacqua** · **Sebastián Clement**  
Ingeniería en Computación — Universidad Nacional de Rafaela  
Comunicación de Datos — 2026

---

> 📄 **Informe técnico completo:** [`Bevilacqua_Clement_CdD_T2_2026.pdf`](docs/Bevilacqua_Clement_CdD_T2_25.pdf)  
> 🎬 **Imagenes y videos de funcionamiento:** [ https://drive.google.com/drive/folders/1P5FxIJzY4hZtIR2Mls0UCtlhy-UBNy5z?usp=drive_link]
