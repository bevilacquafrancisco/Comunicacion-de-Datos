# Dispositivo 2 — Maestro IR + Servidor Wi-Fi

> **TP3 Integrador — Comunicación de Datos**  
> Ingeniería en Computación | Bevilacqua, Francisco — Clement, Sebastián | 8 de junio 2026  
> Plataforma: ESP32 DevKit v1 | Framework: Arduino sobre ESP-IDF | Entorno: PlatformIO

---

## Índice

1. [Rol del Dispositivo 2 en el sistema](#1-rol-del-dispositivo-2-en-el-sistema)
2. [Arquitectura del firmware](#2-arquitectura-del-firmware)
3. [Estructura de archivos](#3-estructura-de-archivos)
4. [Configuración centralizada — `config.h`](#4-configuración-centralizada--configh)
5. [Red Wi-Fi — Access Point del ESP32](#5-red-wi-fi--access-point-del-esp32)
6. [Servidor HTTP — ESPAsyncWebServer](#6-servidor-http--espasyncwebserver)
7. [Comunicación en tiempo real — WebSocket](#7-comunicación-en-tiempo-real--websocket)
8. [Protocolo de mensajes D2 ↔ D3 (JSON sobre WebSocket)](#8-protocolo-de-mensajes-d2--d3-json-sobre-websocket)
9. [Keepalive — Detección de desconexión real](#9-keepalive--detección-de-desconexión-real)
10. [Comunicación infrarroja con D1 — Protocolo NEC Raw](#10-comunicación-infrarroja-con-d1--protocolo-nec-raw)
11. [Modelo de concurrencia — Dual-Core del ESP32](#11-modelo-de-concurrencia--dual-core-del-esp32)
12. [Persistencia de estado — NVS (Preferences)](#12-persistencia-de-estado--nvs-preferences)
13. [Control PWM del LED RGB — Módulo LEDC](#13-control-pwm-del-led-rgb--módulo-ledc)
14. [LEDs de estado de conectividad](#14-leds-de-estado-de-conectividad)
15. [Tabla de colores — Secuencia del TP2](#15-tabla-de-colores--secuencia-del-tp2)
16. [Flujo completo de un comando](#16-flujo-completo-de-un-comando)
17. [Diagrama de flujo del firmware](#17-diagrama-de-flujo-del-firmware)
18. [Gestión de dependencias con PlatformIO](#18-gestión-de-dependencias-con-platformio)
19. [Criterios de selección de pines](#19-criterios-de-selección-de-pines)
20. [Resumen de parámetros configurables](#20-resumen-de-parámetros-configurables)

---

## 1. Rol del Dispositivo 2 en el sistema

El Dispositivo 2 (D2) es el **nodo central** del sistema TP3. Opera simultáneamente en dos roles complementarios:

**Rol A — Maestro IR:** actúa como el controlador maestro de la comunicación infrarroja con el Dispositivo 1 (D1, Esclavo IR). Genera tramas de 32 bits en formato NEC Raw, las envía al D1 y gestiona un protocolo de handshake con reintentos para garantizar la entrega confiable de comandos de color.

**Rol B — Servidor Wi-Fi:** crea su propia red inalámbrica (Access Point WPA2), levanta un servidor HTTP que sirve la interfaz web del Dispositivo 3 (D3), y mantiene una conexión WebSocket persistente con D3 para recibir comandos y enviar actualizaciones de estado en tiempo real.

El D2 es el único nodo que conoce el estado global del sistema: recibe órdenes desde D3 vía Wi-Fi/WebSocket, las traduce a tramas IR y las envía a D1, luego reporta el resultado (éxito o fallo de ACK) de vuelta a D3.

```
D3 (celular/PC/tablet)
       │  Wi-Fi / WebSocket (JSON)
       ▼
    D2 — Maestro IR + Servidor Wi-Fi (ESP32)
       │  Infrarrojo NEC Raw 32 bits
       ▼
    D1 — Esclavo IR (ESP32)
       │  PWM / LEDC
       ▼
    LED RGB
```

---

## 2. Arquitectura del firmware

El firmware del D2 sigue una **arquitectura orientada a eventos** con dos contextos de ejecución diferenciados, aprovechando la naturaleza dual-core del ESP32:

| Contexto | Core ESP32 | Responsabilidad |
|---|---|---|
| Loop principal (`loop()`) | Core 1 (tarea Arduino) | Ejecución del ciclo IR, mantenimiento del servidor WS |
| Callbacks de red (`onWsEvent()`) | Core 0 (tarea lwIP/TCP) | Recepción de eventos WebSocket, parsing JSON |

Esta separación garantiza que las operaciones de red sean siempre responsivas, incluso durante la ejecución del ciclo IR (que puede tomar hasta ~6,9 s en el peor caso con 3 reintentos).

La comunicación entre ambos contextos se realiza mediante **variables volátiles** que actúan como flags atómicos, un patrón seguro para el ESP32 dado que la escritura/lectura de tipos primitivos (`bool`, `uint8_t`) es atómica en la arquitectura Xtensa LX6.

---

## 3. Estructura de archivos

```
D2_Maestro/
├── platformio.ini          # Configuración del entorno de compilación
├── include/
│   ├── config.h            # Constantes de hardware, red, protocolo y NVS
│   └── web_ui.h            # Interfaz HTML/CSS/JS del D3 embebida en flash
└── src/
    └── main.cpp            # Lógica principal del firmware
```

**`config.h`** centraliza todas las constantes del sistema. Modificar este archivo es suficiente para adaptar el firmware a distintas credenciales Wi-Fi, pinouts o parámetros de protocolo, sin tocar la lógica de `main.cpp`. Este diseño sigue el principio **OCP** (Open/Closed Principle): el módulo está cerrado a modificaciones internas pero abierto a configuración externa.

**`web_ui.h`** almacena la interfaz web completa del D3 como un string literal `PROGMEM` en flash del ESP32. Esto elimina la necesidad de un sistema de archivos (SPIFFS/LittleFS) para este proyecto, simplificando el build y garantizando que la interfaz esté siempre disponible sin dependencias externas.

---

## 4. Configuración centralizada — `config.h`

El archivo `config.h` está dividido en secciones lógicas:

### Credenciales Wi-Fi del Access Point

```cpp
#define AP_SSID       "Nombre-RED"
#define AP_PASSWORD   "Contraseña-RED"
#define AP_CHANNEL    6
#define AP_MAX_CONN   1
```

El valor `AP_MAX_CONN = 1` limita la red a un único cliente simultáneo, que es el D3. Esto reduce la superficie de ataque y evita que dispositivos no autorizados consuman recursos del servidor.

El canal 6 es uno de los tres canales no solapados del estándar 802.11b/g/n en la banda de 2,4 GHz (canales 1, 6 y 11). Usar un canal no solapado evita interferencia por co-canal con redes vecinas que utilicen los mismos canales.

### Configuración de red y servidor

```cpp
#define AP_IP           "192.168.4.1"
#define SERVER_PORT     80
#define WS_PATH         "/ws"
#define WS_PING_INTERVAL_MS   10000
```

La dirección `192.168.4.1` es la IP por defecto del modo AP del ESP32 asignada por el stack lwIP interno. D3 accede a `http://192.168.4.1` para obtener la interfaz web y a `ws://192.168.4.1/ws` para la conexión WebSocket.

### Pines de hardware

```cpp
// IR
#define IR_SEND_PIN   4
#define IR_RECV_PIN   5

// LED RGB del D2
#define PIN_R   25
#define PIN_G   26
#define PIN_B   27

// LEDs de estado
#define PIN_LED_CONN   32   // verde: D3 conectado
#define PIN_LED_DISC   33   // rojo: desconexión detectada
#define PIN_LED_AP     14   // amarillo: AP Wi-Fi activo
```

### Parámetros del protocolo IR

```cpp
#define MAX_REINTENTOS       3
#define TIMEOUT_ACK_MS    2000
#define DELAY_REINTENTO_MS  300
```

Estos valores equilibran la robustez del protocolo con la latencia percibida por el usuario de D3. Con 3 intentos y 2 s de timeout por intento, el peor caso es ~6,9 s (3 × 2 s + 2 × 300 ms), tras lo cual D2 reporta fallo a D3.

### Mensajes JSON del protocolo D2 ↔ D3

```cpp
// D2 → D3
#define MSG_ESTADO_ACTUAL   "ESTADO_ACTUAL"
#define MSG_RESULTADO_CMD   "RESULTADO_CMD"

// D3 → D2
#define MSG_CMD_COLOR       "CMD_COLOR"
#define MSG_CMD_ENCENDER    "CMD_ENCENDER"
#define MSG_CMD_APAGAR      "CMD_APAGAR"
```

Centralizar los tipos de mensaje en macros garantiza consistencia entre el servidor (C++) y el cliente (JavaScript), y facilita refactorizaciones futuras.

---

## 5. Red Wi-Fi — Access Point del ESP32

### Modo de operación: Access Point (AP) autónomo

El D2 opera en **modo Access Point puro** (`WiFi.softAP()`), creando su propia red inalámbrica independiente. Este modo fue elegido por las siguientes razones:

**Decisión arquitectónica — ADR-01:**

| Campo | Contenido |
|---|---|
| **Título** | Modo Wi-Fi del D2: AP vs. Station |
| **Contexto** | D2 necesita comunicarse con D3 sin depender de infraestructura externa |
| **Opciones consideradas** | (A) Station (STA): conectarse a una red existente; (B) Access Point (AP): crear red propia; (C) AP+STA simultáneo |
| **Decisión** | Access Point (AP) autónomo |
| **Justificación** | Independencia total de infraestructura (funciona en campo sin router); IP predecible (192.168.4.1); latencia mínima (sin saltos de red); simplicidad del firmware; alineado con el TP (comunicación ad hoc entre dispositivos) |
| **Consecuencias** | D3 pierde acceso a Internet mientras está conectado a la red de D2; se acepta este trade-off dado que D3 solo necesita comunicarse con D2 |

### Inicialización del Access Point

```cpp
WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
IPAddress ip = WiFi.softAPIP();
```

Los parámetros de `softAP()`:

| Parámetro | Valor | Descripción |
|---|---|---|
| `ssid` | `AP_SSID` | Nombre de la red visible en el scan Wi-Fi de D3 |
| `passphrase` | `AP_PASSWORD` | Contraseña WPA2-PSK |
| `channel` | `6` | Canal de radio frecuencia (1-13) |
| `ssid_hidden` | `0` | SSID visible (0 = broadcast, 1 = oculto) |
| `max_connection` | `1` | Máximo de clientes simultáneos |

### Características del stack Wi-Fi del ESP32

El módulo Wi-Fi del ESP32 está implementado en el firmware de Espressif (corre en el Core 0, gestionado por la tarea `wifi` de FreeRTOS) e implementa el estándar **IEEE 802.11b/g/n** en la banda de **2,4 GHz**. Sus características principales en modo AP:

- **Velocidades soportadas:** 802.11b (1–11 Mbps), 802.11g (6–54 Mbps), 802.11n (hasta 150 Mbps con HT20)
- **Seguridad:** WPA2-PSK (AES-CCMP), que es el estándar mínimo recomendado actualmente
- **DHCP servidor:** el ESP32 asigna IPs automáticamente a los clientes (D3 recibe 192.168.4.2 por defecto)
- **Stack TCP/IP:** lwIP (Lightweight IP), integrado en ESP-IDF
- **Antena:** PCB integrada o conector U.FL según la variante del módulo

### Seguridad de la red

La red opera con autenticación **WPA2-PSK** (Wi-Fi Protected Access 2, Pre-Shared Key). El cifrado de datos en tránsito utiliza AES-CCMP (128 bits), que protege los mensajes WebSocket contra sniffing pasivo en el canal de radio. 

Para un entorno de producción, se recomendaría adicionalmente implementar TLS sobre el WebSocket (WSS), pero para el alcance del TP se considera WPA2 suficiente dado que la red es de corto alcance y acceso controlado.

---

## 6. Servidor HTTP — ESPAsyncWebServer

### Elección de ESPAsyncWebServer

**Decisión arquitectónica — ADR-02:**

| Campo | Contenido |
|---|---|
| **Título** | Biblioteca de servidor HTTP: ESPAsyncWebServer vs. WebServer nativo |
| **Contexto** | Se necesita servir una página HTML y mantener WebSocket simultáneamente |
| **Opciones consideradas** | (A) `WebServer.h` (incluido en Arduino Core ESP32); (B) `ESPAsyncWebServer` |
| **Decisión** | `ESPAsyncWebServer` |
| **Justificación** | Arquitectura asíncrona no bloqueante: el servidor gestiona solicitudes HTTP y eventos WS sin bloquear `loop()`; soporte nativo de WebSocket (`AsyncWebSocket`); manejo eficiente de múltiples solicitudes concurrentes; activamente mantenido con soporte para ESP32 |
| **Consecuencias** | Dependencia adicional (`AsyncTCP`); callbacks ejecutados en Core 0, requiere cuidado con acceso a recursos compartidos |

### Inicialización y rutas

```cpp
AsyncWebServer  server(SERVER_PORT);   // puerto 80
AsyncWebSocket  ws(WS_PATH);           // ruta "/ws"

// Ruta principal: servir la interfaz web
server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", WEB_UI);
});

// Ruta 404
server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
});

ws.onEvent(onWsEvent);
server.addHandler(&ws);
server.begin();
```

`send_P()` lee el string directamente desde flash (PROGMEM) sin copiarlo a RAM, lo que es crítico dado que `WEB_UI` puede pesar varios kilobytes y la RAM del ESP32 (320 KB total, ~180 KB disponibles para el usuario) es limitada.

### Ciclo de vida del servidor

El servidor no requiere llamadas explícitas en `loop()` para procesar solicitudes HTTP regulares: ESPAsyncWebServer gestiona todo en el Core 0 mediante la tarea de red de lwIP. La única llamada necesaria en `loop()` es:

```cpp
ws.cleanupClients();
```

Esta función libera los recursos de conexiones WebSocket cerradas que el servidor todavía mantiene en su lista interna. Sin esta llamada periódica, la memoria heap del servidor se fragmenta gradualmente con cada conexión/desconexión.

---

## 7. Comunicación en tiempo real — WebSocket

### Por qué WebSocket y no HTTP polling

El protocolo WebSocket (RFC 6455) fue elegido sobre HTTP polling (solicitudes periódicas del cliente) por las siguientes razones técnicas:

- **Full-duplex:** el servidor puede enviar datos al cliente en cualquier momento sin esperar una solicitud, esencial para que D2 informe proactivamente el resultado de los comandos IR.
- **Baja latencia:** sin overhead de handshake HTTP por cada intercambio de datos.
- **Eficiencia de recursos:** una única conexión TCP persistente reemplaza múltiples conexiones efímeras.
- **Estado compartido:** D2 siempre conoce si D3 está conectado (evento `WS_EVT_CONNECT` / `WS_EVT_DISCONNECT`).

### Callback central de eventos WebSocket

Toda la lógica de red del D2 se concentra en `onWsEvent()`, que es registrado como handler del objeto `AsyncWebSocket`:

```cpp
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len)
```

Los eventos manejados son:

| Evento (`AwsEventType`) | Acción del D2 |
|---|---|
| `WS_EVT_CONNECT` | Encender LED_CONN, apagar LED_DISC, enviar `ESTADO_ACTUAL` al cliente |
| `WS_EVT_DISCONNECT` | Apagar LED_CONN, encender LED_DISC |
| `WS_EVT_DATA` | Verificar que el frame es texto completo y no fragmentado, delegar a `procesarMensajeWS()` |
| `WS_EVT_ERROR` | Log del error por puerto serie |

### Verificación de frames completos

Antes de procesar un mensaje se verifica que el frame WebSocket esté completo y no fragmentado:

```cpp
if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
```

- `info->final`: este es el frame final de un mensaje (no hay más fragmentos).
- `info->index == 0`: es el primer (y en este caso único) fragmento.
- `info->len == len`: el largo declarado del frame coincide con los bytes recibidos.
- `info->opcode == WS_TEXT`: es un frame de texto (no binario, no ping/pong de protocolo).

Esta verificación es necesaria porque WebSocket permite fragmentar mensajes grandes en múltiples frames. Para los mensajes JSON de este proyecto (< 200 bytes), nunca habrá fragmentación, pero la verificación es una salvaguarda defensiva.

---

## 8. Protocolo de mensajes D2 ↔ D3 (JSON sobre WebSocket)

Todos los mensajes son objetos JSON planos. El campo `"tipo"` actúa como discriminador de tipo de mensaje.

### Mensajes D3 → D2 (comandos)

#### CMD_COLOR — Cambiar al color especificado

```json
{"tipo": "CMD_COLOR", "color": "VERDE"}
```

El campo `"color"` debe coincidir exactamente con uno de los nombres en la tabla `COLORES[]` de `config.h`. D2 realiza una búsqueda lineal por nombre y retorna error si no lo encuentra.

#### CMD_ENCENDER — Encender con el último color

```json
{"tipo": "CMD_ENCENDER"}
```

D2 usa el valor actual de `colorActual` (persistido en NVS) para reencender el LED con el último color conocido.

#### CMD_APAGAR — Apagar el LED

```json
{"tipo": "CMD_APAGAR"}
```

D2 envía `RGB = {0, 0, 0}` al D1. El índice `colorActual` se preserva para el próximo encendido.

#### PING — Keepalive del cliente

```json
{"tipo": "PING"}
```

D2 responde inmediatamente con `{"tipo": "PONG"}`. Forma parte del mecanismo de keepalive a nivel de aplicación (ver sección 9).

### Mensajes D2 → D3 (respuestas y notificaciones)

#### ESTADO_ACTUAL — Enviado al conectar

```json
{
  "tipo": "ESTADO_ACTUAL",
  "encendido": true,
  "color": "VERDE",
  "r": 0,
  "g": 255,
  "b": 0
}
```

Enviado una única vez al cliente recién conectado, para que D3 muestre el estado real del sistema sin necesidad de enviar un comando de consulta.

#### RESULTADO_CMD — Resultado de un comando

```json
{
  "tipo": "RESULTADO_CMD",
  "exito": true,
  "encendido": true,
  "color": "VERDE",
  "r": 0,
  "g": 255,
  "b": 0
}
```

Enviado tras completar el ciclo IR (con ACK o timeout). Si `"exito": false`, D3 muestra un mensaje de error y el estado no cambia.

### Serialización con ArduinoJson

D2 usa la biblioteca **ArduinoJson v7** para serializar y deserializar los mensajes:

```cpp
// Serialización (D2 → D3)
JsonDocument doc;
doc["tipo"]      = MSG_RESULTADO_CMD;
doc["exito"]     = exito;
doc["encendido"] = (bool)encendido;
doc["color"]     = encendido ? COLORES[colorActual].nombre : "";
// ...
char buf[200];
serializeJson(doc, buf, sizeof(buf));
ws.textAll(buf);

// Deserialización (D3 → D2)
JsonDocument doc;
DeserializationError err = deserializeJson(doc, payload, len);
const char *tipo = doc["tipo"] | "";
```

`ws.textAll()` envía el mensaje a todos los clientes WebSocket conectados (en este proyecto solo puede haber uno, dado `AP_MAX_CONN = 1`).

---

## 9. Keepalive — Detección de desconexión real

### El problema de las conexiones TCP "zombie"

TCP no tiene un mecanismo nativo de keepalive a nivel de aplicación que funcione en todos los escenarios. Si D3 pierde la conexión abruptamente (apagado del dispositivo, salida del rango Wi-Fi, cierre forzado del navegador), el stack TCP del D2 puede tardar minutos en detectar la desconexión mediante los timeouts del protocolo TCP. Durante ese tiempo, D2 cree que D3 sigue conectado.

### Solución: keepalive a nivel de aplicación

ESPAsyncWebServer incluye un mecanismo de ping/pong de protocolo WebSocket (`WS_PING_INTERVAL_MS`), que detecta conexiones muertas desde el lado del servidor.

Adicionalmente, la interfaz web del D3 implementa un keepalive a nivel de aplicación:

**D3 (cliente, JavaScript):**
```javascript
const PING_MS   = 5000;   // enviar PING cada 5 segundos
const PONG_WAIT = 3000;   // esperar PONG por 3 segundos

pingTimer = setInterval(() => {
    ws.send(JSON.stringify({ tipo: 'PING' }));
    pongTimer = setTimeout(() => {
        ws.close();   // cerrar si no llega PONG
    }, PONG_WAIT);
}, PING_MS);
```

**D2 (servidor, C++):**
```cpp
if (strcmp(tipo, "PING") == 0) {
    client->text("{\"tipo\":\"PONG\"}");
    return;
}
```

Este esquema garantiza que ambos extremos detecten la desconexión en un tiempo máximo de `PING_MS + PONG_WAIT = 8 segundos`, sin depender de los timeouts del nivel TCP (que pueden superar varios minutos).

Al detectar la desconexión, D3 muestra el banner de error, deshabilita los controles y presenta el botón "Reconectar". D2 actualiza los LEDs de estado (`LED_CONN` apagado, `LED_DISC` encendido) y conserva el estado actual del sistema sin modificación.

---

## 10. Comunicación infrarroja con D1 — Protocolo NEC Raw

### Formato de trama de 32 bits

```
┌────────────┬───────────┬───────────┬───────────┐
│ bits 31-24 │ bits 23-16│ bits 15-8 │ bits 7-0  │
│  Cabecera  │     R     │     G     │     B     │
└────────────┴───────────┴───────────┴───────────┘
```

- **Cabecera `0xAA`** (comando D2 → D1): indica una trama de comando de color.
- **Cabecera `0xBB`** (ACK D1 → D2): indica una trama de confirmación del Esclavo.

El uso de NEC **Raw** (en lugar del NEC estándar) evita las inversiones y correcciones de bit que el protocolo NEC estándar aplica automáticamente, permitiendo que los bytes de la trama lleguen al receptor exactamente como fueron construidos en el código.

### Gestión half-duplex del canal IR

El canal IR es físicamente half-duplex: el emisor y el receptor no pueden operar simultáneamente en el mismo ESP32 sin interferirse. La gestión se realiza deteniendo el receptor antes de transmitir y reactivándolo inmediatamente después:

```cpp
IrReceiver.stop();
IrSender.sendNECRaw(trama, 0);   // 0 = sin repeticiones automáticas
IrReceiver.start();
```

### Protocolo de handshake con reintentos

La función `enviarColorIR()` implementa el protocolo completo:

1. Construir la trama de 32 bits: `[0xAA][R][G][B]`
2. Detener el receptor (evitar eco propio en KY-022)
3. Transmitir con `sendNECRaw` (sin repeticiones automáticas)
4. Reactivar el receptor para capturar el ACK
5. Esperar hasta `TIMEOUT_ACK_MS` (2000 ms) recibiendo del D1:
   - Filtrar repeticiones automáticas NEC (`IRDATA_FLAGS_IS_REPEAT`)
   - Verificar cabecera `0xBB` y coincidencia byte a byte de R, G, B
   - Retornar `true` en el primer ACK válido
6. Retornar `false` si el timeout expira sin ACK válido

`ejecutarCicloIR()` envuelve `enviarColorIR()` con la lógica de reintentos:

```cpp
for (int i = 0; i < MAX_REINTENTOS && !ack; i++) {
    if (i > 0) delay(DELAY_REINTENTO_MS);
    ack = enviarColorIR(r, g, b);
}
```

Si algún intento recibe ACK válido: actualizar estado, aplicar color al LED del D2, persistir en NVS, notificar a D3.  
Si ningún intento recibe ACK: el estado no cambia, notificar a D3 con `exito: false`.

### Filtrado de cabecera para evitar ecos

Dado que D2 tiene un receptor IR (KY-022) propio, podría capturar los ecos de sus propias transmisiones. El filtro de cabecera `0xBB` garantiza que D2 solo procese como ACK las tramas enviadas por D1, descartando silenciosamente cualquier otra trama (incluyendo sus propios ecos o tramas de dispositivos IR externos).

---

## 11. Modelo de concurrencia — Dual-Core del ESP32

El ESP32 integra dos núcleos Xtensa LX6 a 240 MHz, gestionados por **FreeRTOS**. El framework Arduino asigna `loop()` al Core 1 y las tareas de red (Wi-Fi, TCP/IP, WebSocket) al Core 0.

```
Core 0 (tarea lwIP / ESPAsync)
├── Gestión del Access Point Wi-Fi
├── Stack TCP/IP (lwIP)
├── Servidor HTTP (ESPAsyncWebServer)
└── Callback onWsEvent() ← escritura de irPendiente, r/g/b_pendiente

Core 1 (tarea Arduino)
├── setup()
└── loop()
    ├── ws.cleanupClients()
    ├── if(irPendiente) → ejecutarCicloIR()   ← lectura de irPendiente
    └── delay(5)
```

### Coordinación entre cores — Variables volátiles

El callback `onWsEvent()` (Core 0) escribe las variables de comando pendiente, y `loop()` (Core 1) las lee. Para evitar condiciones de carrera:

```cpp
volatile bool    irPendiente        = false;
volatile uint8_t r_pendiente        = 0;
volatile uint8_t g_pendiente        = 0;
volatile uint8_t b_pendiente        = 0;
volatile uint8_t colorIdx_pendiente = 0;
volatile bool    encendido_pendiente = false;
```

El calificador `volatile` instruye al compilador a no optimizar las lecturas de estas variables mediante registros, forzando siempre la lectura desde memoria. Esto es necesario cuando una variable puede ser modificada por un contexto de ejecución diferente (otro core o una interrupción).

**¿Por qué no se necesita mutex?** La seguridad se basa en el patrón de acceso:
- El Core 0 **solo escribe** `irPendiente = true` y los pendientes.
- El Core 1 **solo lee** `irPendiente` y luego lo pone en `false`.
- El ESP32 garantiza atomicidad de escritura/lectura en tipos de 1 byte (`bool`, `uint8_t`).
- La flag `irPendiente` actúa como semáforo binario: el Core 0 no produce un nuevo comando hasta que el Core 1 procese el anterior (el callback descarta comandos si `irPendiente == true`).

---

## 12. Persistencia de estado — NVS (Preferences)

### Qué es la NVS del ESP32

La **NVS (Non-Volatile Storage)** es una partición de la flash interna del ESP32 organizada como un almacén clave-valor. Implementa **wear leveling** (nivelado de desgaste) automático para distribuir las escrituras uniformemente y extender la vida útil de la flash. A diferencia de la EEPROM de los microcontroladores AVR (que tiene ciclos de escritura limitados), la NVS del ESP32 es práctica para escrituras frecuentes.

La biblioteca **`Preferences`** (incluida en el ESP32 Arduino Core, sin necesidad de declararse en `lib_deps`) provee una API de alto nivel sobre NVS:

```cpp
Preferences prefs;
prefs.begin("d2_state", false);   // false = lectura/escritura
prefs.putUChar("magic",     0xA5);
prefs.putUChar("color_idx", colorActual);
prefs.putBool ("encendido", encendido);
prefs.end();
```

### Qué se persiste y por qué

D2 persiste únicamente el **índice de color** (`color_idx`) y el **estado de encendido** (`encendido`), en lugar de los valores RGB directos. Esto minimiza escrituras en flash (3 escrituras por cambio de estado en lugar de 4) y es posible porque los valores RGB se reconstruyen deterministamente desde `COLORES[colorActual]`.

| Clave NVS | Tipo | Valor |
|---|---|---|
| `"magic"` | `uint8_t` | `0xA5` — centinela de validación |
| `"color_idx"` | `uint8_t` | Índice en `COLORES[]` (0-7) |
| `"encendido"` | `bool` | Estado del LED |

### Valor centinela (magic byte)

El byte `0xA5` en la clave `"magic"` permite distinguir entre una NVS que nunca fue escrita (NVS vacía, devuelve el valor por defecto `0x00`) y una NVS con datos válidos. Si el centinela no coincide, `cargarNVS()` inicializa con valores por defecto en lugar de aplicar datos corruptos.

### Comportamiento al encender

Al iniciar, D2 recupera el estado de la NVS y aplica el color al LED local:

```cpp
cargarNVS();
if (encendido) {
    setColor(COLORES[colorActual].r, COLORES[colorActual].g, COLORES[colorActual].b);
} else {
    setColor(0, 0, 0);
}
```

Esto garantiza que D2 recuerde su último estado incluso tras un corte de energía o reinicio.

---

## 13. Control PWM del LED RGB — Módulo LEDC

### LEDC vs. `analogWrite()` en ESP32

El ESP32 no implementa la función `analogWrite()` de la API Arduino estándar (que existe en AVR como Arduino Uno/Nano). En su lugar, provee el módulo **LEDC (LED Control)**, que ofrece 16 canales PWM independientes con mayor flexibilidad de configuración.

### Configuración

```cpp
#define LEDC_FREQ_HZ    5000   // frecuencia PWM: 5 kHz
#define LEDC_RESOLUTION    8   // resolución: 8 bits (0-255)
#define LEDC_CH_R          3   // canal LEDC para rojo
#define LEDC_CH_G          1   // canal LEDC para verde
#define LEDC_CH_B          2   // canal LEDC para azul

ledcSetup(LEDC_CH_R, LEDC_FREQ_HZ, LEDC_RESOLUTION);
ledcAttachPin(PIN_R, LEDC_CH_R);
```

La frecuencia de 5 kHz está por encima del umbral de percepción de parpadeo del ojo humano (~60 Hz), eliminando el efecto de flicker visible. La resolución de 8 bits provee 256 niveles de intensidad por canal, suficientes para representar cualquier color del espacio RGB-8bit.

### Aplicación de color

```cpp
void setColor(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LEDC_CH_R, r);
    ledcWrite(LEDC_CH_G, g);
    ledcWrite(LEDC_CH_B, b);
}
```

El LED RGB del D2 refleja el último color confirmado por D1, proveyendo retroalimentación visual local del estado del sistema, independientemente de si D3 está conectado.

---

## 14. LEDs de estado de conectividad

D2 incorpora tres LEDs de estado que comunican visualmente la condición de la red y la conexión con D3:

| LED | Pin | Color | Estado activo | Significado |
|---|---|---|---|---|
| `LED_AP` | GPIO 14 | Amarillo | Encendido desde `setup()` | Access Point Wi-Fi activo y aceptando conexiones |
| `LED_CONN` | GPIO 32 | Verde | `WS_EVT_CONNECT` | D3 tiene sesión WebSocket activa |
| `LED_DISC` | GPIO 33 | Rojo | `WS_EVT_DISCONNECT` | D3 se desconectó (abruptamente o de forma ordenada) |

`LED_CONN` y `LED_DISC` son mutuamente excluyentes: al activarse uno, el otro se apaga. `LED_AP` permanece encendido durante toda la operación normal del sistema.

---

## 15. Tabla de colores — Secuencia del TP2

La tabla de colores es compartida entre el firmware C++ (D2) y la interfaz JavaScript (D3), garantizando coherencia visual:

```cpp
struct ColorRGB {
    const char* nombre;
    uint8_t     r, g, b;
};

static const ColorRGB COLORES[NUM_COLORES] = {
    { "ROJO",     255,   0,   0 },
    { "AMARILLO", 255, 200,   0 },
    { "VERDE",      0, 255,   0 },
    { "CELESTE",    0, 255, 255 },
    { "AZUL",       0,   0, 255 },
    { "LILA",     180,   0, 255 },
    { "BLANCO",   255, 255, 255 },
    { "ROSA",     255, 105, 180 }
};
```

La tabla es `static const`, lo que permite al compilador colocarla en la sección de datos de solo lectura (flash), sin ocupar RAM en tiempo de ejecución.

---

## 16. Flujo completo de un comando

A continuación se describe el flujo completo desde que el usuario de D3 presiona un botón de color hasta que el LED del D1 cambia:

```
[D3 — Navegador]
  1. Usuario presiona "VERDE" en la interfaz web
  2. JS envía: {"tipo":"CMD_COLOR","color":"VERDE"} por WebSocket

[D2 — Core 0, callback onWsEvent()]
  3. Recibe el frame WebSocket de texto
  4. Deserializa JSON con ArduinoJson
  5. Identifica tipo "CMD_COLOR", nombre "VERDE"
  6. Busca "VERDE" en COLORES[]: índice 2, R=0 G=255 B=0
  7. Carga variables pendientes: r=0, g=255, b=0, idx=2, enc=true
  8. irPendiente = true

[D2 — Core 1, loop()]
  9. Detecta irPendiente == true
 10. Llama ejecutarCicloIR()

[D2 — ejecutarCicloIR() → enviarColorIR()]
 11. Construye trama IR: 0xAA00FF00
 12. IrReceiver.stop()
 13. IrSender.sendNECRaw(0xAA00FF00, 0) → KY-005 emite IR a D1
 14. IrReceiver.start()
 15. Espera ACK hasta 2000 ms

[D1 — loop(), recibe trama IR]
 16. Decodifica: cabecera=0xAA, R=0, G=255, B=0
 17. setColor(0, 255, 0) → LED RGB D1 se pone VERDE
 18. guardarNVS(0, 255, 0)
 19. Construye ACK: 0xBB00FF00
 20. IrReceiver.stop()
 21. delay(120 ms)
 22. IrSender.sendNECRaw(0xBB00FF00, 0) → KY-005 D1 emite IR a D2
 23. IrReceiver.start()

[D2 — enviarColorIR(), esperando ACK]
 24. Decodifica: cabecera=0xBB, R=0, G=255, B=0 ✓ ACK válido
 25. Retorna true

[D2 — ejecutarCicloIR(), ACK recibido]
 26. colorActual = 2 (VERDE)
 27. encendido = true
 28. setColor(0, 255, 0) → LED RGB D2 se pone VERDE
 29. guardarNVS() → persiste idx=2, enc=true en NVS
 30. notificarD3Estado(true)

[D2 — notificarD3Estado()]
 31. Serializa: {"tipo":"RESULTADO_CMD","exito":true,"encendido":true,"color":"VERDE","r":0,"g":255,"b":0}
 32. ws.textAll(buf) → envía por WebSocket a D3

[D3 — Navegador, recibe RESULTADO_CMD]
 33. Actualiza color-preview a rgb(0,255,0)
 34. Chips R=0, G=255, B=0
 35. Botón "VERDE" recibe clase "activo" (borde blanco)
 36. Banner: "✓ Comando aplicado — VERDE"
```

---

## 17. Diagrama de flujo del firmware

```
setup()
  │
  ├─ Serial.begin(115200)
  ├─ iniciarLedc()
  ├─ pinMode(LED_CONN/DISC/AP, OUTPUT)
  ├─ IrReceiver.begin(IR_RECV_PIN)
  ├─ cargarNVS()
  │    ├─ [válida] → aplicar color guardado
  │    └─ [vacía]  → setColor(0,0,0)
  ├─ WiFi.softAP(SSID, PASS, CH, 0, 1)
  ├─ ws.onEvent(onWsEvent)
  ├─ server.on("/", HTTP_GET, ...)
  ├─ server.begin()
  └─ digitalWrite(LED_AP, HIGH)
       │
       ▼
loop() ──────────────────────────────────────────────┐
  │                                                   │
  ├─ ws.cleanupClients()                              │
  │                                                   │
  ├─ irPendiente? ─── NO ─────────────────────────── ┤
  │       │                                           │
  │      YES                                          │
  │       │                                           │
  │  irPendiente = false                              │
  │  ejecutarCicloIR()                                │
  │       │                                           │
  │  for(i < MAX_REINTENTOS && !ack)                  │
  │       │                                           │
  │  enviarColorIR(r,g,b)                             │
  │       ├─ stop receptor                            │
  │       ├─ sendNECRaw(trama)                        │
  │       ├─ start receptor                           │
  │       └─ while(millis-t0 < TIMEOUT)               │
  │             ├─ decode()?                          │
  │             ├─ filtrar REPEAT                     │
  │             ├─ cab==0xBB && RGB==RGB? → true      │
  │             └─ timeout → false                    │
  │       │                                           │
  │  ack? ─── NO → notificarD3(false)                 │
  │       │                                           │
  │      YES                                          │
  │       ├─ actualizar colorActual, encendido        │
  │       ├─ setColor(r,g,b)                          │
  │       ├─ guardarNVS()                             │
  │       └─ notificarD3(true)                        │
  │                                                   │
  ├─ delay(5)                                         │
  └─────────────────────────────────────────────────-─┘

onWsEvent() [Core 0, asíncrono]
  │
  ├─ WS_EVT_CONNECT  → LED_CONN=ON, LED_DISC=OFF, enviarEstadoInicial()
  ├─ WS_EVT_DISCONNECT → LED_CONN=OFF, LED_DISC=ON
  ├─ WS_EVT_DATA     → procesarMensajeWS()
  │       ├─ PING        → responder PONG
  │       ├─ CMD_COLOR   → cargar pendientes, irPendiente=true
  │       ├─ CMD_ENCENDER→ cargar pendientes (colorActual), irPendiente=true
  │       └─ CMD_APAGAR  → cargar pendientes (RGB=0), irPendiente=true
  └─ WS_EVT_ERROR    → log
```

---

## 18. Gestión de dependencias con PlatformIO

### Declaración en `platformio.ini`

```ini
[env:esp32dev]
platform         = espressif32
board            = esp32dev
framework        = arduino

monitor_speed    = 115200
upload_speed     = 921600
upload_port      = COM5
monitor_port     = COM5

lib_deps =
    z3t0/IRremote @ ^4.7.1
    me-no-dev/AsyncTCP @ ^1.1.1
    me-no-dev/ESPAsyncWebServer @ ^1.2.3
    bblanchon/ArduinoJson @ ^7.2.0

build_flags =
    -DCORE_DEBUG_LEVEL=0

monitor_filters = esp32_exception_decoder
```

### Justificación de cada dependencia

| Biblioteca | Versión | Propósito | Por qué esta biblioteca |
|---|---|---|---|
| `z3t0/IRremote` | `^4.7.1` | Emisión y recepción IR (NEC Raw 32 bits) | Soporte nativo de `sendNECRaw` / `decodedRawData`; usa el periférico RMT del ESP32 para generar la portadora de 38 kHz con precisión hardware |
| `me-no-dev/AsyncTCP` | `^1.1.1` | Capa de transporte TCP asíncrona | Dependencia requerida por ESPAsyncWebServer; implementa sockets TCP no bloqueantes sobre lwIP |
| `me-no-dev/ESPAsyncWebServer` | `^1.2.3` | Servidor HTTP + WebSocket | Arquitectura completamente asíncrona; soporte nativo de `AsyncWebSocket`; no bloquea el loop |
| `bblanchon/ArduinoJson` | `^7.2.0` | Serialización/deserialización JSON | API simple y eficiente; sin memoria dinámica ilimitada; ampliamente usado en ecosistema ESP32 |

### Bibliotecas incluidas en el ESP32 Core (sin declaración en `lib_deps`)

| Biblioteca | Propósito |
|---|---|
| `Preferences` | Acceso a NVS (almacenamiento clave-valor persistente) |
| `WiFi` | Stack Wi-Fi del ESP32 (AP, STA, AP+STA) |
| `Arduino.h` | API base del framework Arduino |

### Operador `^` (caret) en SemVer

El operador `^` acepta versiones `>= X.Y.Z` y `< (X+1).0.0`. Esto permite recibir correcciones de bugs y nuevas features compatibles dentro del mismo major sin quedar atado a un patch exacto, mientras se garantiza que cambios de API incompatibles (nuevo major) no rompan el proyecto automáticamente.

### Flag `CORE_DEBUG_LEVEL`

```ini
build_flags =
    -DCORE_DEBUG_LEVEL=0
```

Controla el nivel de logs internos del ESP32 Arduino Core (Wi-Fi, NVS, Bluetooth, etc.). El nivel `0` silencia estos logs en producción, reduciendo el tráfico del puerto serie y mejorando la legibilidad de los logs propios del firmware. Para depuración del stack Wi-Fi, cambiar a `3`.

---

## 19. Criterios de selección de pines

Los siguientes GPIO del ESP32 DevKit v1 tienen restricciones de uso:

| GPIO | Restricción |
|---|---|
| 0 | Selector de modo de boot; INPUT solo |
| 2 | Debe estar en LOW durante programación |
| 6–11 | Conectados a la flash SPI interna; **nunca usar** |
| 12 | Configura voltaje de flash en boot (MTDI); evitar OUTPUT |
| 15 | Silencia log de boot si está en LOW |
| 34–39 | INPUT ONLY (sin driver de salida ni pull-up interno) |

Los pines del D2 fueron seleccionados entre los GPIO de propósito general sin restricciones:

| Función | Pin | Justificación |
|---|---|---|
| IR Emisor (KY-005) | GPIO 4 | Salida RMT-capable; sin función especial en boot |
| IR Receptor (KY-022) | GPIO 5 | Entrada con soporte de interrupción; sin función especial |
| LED RGB Rojo | GPIO 25 | GPIO de propósito general, soporte LEDC |
| LED RGB Verde | GPIO 26 | GPIO de propósito general, soporte LEDC |
| LED RGB Azul | GPIO 27 | GPIO de propósito general, soporte LEDC |
| LED_CONN | GPIO 32 | GPIO de propósito general (INPUT/OUTPUT) |
| LED_DISC | GPIO 33 | GPIO de propósito general (INPUT/OUTPUT) |
| LED_AP | GPIO 14 | GPIO de propósito general; precaución: pull-up interno en boot, pero sin efecto en OUTPUT |

---

## 20. Resumen de parámetros configurables

Todos los parámetros que pueden requerir ajuste están centralizados en `config.h`:

| Parámetro | Macro | Valor por defecto | Descripción |
|---|---|---|---|
| SSID del AP | `AP_SSID` | *(configurar)* | Nombre de la red Wi-Fi del D2 |
| Contraseña del AP | `AP_PASSWORD` | *(configurar)* | Contraseña WPA2 (mín. 12 chars) |
| Canal Wi-Fi | `AP_CHANNEL` | `6` | Canal de radiofrecuencia (1, 6 u 11) |
| Máx. clientes AP | `AP_MAX_CONN` | `1` | Solo D3 se conecta |
| IP del AP | `AP_IP` | `192.168.4.1` | IP estática del servidor |
| Puerto HTTP/WS | `SERVER_PORT` | `80` | Puerto del servidor web |
| Ruta WebSocket | `WS_PATH` | `"/ws"` | Endpoint del WebSocket |
| Pin IR Emisor | `IR_SEND_PIN` | `4` | KY-005 |
| Pin IR Receptor | `IR_RECV_PIN` | `5` | KY-022 |
| Pin LED R | `PIN_R` | `25` | Canal LEDC rojo |
| Pin LED G | `PIN_G` | `26` | Canal LEDC verde |
| Pin LED B | `PIN_B` | `27` | Canal LEDC azul |
| Pin LED_CONN | `PIN_LED_CONN` | `32` | LED indicador de conexión |
| Pin LED_DISC | `PIN_LED_DISC` | `33` | LED indicador de desconexión |
| Pin LED_AP | `PIN_LED_AP` | `14` | LED indicador de AP activo |
| Reintentos IR | `MAX_REINTENTOS` | `3` | Intentos antes de reportar fallo |
| Timeout ACK IR | `TIMEOUT_ACK_MS` | `2000` | ms de espera por ACK de D1 |
| Pausa reintentos | `DELAY_REINTENTO_MS` | `300` | ms entre reintentos IR |
| Namespace NVS | `NVS_NAMESPACE` | `"d2_state"` | Partición lógica en NVS |
| Velocidad serie | `SERIAL_BAUD` | `115200` | Baud rate del monitor serial |

---

*Documentación del Dispositivo 2 — TP3 Integrador, Comunicación de Datos.*  
*Bevilacqua, Francisco — Clement, Sebastián — Junio 2026.*  
*Ingeniería en Computación | Universidad Nacional de Rafaela.*
