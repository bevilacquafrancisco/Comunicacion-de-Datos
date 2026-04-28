# Tarea N°1 — Ejercicio 2: Comunicación Bidireccional de Tipo Serie

**Asignatura:** Comunicación de Datos — 4° año Ingeniería en Computación  
**Autores:** Francisco Bevilacqua, Sebastian Clement  
**Fecha:** 2026-04-22  
**Versión del documento:** 1.0.0

---
# Ejercicio 2: Comunicación Serie Bidireccional

Este directorio contiene el código fuente y la documentación correspondientes al **Ejercicio 2** de la Tarea N°1 de la materia Comunicación de Datos. El objetivo de esta práctica es evolucionar hacia una arquitectura de comunicación bidireccional asíncrona implementando un protocolo serial con mecanismo de handshake y detección de errores.

##  Consigna del Ejercicio

Mediante la utilización de dos dispositivos programables, establecer una comunicación bidireccional de tipo serie entre ellos cumpliendo con los siguientes requisitos:

* **Dispositivo 1 :**  Dispondrá de dos pulsadores: uno para *encender/apagar* y otro para *cambiar*.
    * Poseerá dos LEDs de control: El **LED1** indicará el acuse de recibo (ACK) por parte del dispositivo 2. El **LED2** indicará que el dispositivo 2 se encuentra conectado de forma exitosa.
* **Dispositivo 2 :**  En sus salidas PWM se conectará un LED RGB.
    * Dispondrá de un botón físico de *conexión*.
* **Proceso de Handshake (Conexión):** Al presionar el botón de conexión en el dispositivo 2, éste deberá solicitar conexión al dispositivo 1 repetidamente hasta que el dispositivo 1 le habilite la conexión. Una vez establecida, el dispositivo 1 deberá encender su LED2 indicando el éxito del enlace.
* **Acción de Encendido/Apagado:** Al presionar el botón *encender/apagar*, el dispositivo 1 le indicará al dispositivo 2 que debe prender o apagar el LED RGB.
* **Acción de Cambio de Color:** Al presionar el botón *cambiar*, el dispositivo 1 enviará al dispositivo 2 el color al cual debe cambiar. El color se determinará de forma aleatoria internamente en el dispositivo 1.
* **Confirmación de Recepción (Acuse de Recibo):** En ambos envíos de datos (on/off o color), el dispositivo 2 debe avisar al dispositivo 1 que recibió los datos correctamente. Al recibir este aviso, el dispositivo 1 deberá encender el LED1 por un tiempo de 2 segundos.
* **Control de Errores e Integridad:** Para garantizar la integridad de la información, se debe implementar por software un mecanismo de detección de errores que se adjuntará al final de cada trama enviada. El receptor deberá recalcular este valor y compararlo para validar el mensaje.

---
## Índice

1. [Descripción general del sistema](#1-descripción-general-del-sistema)  
2. [Esquema en bloques de la comunicación](#2-esquema-en-bloques-de-la-comunicación)  
3. [Protocolo de comunicación — UART + protocolo de capa de aplicación](#3-protocolo-de-comunicación)  
4. [Estructura de tramas](#4-estructura-de-tramas)  
5. [Algoritmo de detección de errores — Checksum suma módulo 256](#5-algoritmo-de-detección-de-errores)  
6. [Reacción ante trama corrupta](#6-reacción-ante-trama-corrupta)  
7. [Periférico UART — Funcionamiento y configuración](#7-periférico-uart)  
8. [Máquinas de estado](#8-máquinas-de-estado)  
9. [Sistema de logging para debug](#9-sistema-de-logging)  
10. [Conexión física — Pinout y diagrama eléctrico](#10-conexión-física)  
11. [Diagrama de flujo del sistema](#11-diagrama-de-flujo)  
12. [Inventario de componentes](#12-inventario-de-componentes)  
13. [Decisiones de diseño — justificación técnica](#13-decisiones-de-diseño)  
14. [Comparación con protocolos alternativos](#14-comparación-con-protocolos-alternativos)  
15. [Comparación con el Ejercicio 1 (comunicación paralela)](#15-comparación-con-el-ejercicio-1)

---

## 1. Descripción general del sistema

El sistema establece una **comunicación bidireccional de tipo serie** entre dos Arduino UNO. A diferencia del Ejercicio 1 (paralela unidireccional, 10 cables), este ejercicio requiere comunicación en ambas direcciones con solo 3 conductores (TX, RX, GND).

### Roles de cada dispositivo

| Dispositivo | Rol | Responsabilidades |
|---|---|---|
| D1 (Maestro) | Iniciador de comandos | Aceptar conexión, enviar CMD_ONOFF / CMD_COLOR, recibir ACK/NACK |
| D2 (Receptor) | Controlador del LED RGB | Solicitar conexión, recibir y validar comandos, ejecutar acciones, enviar ACK/NACK |

### Flujo general del sistema

```
D2: [BTN_CONEXION] → CONN_REQ ──────────────────→ D1: verifica CHK → CONN_ACK
D1: LED2 enciende ←────────── CONN_ACK ──────────

D1: [BTN_ONOFF] → CMD_ONOFF ─────────────────── → D2: valida CHK → ejecuta LED
D1: LED1 enciende 2s ←──── ACK ←─────────────── D2: envía ACK

D1: [BTN_CAMBIAR] → CMD_COLOR ───────────────── → D2: valida CHK → aplica color
D1: LED1 enciende 2s ←──── ACK ←─────────────── D2: envía ACK
```

---

## 2. Esquema en bloques de la comunicación

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        SISTEMA DE COMUNICACIÓN SERIE                        │
└─────────────────────────────────────────────────────────────────────────────┘

  DISPOSITIVO 1 (Maestro)                         DISPOSITIVO 2 (Receptor)
  ──────────────────────                          ──────────────────────────

 ┌──────────────┐                                              ┌────────────┐
 │    FUENTE    │                                              │   DESTINO  │
 │  BTN_ONOFF   │                                              │  LED RGB   │
 │  BTN_CAMBIAR │                                              │  (PWM R/G/B│
 └──────┬───────┘                                              └────────────┘
        │                                                            ▲
        ▼                                                            │
 ┌──────────────┐    ┌───────────────────────────┐   ┌──────────────┴──────┐
 │ CODIFICADOR  │    │         CANAL             │   │    DECODIFICADOR    │
 │ SoftwareSerial    │  TX ──────────────→ RX    │   │  SoftwareSerial     │
 │ 9600 bps     ├───►│  RX ←────────────── TX   │◄──┤  9600 bps           │
 │ Protocolo:   │    │  GND ─────────────── GND  │   │  Protocolo:         │
 │ CONN_ACK     │    │  (3 cables, 0.5–2m)       │   │  CONN_REQ           │
 │ CMD_ONOFF    │    │  Nivel lógico: 0–5V TTL    │   │  ACK / NACK         │
 │ CMD_COLOR    │    └───────────────────────────┘   │  CHK sum mod 256    │
 │ CHK sum256   │                                    └─────────────────────┘
 └──────────────┘
        │                                                            
        ▼
 ┌──────┴───────┐
 │   DESTINO    │
 │  LED1 (ACK)  │
 │  LED2 (CONN) │
 └──────────────┘
```

### Descripción de cada bloque

**Fuente (D1):** Los pulsadores BTN_ONOFF y BTN_CAMBIAR son la fuente de información. Generan eventos discretos que el firmware traduce a tramas del protocolo. En D2, el BTN_CONEXION genera el evento de solicitud de conexión.

**Codificador / Modulador:** La capa SoftwareSerial convierte los bytes del protocolo en señales serie asíncronas (UART). Cada byte se encapsula en un frame de 10 bits (1 start bit + 8 datos + 1 stop bit). El protocolo de capa de aplicación añade semántica (HEADER + PAYLOAD + CHK).

**Canal de transmisión:** Tres conductores físicos entre los Arduinos. La señal viaja como niveles lógicos TTL (0 V = LOW, 5 V = HIGH). El GND común es imprescindible como referencia de tensión; sin él, los niveles lógicos son indefinidos y la comunicación falla.

**Decodificador:** SoftwareSerial en el receptor reconstruye los bytes a partir de la señal serie. El firmware del receptor verifica el CHK antes de ejecutar cualquier acción.

**Destino (D2):** El LED RGB es el actuador que materializa la información recibida. En D1, los LEDs de estado (LED1 y LED2) son los destinos de la información de retorno.

---

## 3. Protocolo de comunicación

### 3.1 Selección del protocolo de transporte: UART

El periférico elegido para la comunicación física entre los dos Arduino UNO es **UART (Universal Asynchronous Receiver-Transmitter)**, que es el estándar serie asíncrono nativo del microcontrolador ATmega328P.

**Justificación técnica:**

| Criterio | Análisis |
|---|---|
| **Eficiencia de cableado** | Solo 2 hilos de datos (TX y RX) + GND = 3 cables en total. El Ejercicio 1 necesitó 10 cables. Reducción del 70% en conductores. |
| **Bidireccionalidad full-duplex** | TX y RX son líneas independientes: D1 puede enviar mientras D2 responde simultáneamente sin colisión. |
| **Topología** | El sistema es estrictamente punto a punto (dos nodos), que es el caso de uso óptimo de UART. No requiere mecanismo de arbitraje ni direccionamiento. |
| **Sin jerarquía fija** | Ambos dispositivos pueden iniciar transmisiones (D1 envía comandos, D2 envía CONN_REQ y ACK/NACK). UART no impone roles Master/Slave a nivel hardware. |
| **Disponibilidad** | El ATmega328P tiene UART hardware integrado (USART0). La librería SoftwareSerial emula UART en pines adicionales, liberando el USART0 para el monitor de debug. |

**Parámetros de configuración UART (9600 8N1):**

| Parámetro | Valor | Justificación |
|---|---|---|
| Baud rate | 9600 bps | Suficiente para tramas de máximo 5 bytes. Tiempo por trama ≈ 5,2 ms. Compatible con SoftwareSerial sin pérdida de datos en Arduino UNO. |
| Bits de datos | 8 | Un byte por frame — máxima granularidad para los valores RGB (0–255). |
| Paridad | Ninguna (N) | El checksum por software (suma mod 256) ofrece mejor cobertura que el bit de paridad hardware, que solo detecta errores de 1 bit. |
| Stop bits | 1 | Estándar mínimo; suficiente dado que el receptor procesa con holgura a 9600 bps. |

### 3.2 Elección de SoftwareSerial

El Arduino UNO tiene un único puerto UART hardware (pines 0/RX y 1/TX), compartido con el convertidor USB-Serial del chip CH340/FT232. Usar ese puerto para la comunicación D1↔D2 **impediría el monitor serial de debug**, que es un requisito clave del sistema.

**Solución adoptada:** SoftwareSerial en pines 4 (RX) y 5 (TX), dejando el USART0 hardware libre para `Serial.begin(115200)` con el monitor de debug. Esta es exactamente la misma estrategia de separación de canales que en el Ejercicio 1 (bus paralelo separado del debug por Serial).

**Limitación conocida de SoftwareSerial:** No es full-duplex real; mientras recibe, no puede transmitir simultáneamente. En este protocolo no hay transmisión simultánea (siempre es request-response), por lo que no es un problema práctico.

---

## 4. Estructura de tramas

Toda trama del protocolo sigue la estructura general:

```
[ HEADER (1 byte) ] [ PAYLOAD (0–3 bytes) ] [ CHK (1 byte) ]
```

### Tabla completa de tramas

| Trama | Dirección | Bytes totales | Estructura | Descripción |
|---|---|---|---|---|
| CONN_REQ | D2 → D1 | 2 | `0xAA \| CHK` | D2 solicita conexión a D1 |
| CONN_ACK | D1 → D2 | 2 | `0xBB \| CHK` | D1 acepta la conexión |
| CMD_ONOFF | D1 → D2 | 3 | `0x01 \| STATE \| CHK` | Encender (STATE=1) o apagar (STATE=0) el LED RGB |
| CMD_COLOR | D1 → D2 | 5 | `0x02 \| R \| G \| B \| CHK` | Cambiar color RGB del LED |
| ACK | D2 → D1 | 2 | `0xFF \| CHK` | D2 confirmó recepción correcta |
| NACK | D2 → D1 | 2 | `0xFE \| CHK` | D2 detectó checksum incorrecto |

### Justificación de los códigos de header

Los valores de los headers fueron elegidos deliberadamente para maximizar la robustez de detección:

| Header | Valor | Justificación |
|---|---|---|
| CONN_REQ | `0xAA` | Patrón alternante (10101010b). Fácilmente distinguible en osciloscopio; maximiza las transiciones en el canal, útil para sincronización. |
| CONN_ACK | `0xBB` | Patrón similar al CONN_REQ (10111011b) pero diferenciable. Agrupa semánticamente los mensajes de handshake en el rango `0xAA`–`0xBB`. |
| CMD_ONOFF | `0x01` | Valor mínimo con semántica clara: "comando 1". |
| CMD_COLOR | `0x02` | "Comando 2" — secuencial con CMD_ONOFF. Permite agregar CMD_3, CMD_4 en futuras versiones sin colisión. |
| ACK | `0xFF` | Valor máximo de un byte: visualmente inequívoco en cualquier representación (binario, hex, decimal). Estándar en muchos protocolos embebidos. |
| NACK | `0xFE` | Adyacente al ACK, diferenciable con un solo bit diferente (bit 0). Una sola comparación `== 0xFF` o `== 0xFE` es suficiente para distinguirlos. |

**Comparación con alternativa ASCII:** Se evaluó usar caracteres ASCII ('A', 'B', '1', '2') como headers. Se descartó porque: (a) los valores RGB pueden contener bytes que coinciden con caracteres ASCII, causando ambigüedad; (b) los valores elegidos están claramente fuera del rango ASCII imprimible (excepto `0x01` y `0x02` que son caracteres de control no imprimibles), separando el plano de protocolo del plano de datos.

---

## 5. Algoritmo de detección de errores

### Checksum — Suma módulo 256

**Algoritmo:**

```
CHK = (B₀ + B₁ + B₂ + ... + Bₙ) MOD 256
```

Donde B₀...Bₙ son todos los bytes del HEADER + PAYLOAD, sin incluir el CHK.

**Implementación en C++:**

```cpp
uint8_t calcularChecksum(uint8_t* datos, uint8_t longitud) {
    uint16_t suma = 0;
    for (uint8_t i = 0; i < longitud; i++) {
        suma += datos[i];
    }
    return (uint8_t)(suma & 0xFF);  // MOD 256
}
```

**Ejemplo completo — CMD_COLOR con R=200, G=100, B=50:**

```
Bytes del payload:  0x02  0xC8  0x64  0x32
                   (HDR)  (R=200)(G=100)(B=50)

Suma:  0x02 + 0xC8 + 0x64 + 0x32
     =    2 +  200 +  100 +   50
     = 352 = 0x160

MOD 256: 0x160 & 0xFF = 0x60

Trama enviada: 02 C8 64 32 60
```

### Comparación con el XOR del Ejercicio 1

| Propiedad | XOR (Ejercicio 1) | Suma mod 256 (Ejercicio 2) |
|---|---|---|
| Detecta error en 1 bit | ✓ | ✓ |
| Detecta error en 1 byte completo | ✓ | ✓ |
| Detecta error en 2 bytes | Solo si el XOR no se cancela | Sí (salvo que la diferencia sume múltiplo de 256) |
| Detecta adición/eliminación de byte | ✗ | Sí (cambia la suma) |
| Detecta intercambio de dos bytes | ✗ (XOR es conmutativo) | ✗ (suma también es conmutativa) |
| Referencia industrial | Ethernet FCS (CRC, más fuerte) | Modbus RTU (LRC = complemento de suma) |
| Costo computacional | Muy bajo | Bajo |

La suma módulo 256 es superior al XOR para este caso de uso porque detecta la adición accidental de bytes, escenario posible cuando SoftwareSerial recibe bytes espurios por ruido en el canal.

---

## 6. Reacción ante trama corrupta

### En Dispositivo 2 (receptor de comandos)

Cuando D2 calcula un CHK diferente al recibido:

1. **Descarta la trama** sin modificar el estado del LED RGB.
2. **Envía NACK** (`0xFE | CHK`) a D1 por SoftwareSerial.
3. **Loguea el error** con detalle completo: CHK recibido, CHK calculado, diferencia, causa probable.
4. **Incrementa el contador** `erroresChk` para el resumen de sesión.

### En Dispositivo 1 (receptor de ACK/NACK)

Cuando D1 recibe un NACK de D2:

1. **Loguea el NACK** con el contexto del comando que lo originó.
2. **LED1 NO enciende** (solo enciende al recibir ACK válido).
3. **El sistema queda disponible** para un nuevo comando del operador.
4. **No reenvía automáticamente** el comando (ARQ — Automatic Repeat reQuest — está fuera del alcance del ejercicio, pero se documenta como mejora futura en el log).

### Ante un CHK incorrecto en el propio ACK/NACK

Si la respuesta de D2 llega con CHK inválido, D1 loguea el error y no enciende LED1, garantizando que el indicador visual solo refleja confirmaciones reales y verificadas.

---

## 7. Periférico UART

### Funcionamiento interno del USART0 (ATmega328P)

El UART del ATmega328P (denominado USART0) opera como un transceptor asíncrono que convierte datos en paralelo (registro de la CPU) a datos en serie (pin físico) y viceversa.

**Registros clave:**

| Registro | Función |
|---|---|
| `UDR0` | Buffer de datos TX/RX. Escribir aquí inicia la transmisión; leerlo obtiene el byte recibido. |
| `UBRR0` | Divisor de baud rate: `UBRR0 = (F_CPU / (16 × baud)) - 1`. Para 9600 bps con F_CPU=16 MHz: `UBRR0 = 103`. |
| `UCSR0B` | Control: `RXEN0` habilita receptor; `TXEN0` habilita transmisor; `RXCIE0` habilita interrupción por recepción. |
| `UCSR0C` | Formato del frame: `UCSZ01:00 = 11` para 8 bits de datos; `UPM01:00 = 00` para sin paridad; `USBS0 = 0` para 1 stop bit. |

**Frame UART (8N1):**

```
Línea en reposo: HIGH (1 lógico)

  ┌─────────────────────────────────────────────────────────────────┐
  │ START │  D0  │  D1  │  D2  │  D3  │  D4  │  D5  │  D6  │  D7  │ STOP │
  │  (0)  │ LSB  │      │      │      │      │      │      │ MSB  │  (1) │
  └─────────────────────────────────────────────────────────────────┘
  ← 1 bit → ←──────────────── 8 bits de datos ────────────────────→ ←1 bit→

  Duración de cada bit a 9600 bps: 1/9600 ≈ 104,2 μs
  Duración total del frame: 10 × 104,2 μs ≈ 1,04 ms por byte
```

### SoftwareSerial como emulación de UART

En este sistema se utiliza **SoftwareSerial** (librería estándar de Arduino) en lugar del USART0 hardware, para preservar el USART0 para el monitor de debug.

SoftwareSerial emula UART por software mediante:
- **Temporizadores y delays calibrados** para reproducir el baud rate correcto.
- **Interrupciones por cambio de estado** en el pin RX para detectar el bit de start.
- **Bit-banging en el pin TX** para generar los pulsos de cada bit.

**Limitaciones de SoftwareSerial vs. UART hardware:**
- No es full-duplex real (no puede recibir mientras transmite de forma confiable).
- Sensible a interferencias de otras interrupciones (como la del Timer0 que gestiona `millis()`).
- Baud rates altos (>57600) pueden ser poco confiables en Arduino UNO.

Para este sistema estas limitaciones no son problema: las tramas son cortas, el protocolo es secuencial (request-response), y 9600 bps tiene margen suficiente.

---

## 8. Máquinas de estado

### Dispositivo 1 — Máquina de estados

```
                    ┌──────────────────────────────────────────┐
                    │                                          │
                    ▼                                          │
              ┌──────────┐                                     │
     INICIO──►│   IDLE   │◄── (CHK incorrecto en CONN_REQ)    │
              └────┬─────┘                                     │
                   │ CONN_REQ recibido (CHK OK)                │
                   │ envía CONN_ACK                            │
                   │ LED2 = ON                                 │
                   ▼                                           │
              ┌────────────┐                                   │
              │ CONECTADO  │                                   │
              └──┬──────┬──┘                                   │
                 │      │                                      │
    BTN_ONOFF ───┘      └─── BTN_CAMBIAR                      │
         │                        │                           │
         ▼                        ▼                           │
    envía CMD_ONOFF         envía CMD_COLOR                   │
    espera ACK/NACK         espera ACK/NACK                   │
         │                        │                           │
    ┌────┴────┐              ┌────┴────┐                       │
    │ ACK OK  │              │ ACK OK  │                       │
    │ LED1=ON │              │ LED1=ON │                       │
    │  (2 s)  │              │  (2 s)  │                       │
    └────┬────┘              └────┬────┘                       │
         │                        │                           │
    NACK/timeout → log, LED1=OFF  │                           │
         │                        │                           │
         └────────────────────────┘                           │
                   │                                          │
              ┌────▼─────┐                                    │
              │ CONECTADO│────────────────────────────────────┘
              └──────────┘ (permanece conectado)
```

### Dispositivo 2 — Máquina de estados

```
                    ┌─────────────────────────────────────────┐
                    │                                         │
                    ▼                                         │
              ┌─────────────┐                                 │
     INICIO──►│DESCONECTADO │◄── (CHK incorrecto en CONN_ACK) │
              └──────┬──────┘                                 │
                     │ BTN_CONEXION                           │
                     │ envía CONN_REQ                         │
                     │ espera CONN_ACK                        │
                     │                                        │
                 ┌───┴───┐                                    │
                 │CONN_ACK│                                   │
                 │recibido│                                   │
                 │(CHK OK)│                                   │
                 └───┬───┘                                    │
                     │                                        │
                     ▼                                        │
              ┌────────────┐                                  │
              │ CONECTADO  │◄─────────────────────────────────┘
              └──┬──────┬──┘ (permanece, espera más comandos)
                 │      │
          CMD   ─┘      └─── CMD
        ONOFF               COLOR
          │                    │
          ▼                    ▼
    valida CHK           valida CHK
          │                    │
     ┌────┴────┐          ┌────┴────┐
     │ CHK OK  │          │ CHK OK  │
     │ejecuta  │          │ ejecuta │
     │LED ON/OF│          │ aplica  │
     │envía ACK│          │ color   │
     └─────────┘          │envía ACK│
                          └─────────┘
     CHK MAL → descarta trama → envía NACK → log error
```

---

## 9. Sistema de logging

El sistema de debug por Serial Monitor sigue y extiende la metodología del Ejercicio 1, adaptada para la naturaleza bidireccional del protocolo serie.

### Estrategia de logging

Cada dispositivo tiene un monitor serial independiente (uno por USB) a **115200 bps** (hardware Serial), con el canal de datos en SoftwareSerial a 9600 bps. Esto elimina la interferencia temporal del logging sobre la comunicación.

### Niveles de log implementados

| Prefijo | Significado |
|---|---|
| `[INIT]` | Inicialización del sistema |
| `[BTN]` | Evento de botón detectado |
| `[TX]` / `[TX #N]` | Trama enviada (con número de secuencia) |
| `[RX]` / `[RX #N]` | Trama recibida (con número de secuencia) |
| `[CMD]` | Comando ejecutado |
| `[LED1]` / `[LED2]` / `[LED-RGB]` | Estado de los indicadores físicos |
| `[ESPERA]` | Inicio de espera con timeout |
| `[ESTADO]` | Transición de estado del sistema |
| `[ERROR]` | Error de protocolo (CHK, timeout, header desconocido) |
| `[WARN]` | Advertencia no crítica |

### Ejemplo completo de sesión D1

```
=======================================================
  DISPOSITIVO 1 | Maestro | UART Serie
  Comunicacion de Datos — Ejercicio 2 | v1.0.0
=======================================================
  Baud SoftSerial  : 9600
  PINOUT SoftSerial: RX=4, TX=5
  BTN ON/OFF       : pin 11
  BTN CAMBIAR      : pin 12
  LED1 (ACK)       : pin 8
  LED2 (CONEXION)  : pin 9
  Checksum         : Suma de bytes MOD 256
=======================================================

[INIT] Estado inicial: IDLE
[INIT] Esperando solicitud de conexion de D2...

-------------------------------------------------------
[RX] Trama CONN_REQ:
    Byte 0: 0xAA (dec=170)
    Byte 1: 0xAA (dec=170)
    Verificacion CHK: suma(0xAA) = 0xAA → mod256 = 0xAA | Recibido: 0xAA  → OK ✓

[TX #0] Enviando CONN_ACK
    Byte 0: 0xBB (dec=187)  ← HEADER CONN_ACK
    CHK (Suma mod256): 0xBB = 0xBB → mod256 = 0xBB
    Byte CHK: 0xBB (adjuntado al final de la trama)
    --> Trama enviada por SoftwareSerial.
[LED2] Encendido — conexion activa
[ESTADO] Sistema CONECTADO. Esperando botones...

[BTN] ON/OFF presionado
      Estado nuevo del LED RGB: ENCENDIDO
-------------------------------------------------------
[TX #1] Enviando CMD_ONOFF
    Byte 0: 0x01 (dec=1)  ← HEADER CMD_ONOFF
    Byte 1: 0x01 (dec=1)  ← STATE (1=ENC, 0=APG) → ENCENDER
    CHK (Suma mod256): 0x01 + 0x01 = 0x02 → mod256 = 0x02
    Byte CHK: 0x02 (adjuntado al final de la trama)
    --> Trama enviada por SoftwareSerial.
[ESPERA] Aguardando ACK/NACK de D2... (timeout: 3000 ms)
[RX] Trama ACK:
    Byte 0: 0xFF (dec=255)
    Byte 1: 0xFF (dec=255)
    Verificacion CHK: suma(0xFF) = 0xFF → mod256 = 0xFF | Recibido: 0xFF  → OK ✓
[RX] ACK valido recibido para CMD_ONOFF
[LED1] Encendido por 2000 ms (ACK confirmado)
[LED1] Apagado
```

### Ejemplo con error de checksum (D2)

```
[RX #3] Header recibido: 0x02
    Tipo identificado: CMD_COLOR
    Byte 0: 0x02 (dec=2)    ← HEADER
    Byte 1: 0xC8 (dec=200)  ← R (Rojo)   = 200
    Byte 2: 0x64 (dec=100)  ← G (Verde)  = 100
    Byte 3: 0x32 (dec=50)   ← B (Azul)   = 50
    Byte 4: 0x61 (dec=97)   ← CHK recibido
    Verificacion CHK: 0x02+0xC8+0x64+0x32 = 0x160 → mod256 = 0x60 | Recibido: 0x61  → ERROR ✗

*** ERROR DE CHECKSUM ***
    Trama RX #3
    CHK recibido : 0x61 (dec=97)
    CHK calculado: 0x60 (dec=96)
    Diferencia   : 1 unidades
    Accion       : Trama DESCARTADA. Estado LED RGB: SIN CAMBIOS.
    Respuesta    : NACK enviado a D1.
    DIAGNOSTICO  :
      1. Ruido electrico en el canal TX/RX (cable largo o cerca de fuente).
      2. GND no comun entre D1 y D2 — causa mas frecuente.
      3. Baud rate diferente entre D1 y D2 (debe ser 9600 en ambos).
      4. SoftwareSerial con interferencia de otras interrupciones.
    Errores CHK acumulados: 1
```

---

## 10. Conexión física

### Diagrama de conexión eléctrica

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                     CONEXIÓN FÍSICA DEL SISTEMA                             ║
╠══════════════════════════════════════════════════════════════════════════════╣

  ARDUINO UNO — DISPOSITIVO 1 (D1)         ARDUINO UNO — DISPOSITIVO 2 (D2)
  ┌─────────────────────────────┐           ┌────────────────────────────────┐
  │                         5   ├──────────►│  4  (RX SoftSerial)            │
  │                    TX (pin) │           │  (pin)                         │
  │                             │           │                                │
  │                         4   │◄──────────┤  5  (TX SoftSerial)            │
  │                    RX (pin) │           │  (pin)                         │
  │                             │           │                                │
  │                        GND  ├───────────┤  GND                           │
  │                             │           │                       ┌───────┐│
  │  LED1────────────────── 8   │           │  9  ──[220Ω]──► R ──►│       ││
  │  [220Ω]                     │           │  10 ──[220Ω]──► G ──►│ LED   ││
  │  GND                        │           │  6  ──[220Ω]──► B ──►│ RGB   ││
  │                             │           │                       │(CC)   ││
  │  LED2────────────────── 9   │           │                       └──┬────┘│
  │  [220Ω]                     │           │                         GND    │
  │  GND                        │           │                                │
  │                             │           │  BTN_CONEXION ─── 11           │
  │  BTN_ONOFF ──────────── 11  │           │  ─────────────────             │
  │  (INPUT_PULLUP → GND)       │           │  (INPUT_PULLUP → GND)          │
  │                             │           │                                │
  │  BTN_CAMBIAR ─────────── 12 │           │                                │
  │  (INPUT_PULLUP → GND)       │           │                                │
  │                             │           │                                │
  │  USB ─────────────────── Serial (debug) │  USB ─────── Serial (debug)    │
  └─────────────────────────────┘           └────────────────────────────────┘

  LEYENDA:
  ──────► : Señal TX → RX (cruzado)
  ───────  : Línea de GND común (OBLIGATORIA)
  [220Ω]  : Resistencia limitadora de corriente
  (CC)    : LED RGB cátodo común (el cátodo va a GND)
  INPUT_PULLUP: botón conectado entre el pin y GND
```

### Pinout completo

**Dispositivo 1:**

| Pin | Tipo | Función | Conexión |
|---|---|---|---|
| 4 | INPUT | SoftSerial RX | ← Pin 5 de D2 |
| 5 | OUTPUT | SoftSerial TX | → Pin 4 de D2 |
| 8 | OUTPUT | LED1 (indicador ACK) | → 220Ω → LED → GND |
| 9 | OUTPUT | LED2 (indicador conexión) | → 220Ω → LED → GND |
| 11 | INPUT | BTN_ONOFF | → GND (INPUT_PULLUP) |
| 12 | INPUT | BTN_CAMBIAR | → GND (INPUT_PULLUP) |
| 0/1 | HW Serial | Debug USB | Monitor Arduino IDE |
| GND | GND | Referencia de tensión | GND de D2 |

**Dispositivo 2:**

| Pin | Tipo | Función | Conexión |
|---|---|---|---|
| 4 | INPUT | SoftSerial RX | ← Pin 5 de D1 |
| 5 | OUTPUT | SoftSerial TX | → Pin 4 de D1 |
| 6 | OUTPUT (PWM) | LED RGB Azul | → 220Ω → LED B → GND |
| 9 | OUTPUT (PWM) | LED RGB Rojo | → 220Ω → LED R → GND |
| 10 | OUTPUT (PWM) | LED RGB Verde | → 220Ω → LED G → GND |
| 11 | INPUT | BTN_CONEXION | → GND (INPUT_PULLUP) |
| 0/1 | HW Serial | Debug USB | Monitor Arduino IDE |
| GND | GND | Referencia de tensión | GND de D1 |

### Cálculo de resistencias limitadoras

Para LEDs con V_f ≈ 2V (rojo/verde) y V_f ≈ 3.2V (azul), con I_LED = 10 mA:

```
R = (V_CC - V_f) / I_LED

LED rojo/verde: R = (5 - 2) / 0.010 = 300 Ω → usar 220 Ω (corriente ≈ 13.6 mA, seguro)
LED azul:       R = (5 - 3.2) / 0.010 = 180 Ω → usar 220 Ω (corriente ≈ 8.2 mA, correcto)
```

220 Ω es el valor estándar disponible que garantiza operación segura en todos los canales.

---

## 11. Diagrama de flujo

### Flujo completo del sistema

```
D1 (Maestro)                                        D2 (Receptor)
══════════════════                                  ════════════════════

INICIO                                              INICIO
  │                                                   │
  ▼                                                   ▼
[Configura pines]                               [Configura pines]
[Serial 115200]                                 [Serial 115200]
[SoftSerial 9600]                               [SoftSerial 9600]
  │                                             [apaga LED RGB]
  ▼                                                   │
ESTADO = IDLE                                   ESTADO = DESCONECTADO
  │                                                   │
  ▼                                                   ▼
┌─────────────────────────────┐             ┌────────────────────────┐
│  LOOP — ESTADO IDLE         │             │  LOOP — DESCONECTADO   │
│                             │             │                        │
│  ¿byte disponible en canal? │             │  ¿BTN_CONEXION pulsado?│
│  NO → continúa polling      │             │  NO → vuelve a inicio  │
│  SÍ ↓                       │             │  SÍ ↓                  │
│                             │             │                        │
│  ¿header == 0xAA?           │             │  envía CONN_REQ        │
│  NO → log WARN, descarta    │             │  (0xAA | CHK)          │
│  SÍ ↓                       │             │       │                │
│                             │             │  espera CONN_ACK       │
│  lee CHK                    │             │  (timeout 1500ms)      │
│  verifica CHK               │             │       │                │
│  CHK MAL → log ERROR        │             │  ¿timeout?             │
│  CHK OK ↓                   │             │  SÍ → log WARN         │
│                             │             │       vuelve a inicio  │
│  envía CONN_ACK             │◄────────────┤  NO ↓                  │
│  (0xBB | CHK)               │─────────────►       │                │
│  LED2 = ON                  │             │  ¿header == 0xBB?      │
│  ESTADO = CONECTADO         │             │  NO → log ERROR        │
└─────────────────────────────┘             │  SÍ ↓                  │
  │                                         │                        │
  ▼                                         │  verifica CHK          │
┌─────────────────────────────┐             │  CHK MAL → log ERROR   │
│  LOOP — CONECTADO           │             │  CHK OK ↓              │
│                             │             │                        │
│  ¿BTN_ONOFF?                │             │  ESTADO = CONECTADO    │
│  SÍ → toggle estado         │             └────────────────────────┘
│       envía CMD_ONOFF       │                       │
│       espera ACK (3000ms)   │                       ▼
│       ¿ACK OK?              │             ┌────────────────────────┐
│       SÍ → LED1 ON 2s       │             │  LOOP — CONECTADO      │
│       NO → log ERROR/NACK   │             │                        │
│                             │             │  ¿byte en canal?       │
│  ¿BTN_CAMBIAR?              │             │  NO → vuelve           │
│  SÍ → genera RGB aleatorio  │             │  SÍ ↓                  │
│       envía CMD_COLOR       │             │                        │
│       espera ACK (3000ms)   │             │  lee header            │
│       ¿ACK OK?              │             │  ¿0x01? → CMD_ONOFF    │
│       SÍ → LED1 ON 2s       │             │  ¿0x02? → CMD_COLOR    │
│       NO → log ERROR/NACK   │             │  otro → log WARN       │
│                             │             │       descarta byte     │
│  vuelve al inicio del loop  │             │       │                │
└─────────────────────────────┘             │  lee payload + CHK     │
                                            │  ¿timeout?             │
                                            │  SÍ → log ERROR        │
                                            │       descarta trama   │
                                            │  NO ↓                  │
                                            │                        │
                                            │  calcula CHK           │
                                            │  ¿CHK OK?              │
                                            │  SÍ → ejecuta comando  │
                                            │       envía ACK        │
                                            │  NO → descarta trama   │
                                            │       envía NACK       │
                                            │       log error CHK    │
                                            │                        │
                                            │  vuelve al inicio      │
                                            └────────────────────────┘
```

---

## 12. Inventario de componentes

| Componente | Cantidad | Uso |
|---|---|---|
| Arduino UNO | 2 | D1 y D2 |
| LED RGB cátodo común | 1 | Salida visual D2 |
| LED individual (verde o rojo) | 2 | LED1 (ACK) y LED2 (Conexión) en D1 |
| Resistencia 220 Ω | 5 | 3 para LED RGB + 1 para LED1 + 1 para LED2 |
| Pulsador 4 pines NO | 3 | BTN_ONOFF y BTN_CAMBIAR (D1) + BTN_CONEXION (D2) |
| Cable/jumper macho-macho | ≥ 15 | Conexiones entre protoboard y Arduinos |
| Cable/jumper macho-macho | 3 | TX, RX y GND entre los dos Arduinos |
| Protoboard | 2 | Montaje sin soldadura |
| Cable USB tipo B | 2 | Programación y monitor serial (uno por Arduino) |

---

## 13. Decisiones de diseño — justificación técnica

### 13.1 SoftwareSerial vs. USART0 hardware

**Decisión:** Usar SoftwareSerial (pines 4/5) para D1↔D2 y reservar USART0 (pines 0/1) para el monitor de debug.

**Justificación:** La depuración de un protocolo de comunicación requiere visibilidad total de las tramas enviadas y recibidas. Si se usara el USART0 para los datos, sería imposible usar el monitor serial del IDE sin desconectar el cable de datos. La separación de canales es el patrón correcto para sistemas embebidos donde el debug por puerto serie es un requisito de desarrollo.

### 13.2 Baud rate 9600 bps

**Decisión:** 9600 bps para SoftwareSerial, 115200 bps para el monitor de debug.

**Justificación:** 9600 bps es el baud rate más confiable con SoftwareSerial en Arduino UNO. La trama más larga del protocolo (CMD_COLOR) tiene 5 bytes = 50 bits en UART = 5,2 ms de transmisión, lo que es completamente aceptable para este sistema. El monitor de debug a 115200 bps garantiza que el Serial.print() no bloquea el loop durante más de 1 ms por mensaje.

### 13.3 Checksum suma módulo 256 vs. XOR

**Decisión:** Usar suma mod 256 en este ejercicio (XOR en el Ejercicio 1).

**Justificación técnica:** Además de las ventajas ya documentadas en la sección 5, esta elección permite al informe comparar dos mecanismos de detección de errores distintos, cumpliendo el objetivo pedagógico de "describir el algoritmo utilizado". La suma mod 256 es la base del LRC (Longitudinal Redundancy Check) de Modbus, lo que lo conecta con protocolos industriales reales.

### 13.4 NACK sin reenvío automático (no ARQ)

**Decisión:** Al recibir NACK, D1 loguea el error pero no reenvía el comando.

**Justificación:** El enunciado pide implementar un mecanismo de detección de errores, no de corrección o retransmisión. Implementar ARQ (Automatic Repeat reQuest) agregaría complejidad (contadores de secuencia, ventanas de retransmisión, timeouts de retry) que está fuera del alcance del ejercicio. El NACK documenta el error para el operador, quien decide si repetir la acción. Se documenta como mejora futura.

### 13.5 Pin 6 para LED RGB azul (en lugar de pin 11 del Ejercicio 1)

**Decisión:** Mover el canal azul del LED RGB del pin 11 al pin 6.

**Justificación:** El pin 11 es necesario para BTN_CONEXION en D2. El pin 6 tiene PWM hardware en el Arduino UNO (controlado por Timer0), por lo que `analogWrite(6, valor)` funciona igual que en el pin 11. No hay pérdida de funcionalidad PWM.

---

## 14. Comparación con protocolos alternativos

### UART vs. I²C

| Criterio | UART (elegido) | I²C |
|---|---|---|
| Cables de datos | 2 (TX, RX) | 2 (SDA, SCL) |
| Modo de operación | Full-duplex | Half-duplex (bus compartido) |
| Topología | Punto a punto | Multi-master, multi-slave |
| Velocidad | 9600–115200 bps típico | 100 kbps (Standard), 400 kbps (Fast) |
| Direccionamiento | No (conexión directa) | Sí (7 bits de dirección) |
| Idoneidad para este sistema | ✓ Óptimo (solo 2 nodos, full-duplex) | ✗ Half-duplex complica ACK simultáneo |
| Implementación en Arduino | SoftwareSerial (simple) | Wire.h (requiere dirección, roles M/S) |

I²C requiere asignar roles Master y Slave fijos, lo que complica el protocolo de este sistema donde tanto D1 como D2 inician transmisiones (CONN_REQ desde D2, ACK desde D2). Con UART, la bidireccionalidad es natural.

### UART vs. SPI

| Criterio | UART (elegido) | SPI |
|---|---|---|
| Cables de datos | 2 (TX, RX) + GND | 4 (MOSI, MISO, SCK, CS) + GND |
| Sincronización | Asíncrona (sin clock) | Síncrona (clock compartido) |
| Velocidad | Limitada por baud rate | Hasta 4 MHz o más |
| Roles | Pares iguales | Master/Slave estricto |
| Eficiencia de cableado | ✓ 3 cables totales | ✗ 5 cables totales |

SPI es más rápido pero requiere 4 cables de señal y roles fijos Master/Slave, que son innecesarios para este sistema de 2 nodos con velocidad modesta.

**Conclusión:** UART es la elección óptima para comunicación punto a punto bidireccional con mínimo cableado y sin requisito de jerarquía fija entre nodos. Los 3 cables totales (TX, RX, GND) representan una reducción del 70% respecto al bus paralelo del Ejercicio 1 (10 cables), cumpliendo el objetivo de eficiencia de cableado planteado en el enunciado.

---

## 15. Comparación con el Ejercicio 1

| Aspecto | Ejercicio 1 (Paralelo) | Ejercicio 2 (Serie) |
|---|---|---|
| Tipo de comunicación | Paralela, unidireccional | Serie, bidireccional |
| Cables de datos | 8 (bus) + 1 (STR) + 1 (GND) = 10 | 1 (TX) + 1 (RX) + 1 (GND) = 3 |
| Velocidad | ~85 ms para 5 bytes (timing manual) | 5,2 ms para 5 bytes (9600 bps) |
| Detección de errores | XOR de 4 bytes | Suma módulo 256 |
| Confirmación de recepción | No (unidireccional) | Sí (ACK/NACK) |
| Handshake de conexión | No | Sí (CONN_REQ / CONN_ACK) |
| Protocolo de transporte | Manual (bus paralelo + STR) | UART (SoftwareSerial) |
| Complejidad del firmware | Media | Alta (máquina de estados, bidireccional) |
| Robustez | Media (sin confirmación) | Alta (ACK/NACK + CHK) |

---

*Documento generado como resolución del Ejercicio 2 de la Tarea N°1 de Comunicación de Datos.*  
*Firmware: `dispositivo1_serie.ino` y `dispositivo2_serie.ino` — versión 1.0.0*
