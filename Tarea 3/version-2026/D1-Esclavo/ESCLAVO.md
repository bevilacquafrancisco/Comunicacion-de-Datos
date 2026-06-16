# Dispositivo 1 — Esclavo IR
## Documentación del Firmware

> **TP3 Integrador — Comunicación de Datos**  
> Ingeniería en Computación | Bevilacqua, Francisco — Clement, Sebastián | Junio 2026

---

## Índice

1. [Rol del Dispositivo 1 en el sistema](#1-rol-del-dispositivo-1-en-el-sistema)
2. [Hardware y conexionado](#2-hardware-y-conexionado)
3. [Estructura del firmware](#3-estructura-del-firmware)
4. [Configuración: `config.h`](#4-configuración-configh)
5. [Módulo LEDC — Control PWM del LED RGB](#5-módulo-ledc--control-pwm-del-led-rgb)
6. [Módulo IR — Recepción y transmisión infrarroja](#6-módulo-ir--recepción-y-transmisión-infrarroja)
7. [Protocolo de comunicación IR](#7-protocolo-de-comunicación-ir)
8. [Persistencia de estado: NVS / Preferences](#8-persistencia-de-estado-nvs--preferences)
9. [Secuencia de arranque: `setup()`](#9-secuencia-de-arranque-setup)
10. [Ciclo principal: `loop()`](#10-ciclo-principal-loop)
11. [Flujo de datos completo](#11-flujo-de-datos-completo)
12. [Dependencias](#12-dependencias)
13. [Configuración de entorno: `platformio.ini`](#13-configuración-de-entorno-platformioini)
14. [Logs de depuración](#14-logs-de-depuración)

---

## 1. Rol del Dispositivo 1 en el sistema

El Dispositivo 1 es el **nodo receptor y actuador** del sistema. Dentro de la arquitectura de tres capas del TP3, su posición y responsabilidades son:

```
[ D3: Navegador web ]
         |
    Wi-Fi / WebSocket
         |
[ D2: Maestro IR + Servidor ]
         |
   Infrarrojo (NEC Raw)        ← única interfaz de entrada de D1
         |
[ D1: Esclavo IR ]  →  LED RGB
```

D1 **no tiene conocimiento de la existencia de D3**, ni de la red Wi-Fi, ni de ninguna capa superior del sistema. Su única interfaz con el exterior es el enlace infrarrojo con D2. Este diseño cumple el principio de responsabilidad única: D1 hace exactamente una cosa — recibir un color, mostrarlo y confirmarlo — y lo hace de forma determinista y robusta.

### Responsabilidades

| Responsabilidad | Implementación |
|---|---|
| Recibir tramas de color desde D2 | Receptor KY-022 + IRremote |
| Validar integridad de la trama | Verificación de cabecera `0xAA` |
| Aplicar el color al LED RGB | Módulo LEDC (PWM hardware) |
| Persistir el estado ante cortes de energía | Biblioteca Preferences (NVS) |
| Confirmar la recepción al Maestro | Trama de ACK por IR con cabecera `0xBB` |
| Restaurar el último estado al encender | Lectura de NVS en `setup()` |

### Lo que D1 explícitamente NO hace

- No inicia comunicación por propia iniciativa; siempre responde a D2.
- No tiene lógica de secuencia de colores; eso pertenece al Maestro.
- No acepta comandos de ninguna fuente que no sea el canal IR.
- No genera ACK ante tramas con cabecera desconocida, evitando respuestas espurias.

---

## 2. Hardware y conexionado

### Componentes

| Componente | Función | Interfaz con ESP32 |
|---|---|---|
| ESP32 DevKit v1 | Microcontrolador principal | — |
| KY-022 | Receptor IR (TSOP1838 interno) | GPIO 22 → señal OUT |
| KY-005 | Emisor IR (LED infrarrojo 940 nm) | GPIO 23 → señal S |
| LED RGB cátodo común | Actuador visual | GPIO 25/26/27 |
| Resistencias 220 Ω (×3) | Limitación de corriente del LED | En serie con cada canal RGB |

### Diagrama de conexionado

```
ESP32 DevKit v1
┌─────────────────────────────────────────────────────┐
│                                                     │
│  GPIO 22 ──────────────────────── KY-022  OUT       │
│  3.3V ─────────────────────────── KY-022  VCC       │
│  GND ──────────────────────────── KY-022  GND       │
│                                                     │
│  GPIO 23 ──────────────────────── KY-005  S         │
│  3.3V ─────────────────────────── KY-005  VCC       │
│  GND ──────────────────────────── KY-005  GND       │
│                                                     │
│  GPIO 25 ──── [220 Ω] ─────────── LED RGB  R (Anodo)│
│  GPIO 26 ──── [220 Ω] ─────────── LED RGB  G (Anodo)│
│  GPIO 27 ──── [220 Ω] ─────────── LED RGB  B (Anodo)│
│  GND ──────────────────────────── LED RGB  GND (K)  │
│                                                     │
│  USB ──────────────────────────── Alimentación / Prog│
└─────────────────────────────────────────────────────┘
```

### Criterios de selección de pines

El ESP32 tiene restricciones de uso en ciertos GPIO que deben respetarse para evitar comportamientos impredecibles en el arranque o durante la operación:

| GPIO a evitar | Motivo |
|---|---|
| 0 | Boot mode selector; debe estar en HIGH para boot normal |
| 2 | Debe estar en LOW durante la programación por UART |
| 6–11 | Conectados a la flash SPI interna; uso exclusivo del sistema |
| 12 | MTDI: configura el voltaje de flash en boot; evitar OUTPUT |
| 15 | MTDO: silencia el log de boot si está en LOW |
| 34–39 | Input-only; no tienen driver de salida ni resistencia pull interna |

Los pines elegidos (22, 23, 25, 26, 27) son todos GPIO de propósito general, libres de funciones especiales en boot, con soporte de interrupción, PWM por LEDC y compatibles con niveles de 3.3 V.

### Cálculo de resistencias para el LED RGB

Con Vcc = 3.3 V, Vf ≈ 2.0 V (LED rojo/verde) y corriente objetivo de 10 mA:

```
R = (Vcc - Vf) / I = (3.3 - 2.0) / 0.010 = 130 Ω
```

Se usa 220 Ω como valor estándar disponible, resultando en ~6 mA por canal. Suficiente para indicación visual clara, con margen de seguridad para el GPIO del ESP32 (máximo 12 mA por pin).

---

## 3. Estructura del firmware

El firmware del Esclavo se organiza en tres archivos:

```
D1_Esclavo/
├── src/
│   └── main.cpp        ← lógica completa del firmware
├── include/
│   └── config.h        ← constantes de hardware, protocolo y persistencia
└── platformio.ini      ← entorno de compilación y dependencias
```

`config.h` centraliza todas las constantes modificables. Cambiar un pin, un parámetro de protocolo o una clave NVS requiere editar únicamente ese archivo, sin tocar la lógica de `main.cpp`. Esta separación respeta el principio de configuración aislada de la lógica, facilita el mantenimiento y hace al firmware adaptable a distintos pinouts o variantes de hardware sin riesgo de introducir errores en la lógica.

`main.cpp` implementa cinco funciones auxiliares más `setup()` y `loop()`. No hay clases ni archivos adicionales: el firmware es deliberadamente simple y lineal, lo que maximiza su determinismo y facilita la verificación formal de su comportamiento.

---

## 4. Configuración: `config.h`

```cpp
#pragma once
#include <Arduino.h>

// --- Pines de hardware ---
#define IR_SEND_PIN   23    // KY-005: emisor IR, GPIO 23 (RMT-capable)
#define IR_RECV_PIN   22    // KY-022: receptor IR, GPIO 22 (interrupt-capable)

#define PIN_R         25    // LED RGB canal rojo
#define PIN_G         26    // LED RGB canal verde
#define PIN_B         27    // LED RGB canal azul

// --- Configuración LEDC (PWM) ---
#define LEDC_FREQ_HZ    5000  // Frecuencia PWM: 5 kHz
#define LEDC_RESOLUTION    8  // Resolución: 8 bits (0–255)
#define LEDC_CH_R          3  // Canal LEDC para rojo   ← Canal 3, no 0
#define LEDC_CH_G          1  // Canal LEDC para verde
#define LEDC_CH_B          2  // Canal LEDC para azul

// --- Protocolo IR ---
#define HDR_CMD   0xAA        // Cabecera de comando   (D2 → D1)
#define HDR_ACK   0xBB        // Cabecera de ACK       (D1 → D2)
#define DELAY_ANTES_ACK_MS  120

// --- Persistencia NVS ---
#define NVS_NAMESPACE   "d1_state"
#define NVS_KEY_MAGIC   "magic"
#define NVS_KEY_R       "r"
#define NVS_KEY_G       "g"
#define NVS_KEY_B       "b"
#define NVS_MAGIC_VAL   0xA5

// --- Depuración ---
#define SERIAL_BAUD   115200
```

### `#include <Arduino.h>` en el header

La directiva `#include <Arduino.h>` en `config.h` es necesaria porque el archivo define la estructura `ColorRGB` usando `uint8_t`, un tipo del estándar C99 (`<stdint.h>`) que en el entorno Arduino/ESP32 se resuelve a través del framework. Sin este include, el compilador no puede resolver `uint8_t` durante el preprocesamiento del header en unidades de compilación que no incluyan `Arduino.h` explícitamente.

### `LEDC_CH_R = 3`: corrección de colisión de hardware

La asignación del canal rojo al canal LEDC 3 (en lugar del 0 natural) es una corrección crítica descubierta durante las pruebas físicas. La librería IRremote usa por defecto el **canal LEDC 0** del ESP32 para generar la portadora de 38 kHz de la señal infrarroja. Si el canal rojo del LED también usa el canal 0, se produce una **colisión de recursos hardware**: al transmitir el ACK, IRremote reconfigura el canal 0, apagando el LED rojo y deformando los pulsos de la trama IR. El resultado observable era que los colores con componente roja se perdían al enviar el ACK, y los colores complejos (como LILA) generaban tramas de respuesta corruptas que el Maestro interpretaba como timeout. Mover el canal rojo a LEDC 3 aisla completamente el PWM del LED de la modulación IR.

---

## 5. Módulo LEDC — Control PWM del LED RGB

### ¿Qué es LEDC?

LEDC (LED Control) es el periférico hardware del ESP32 diseñado para generar señales PWM. Dispone de **16 canales independientes** (numerados 0–15), cada uno con frecuencia y resolución configurables. A diferencia del `analogWrite()` de AVR (que usa timers compartidos y tiene solo 6 pines disponibles), LEDC puede asignarse a **cualquier GPIO de salida** del ESP32 con plena independencia entre canales.

### Configuración e inicialización

```cpp
void iniciarLedc() {
    // Configurar cada canal: frecuencia 5 kHz, resolución 8 bits
    ledcSetup(LEDC_CH_R, LEDC_FREQ_HZ, LEDC_RESOLUTION);  // canal 3
    ledcSetup(LEDC_CH_G, LEDC_FREQ_HZ, LEDC_RESOLUTION);  // canal 1
    ledcSetup(LEDC_CH_B, LEDC_FREQ_HZ, LEDC_RESOLUTION);  // canal 2

    // Vincular cada canal a su GPIO
    ledcAttachPin(PIN_R, LEDC_CH_R);  // GPIO 25 → canal 3
    ledcAttachPin(PIN_G, LEDC_CH_G);  // GPIO 26 → canal 1
    ledcAttachPin(PIN_B, LEDC_CH_B);  // GPIO 27 → canal 2
}
```

### Aplicación de color

```cpp
void setColor(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LEDC_CH_R, r);  // ciclo de trabajo 0–255 en canal rojo
    ledcWrite(LEDC_CH_G, g);
    ledcWrite(LEDC_CH_B, b);
}
```

`ledcWrite(canal, valor)` establece el ciclo de trabajo del PWM. Con resolución de 8 bits, el rango es 0 (apagado) a 255 (máxima intensidad). Los valores RGB del protocolo (0–255) mapean directamente a este rango, sin necesidad de conversión.

### Elección de frecuencia

5 kHz es significativamente mayor que la frecuencia de fusión del parpadeo del ojo humano (~60–80 Hz), por lo que el LED es percibido como luz continua en cualquier nivel de brillo. Frecuencias más bajas (como los 490 Hz o 980 Hz del `analogWrite()` de Arduino UNO) pueden producir un efecto de parpadeo visible, especialmente en periférico de visión o al mover el ojo.

---

## 6. Módulo IR — Recepción y transmisión infrarroja

### Hardware: KY-022 y KY-005

**KY-022** contiene un receptor IR integrado (típicamente TSOP1838 o equivalente). Internamente este integrado amplifica, filtra y demodula la portadora de 38 kHz, entregando al microcontrolador la señal de datos digital limpia en el pin OUT. El pin OUT es **activo en LOW**: el nivel reposa en HIGH y baja a LOW durante los pulsos de luz IR recibidos.

**KY-005** es un módulo con un LED infrarrojo de 940 nm y una resistencia limitadora. La modulación (encendido/apagado a 38 kHz) es responsabilidad del microcontrolador, en este caso del periférico RMT del ESP32 a través de IRremote.

### IRremote v4 en ESP32

La librería **IRremote** usa en ESP32 el periférico **RMT (Remote Control Transceiver)**, un módulo hardware dedicado a la generación y recepción de señales con modulación precisa. RMT opera de forma completamente autónoma respecto a la CPU: la portadora de 38 kHz y los tiempos de los pulsos son generados en hardware sin intervención del procesador, garantizando precisión de microsegundos independientemente de la carga del sistema.

Para usar IRremote correctamente en ESP32, `IR_SEND_PIN` **debe estar definido como macro de preprocesador antes de incluir `IRremote.hpp`**. Esta es la razón por la que `config.h` (que contiene el `#define IR_SEND_PIN`) se incluye como primer header en `main.cpp`, antes que `<Arduino.h>` e `<IRremote.hpp>`.

### Inicialización del receptor

```cpp
IrReceiver.begin(IR_RECV_PIN, DISABLE_LED_FEEDBACK);
```

`DISABLE_LED_FEEDBACK` evita que IRremote use el LED interno del pin 13 (LED_BUILTIN del módulo) como indicador de actividad de recepción. Esto es importante porque el GPIO 13 podría estar en uso o no disponible en el módulo específico, y el parpadeo del LED de feedback interfiere con la medición de señal en osciloscopia.

---

## 7. Protocolo de comunicación IR

### Elección: NEC Raw

Se utiliza la variante **NEC Raw** del protocolo NEC, invocada mediante `sendNECRaw()` y leída por `decodedRawData`. La diferencia con el NEC estándar es crucial: el NEC estándar aplica una inversión lógica a los bytes de comando y dirección como mecanismo de verificación de integridad. Esta inversión hace imposible transportar datos arbitrarios de 32 bits sin que el protocolo los modifique.

NEC Raw envía los 32 bits exactamente como están construidos en la aplicación, usando la misma modulación de portadora (38 kHz) y los mismos tiempos de bit, pero sin las transformaciones de datos. Esto permite al firmware definir libremente la estructura interna de la trama.

### Estructura de la trama de 32 bits

```
 Bit 31        Bit 24  Bit 23        Bit 16  Bit 15         Bit 8  Bit 7          Bit 0
┌──────────────────────┬──────────────────────┬──────────────────────┬──────────────────────┐
│      CABECERA        │    COMPONENTE  R     │    COMPONENTE  G     │    COMPONENTE  B     │
│       1 byte         │       1 byte         │       1 byte         │       1 byte         │
│  0xAA = CMD          │      0–255           │      0–255           │      0–255           │
│  0xBB = ACK          │                      │                      │                      │
└──────────────────────┴──────────────────────┴──────────────────────┴──────────────────────┘
```

### Construcción de la trama en código

```cpp
// Trama de ACK (D1 → D2):
uint32_t trama_ack = ((uint32_t)HDR_ACK << 24) |  // 0xBB en bits 31-24
                     ((uint32_t)r        << 16) |  // R en bits 23-16
                     ((uint32_t)g        <<  8) |  // G en bits 15-8
                      (uint32_t)b;                 // B en bits 7-0
```

Los casts explícitos a `uint32_t` antes de los desplazamientos son necesarios para evitar comportamiento indefinido: sin el cast, el desplazamiento se haría sobre `uint8_t` (8 bits), y desplazar más de 7 posiciones produciría UB en C++. El cast promueve el operando a 32 bits antes del shift.

### Protocolo de handshaking

El mecanismo de confirmación garantiza la coherencia de estado entre D1 y D2:

```
D2                                    D1
 │                                     │
 │── CMD: [0xAA][R][G][B] ──────────── │
 │   (IrReceiver.stop en D2)           │
 │                                     │── setColor(R,G,B)
 │                                     │── guardarNVS(R,G,B)
 │                                     │── delay(120 ms)   ← D2 reactiva receptor
 │                                     │── IrReceiver.stop()
 │◄── ACK: [0xBB][R][G][B] ───────────  │── sendNECRaw(trama_ack)
 │                                     │── IrReceiver.start()
 │   (valida cab==0xBB && R,G,B == enviados)
 │   ¿ACK válido? → actualizar estado
```

El delay de 120 ms (`DELAY_ANTES_ACK_MS`) en D1 es el margen de tiempo que tiene D2 para completar su propia secuencia de `IrReceiver.stop()` → transmisión → `IrReceiver.start()` antes de que llegue el ACK. Si el ACK llegara antes de que D2 reactive su receptor, se perdería. Este valor fue determinado experimentalmente y contempla el tiempo de transmisión NEC (~67 ms para una trama de 32 bits a 38 kHz) más margen de procesamiento.

### Gestión half-duplex del canal IR compartido

Cada nodo tiene tanto un emisor (KY-005) como un receptor (KY-022) apuntando en la misma dirección. Sin gestión explícita, el receptor de D1 capturaría el eco de su propio emisor al transmitir el ACK, generando una trama espuria que bloquearía el receptor. La solución es:

```cpp
void enviarACK(uint8_t r, uint8_t g, uint8_t b) {
    IrReceiver.stop();            // 1. Deshabilitar receptor ANTES de emitir
    delay(DELAY_ANTES_ACK_MS);   // 2. Esperar que D2 reactive su receptor
    IrSender.sendNECRaw(trama_ack, 0);  // 3. Transmitir (0 = sin repeticiones)
    IrReceiver.start();           // 4. Reactivar receptor para próximo comando
}
```

El argumento `0` en `sendNECRaw()` indica que no se envían repeticiones automáticas NEC. Las repeticiones NEC son un mecanismo del protocolo estándar para indicar que una tecla se mantiene presionada; en este sistema no tienen sentido y su envío generaría tramas adicionales que el Maestro interpretaría incorrectamente.

### Filtrado de repeticiones en recepción

Aunque D2 envía con 0 repeticiones, el loop de D1 incluye un filtro defensivo:

```cpp
if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
    IrReceiver.resume();
    return;
}
```

El flag `IRDATA_FLAGS_IS_REPEAT` es activado por IRremote cuando detecta una trama de repetición NEC (un pulso especial más corto que señaliza mantenimiento de tecla). Cualquier interferencia ambiental que produzca este patrón es descartada sin procesamiento.

### Orden crítico: leer antes de resume()

```cpp
uint32_t trama = IrReceiver.decodedIRData.decodedRawData;  // 1. Leer
IrReceiver.resume();                                         // 2. Liberar buffer
```

`resume()` limpia el buffer interno del receptor y lo habilita para recibir la siguiente trama. Si se invocara antes de leer `decodedRawData`, el dato sería sobreescrito o borrado. El orden es por lo tanto inviolable, y el `resume()` debe ser llamado lo antes posible tras la lectura para no perder la siguiente trama.

---

## 8. Persistencia de estado: NVS / Preferences

### ¿Por qué persistir el estado?

La consigna requiere que al encender el sistema, cada dispositivo recupere el último color establecido y lo muestre sin necesidad de recibir un nuevo comando. En el TP2 esto se implementaba con la EEPROM del ATmega328P (1 KB, ~100.000 ciclos de escritura por celda). En el ESP32 la solución equivalente es la **Non-Volatile Storage (NVS)**, accesible a través de la librería `Preferences`.

### NVS vs. EEPROM de AVR

| Característica | EEPROM ATmega328P | NVS ESP32 (Preferences) |
|---|---|---|
| Capacidad | 1 KB | ~24 KB por defecto (partición nvs) |
| Ciclos de escritura por celda | ~100.000 | Sin límite práctico (wear leveling) |
| Granularidad | 1 byte | Par clave-valor tipado |
| Integridad | Sin verificación | CRC automático por bloque |
| API | `EEPROM.read/write(addr)` | `prefs.getUChar/putUChar(key)` |

El **wear leveling** de NVS distribuye automáticamente las escrituras entre las páginas físicas de flash, evitando el desgaste concentrado que produciría la EEPROM si se escribiera repetidamente en las mismas celdas.

### Estructura de datos en NVS

D1 guarda tres claves bajo el namespace `"d1_state"`:

| Clave | Tipo | Valor | Descripción |
|---|---|---|---|
| `"magic"` | `uint8_t` | `0xA5` | Centinela de validez de datos |
| `"r"` | `uint8_t` | 0–255 | Componente rojo del último color |
| `"g"` | `uint8_t` | 0–255 | Componente verde del último color |
| `"b"` | `uint8_t` | 0–255 | Componente azul del último color |

### El valor centinela (magic byte)

```cpp
#define NVS_MAGIC_VAL   0xA5
```

La primera vez que D1 arranca (NVS vacía), todas las claves retornan sus valores por defecto (0 en este caso). Sin el centinela, el firmware no podría distinguir entre "el último color guardado era negro (0,0,0)" y "nunca se guardó nada". El centinela `0xA5` (`10100101` en binario — patrón con bits alternados elegido por su baja probabilidad de aparecer por corrupción) resuelve esto: si no está presente con el valor exacto, los datos se consideran inválidos.

### Implementación de escritura

```cpp
void guardarNVS(uint8_t r, uint8_t g, uint8_t b) {
    prefs.begin(NVS_NAMESPACE, false);   // false = modo lectura/escritura
    prefs.putUChar(NVS_KEY_MAGIC, NVS_MAGIC_VAL);
    prefs.putUChar(NVS_KEY_R, r);
    prefs.putUChar(NVS_KEY_G, g);
    prefs.putUChar(NVS_KEY_B, b);
    prefs.end();                         // commit y liberación del namespace
}
```

`prefs.end()` cierra el namespace y garantiza que las escrituras quedan confirmadas en flash antes de continuar. Sin esta llamada, las escrituras podrían quedar en el buffer interno de NVS y perderse en un reset abrupto.

### Implementación de lectura con validación

```cpp
bool cargarNVS(uint8_t &r, uint8_t &g, uint8_t &b) {
    prefs.begin(NVS_NAMESPACE, true);   // true = solo lectura
    uint8_t magic = prefs.getUChar(NVS_KEY_MAGIC, 0x00);
    bool valido = (magic == NVS_MAGIC_VAL);
    if (valido) {
        r = prefs.getUChar(NVS_KEY_R, 0);
        g = prefs.getUChar(NVS_KEY_G, 0);
        b = prefs.getUChar(NVS_KEY_B, 0);
    }
    prefs.end();
    return valido;
}
```

Los parámetros `r`, `g`, `b` son pasados por referencia. Si los datos son inválidos (`magic != 0xA5`), la función retorna `false` sin modificar las variables del llamador, que habrán sido inicializadas en 0 en `setup()`. La función de apertura en modo read-only (`true`) es una protección adicional contra escrituras accidentales durante la lectura.

---

## 9. Secuencia de arranque: `setup()`

```
SETUP
  │
  ├─ Serial.begin(115200)          Iniciar puerto serie para logs
  │   delay(500)                   Dar tiempo al monitor para conectarse
  │
  ├─ iniciarLedc()                 Configurar canales LEDC 3, 1, 2
  │   ledcAttachPin(25/26/27, …)   Vincular GPIOs a canales PWM
  │
  ├─ IrReceiver.begin(22, …)       Activar receptor IR en GPIO 22
  │
  ├─ cargarNVS(r, g, b)
  │   ├─ NVS válida (magic=0xA5)
  │   │   └─ setColor(r, g, b)     Restaurar último color en el LED
  │   └─ NVS inválida / vacía
  │       └─ setColor(0, 0, 0)     LED apagado (estado seguro por defecto)
  │
  └─ [Fin setup] → loop()
```

La decisión de usar `LED apagado` como estado por defecto ante NVS inválida es deliberada: en un sistema de iluminación, mostrar un color arbitrario podría ser más confuso o problemático que no mostrar nada. El operador puede saber que debe enviar un comando de color desde D2.

---

## 10. Ciclo principal: `loop()`

El loop de D1 es minimalista y completamente reactivo. No hay timers, no hay lógica de estado propio, no hay polling de ninguna otra fuente:

```
LOOP (ejecutado continuamente)
  │
  ├─ IrReceiver.decode()
  │   └─ false → return           Sin datos, ceder CPU inmediatamente
  │
  ├─ ¿Flag IRDATA_FLAGS_IS_REPEAT?
  │   └─ true → resume() → return  Descartar repetición NEC
  │
  ├─ trama = decodedIRData.decodedRawData   ← LEER PRIMERO
  ├─ IrReceiver.resume()                    ← LIBERAR BUFFER INMEDIATAMENTE
  │
  ├─ Extraer: cabecera, R, G, B de la trama de 32 bits
  │
  ├─ ¿cabecera == 0xAA?
  │   └─ No → log + return         Descartar silenciosamente
  │
  ├─ setColor(R, G, B)             Aplicar color al LED
  ├─ guardarNVS(R, G, B)           Persistir en flash
  └─ enviarACK(R, G, B)            Confirmar al Maestro por IR
       │
       ├─ IrReceiver.stop()
       ├─ delay(120 ms)
       ├─ IrSender.sendNECRaw(trama_ack, 0)
       └─ IrReceiver.start()
```

### Por qué `return` inmediato cuando no hay datos

```cpp
if (!IrReceiver.decode()) {
    return;
}
```

`IrReceiver.decode()` es no bloqueante: retorna inmediatamente `false` si no hay trama disponible. El `return` devuelve el control al scheduler de FreeRTOS, que puede ejecutar otras tareas del sistema (watchdog, logs internos, etc.). Un bucle vacío (`while(!decode()){}`) bloquearía el core y podría disparar el Watchdog Timer, resultando en un reset del sistema.

### Por qué descartar tramas con cabecera desconocida sin enviar ACK

Si D1 recibiera su propio ACK recién transmitido (por reflexión en superficies cercanas), e intentara procesar ese ACK como un comando, el bucle resultaría en un ACK de ACK infinito. La validación de cabecera `0xAA` es la barrera que previene este escenario. Cualquier trama con cabecera distinta —incluyendo `0xBB` (ACK emitido por D1 mismo)— es descartada sin respuesta.

---

## 11. Flujo de datos completo

Secuencia completa de una operación exitosa, desde que D3 presiona un botón de color hasta que el LED de D1 cambia:

```
D3 (Navegador)    D2 (Maestro)         D1 (Esclavo)
      │                │                     │
      │─ CMD_COLOR ────►│                     │
      │  {"tipo":       │                     │
      │  "CMD_COLOR",   │                     │
      │  "color":"AZUL"}│                     │
      │                 │                     │
      │          irPendiente=true             │
      │          [loop detecta flag]          │
      │                 │                     │
      │                 │─ IR CMD ────────────►│
      │                 │  [0xAA][0][0][255]  │
      │                 │  IrReceiver.stop()  │  loop() detecta trama
      │                 │                     │  IrReceiver.decode() = true
      │                 │                     │  cabecera = 0xAA ✓
      │                 │                     │  setColor(0, 0, 255)
      │                 │                     │  guardarNVS(0, 0, 255)
      │                 │                     │  enviarACK():
      │                 │                     │    IrReceiver.stop()
      │                 │                     │    delay(120ms)
      │                 │◄─ IR ACK ───────────│    sendNECRaw(0xBB0000FF)
      │                 │  [0xBB][0][0][255]  │    IrReceiver.start()
      │                 │  IrReceiver.start() │
      │                 │                     │
      │          valida cab=0xBB && R,G,B ✓   │
      │          colorActual = idx AZUL        │
      │          setColor(0,0,255) en D2       │
      │          guardarNVS D2                 │
      │◄─ RESULTADO_CMD ─│                     │
      │  {exito:true,    │                     │
      │   color:"AZUL"}  │                     │
      │                  │                     │
   LED D3          LED D2               LED D1
   muestra AZUL   muestra AZUL         muestra AZUL
```

---

## 12. Dependencias

El firmware del Esclavo requiere una única dependencia externa:

### `z3t0/IRremote @ ^4.7.1`

**IRremote** es la librería estándar de facto para comunicación infrarroja en el ecosistema Arduino. La versión 4.x introduce soporte nativo para ESP32 mediante el periférico RMT, eliminando la implementación basada en interrupciones de software de versiones anteriores.

El identificador correcto en el registro de PlatformIO es `z3t0/IRremote` (alias histórico del autor original). El nombre alternativo `IRremote/IRremote` puede no resolverse correctamente dependiendo de la versión del Dependency Finder de PlatformIO.

**Funciones utilizadas en D1:**

| Función / Objeto | Descripción |
|---|---|
| `IrReceiver.begin(pin, feedback)` | Inicializa el receptor IR en el GPIO indicado |
| `IrReceiver.decode()` | Verifica si hay una trama completa disponible; no bloqueante |
| `IrReceiver.decodedIRData.decodedRawData` | Trama de 32 bits decodificada |
| `IrReceiver.decodedIRData.flags` | Flags de la trama (repetición, errores, etc.) |
| `IrReceiver.resume()` | Libera el buffer y habilita la recepción de la siguiente trama |
| `IrReceiver.stop()` | Deshabilita el receptor (gestión half-duplex) |
| `IrReceiver.start()` | Reactiva el receptor tras una transmisión |
| `IrSender.sendNECRaw(data, reps)` | Transmite 32 bits en formato NEC Raw con N repeticiones |
| `IRDATA_FLAGS_IS_REPEAT` | Flag que indica trama de repetición NEC |
| `DISABLE_LED_FEEDBACK` | Constante para deshabilitar el LED de actividad de IRremote |

### `Preferences` (incluida en ESP32 Arduino Core)

No requiere declaración en `lib_deps`. Es parte del ESP32 Arduino Core que PlatformIO descarga al configurar `platform = espressif32`.

---

## 13. Configuración de entorno: `platformio.ini`

```ini
[env:esp32dev]
platform         = espressif32     ; Toolchain Xtensa LX6 + esptool + ESP-IDF headers
board            = esp32dev        ; Perfil ESP32 DevKit v1: 4MB flash, 520KB RAM, 240MHz
framework        = arduino         ; API Arduino sobre ESP-IDF

monitor_speed    = 115200          ; Debe coincidir con Serial.begin() en el firmware
upload_speed     = 921600          ; Velocidad de flasheo (6x más rápido que 115200)

; Descomentar y ajustar si PlatformIO no detecta el puerto automáticamente:
; upload_port    = COM3            ; Windows
; upload_port    = /dev/ttyUSB0   ; Linux
; upload_port    = /dev/cu.usbserial-0001  ; macOS

lib_deps =
    z3t0/IRremote @ ^4.7.1         ; Protocolo IR + modulación RMT hardware

build_flags =
    -DCORE_DEBUG_LEVEL=0           ; Sin logs internos del core ESP32
                                   ; Cambiar a 3 para depuración verbose

monitor_filters = esp32_exception_decoder
; Traduce backtraces de Guru Meditation Error a función + número de línea
```

### Sobre `upload_speed = 921600`

La velocidad de programación del ESP32 puede aumentarse hasta 921600 bps (vs. el default de 115200 bps) gracias al chip USB-Serial CP2102/CH340 que soporta estas velocidades. A 921600 bps, un firmware de ~470 KB tarda aproximadamente 5 segundos en lugar de los ~35 segundos que tomaría a 115200 bps. No todos los cables USB o adaptadores toleran esta velocidad; ante errores de flasheo, reducir a 460800 bps.

---

## 14. Logs de depuración

Con el Monitor Serial abierto (`Ctrl+Alt+S` en VS Code), el firmware imprime el estado de cada operación:

### Arranque con estado previo en NVS

```
[D1] === Esclavo IR — Iniciando ===
[D1] LEDC configurado
[D1] Receptor IR activo en GPIO 22
[D1] Estado recuperado de NVS: R=0 G=0 B=255
[D1] Listo, esperando comandos IR...
```

### Arranque sin datos previos (primera vez)

```
[D1] === Esclavo IR — Iniciando ===
[D1] LEDC configurado
[D1] Receptor IR activo en GPIO 22
[D1] NVS vacía — LED apagado
[D1] Listo, esperando comandos IR...
```

### Recepción de comando y envío de ACK

```
[IR-RX] Trama: 0xAA00FF00 | cab=0xAA R=0 G=255 B=0
[D1] Aplicando color R=0 G=255 B=0
[NVS] Guardado R=0 G=255 B=0
[IR-TX] ACK enviado: 0xBB00FF00
```

### Trama con cabecera inválida (eco u otra fuente)

```
[IR-RX] Trama: 0xBB00FF00 | cab=0xBB R=0 G=255 B=0
[IR-RX] Cabecera inválida (0xBB) — descartada
```

---

*Documentación elaborada en el marco del TP3 Integrador — Comunicación de Datos, Ingeniería en Computación.*  
*Bevilacqua, Francisco — Clement, Sebastián — Junio 2026.*