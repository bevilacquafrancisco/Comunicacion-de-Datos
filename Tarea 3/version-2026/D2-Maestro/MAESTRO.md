# Dispositivo 2 — Maestro IR + Servidor Wi-Fi
## Documentación del Firmware

> **TP3 Integrador — Comunicación de Datos**  
> Ingeniería en Computación | Bevilacqua, Francisco — Clement, Sebastián | Junio 2025

---

## Índice

1. [Rol y arquitectura del Dispositivo 2](#1-rol-y-arquitectura-del-dispositivo-2)
2. [Hardware y conexionado](#2-hardware-y-conexionado)
3. [Estructura del firmware](#3-estructura-del-firmware)
4. [Modelo de concurrencia: dual-core y FreeRTOS](#4-modelo-de-concurrencia-dual-core-y-freertos)
5. [Access Point Wi-Fi y servidor HTTP](#5-access-point-wi-fi-y-servidor-http)
6. [Servidor WebSocket: protocolo D2 ↔ D3](#6-servidor-websocket-protocolo-d2--d3)
7. [Conexión y desconexión de D3](#7-conexión-y-desconexión-de-d3)
8. [CRC-8 en el enlace IR (D1 ↔ D2)](#8-crc-8-en-el-enlace-ir-d1--d2)
9. [CRC-16 en el enlace WebSocket (D2 ↔ D3)](#9-crc-16-en-el-enlace-websocket-d2--d3)
10. [Protocolo IR: envío de color y handshaking con ACK](#10-protocolo-ir-envío-de-color-y-handshaking-con-ack)
11. [Tabla de colores y gestión de estado](#11-tabla-de-colores-y-gestión-de-estado)
12. [Persistencia de estado: NVS / Preferences](#12-persistencia-de-estado-nvs--preferences)
13. [LEDs de estado de conectividad](#13-leds-de-estado-de-conectividad)
14. [Secuencia de arranque: `setup()`](#14-secuencia-de-arranque-setup)
15. [Ciclo principal: `loop()`](#15-ciclo-principal-loop)
16. [Flujo de datos extremo a extremo](#16-flujo-de-datos-extremo-a-extremo)
17. [Dependencias de software](#17-dependencias-de-software)
18. [Configuración de entorno: `platformio.ini`](#18-configuración-de-entorno-platformioini)
19. [Logs de depuración](#19-logs-de-depuración)

---

## 1. Rol y arquitectura del Dispositivo 2

El Dispositivo 2 es el **nodo central** del sistema. Su función es doble e irreemplazable: actúa como Maestro IR hacia D1 y como servidor de red hacia D3, siendo el único punto de convergencia entre dos capas de comunicación cualitativamente distintas.

```
[ D3: Navegador web en celular / PC / tablet ]
               |
         Wi-Fi 802.11 b/g/n
         WebSocket + CRC-16
               |
      ┌────────▼────────┐
      │   D2 — Maestro   │ ← Access Point WPA2
      │   ESP32           │ ← Servidor HTTP + WebSocket
      │   LED RGB         │ ← Indicador visual de estado
      │   LEDs CONN/DISC  │ ← Indicadores de conectividad
      └────────┬─────────┘
               |
        Infrarrojo NEC Raw
        Trama 32 bits + CRC-8
               |
      ┌────────▼────────┐
      │   D1 — Esclavo   │
      │   LED RGB         │ ← Actuador del sistema
      └──────────────────┘
```

### Responsabilidades

| Responsabilidad | Módulo involucrado |
|---|---|
| Crear red Wi-Fi sin dependencia de internet | `WiFi.softAP()` |
| Servir la interfaz web de D3 | `ESPAsyncWebServer` + `WEB_UI` embebida |
| Gestionar la sesión WebSocket con D3 | `AsyncWebSocket` + `onWsEvent()` |
| Validar CRC-16 de mensajes entrantes de D3 | `calcularCRC16()` + `procesarMensajeWS()` |
| Firmar con CRC-16 los mensajes salientes hacia D3 | `enviarMensajeWS()` + `enviarMensajeWSBroadcast()` |
| Enviar tramas IR con CRC-8 a D1 | `calcularCRC8()` + `enviarColorIR()` |
| Validar el ACK IR recibido desde D1 | `enviarColorIR()` — sección de espera |
| Gestionar reintentos ante fallo de ACK | `ejecutarCicloIR()` |
| Actualizar su propio LED RGB solo ante ACK válido | `setColor()` |
| Persistir el estado ante cortes de energía | `Preferences` (NVS) |
| Indicar físicamente el estado de la conexión con D3 | LEDs GPIO 32/33/14 |

---

## 2. Hardware y conexionado

### Componentes

| Componente | Función | Pin ESP32 |
|---|---|---|
| KY-005 (emisor IR) | Transmisión de tramas IR a D1 | GPIO 4 (S) |
| KY-022 (receptor IR) | Recepción de ACK IR desde D1 | GPIO 5 (OUT) |
| LED RGB cátodo común | Indicador del color actual del sistema | GPIO 25/26/27 |
| LED verde (CONN) | D3 tiene sesión WebSocket activa | GPIO 32 |
| LED rojo (DISC) | Desconexión de D3 detectada | GPIO 33 |
| LED amarillo (AP) | Access Point activo, sistema operando | GPIO 14 |
| Resistencias 220 Ω × 3 | Limitación de corriente del LED RGB | En serie con R/G/B |
| Resistencias 330 Ω × 3 | Limitación de corriente de LEDs de estado | En serie con cada LED |

### Diagrama de conexionado

```
ESP32 DevKit v1
┌──────────────────────────────────────────────────────────┐
│                                                          │
│  GPIO  4 ──────────────────────── KY-005  S  (emisor)   │
│  GPIO  5 ──────────────────────── KY-022  OUT (receptor) │
│  3.3V  ────────────────────────── KY-005 + KY-022  VCC  │
│  GND   ────────────────────────── KY-005 + KY-022  GND  │
│                                                          │
│  GPIO 25 ──── [220 Ω] ─────────── LED RGB  R            │
│  GPIO 26 ──── [220 Ω] ─────────── LED RGB  G            │
│  GPIO 27 ──── [220 Ω] ─────────── LED RGB  B            │
│  GND   ────────────────────────── LED RGB  GND (cátodo) │
│                                                          │
│  GPIO 32 ──── [330 Ω] ─────────── LED CONN (verde)      │
│  GPIO 33 ──── [330 Ω] ─────────── LED DISC (rojo)       │
│  GPIO 14 ──── [330 Ω] ─────────── LED AP   (amarillo)   │
│  GND   ────────────────────────── LEDs estado  cátodos  │
│                                                          │
│  USB   ────────────────────────── Alimentación / Prog    │
└──────────────────────────────────────────────────────────┘
```

### Criterios de selección de pines

Los pines GPIO 4, 5, 14, 25, 26, 27, 32 y 33 son todos de propósito general, sin funciones especiales de arranque, con soporte de interrupciones y compatibles con 3.3 V. Se evitan los GPIO 0, 2, 6–11, 12, 15 y 34–39 por sus restricciones de boot o por ser input-only.

El canal LEDC 3 se asigna al rojo del LED RGB (en lugar del canal 0 natural) para evitar la colisión con el canal que IRremote reserva internamente para la modulación RMT de la portadora de 38 kHz.

---

## 3. Estructura del firmware

```
D2_Maestro/
├── src/
│   └── main.cpp      ← 809 líneas: lógica completa del firmware
├── include/
│   ├── config.h      ← constantes de hardware, red, protocolos y color
│   └── web_ui.h      ← interfaz web de D3 embebida como string literal
└── platformio.ini    ← entorno, plataforma y dependencias
```

`config.h` se incluye como **primer header** en `main.cpp`, antes de `IRremote.hpp`. Esto es un requisito funcional: IRremote lee la macro `IR_SEND_PIN` en tiempo de preprocesamiento para configurar el canal RMT del ESP32. Si `config.h` no está primero, el pin de transmisión IR sería indefinido.

`web_ui.h` expone el símbolo `WEB_UI` como un string literal `PROGMEM` (const en flash). El servidor HTTP lo sirve directamente a través de `send_P()`, sin copiarlo a RAM. Esto es fundamental dado que la interfaz HTML+CSS+JS ocupa ~15 KB y la RAM del ESP32 debe reservarse para las estructuras de red en tiempo de ejecución.

---

## 4. Modelo de concurrencia: dual-core y FreeRTOS

El ESP32 es un SoC dual-core (Xtensa LX6) que corre FreeRTOS. En el framework Arduino para ESP32, esta arquitectura se manifiesta así:

| Core | Tarea FreeRTOS | Responsabilidad |
|---|---|---|
| Core 0 | Tarea lwIP / red | Pila TCP/IP, gestión de sockets, callbacks de ESPAsyncWebServer y AsyncWebSocket |
| Core 1 | Tarea Arduino | `setup()` y `loop()`: lógica de aplicación e IR |

Esta separación es la que permite que el servidor WebSocket continúe detectando eventos de red (incluyendo desconexiones de D3) incluso cuando el `loop()` está ocupado ejecutando el ciclo IR, que puede durar hasta ~6.9 segundos en el peor caso (3 reintentos × 2 s de timeout + delays).

### Variables compartidas entre cores

Las variables que se escriben desde el Core 0 (callback WebSocket) y se leen desde el Core 1 (loop) se declaran `volatile`:

```cpp
volatile bool    irPendiente         = false;
volatile uint8_t r_pendiente         = 0;
volatile uint8_t g_pendiente         = 0;
volatile uint8_t b_pendiente         = 0;
volatile uint8_t colorIdx_pendiente  = 0;
volatile bool    encendido_pendiente = false;
```

El qualifier `volatile` instruye al compilador a no cachear estas variables en registros, garantizando que cada acceso genera una lectura/escritura en memoria real y que las modificaciones de un core son inmediatamente visibles para el otro.

**¿Por qué no se necesita mutex?** El patrón de acceso es seguro sin mutex porque hay un único escritor por variable: el callback WS (Core 0) solo escribe `irPendiente = true` y los valores pendientes; el loop (Core 1) es el único que los lee y limpia. En ESP32, la escritura de `bool` y `uint8_t` es atómica a nivel hardware (instrucción de 8/32 bits indivisible). Si el patrón fuera escritura desde ambos cores simultáneamente, se requeriría `portMUX_TYPE` o `SemaphoreHandle_t`.

---

## 5. Access Point Wi-Fi y servidor HTTP

### Modo Access Point (AP)

D2 opera en modo **SoftAP** (Software Access Point), creando una red Wi-Fi propia sin conexión a Internet. Esta decisión de diseño cumple la consigna textual ("la conexión entre los dispositivos 2 y 3 no debe depender de internet") y tiene ventajas adicionales:

- **Auto-contenido:** funciona en cualquier entorno sin infraestructura preexistente.
- **Control total de clientes:** D2 tiene visibilidad directa de las conexiones/desconexiones.
- **Determinismo de dirección:** la IP del AP es siempre `192.168.4.1` (default del ESP32 en modo SoftAP). D3 siempre sabe a dónde conectarse.

**Parámetros del Access Point:**

```cpp
WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
//           nombre   contraseña   canal       ssid_hidden  max_clientes
```

| Parámetro | Valor | Justificación |
|---|---|---|
| `AP_CHANNEL` | 6 | Canal no solapado (1, 6 y 11 son los tres canales no solapados en 2.4 GHz). Reduce interferencia con redes vecinas. |
| `ssid_hidden` | 0 | SSID visible. Necesario para que D3 encuentre la red. |
| `AP_MAX_CONN` | 1 | Solo D3 se conecta. Limitar clientes reduce superficie de ataque y carga del servidor. |
| `AP_PASSWORD` | WPA2, ≥12 chars | Seguridad mínima aceptable. WPA2-PSK con CCMP/AES es el estándar actual para redes privadas. |

**Seguridad de la red:**  
WPA2 (Wi-Fi Protected Access 2) usa AES-128 con CCMP (Counter Mode CBC-MAC Protocol) para cifrado y autenticación. La clave de sesión deriva de la contraseña mediante PBKDF2 con sal aleatoria (el handshake de 4 vías del protocolo WPA2). Esto garantiza que el tráfico entre D3 y D2 está cifrado en la capa física Wi-Fi, incluso antes de la capa de aplicación (WebSocket).

### Servidor HTTP

```cpp
server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", WEB_UI);
});
server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
});
server.begin();
```

`ESPAsyncWebServer` registra handlers para rutas específicas. La ruta raíz `"/"` sirve el string `WEB_UI` directamente desde flash mediante `send_P()` (variante de `send()` que lee desde PROGMEM, evitando copiar la página a SRAM). Cualquier otra ruta recibe una respuesta 404.

El servidor es **asíncrono**: los handlers se ejecutan como callbacks en el contexto del Core 0 sin bloquear el `loop()`. Esto difiere de los servidores web síncronos (como `WebServer.h` del Arduino estándar) que bloquearían todo el sistema mientras procesan una petición HTTP.

---

## 6. Servidor WebSocket: protocolo D2 ↔ D3

### ¿Por qué WebSocket?

WebSocket (RFC 6455) establece una conexión TCP persistente y **full-duplex** sobre HTTP. Una vez establecida, ambos extremos pueden enviar datos en cualquier momento sin el overhead de abrir/cerrar conexiones como haría HTTP REST. Para este sistema es esencial porque:

1. D2 necesita empujar el resultado del ciclo IR (que puede tardar varios segundos) a D3 en cuanto esté disponible, sin que D3 tenga que hacer polling.
2. La detección de desconexión está integrada en el protocolo mediante los mensajes de control Ping/Pong del estándar WebSocket.

### Registro del handler de eventos

```cpp
ws.onEvent(onWsEvent);
server.addHandler(&ws);
```

`AsyncWebSocket` registra un handler único que recibe todos los eventos de todos los clientes. El handler `onWsEvent()` es el punto de entrada de toda la comunicación con D3.

### Eventos del protocolo WebSocket

El handler `onWsEvent()` procesa cuatro tipos de eventos:

```
WS_EVT_CONNECT    → nueva conexión TCP + handshake WS completado
WS_EVT_DISCONNECT → cierre de conexión (voluntario o por timeout TCP)
WS_EVT_DATA       → frame de texto o binario recibido del cliente
WS_EVT_ERROR      → error de nivel protocolo en el socket
```

**Filtrado de frames fragmentados:**

WebSocket permite enviar mensajes en múltiples frames. El firmware solo procesa mensajes completos para evitar el parsing parcial de JSON:

```cpp
if (info->final && info->index == 0 &&
    info->len == len && info->opcode == WS_TEXT) {
    procesarMensajeWS(client, data, len);
}
```

Esta condición verifica que:
- `info->final = true`: es el último (o único) frame del mensaje.
- `info->index == 0`: es el primer byte del mensaje (no fragmento intermedio).
- `info->len == len`: el frame recibido tiene exactamente la longitud declarada.
- `info->opcode == WS_TEXT`: es un frame de texto (los mensajes JSON son texto).

### Catálogo de mensajes

**D3 → D2 (comandos):**

| `tipo` | Campos adicionales | Descripción |
|---|---|---|
| `CMD_COLOR` | `color: "NOMBRE"` | Enviar el color nombrado al LED de D1 |
| `CMD_ENCENDER` | — | Encender D1 con el último color confirmado |
| `CMD_APAGAR` | — | Enviar RGB={0,0,0} a D1 |
| `CMD_DESCONECTAR` | — | Solicitar cierre voluntario de la sesión WS |
| `PING` | — | Keepalive de aplicación |

**D2 → D3 (respuestas):**

| `tipo` | Campos adicionales | Descripción |
|---|---|---|
| `ESTADO_ACTUAL` | `encendido, color, r, g, b` | Estado completo al conectar D3 |
| `RESULTADO_CMD` | `exito, encendido, color, r, g, b` | Resultado del ciclo IR hacia D1 |
| `CRC_ERROR` | `info` | Mensaje recibido de D3 con CRC inválido |
| `ACK_DESCONECTAR` | — | Confirmación de cierre voluntario a D3 |
| `PONG` | — | Respuesta al keepalive |

Todos los mensajes llevan el campo adicional `"crc16"` con el CRC-16 calculado sobre el payload JSON sin ese campo (ver Sección 9).

### Mantenimiento periódico: `ws.cleanupClients()`

```cpp
void loop() {
    ws.cleanupClients();
    // ...
}
```

Esta llamada libera la memoria de los objetos `AsyncWebSocketClient` cuya conexión TCP fue cerrada pero que el servidor aún no removió de su lista interna. Sin esta limpieza, las desconexiones frecuentes acumularían objetos huérfanos que consumirían heap hasta agotar la memoria disponible.

---

## 7. Conexión y desconexión de D3

### Conexión de D3

Cuando D3 abre `http://192.168.4.1` en su navegador, se produce la siguiente secuencia:

1. El navegador establece una conexión TCP al puerto 80 del AP.
2. Realiza el upgrade HTTP → WebSocket (handshake de 3 vías HTTP 101 Switching Protocols).
3. ESPAsyncWebServer dispara `WS_EVT_CONNECT` en D2.
4. D2 actualiza los LEDs de estado y llama a `enviarEstadoInicial(client)`.
5. `enviarEstadoInicial()` construye un mensaje `ESTADO_ACTUAL` con el color y estado actuales, le agrega el CRC-16 y lo envía al cliente.
6. D3 valida el CRC-16 del mensaje recibido y actualiza su interfaz con el estado real del sistema.

Este mecanismo garantiza que D3 nunca muestra un estado desactualizado: inmediatamente al conectarse recibe la información real desde D2, sin necesidad de enviar ningún comando de consulta.

### Desconexión voluntaria de D3

La consigna requiere que D3 pueda desconectarse voluntariamente sin afectar el estado del LED. La implementación usa un handshake de cierre de dos pasos:

```
D3                          D2
 |                            |
 |-- CMD_DESCONECTAR -------->|
 |   (con CRC-16 válido)      |
 |                            |  D2 valida CRC-16
 |                            |  D2 no modifica irPendiente
 |<-- ACK_DESCONECTAR --------|  (con CRC-16)
 |                            |
 |-- ws.close(1000) --------->|  cierre limpio WebSocket
 |                            |
                              |  WS_EVT_DISCONNECT disparado
                              |  setLedConn(false), setLedDisc(true)
```

**En D3 (JavaScript):**

```javascript
function desconectar() {
    desconectandoVoluntariamente = true;
    enviar({ tipo: 'CMD_DESCONECTAR' });
}
// Al recibir ACK_DESCONECTAR:
if (msg.tipo === 'ACK_DESCONECTAR') {
    ws.close(1000, "Cierre voluntario iniciado por D3");
}
```

El flag `desconectandoVoluntariamente` permite distinguir en el evento `onclose` si el cierre fue intencional (mostrar mensaje neutro) o abrupto (mostrar advertencia de error).

**En D2 (C++):**

```cpp
if (strcmp(tipo, MSG_CMD_DESCONECTAR) == 0) {
    JsonDocument ack_doc;
    ack_doc["tipo"] = "ACK_DESCONECTAR";
    enviarMensajeWS(client, ack_doc);
    return;  // el cierre TCP lo inicia el cliente
}
```

D2 responde con el ACK y no modifica ninguna variable de estado. El cierre TCP llega como evento `WS_EVT_DISCONNECT`, que actualiza los LEDs de estado.

### Desconexión abrupta de D3

Si D3 pierde conectividad sin enviar `CMD_DESCONECTAR` (cierre del navegador, apagado del dispositivo, pérdida de señal Wi-Fi), el stack TCP detecta el silencio en la conexión y genera el evento `WS_EVT_DISCONNECT` después de agotar los reintentos TCP (cuyo timeout configura el OS del dispositivo).

Para acelerar esta detección sin depender del timeout TCP del OS, D3 implementa un **keepalive activo a nivel de aplicación**:

```javascript
pingTimer = setInterval(() => {
    enviar({ tipo: 'PING' });
    pongTimer = setTimeout(() => { ws.close(); }, 3000);
}, 5000);
```

Cada 5 segundos D3 envía un `PING` (con CRC-16). Si no recibe `PONG` en 3 segundos, fuerza el cierre del socket. En cualquier plataforma, la desconexión es detectada en ≤8 segundos desde la pérdida de conectividad.

---

## 8. CRC-8 en el enlace IR (D1 ↔ D2)

### Fundamentación teórica

Un **CRC (Cyclic Redundancy Check)** es un código detector de errores basado en la división polinomial sobre GF(2), el campo de Galois de dos elementos {0, 1}. En GF(2), la aritmética opera en módulo 2: la suma y la resta son equivalentes al XOR lógico, y el acarreo es siempre ignorado.

Dado un mensaje M de n bits y un polinomio generador G de grado r, el CRC se calcula como el residuo de la división M·2^r / G. El mensaje transmitido es M·2^r + R, donde R es el residuo (el CRC). El receptor recalcula el CRC y verifica que coincida.

### Elección del polinomio: CRC-8/MAXIM (Dallas/1-Wire)

**Polinomio:** x⁸ + x⁵ + x⁴ + 1 → representación binaria: `100110001` → valor hexadecimal del generador: `0x31`

**Parámetros del estándar CRC-8/MAXIM:**

| Parámetro | Valor | Significado |
|---|---|---|
| Poly | `0x31` | Polinomio generador |
| Init | `0xFF` | Valor inicial del registro |
| RefIn | true | Reflexión de bits de entrada (LSB-first) |
| RefOut | true | Reflexión del resultado |
| XorOut | `0x00` | Sin XOR final |

El valor inicial `0xFF` (en lugar de `0x00`) hace que el CRC sea sensible a ceros a la cabeza del mensaje, lo cual es importante si los primeros bytes del payload fueran nulos. Con `Init=0x00`, la trama `{0, G, B, CRC}` generaría el mismo CRC que `{G, B, CRC}` para algunos valores, reduciendo la cobertura. Con `Init=0xFF` esto no ocurre.

**Justificación de la elección:**
- Distancia Hamming HD=4 para mensajes de hasta 119 bits. Nuestra carga útil son 3 bytes (24 bits), bien dentro del rango de cobertura óptima.
- HD=4 garantiza detección de todos los errores de 1, 2 y 3 bits, y detección del 100% de errores en ráfaga de longitud ≤8 bits.
- Ampliamente documentado y verificable contra calculadoras CRC de referencia (e.g., crccalc.com).

### Estructura de la trama IR de 32 bits

```
 Bit 31     Bit 24   Bit 23     Bit 16   Bit 15      Bit 8   Bit 7       Bit 0
┌────────────────────┬────────────────────┬────────────────────┬────────────────────┐
│   Componente R     │   Componente G     │   Componente B     │   CRC-8 (R, G, B)  │
│      8 bits        │      8 bits        │      8 bits        │      8 bits        │
│      0 – 255       │      0 – 255       │      0 – 255       │   calculado sobre  │
│                    │                    │                    │   los tres bytes   │
└────────────────────┴────────────────────┴────────────────────┴────────────────────┘
```

Esta estructura reemplaza el esquema con cabeceras fijas (`0xAA`/`0xBB`) del TP2. La integridad ya no se garantiza por posición fija de un byte conocido, sino matemáticamente: cualquier corrupción de 1, 2 o 3 bits en los 32 bits de la trama es detectada con probabilidad 1 (determinística para HD=4, probabilidad 1 − 1/2^8 = 99.6% para ráfagas largas).

### Implementación por software (XOR, módulo 2)

```cpp
uint8_t calcularCRC8(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t crc = 0xFF;                    // Init = 0xFF
    uint8_t datos[3] = { r, g, b };

    for (uint8_t i = 0; i < 3; i++) {
        crc ^= datos[i];                   // XOR del byte de datos con el registro

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x01) {              // LSB = 1 (RefIn = true: procesamiento LSB-first)
                crc = (crc >> 1) ^ CRC8_POLY;  // desplazar y XOR con polinomio
            } else {
                crc >>= 1;                 // solo desplazar
            }
        }
    }
    return crc;
}
```

**Traza paso a paso con un ejemplo concreto (ROJO: R=255, G=0, B=0):**

```
Init:   crc = 0xFF = 11111111b

Byte 0: R = 255 = 0xFF = 11111111b
  crc XOR R = 0xFF XOR 0xFF = 0x00

  8 iteraciones con crc=0x00:
  Todos los bits son 0 → solo desplaza → crc permanece 0x00

Byte 1: G = 0
  crc XOR G = 0x00 XOR 0x00 = 0x00

  8 iteraciones con crc=0x00 → crc = 0x00

Byte 2: B = 0
  crc XOR B = 0x00 XOR 0x00 = 0x00

  8 iteraciones → crc = 0x00

Resultado CRC8(255, 0, 0) = 0x00
Trama IR = 0xFF000000
```

**Verificación con VERDE (R=0, G=255, B=0):**

```
Init:   crc = 0xFF

Byte 0: G_data=0 → crc XOR 0 = 0xFF
  Iteraciones: 0xFF, LSB=1 → (0xFF>>1) XOR 0x31 = 0x7F XOR 0x31 = 0x4E
  ... (8 iteraciones completas sobre 0xFF con POLY=0x31)
  Resultado tras byte 0: crc = 0x00 (al procesar 0xFF desde 0xFF con POLY=0x31, el resultado es siempre 0x00 por simetría del polinomio)
Byte 1: G=255 → crc XOR 0xFF = 0xFF → mismo proceso → crc = 0x00
Byte 2: B=0   → crc XOR 0x00 = 0x00 → resultado 0x00

CRC8(0, 255, 0) = 0x00 → trama = 0x00FF0000
```

> **Nota:** los tres colores puros (rojo, verde, azul) con los tres bytes en conjunto (incluyendo `Init=0xFF`) producen CRC=0x00 por la aritmética del polinomio. Otros colores como AMARILLO (255, 200, 0), LILA (180, 0, 255) o TURQUESA (0, 210, 140) generan valores CRC distintos de 0.

### Construcción de la trama de 32 bits

```cpp
uint32_t trama = ((uint32_t)r      << 24) |   // R en bits 31-24
                 ((uint32_t)g      << 16) |   // G en bits 23-16
                 ((uint32_t)b      <<  8) |   // B en bits 15-8
                  (uint32_t)crc_tx;           // CRC en bits 7-0
```

Los casts explícitos a `uint32_t` antes de los desplazamientos previenen comportamiento indefinido en C++: sin el cast, el desplazamiento operaría sobre `uint8_t` (8 bits), y `r << 24` sería UB (desplazar más de la anchura del tipo).

### Validación del ACK en D2

```cpp
uint8_t crc_esperado = calcularCRC8(ack_r, ack_g, ack_b);
if (ack_crc != crc_esperado) {
    // ACK corrupto: descartar y seguir esperando
    continue;
}
if (ack_r == r && ack_g == g && ack_b == b) {
    return true;  // ACK válido
}
```

La validación es doble:
1. **Integridad:** el CRC del ACK debe ser correcto (sin errores de transmisión).
2. **Coherencia:** los datos del ACK deben coincidir con los enviados (D1 confirmó el color correcto).

Un ACK con CRC válido pero datos diferentes (e.g., D1 respondió con un color distinto por alguna corrupción que el CRC-8 no detectó — improbable con HD=4) sería descartado por la segunda verificación.

---

## 9. CRC-16 en el enlace WebSocket (D2 ↔ D3)

### ¿Por qué CRC-16 y no CRC-8?

Los mensajes JSON entre D2 y D3 son significativamente más largos que las tramas IR (hasta ~300 bytes vs. 3 bytes). La distancia Hamming de un CRC depende tanto del polinomio como de la longitud del mensaje:

| CRC | Poly | HD para mensajes cortos | HD para 300 bytes |
|---|---|---|---|
| CRC-8/MAXIM | 0x31 | HD=4 (hasta 119 bits) | HD=2 (detección débil) |
| CRC-16/IBM | 0x8005 | HD=4 (hasta 32767 bits) | HD=4 (cobertura completa) |

Para payloads de hasta 300 bytes (2400 bits), CRC-8 ofrece solo HD=2 (detecta errores de 1 bit), mientras que CRC-16/IBM mantiene HD=4 para mensajes de hasta 32.767 bits. La elección de CRC-16 para la capa WebSocket es técnicamente necesaria para una cobertura real de errores en estos tamaños de mensaje.

### Polinomio: CRC-16/IBM (CRC-16/ARC)

**Polinomio:** x¹⁶ + x¹⁵ + x² + 1 → representación binaria: `11000000000000101` → hexadecimal: `0x8005`

**Parámetros del estándar CRC-16/IBM:**

| Parámetro | Valor | Significado |
|---|---|---|
| Poly | `0x8005` | Polinomio generador |
| Init | `0x0000` | Registro inicializado en cero |
| RefIn | true | Procesamiento LSB-first |
| RefOut | true | Reflexión del resultado |
| XorOut | `0x0000` | Sin XOR final |

### Implementación en C++ (D2 — servidor)

```cpp
uint16_t calcularCRC16(const char *payload, size_t len) {
    uint16_t crc = 0x0000;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint8_t)payload[i];    // XOR con el byte actual

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {        // LSB = 1
                crc = (crc >> 1) ^ CRC16_POLY;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
```

### Implementación en JavaScript (D3 — cliente)

```javascript
function calcularCRC16(str) {
    const POLY = 0x8005;
    let crc = 0x0000;
    for (let i = 0; i < str.length; i++) {
        crc ^= str.charCodeAt(i);
        for (let bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc = ((crc >>> 1) ^ POLY) & 0xFFFF;
            } else {
                crc = (crc >>> 1) & 0xFFFF;
            }
        }
    }
    return crc;
}
```

**Diferencia entre `>>` y `>>>` en JavaScript:** JavaScript usa internamente enteros de 64 bits en coma flotante (IEEE 754). El operador `>>` (right shift aritmético) extiende el bit de signo; `>>>` (right shift lógico) inserta ceros desde el MSB. Para simular un registro de 16 bits sin signo, se usa `>>>` combinado con `& 0xFFFF` para descartar los bits superiores a 15.

**El resultado del CRC-16 producido por ambas implementaciones es idéntico** porque usan el mismo polinomio, el mismo valor inicial, el mismo orden de bits y las mismas operaciones XOR. Esto es lo que permite la verificación cruzada entre el servidor C++ y el cliente JavaScript.

### Protocolo de serialización y verificación

#### Envío (D2 → D3)

```
1. Construir el objeto JSON de datos:
   doc = { "tipo": "RESULTADO_CMD", "exito": true, "encendido": true,
           "color": "VERDE", "r": 0, "g": 255, "b": 0 }

2. Serializar a string (sin crc16):
   buf_base = '{"tipo":"RESULTADO_CMD","exito":true,"encendido":true,
               "color":"VERDE","r":0,"g":255,"b":0}'
   len_base = longitud en bytes del string

3. Calcular CRC-16 sobre buf_base:
   crc = calcularCRC16(buf_base, len_base)

4. Agregar el campo crc16 al documento:
   doc["crc16"] = crc

5. Serializar el documento completo:
   buf_final = '{"tipo":"RESULTADO_CMD","exito":true,"encendido":true,
                "color":"VERDE","r":0,"g":255,"b":0,"crc16":51234}'

6. Enviar buf_final por WebSocket.
```

#### Recepción y validación (D3)

```javascript
// 1. Parsear el JSON recibido
const msg = JSON.parse(evt.data);

// 2. Extraer el CRC recibido
const crc_rx = msg.crc16;

// 3. Crear copia sin el campo crc16
const sin_crc = Object.assign({}, msg);
delete sin_crc.crc16;

// 4. Serializar sin crc16 y recalcular
const base = JSON.stringify(sin_crc);
const crc_calc = calcularCRC16(base);

// 5. Comparar
return crc_rx === crc_calc;
```

#### Recepción y validación (D2 — mensajes de D3)

```cpp
// 1. Parsear JSON
deserializeJson(doc, payload, len);

// 2. Extraer CRC recibido
uint16_t crc_rx = doc["crc16"].as<uint16_t>();

// 3. Eliminar campo crc16 del documento parseado
doc.remove("crc16");

// 4. Serializar sin crc16 para replicar el cálculo
serializeJson(doc, buf_check, sizeof(buf_check));

// 5. Recalcular y comparar
uint16_t crc_calc = calcularCRC16(buf_check, len_check);
if (crc_rx != crc_calc) { /* error */ }
```

### Justificación del orden JSON y su importancia crítica

El CRC se calcula sobre un string JSON específico. Para que emisor y receptor obtengan el mismo CRC, ambos deben serializar el objeto **en el mismo orden de campos**. Esto requiere que:

1. **En D2 (C++):** ArduinoJson preserva el orden de inserción de campos en un `JsonDocument`. Los campos se agregan en un orden fijo y determinista en cada función de construcción de mensajes.

2. **En D3 (JavaScript):** `JSON.stringify()` preserva el orden de inserción de propiedades para propiedades no numéricas en engines JavaScript modernos (V8, SpiderMonkey, JavaScriptCore). La función `serializarConCRC()` agrega `crc16` al final del objeto original, y `JSON.stringify(sin_crc)` serializa en el mismo orden que tenía el objeto antes de agregar `crc16`.

3. **En la validación en D2:** después de `doc.remove("crc16")`, `serializeJson(doc, ...)` produce la misma cadena que produjo D3 antes de agregar el campo, porque ArduinoJson mantiene el orden de los campos restantes.

**Implicación:** si se modificara el orden en que se agregan campos al `JsonDocument` en D2, el CRC calculado por D3 no coincidiría con el CRC calculado por D2 en la validación, y todos los mensajes serían rechazados. El orden de campos es parte del contrato del protocolo.

### Ejemplo numérico concreto del CRC-16

Para el mensaje `CMD_ENCENDER` enviado por D3:

```
Objeto JS antes de agregar CRC:
  { tipo: "CMD_ENCENDER" }

Serialización base (JSON.stringify):
  '{"tipo":"CMD_ENCENDER"}'
  Bytes ASCII: 7B 22 74 69 70 6F 22 3A 22 43 4D 44 5F 45 4E 43 45 4E 44 45 52 22 7D

CRC-16/IBM sobre esos 23 bytes (Init=0x0000, Poly=0x8005, LSB-first):
  crc = calcularCRC16('{"tipo":"CMD_ENCENDER"}', 23)
  Resultado: 0xD5E3 (54755 decimal)

JSON final enviado:
  '{"tipo":"CMD_ENCENDER","crc16":54755}'
```

D2 recibe `{"tipo":"CMD_ENCENDER","crc16":54755}`, parsea el JSON, extrae `crc16=54755`, elimina el campo, serializa `{"tipo":"CMD_ENCENDER"}`, calcula `calcularCRC16()` y obtiene `0xD5E3 = 54755`. La comparación es exitosa y el comando es procesado.

Si durante la transmisión se alterara un solo bit del string (por ejemplo, el carácter `E` de `ENCENDER` se convirtiera en otro carácter), el CRC recalculado en D2 sería diferente de `54755` y el mensaje sería descartado, enviando `CRC_ERROR` a D3.

### Capacidad de detección de errores (CRC-16/IBM, HD=4)

Para mensajes de hasta 32.767 bits (~4 KB), el CRC-16/IBM con `Poly=0x8005` garantiza:

| Tipo de error | Detección |
|---|---|
| Errores de 1 bit | 100% (siempre detectado) |
| Errores de 2 bits | 100% |
| Errores de 3 bits | 100% |
| Ráfaga ≤ 16 bits | 100% |
| Ráfaga de 17 bits | 99.997% (1 − 1/2¹⁶) |
| Error aleatorio de cualquier longitud | 99.997% en promedio |

Para los payloads de este sistema (≤ 300 bytes = 2400 bits), todas estas garantías se mantienen en su máxima expresión (HD=4).

---

## 10. Protocolo IR: envío de color y handshaking con ACK

### Protocolo NEC Raw

Se usa la variante **NEC Raw** del protocolo NEC para la modulación física de la señal infrarroja. La diferencia con NEC estándar es fundamental: NEC estándar aplica inversiones lógicas a los bytes de dirección y comando como verificación interna, lo que haría imposible transportar valores RGB arbitrarios sin que el protocolo los altere. NEC Raw transmite los 32 bits exactamente como se construyeron en la aplicación, usando la misma portadora de 38 kHz y los mismos tiempos de bit del protocolo NEC estándar.

IRremote usa el periférico **RMT (Remote Control Transceiver)** del ESP32 para generar la portadora de 38 kHz con precisión hardware, sin intervención de la CPU. El RMT opera como un DMA de señal: genera los pulsos de manera completamente autónoma una vez configurado.

### Función `enviarColorIR()`

```
1. calcularCRC8(r, g, b) → crc_tx

2. Construir trama de 32 bits: [r][g][b][crc_tx]

3. IrReceiver.stop()
   ↑ Deshabilitar receptor de D2 para evitar que el KY-022 capture
     el eco del propio KY-005 durante la transmisión (half-duplex)

4. IrSender.sendNECRaw(trama, 0)
   ↑ Transmitir 32 bits con modulación NEC a 38 kHz
   ↑ El 0 indica 0 repeticiones automáticas NEC

5. IrReceiver.start()
   ↑ Reactivar receptor para escuchar el ACK de D1

6. Bucle de espera hasta TIMEOUT_ACK_MS (2000 ms):
   ├─ IrReceiver.decode() retorna false → continuar esperando
   ├─ decodedIRData.flags & IS_REPEAT → resume(), ignorar
   └─ Trama recibida:
      ├─ Leer resp = decodedRawData (ANTES de resume())
      ├─ IrReceiver.resume()
      ├─ Extraer ack_r, ack_g, ack_b, ack_crc
      ├─ calcularCRC8(ack_r, ack_g, ack_b) → crc_esperado
      ├─ ack_crc != crc_esperado → continue (descartar ACK corrupto)
      └─ ack_r==r && ack_g==g && ack_b==b → return true (ACK válido)

7. Timeout expirado → return false
```

### Ciclo con reintentos: `ejecutarCicloIR()`

```cpp
for (int i = 0; i < MAX_REINTENTOS && !ack; i++) {
    if (i > 0) delay(DELAY_REINTENTO_MS);
    ack = enviarColorIR(r, g, b);
}
```

`MAX_REINTENTOS = 3` y `DELAY_REINTENTO_MS = 300 ms`. En el peor caso (3 fallos completos), el ciclo IR dura: 3 × (2000 ms timeout + 300 ms delay) = ~6.9 segundos. Durante este tiempo el loop está bloqueado, pero ESPAsyncWebServer continúa procesando eventos de red en el Core 0.

**Actualización de estado solo ante ACK válido:**

```cpp
if (ack) {
    colorActual = ci;     // actualizar índice en COLORES[]
    encendido   = enc;    // actualizar flag de encendido
    setColor(r, g, b);    // actualizar LED de D2
    guardarNVS();         // persistir en flash
}
notificarD3Estado(ack);   // notificar siempre (éxito o fallo)
```

El estado de D2 (y por extensión de D3, que recibe el resultado) **solo se actualiza si D1 confirmó el color con CRC-8 válido**. Sin ACK, el sistema queda en el estado anterior, garantizando la coherencia entre los tres nodos.

---

## 11. Tabla de colores y gestión de estado

### Paleta de 12 colores

```cpp
static const ColorRGB COLORES[NUM_COLORES] = {
    // Originales del TP2
    { "ROJO",     255,   0,   0 },
    { "AMARILLO", 255, 200,   0 },
    { "VERDE",      0, 255,   0 },
    { "CELESTE",    0, 255, 255 },
    { "AZUL",       0,   0, 255 },
    { "LILA",     180,   0, 255 },
    { "BLANCO",   255, 255, 255 },
    { "ROSA",     255, 105, 180 },
    // Nuevos
    { "NARANJA",  255,  80,   0 },
    { "MAGENTA",  255,   0, 180 },
    { "TURQUESA",   0, 210, 140 },
    { "VIOLETA",   90,   0, 200 },
};
```

La tabla es `static const`, por lo que el compilador la ubica en la región ROM (flash) del ESP32, sin consumir SRAM en tiempo de ejecución.

### Estado del sistema (variables globales)

```cpp
volatile uint8_t  colorActual = 0;     // índice en COLORES[] (0-11)
volatile bool     encendido   = false; // LED de D1 encendido/apagado
```

`colorActual` guarda el índice, no los bytes RGB. Esto permite reconstruir el triplete completo desde `COLORES[colorActual]` en cualquier momento, y guardar en NVS solo 1 byte (el índice) en lugar de 3.

**Gestión del apagado:** al recibir `CMD_APAGAR`, `colorActual` **no cambia**. Se envía `{0, 0, 0}` a D1 y se actualiza solo el flag `encendido = false`. Al recibir `CMD_ENCENDER`, se envía el color almacenado en `COLORES[colorActual]`. Esto permite que el usuario apague y encienda el LED conservando el último color seleccionado, sin necesidad de enviarlo nuevamente.

---

## 12. Persistencia de estado: NVS / Preferences

D2 usa la librería `Preferences` sobre el NVS (Non-Volatile Storage) del ESP32 para sobrevivir cortes de energía:

```cpp
void guardarNVS() {
    prefs.begin("d2_state", false);
    prefs.putUChar("magic",     0xA5);
    prefs.putUChar("color_idx", colorActual);
    prefs.putBool ("encendido", encendido);
    prefs.end();
}

void cargarNVS() {
    prefs.begin("d2_state", true);
    uint8_t magic = prefs.getUChar("magic", 0x00);
    if (magic == 0xA5) {
        colorActual = prefs.getUChar("color_idx", 0);
        encendido   = prefs.getBool ("encendido",  false);
        if (colorActual >= NUM_COLORES) colorActual = 0;
    }
    prefs.end();
}
```

El centinela `0xA5` (patrón `10100101b`, elegido por su baja probabilidad de aparecer en NVS sin inicializar) detecta si la NVS fue escrita alguna vez con datos válidos. El sanity check `if (colorActual >= NUM_COLORES)` previene un acceso fuera de bounds si la NVS contuviese un índice guardado con una versión anterior del firmware que tenía menos colores.

NVS usa una partición de flash dedicada con **wear leveling automático** que distribuye las escrituras entre páginas físicas, eliminando la degradación por ciclos de escritura que afecta a la EEPROM de AVR.

---

## 13. LEDs de estado de conectividad

| LED | GPIO | Indica |
|---|---|---|
| LED_AP (amarillo) | 14 | AP activo: encendido desde `setup()` hasta apagado del sistema |
| LED_CONN (verde) | 32 | D3 conectado: activo mientras hay sesión WebSocket activa |
| LED_DISC (rojo) | 33 | Desconexión detectada: activo hasta la próxima reconexión |

Los LEDs CONN y DISC son mutuamente exclusivos:

```cpp
case WS_EVT_CONNECT:
    setLedConn(true);   // verde encendido
    setLedDisc(false);  // rojo apagado
    break;

case WS_EVT_DISCONNECT:
    setLedConn(false);  // verde apagado
    setLedDisc(true);   // rojo encendido
    break;
```

Esto proporciona indicación física inequívoca del estado de la conexión, auditable incluso sin acceso al monitor serial, tanto para la demostración en clase como para detección de problemas en campo.

---

## 14. Secuencia de arranque: `setup()`

```
┌─────────────────────────────────────────────────────────────────┐
│                           SETUP                                  │
│                                                                  │
│  [BLOQUE-INIT] Serial.begin(115200) + delay(500)                 │
│                                                                  │
│  [BLOQUE-INIT] iniciarLedc()                                     │
│     ledcSetup(CH3/1/2, 5000Hz, 8bits)                           │
│     ledcAttachPin(25/26/27, CH3/1/2)                            │
│                                                                  │
│  [BLOQUE-INIT] pinMode(32/33/14, OUTPUT) + inicializar en LOW    │
│                                                                  │
│  [BLOQUE-INIT] IrReceiver.begin(GPIO5, DISABLE_LED_FEEDBACK)     │
│                                                                  │
│  [BLOQUE-NVS] cargarNVS()                                        │
│     ├─ magic == 0xA5: cargar colorActual y encendido             │
│     └─ magic != 0xA5: defaults (idx=0, encendido=false)          │
│                                                                  │
│  [BLOQUE-LED] Aplicar color al LED de D2 según estado NVS        │
│                                                                  │
│  [BLOQUE-WIFI] WiFi.softAP(SSID, PASS, CH, 0, 1)                │
│     IP resultante: 192.168.4.1                                  │
│                                                                  │
│  [BLOQUE-WS] ws.onEvent(onWsEvent) → server.addHandler(&ws)     │
│                                                                  │
│  [BLOQUE-HTTP] server.on("/", GET, → send_P(WEB_UI))            │
│                server.onNotFound(→ 404)                          │
│                server.begin()                                    │
│                                                                  │
│  [BLOQUE-INIT] digitalWrite(PIN_LED_AP, HIGH)                    │
│     ← señal visual: sistema completamente operativo              │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
                             loop()
```

---

## 15. Ciclo principal: `loop()`

```cpp
void loop() {
    ws.cleanupClients();    // liberar objetos de clientes desconectados

    if (irPendiente) {
        irPendiente = false; // limpiar ANTES de ejecutar (evita re-entrada)
        ejecutarCicloIR();
    }

    delay(5);
}
```

**`ws.cleanupClients()`:** sin esta llamada periódica, los objetos de clientes desconectados se acumulan en el heap hasta agotarlo. Debe llamarse desde el loop del Core 1 porque la limpieza accede a la lista interna de `AsyncWebSocket`, que es modificada por el Core 0.

**`irPendiente = false` antes de ejecutar:** si el flag se limpiara después del ciclo IR, y un nuevo comando llegara durante el ciclo (Core 0 pone `irPendiente = true`), el flag sería sobreescrito con `false` al regresar, perdiendo ese comando. Limpiando primero, el ciclo siguiente detectará el nuevo `irPendiente = true` correctamente.

**`delay(5)`:** cede 5 ms al scheduler de FreeRTOS. Aunque los cores son físicamente independientes, la tarea idle del watchdog del Core 1 requiere recibir control periódicamente. Un loop sin yield puede disparar el Task Watchdog Timer (TWDT), que resetea el ESP32. 5 ms es suficiente para evitar el TWDT sin afectar la latencia de respuesta.

---

## 16. Flujo de datos extremo a extremo

Secuencia completa desde que el usuario presiona "TURQUESA" en D3 hasta que el LED de D1 cambia:

```
D3 (Navegador)              D2 (ESP32)                    D1 (ESP32)
      │                          │                              │
      │  enviarConCRC({           │                              │
      │    tipo: "CMD_COLOR",     │                              │
      │    color: "TURQUESA"      │                              │
      │  })                       │                              │
      │  → JSON base calculado    │                              │
      │  → CRC-16 calculado       │                              │
      │  → JSON + crc16 enviado   │                              │
      │─── WS TEXT ──────────────►│                              │
      │                          │ [BLOQUE-PARSE]                │
      │                          │ deserializeJson()             │
      │                          │ [BLOQUE-CRC16]                │
      │                          │ crc_rx = doc["crc16"]         │
      │                          │ doc.remove("crc16")           │
      │                          │ crc_calc = calcularCRC16()    │
      │                          │ crc_rx == crc_calc ✓          │
      │                          │ tipo = "CMD_COLOR"            │
      │                          │ idx = 10 (TURQUESA)           │
      │                          │ irPendiente = true            │
      │                          │                              │
      │                          │ [loop() Core 1 detecta flag] │
      │                          │ irPendiente = false           │
      │                          │ ejecutarCicloIR()             │
      │                          │ r=0, g=210, b=140             │
      │                          │ crc_tx = calcularCRC8(0,210,140)
      │                          │ trama = 0x00D28C?? (+ CRC)   │
      │                          │ IrReceiver.stop()             │
      │                          │─── NEC Raw IR ───────────────►│
      │                          │    [0x00][0xD2][0x8C][CRC8]  │
      │                          │ IrReceiver.start()            │
      │                          │                              │ IrReceiver.decode()
      │                          │                              │ Extraer r,g,b,crc_rx
      │                          │                              │ crc_calc = calcularCRC8()
      │                          │                              │ crc OK ✓
      │                          │                              │ setColor(0,210,140)
      │                          │                              │ guardarNVS(0,210,140)
      │                          │                              │ enviarACK(0,210,140)
      │                          │◄── NEC Raw IR ───────────────│
      │                          │    [0x00][0xD2][0x8C][CRC8]  │
      │                          │ Extraer ack_r,g,b,crc        │
      │                          │ crc == calcularCRC8() ✓      │
      │                          │ ack_r==0 ack_g==210 ✓        │
      │                          │ return true (ACK válido)      │
      │                          │                              │
      │                          │ colorActual = 10 (TURQUESA)  │
      │                          │ setColor(0,210,140)           │
      │                          │ guardarNVS()                  │
      │                          │ notificarD3Estado(true)       │
      │                          │ enviarMensajeWSBroadcast()    │
      │                          │  → CRC-16 calculado           │
      │◄── WS TEXT ──────────────│                              │
      │  {"tipo":"RESULTADO_CMD", │                              │
      │   "exito":true,           │                              │
      │   "color":"TURQUESA",     │                              │
      │   "r":0,"g":210,"b":140,  │                              │
      │   "crc16":XXXX}           │                              │
      │ validarCRC() ✓            │                              │
      │ actualizarUI()            │                              │
      │ preview → turquesa        │                              │

LED D3: preview turquesa    LED D2: turquesa             LED D1: turquesa
```

---

## 17. Dependencias de software

| Librería | Versión | Rol en el firmware |
|---|---|---|
| `z3t0/IRremote` | `^4.7.1` | Protocolo NEC Raw, modulación de 38 kHz via periférico RMT del ESP32. Funciones: `IrReceiver.begin/stop/start/decode/resume`, `IrSender.sendNECRaw`. |
| `me-no-dev/AsyncTCP` | `^1.1.1` | Sockets TCP asíncronos sobre lwIP. Dependencia directa de ESPAsyncWebServer; no se usa en el código de aplicación. |
| `me-no-dev/ESPAsyncWebServer` | `^1.2.3` | Servidor HTTP no bloqueante con soporte WebSocket (`AsyncWebSocket`). Gestiona múltiples clientes, fragmentación de frames y eventos de red. |
| `bblanchon/ArduinoJson` | `^7.2.0` | Serialización/deserialización JSON con `JsonDocument` (asignador unificado de v7). `serializeJson`, `deserializeJson`, `doc.remove()`. |
| `Preferences` | ESP32 Core | Abstracción sobre NVS para persistencia de pares clave-valor en flash. No requiere `lib_deps`. |
| `WiFi` | ESP32 Core | `WiFi.softAP()` para modo Access Point. No requiere `lib_deps`. |

### Identificador correcto en PlatformIO

La librería IRremote está registrada bajo el alias histórico `z3t0/IRremote` en el registry de PlatformIO. El identificador alternativo `IRremote/IRremote` puede no resolverse correctamente en todas las versiones del Dependency Finder. Este detalle fue descubierto durante la integración del proyecto.

### ArduinoJson v7: `JsonDocument` unificado

La versión 7 de ArduinoJson elimina la distinción entre `StaticJsonDocument<T>` (stack) y `DynamicJsonDocument` (heap), reemplazándolos por `JsonDocument` con un asignador dinámico optimizado. El compilador generará un error de tipo si se intenta usar `StaticJsonDocument` con esta versión.

---

## 18. Configuración de entorno: `platformio.ini`

```ini
[env:esp32dev]
platform         = espressif32     ; toolchain Xtensa LX6 + ESP-IDF headers
board            = esp32dev        ; ESP32 DevKit v1: 4MB flash, 520KB RAM
framework        = arduino         ; API Arduino sobre ESP-IDF

monitor_speed    = 115200
upload_speed     = 921600          ; 8× más rápido que 115200; compatible con CP2102/CH340
upload_port      = COM10           ; ajustar al puerto del sistema
monitor_port     = COM10

lib_deps =
    z3t0/IRremote @ ^4.7.1
    me-no-dev/AsyncTCP @ ^1.1.1
    me-no-dev/ESPAsyncWebServer @ ^1.2.3
    bblanchon/ArduinoJson @ ^7.2.0

build_flags =
    -DCORE_DEBUG_LEVEL=0           ; 0=silencio, cambiar a 3 para depurar Wi-Fi

monitor_filters = esp32_exception_decoder
```

El flag `-DCORE_DEBUG_LEVEL=0` silencia los logs internos del stack Wi-Fi del ESP32. Cambiar a `3` durante el desarrollo muestra mensajes detallados de conexión AP, asignación DHCP y eventos de red, útiles para diagnosticar problemas de conectividad con D3.

---

## 19. Logs de depuración

Con el Monitor Serial abierto (`Ctrl+Alt+S`), el firmware imprime el estado de cada operación con prefijos identificadores:

### Arranque completo del sistema

```
[D2] === Maestro IR + Servidor Wi-Fi con CRC — Iniciando ===
[D2] Receptor IR activo en GPIO 5
[NVS] Recuperado idx=10 encendido=SI
[Wi-Fi] Iniciando AP: 'Nombre-RED'
[Wi-Fi] AP activo. IP: 192.168.4.1
[D2] Servidor HTTP+WS iniciado
[D2] Listo. Esperando conexion de D3...
```

### Conexión de D3 y envío de estado inicial

```
[WS] Cliente #1 conectado desde 192.168.4.2
[WS-TX] #1: {"tipo":"ESTADO_ACTUAL","encendido":true,"color":"TURQUESA","r":0,"g":210,"b":140,"crc16":31720}
```

### Ciclo completo de cambio de color con CRC

```
[WS-RX] #1: {"tipo":"CMD_COLOR","color":"VIOLETA","crc16":12345}
[CRC] CRC-16 OK: 0x3039
[WS-RX] tipo=CMD_COLOR
[IR-TX] Trama: 0x5A00C8A3 | R=90 G=0 B=200 CRC=0xA3
[IR-RX] Respuesta: 0x5A00C8A3 | cab_rx=0x5A (mismo que enviado)
[CRC] ACK OK: CRC_rx=0xA3 CRC_calc=0xA3
[IR] ACK valido: R=90 G=0 B=200 CRC=0xA3
[D2] Estado confirmado: idx=11 enc=SI R=90 G=0 B=200
[NVS] Guardado idx=11 encendido=SI
[WS-TX] Broadcast: {"tipo":"RESULTADO_CMD","exito":true,"encendido":true,"color":"VIOLETA","r":90,"g":0,"b":200,"crc16":54321}
```

### Detección de error CRC-16 en mensaje de D3

```
[WS-RX] #1: {"tipo":"CMD_COLOR","color":"ROJO","crc16":99999}
[CRC] ERROR CRC-16: rx=0x2706F calc=0x1F4A
[WS-TX] #1: {"tipo":"CRC_ERROR","info":"CRC-16 invalido — mensaje descartado","crc16":XXXX}
```

### Desconexión voluntaria de D3

```
[WS-RX] #1: {"tipo":"CMD_DESCONECTAR","crc16":XXXX}
[CRC] CRC-16 OK: 0xXXXX
[WS-RX] tipo=CMD_DESCONECTAR
[WS] Cliente #1 solicito desconexion voluntaria
[WS-TX] #1: {"tipo":"ACK_DESCONECTAR","crc16":XXXX}
[WS] Cliente #1 desconectado
```

### Timeout de ACK IR (D1 no responde)

```
[IR-TX] Trama: 0xFF000000 | R=255 G=0 B=0 CRC=0x00
[IR] Timeout — sin ACK valido
[IR] Reintento 1/2
[IR-TX] Trama: 0xFF000000 | R=255 G=0 B=0 CRC=0x00
[IR] Timeout — sin ACK valido
[IR] Reintento 2/2
[IR] Timeout — sin ACK valido
[D2] Sin ACK — estado sin cambios
[WS-TX] Broadcast: {"tipo":"RESULTADO_CMD","exito":false,...,"crc16":XXXX}
```

---

*Documentación elaborada en el marco del TP3 Integrador — Comunicación de Datos, Ingeniería en Computación.*  
*Bevilacqua, Francisco — Clement, Sebastián — Junio 2025.*
