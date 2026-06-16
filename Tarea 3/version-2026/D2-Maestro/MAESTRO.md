# Dispositivo 2 — Maestro IR + Servidor Wi-Fi/WebSocket

> **TP3 Integrador — Comunicación de Datos**  
> Ingeniería en Computación | Bevilacqua, Francisco — Clement, Sebastián | Junio 2026  
> Docentes: Mg. Ing. Martín Pico · Ing. Milton Pozzo  
> Universidad Nacional de Rafaela

---

## Índice

1. [Rol del dispositivo en el sistema](#1-rol-del-dispositivo-en-el-sistema)
2. [Hardware y pinout](#2-hardware-y-pinout)
3. [Entorno de desarrollo y dependencias](#3-entorno-de-desarrollo-y-dependencias)
4. [Arquitectura del firmware](#4-arquitectura-del-firmware)
5. [Configuración Wi-Fi: modo Access Point en el ESP32](#5-configuración-wi-fi-modo-access-point-en-el-esp32)
6. [Servidor HTTP y servicio de la interfaz web](#6-servidor-http-y-servicio-de-la-interfaz-web)
7. [Protocolo WebSocket con D3 (CRC-16/IBM)](#7-protocolo-websocket-con-d3-crc-16ibm)
8. [Protocolo IR con D1 (CRC-8/MAXIM)](#8-protocolo-ir-con-d1-crc-8maxim)
9. [Persistencia de estado: NVS / Preferences](#9-persistencia-de-estado-nvs--preferences)
10. [Control PWM del LED RGB (LEDC)](#10-control-pwm-del-led-rgb-ledc)
11. [LEDs de estado](#11-leds-de-estado)
12. [Modelo de concurrencia: Core 0 y Core 1](#12-modelo-de-concurrencia-core-0-y-core-1)
13. [Secuencia de inicialización (setup)](#13-secuencia-de-inicialización-setup)
14. [Ciclo principal (loop)](#14-ciclo-principal-loop)
15. [Paleta de colores y tabla de referencia](#15-paleta-de-colores-y-tabla-de-referencia)
16. [Tabla de mensajes JSON del protocolo D2 ↔ D3](#16-tabla-de-mensajes-json-del-protocolo-d2--d3)
17. [Problemas detectados y soluciones implementadas](#17-problemas-detectados-y-soluciones-implementadas)
18. [Parámetros de compilación y configuración](#18-parámetros-de-compilación-y-configuración)

---

## 1. Rol del dispositivo en el sistema

El Dispositivo 2 (D2) es el **nodo central** del sistema TP3. Opera simultáneamente en tres roles:

| Rol | Enlace | Dirección |
|-----|--------|-----------|
| Maestro IR con CRC-8/MAXIM | D2 ↔ D1 | half-duplex, NEC Raw 38 kHz |
| Access Point Wi-Fi WPA2 | D3 → D2 | IEEE 802.11 b/g/n |
| Servidor HTTP + WebSocket con CRC-16/IBM | D2 ↔ D3 | full-duplex, RFC 6455 |

```
┌─────────────────────────────────────────────────────────┐
│                    Dispositivo 2 (D2)                   │
│                       ESP32 DevKit                      │
│                                                         │
│  ┌─────────────┐    ┌──────────────┐    ┌────────────┐  │
│  │  Maestro IR │    │ Wi-Fi SoftAP │    │ WS Server  │  │
│  │ CRC-8/MAXIM │    │ WPA2 ch.6   │    │CRC-16/IBM  │  │
│  └──────┬──────┘    └──────┬───────┘    └─────┬──────┘  │
│         │                  │                   │         │
└─────────┼──────────────────┼───────────────────┼─────────┘
          │ NEC Raw IR       │ 802.11             │ JSON/WS
          ▼                  ▼                   ▼
      Dispositivo 1      (red interna)      Dispositivo 3
     (ESP32 Esclavo)                      (Navegador web)
```

D2 **no tiene lógica de actuación directa** sobre el LED RGB del sistema. Su función es:

1. Recibir comandos de D3 por WebSocket, validarlos con CRC-16.
2. Traducirlos a tramas IR con CRC-8 y enviarlos a D1.
3. Esperar el ACK asimétrico de D1, confirmar la operación.
4. Notificar el resultado a D3 por WebSocket.
5. Persistir el estado confirmado en la NVS del ESP32.

---

## 2. Hardware y pinout

### 2.1 ESP32 DevKit v1

El ESP32 es un SoC (System on Chip) de Espressif con:

- CPU Xtensa LX6 dual-core a 240 MHz (dos núcleos independientes).
- 520 KB de SRAM.
- 4 MB de flash SPI externa (particionada para firmware, NVS, OTA, etc.).
- Wi-Fi 802.11 b/g/n integrado con stack TCP/IP lwIP.
- 34 GPIO programables (varios con restricciones de boot).
- Periférico RMT (Remote Control Transceiver) para IR por hardware.
- 16 canales LEDC (LED Control) para PWM de alta resolución.
- Soporte de FreeRTOS integrado en el ESP-IDF.

### 2.2 Tabla de conexiones

| Módulo | Pin ESP32 | GPIO | Función | Notas |
|--------|-----------|------|---------|-------|
| KY-005 S | J1-23 | GPIO 4 | Emisor IR | RMT hardware, output |
| KY-022 OUT | J1-24 | GPIO 5 | Receptor IR | Interrupt-capable, input |
| LED RGB R | J2-5 | GPIO 25 | Canal rojo PWM | LEDC canal 3 |
| LED RGB G | J2-6 | GPIO 26 | Canal verde PWM | LEDC canal 1 |
| LED RGB B | J2-7 | GPIO 27 | Canal azul PWM | LEDC canal 2 |
| LED verde (CONN) | — | GPIO 32 | D3 conectado | Digital output |
| LED rojo (DISC) | — | GPIO 33 | D3 desconectado | Digital output |
| LED amarillo (AP) | — | GPIO 14 | AP Wi-Fi activo | Digital output |
| VCC módulos IR | 3.3V | — | Alimentación | KY-005 y KY-022 a 3.3 V |
| GND | GND | — | Referencia común | Todos los módulos |

### 2.3 GPIO prohibidos en ESP32

Los siguientes pines tienen restricciones de boot y **no se usan** en el diseño:

| GPIO | Restricción |
|------|-------------|
| 0 | Boot mode selector; INPUT solo |
| 2 | Debe estar en LOW durante la programación |
| 6–11 | Flash SPI interna; NUNCA usar |
| 12 | Configura voltaje de flash en boot (MTDI) |
| 15 | MTDO; silencia log de boot si está en LOW |
| 34–39 | INPUT ONLY; sin driver de salida ni pull-up |

El canal LEDC rojo se asigna al canal 3 (en lugar del 0) porque IRremote reserva internamente el canal 0 del periférico LEDC para generar la portadora de 38 kHz del RMT.

---

## 3. Entorno de desarrollo y dependencias

### 3.1 PlatformIO

El proyecto usa **PlatformIO** como gestor de build y dependencias. El archivo `platformio.ini` declara completamente el entorno:

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

El operador `^` sigue semver: `^4.7.1` acepta `>= 4.7.1` y `< 5.0.0`, garantizando compatibilidad de API dentro del mismo major.

### 3.2 Librerías declaradas en lib_deps

#### z3t0/IRremote `^4.7.1`

Librería de emisión y recepción IR. En el ESP32, usa el periférico **RMT** (Remote Control Transceiver) del hardware para generar y decodificar las formas de onda con precisión de microsegundos sin consumir CPU:

- `IrReceiver.begin(pin, DISABLE_LED_FEEDBACK)`: inicializa el receptor. `DISABLE_LED_FEEDBACK` evita que IRremote use el pin 13 (LED integrado) como indicador de actividad, lo que interferiría con otras funciones.
- `IrSender.sendNECRaw(trama_32bit, 0)`: transmite una trama de 32 bits usando el protocolo NEC con modulación de 38 kHz. El segundo parámetro `0` indica cero repeticiones automáticas del protocolo NEC.
- `IrReceiver.decode()`: retorna `true` cuando hay una trama completa en el buffer del RMT.
- `IrReceiver.decodedIRData.decodedRawData`: campo de 32 bits con los datos decodificados.
- `IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT`: detecta repeticiones automáticas del protocolo NEC para descartarlas.
- `IrReceiver.resume()`: libera el buffer del receptor para recibir la siguiente trama. **Debe llamarse siempre después de procesar `decodedRawData`**, antes de llamar a `decode()` nuevamente.
- `IrReceiver.stop()` / `IrReceiver.start()`: deshabilita/habilita el receptor para gestión del canal half-duplex.

#### me-no-dev/ESPAsyncWebServer `^1.2.3`

Servidor HTTP y WebSocket asíncrono basado en eventos. Opera completamente en el Core 0 (tarea lwIP) sin bloquear el loop de Arduino en el Core 1:

- `AsyncWebServer server(port)`: instancia del servidor en el puerto indicado.
- `AsyncWebSocket ws(path)`: instancia del handler WebSocket en la ruta `/ws`.
- `ws.onEvent(callback)`: registra el callback de eventos WS (conectar, desconectar, datos, error).
- `server.addHandler(&ws)`: registra el handler WS en el servidor HTTP.
- `server.on("/", HTTP_GET, lambda)`: registra la ruta raíz.
- `server.begin()`: inicia el servidor.
- `client->text(str)`: envía un string de texto al cliente específico.
- `ws.textAll(str)`: broadcast a todos los clientes conectados.
- `client->close(code, reason)`: cierra la sesión WS desde el servidor (RFC 6455 §7.4).
- `ws.cleanupClients()`: libera recursos de clientes desconectados. **Debe llamarse periódicamente desde loop()** para evitar fragmentación del heap.

#### me-no-dev/AsyncTCP `^1.1.1`

Dependencia de ESPAsyncWebServer. Implementa la capa TCP asíncrona sobre el stack lwIP del ESP32. No se usa directamente en el código de la aplicación.

#### bblanchon/ArduinoJson `^7.2.0`

Librería de serialización/deserialización JSON. En la versión 7, el tipo principal es `JsonDocument` (reemplaza al `DynamicJsonDocument` de v6):

- `JsonDocument doc`: documento JSON en el stack (sin heap dinámico para tamaños pequeños).
- `doc["campo"] = valor`: asigna un campo. **El orden de asignación determina el orden de serialización**, lo que es crítico para la compatibilidad del CRC-16 entre D2 y D3.
- `serializeJson(doc, buffer, size)`: serializa a string y retorna el número de bytes escritos.
- `deserializeJson(doc, payload, len)`: deserializa desde buffer. Retorna `DeserializationError`.
- `doc.remove("campo")`: elimina un campo del documento (usado para recalcular el CRC sin el campo `crc16`).
- `doc["campo"].is<T>()`: verifica el tipo del campo antes de extraerlo.
- `doc["campo"].as<T>()`: extrae el valor con el tipo especificado.
- `doc["campo"] | default`: extrae con valor por defecto si el campo no existe.

#### Preferences (incluida en ESP32 Arduino Core)

Abstracción de la NVS (Non-Volatile Storage) del ESP32. No requiere declaración en `lib_deps`. Ver sección 9 para detalles completos.

#### WiFi (incluida en ESP32 Arduino Core)

API de Wi-Fi del ESP32. No requiere declaración en `lib_deps`. Ver sección 5 para detalles completos.

---

## 4. Arquitectura del firmware

### 4.1 Estructura de archivos

```
D2_Maestro/
├── platformio.ini          # Configuración del entorno PlatformIO
├── include/
│   ├── config.h            # Constantes centralizadas (pines, CRC, red, colores, NVS)
│   └── web_ui.h            # Interfaz web D3 embebida como string PROGMEM
└── src/
    └── main.cpp            # Firmware completo: setup(), loop(), callbacks, lógica
```

### 4.2 Módulos funcionales dentro de main.cpp

```
main.cpp
│
├── [CRC-8]   calcularCRC8(r, g, b)          → uint8_t
├── [CRC-16]  calcularCRC16(payload, len)    → uint16_t
│
├── [LEDC]    iniciarLedc()                  → void
├── [LEDC]    setColor(r, g, b)              → void
│
├── [LEDs]    setLedConn(on)                 → void
├── [LEDs]    setLedDisc(on)                 → void
│
├── [NVS]     guardarNVS()                   → void
├── [NVS]     cargarNVS()                    → void
│
├── [IR-TX]   enviarColorIR(r, g, b)         → bool
├── [IR]      ejecutarCicloIR()              → void
│
├── [WS-TX]   enviarMensajeWS(client, doc)   → void
├── [WS-TX]   enviarMensajeWSBroadcast(doc)  → void
├── [WS-TX]   enviarEstadoInicial(client)    → void
├── [WS-TX]   notificarD3Estado(exito)       → void
├── [WS-RX]   procesarMensajeWS(client, data, len) → void
├── [WS-EVT]  onWsEvent(...)                 → void (callback asíncrono)
│
├── setup()                                  → void
└── loop()                                   → void
```

### 4.3 Variables de estado global

```cpp
// Estado confirmado del sistema (escrito solo por loop() tras ACK de D1)
volatile uint8_t  colorActual  = 0;      // índice en COLORES[]
volatile bool     encendido    = false;  // true = LED de D1 encendido

// Handoff de comando WS → ciclo IR
// Escritas por callback WS (Core 0); leídas por loop() (Core 1)
volatile bool    irPendiente         = false;
volatile uint8_t r_pendiente         = 0;
volatile uint8_t g_pendiente         = 0;
volatile uint8_t b_pendiente         = 0;
volatile uint8_t colorIdx_pendiente  = 0;
volatile bool    encendido_pendiente = false;
```

El qualifier `volatile` es obligatorio para variables compartidas entre núcleos en ausencia de mutex. En ESP32, los accesos a `bool` y `uint8_t` son atómicos por arquitectura, lo que es suficiente dado que hay un único escritor por variable.

---

## 5. Configuración Wi-Fi: modo Access Point en el ESP32

### 5.1 Modo SoftAP del ESP32

El ESP32 soporta tres modos de operación Wi-Fi:

| Modo | Constante | Descripción |
|------|-----------|-------------|
| Station (STA) | `WIFI_STA` | Conecta a un AP existente como cliente |
| Access Point (AP) | `WIFI_AP` | Crea su propia red como punto de acceso |
| AP+STA | `WIFI_AP_STA` | Ambos simultáneamente |

D2 opera en modo **SoftAP puro** (`WIFI_AP`). No se conecta a ninguna red externa. Esta decisión garantiza:

- Funcionamiento **autónomo sin infraestructura externa**. El sistema es independiente de la disponibilidad de un router.
- **Sin salida a Internet**: la red creada por D2 es completamente aislada, lo que es necesario porque la interfaz web de D3 está embebida en flash y no tiene recursos externos.
- **Máxima simplicidad** de configuración: no se requieren credenciales de red del usuario.

### 5.2 Inicialización del Access Point

```cpp
WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
```

| Parámetro | Valor | Significado |
|-----------|-------|-------------|
| `AP_SSID` | `"Nombre-RED"` | SSID visible en la lista de redes del dispositivo cliente |
| `AP_PASSWORD` | `"Contraseña-RED"` | Contraseña WPA2 (mínimo 12 caracteres, configurar antes de compilar) |
| `AP_CHANNEL` | `6` | Canal Wi-Fi 2.4 GHz. Se eligió el canal 6 por ser uno de los tres canales no solapados (1, 6, 11) en la banda de 2.4 GHz |
| `hidden` | `0` | SSID visible (no oculto) |
| `AP_MAX_CONN` | `1` | Máximo de clientes concurrentes. Solo D3 se conecta; limitar a 1 previene conexiones no autorizadas |

### 5.3 Dirección IP del AP

El modo SoftAP del ESP32 asigna automáticamente la IP `192.168.4.1` al propio dispositivo cuando no se configura explícitamente. Esta es la IP por defecto del driver `esp_wifi` de Espressif. D3 accede a la interfaz web mediante:

```
http://192.168.4.1
```

El protocolo debe ser HTTP explícito (no HTTPS), ya que el ESP32 no sirve con TLS en esta configuración.

### 5.4 DHCP del AP

El ESP32 en modo SoftAP activa automáticamente un servidor DHCP que asigna IPs a los clientes en el rango `192.168.4.2` – `192.168.4.254`. D3 (el navegador) recibirá típicamente `192.168.4.2`.

### 5.5 Seguridad WPA2

El protocolo de seguridad es **WPA2-Personal** (IEEE 802.11i), el estándar de facto para redes Wi-Fi de infraestructura pequeña. WPA2 usa:

- Autenticación por **pre-shared key (PSK)**: la contraseña se comparte entre el AP (D2) y el cliente (D3).
- Cifrado **AES-CCMP** (Counter mode with CBC-MAC Protocol) para el tráfico de datos.
- **4-way handshake** para derivar claves de sesión únicas (PTK — Pairwise Transient Key) sin transmitir la PSK.

Requisito de contraseña implementado: mínimo 12 caracteres combinando mayúsculas, minúsculas, números y símbolos, conforme a la recomendación del estándar 802.11i.

### 5.6 Características del stack Wi-Fi del ESP32

El ESP32 implementa el stack Wi-Fi completo en firmware propietario de Espressif que corre íntegramente en el **Core 0**, junto con la pila TCP/IP lwIP. Este diseño tiene implicancias importantes:

- El stack Wi-Fi nunca bloquea al Core 1 (donde corre el loop de Arduino).
- Las interrupciones y eventos de red se procesan en el Core 0 mediante la tarea `wifi_task` y la tarea `tcpip_adapter`.
- ESPAsyncWebServer integra directamente con este modelo: todos sus callbacks también corren en el Core 0.
- `CORE_DEBUG_LEVEL=0` en los build flags deshabilita los mensajes de debug del core ESP32 en producción. Cambiar a `3` durante el desarrollo para ver logs detallados del stack Wi-Fi.

---

## 6. Servidor HTTP y servicio de la interfaz web

### 6.1 ESPAsyncWebServer: arquitectura asíncrona basada en eventos

A diferencia del `WebServer` bloqueante de Arduino, ESPAsyncWebServer nunca bloquea el Core 1. Su diseño se basa en:

- Un **listener TCP asíncrono** (AsyncTCP) que registra callbacks para eventos de red.
- Un **dispatcher de handlers** que despacha cada petición al handler registrado que coincida con la ruta y método HTTP.
- Los handlers se ejecutan en el contexto de la tarea de red del Core 0.

### 6.2 Registro de rutas y handlers

```cpp
// Handler WebSocket: registrado antes que las rutas HTTP
ws.onEvent(onWsEvent);
server.addHandler(&ws);

// Ruta raíz: sirve la interfaz web de D3 desde flash (PROGMEM)
server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", WEB_UI);
});

// Ruta catch-all para 404
server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
});

server.begin();  // Inicio efectivo del servidor en el puerto 80
```

### 6.3 Interfaz web D3 embebida en PROGMEM

La interfaz completa de D3 (HTML + CSS + JavaScript, aproximadamente 10 KB) se almacena como string literal en la sección PROGMEM de la flash del ESP32:

```cpp
// web_ui.h
static const char WEB_UI[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
...
)rawhtml";
```

El qualifier `PROGMEM` instrúye al compilador a ubicar el string en la flash en lugar de la SRAM. En el ESP32, esta distinción es menos crítica que en AVR (donde la SRAM es extremadamente limitada), pero sigue siendo buena práctica para strings grandes. `send_P()` lee directamente desde flash sin copiar a SRAM.

Esta arquitectura tiene una ventaja crítica: la interfaz web **funciona sin conexión a Internet**, lo que es necesario dado que D2 opera como AP aislado sin salida a la red pública.

---

## 7. Protocolo WebSocket con D3 (CRC-16/IBM)

### 7.1 WebSocket sobre HTTP (RFC 6455)

WebSocket es un protocolo de comunicación full-duplex sobre una conexión TCP única. El ciclo de vida es:

1. **Handshake HTTP Upgrade**: el cliente (D3) envía una petición HTTP `GET /ws` con los headers `Upgrade: websocket` y `Connection: Upgrade`. El servidor (D2) responde con `101 Switching Protocols`.
2. **Sesión WebSocket**: a partir de ese momento, ambos extremos pueden enviar frames en cualquier momento sin overhead de HTTP.
3. **Cierre**: cualquiera de los dos extremos envía un frame `CLOSE` con un código de 2 bytes (código de estado RFC 6455 §7.4). El receptor responde con su propio frame `CLOSE` y ambos cierran el TCP.

ESPAsyncWebServer gestiona el handshake automáticamente. El código de la aplicación solo necesita registrar el handler `onWsEvent` y llamar a `client->text()` o `client->close()`.

### 7.2 Evento onWsEvent: tipos de eventos

```cpp
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len)
```

| Evento | `type` | Acción en D2 |
|--------|--------|--------------|
| Nueva conexión | `WS_EVT_CONNECT` | Enciende LED_CONN, apaga LED_DISC, envía ESTADO_ACTUAL |
| Desconexión | `WS_EVT_DISCONNECT` | Apaga LED_CONN, enciende LED_DISC |
| Datos recibidos | `WS_EVT_DATA` | Valida y procesa solo frames de texto completos |
| Error de socket | `WS_EVT_ERROR` | Log del código y descripción |

Para `WS_EVT_DATA`, se verifica que el frame sea completo antes de procesarlo:

```cpp
AwsFrameInfo *info = (AwsFrameInfo *)arg;
if (info->final && info->index == 0 &&
    info->len == len && info->opcode == WS_TEXT) {
    procesarMensajeWS(client, data, len);
}
```

Esta guarda descarta frames fragmentados o frames binarios, procesando únicamente mensajes de texto completos.

### 7.3 Implementación del CRC-16/IBM

El CRC-16/IBM (también llamado CRC-16/ARC) se implementa íntegramente por software mediante XOR bit a bit. Los parámetros del estándar son:

| Parámetro | Valor |
|-----------|-------|
| Width | 16 bits |
| Poly | `0x8005` (x¹⁶ + x¹⁵ + x² + 1) |
| Init | `0x0000` |
| RefIn | `true` (LSB-first) |
| RefOut | `true` |
| XorOut | `0x0000` |

```cpp
uint16_t calcularCRC16(const char *payload, size_t len) {
    uint16_t crc = 0x0000;                    // Init = 0x0000

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint8_t)payload[i];           // XOR con byte actual

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {               // LSB=1: dividir en GF(2)
                crc = (crc >> 1) ^ 0x8005;
            } else {
                crc >>= 1;                    // LSB=0: solo desplazar
            }
        }
    }
    return crc;
}
```

Esta implementación es **simétrica** a la función `calcularCRC16()` del JavaScript de D3, garantizando compatibilidad cruzada sin librerías intermediarias. La elección de CRC-16 sobre CRC-8 para este enlace se justifica en que los payloads JSON superan los 119 bits donde CRC-8/MAXIM pierde su garantía de HD=4.

### 7.4 Patrón de serialización con CRC (dos pasos)

El CRC no puede incluirse en el dato sobre el que se calcula (dependencia circular). El proceso en D2 para enviar un mensaje es:

```cpp
void enviarMensajeWS(AsyncWebSocketClient *client, JsonDocument &doc) {
    // Paso 1: serializar sin crc16 → payload base
    char buf_base[300];
    size_t len_base = serializeJson(doc, buf_base, sizeof(buf_base));

    // Paso 2: calcular CRC-16 sobre el payload base
    uint16_t crc = calcularCRC16(buf_base, len_base);

    // Paso 3: agregar crc16 al documento
    doc["crc16"] = crc;

    // Paso 4: serializar el documento completo con crc16
    char buf_final[320];
    serializeJson(doc, buf_final, sizeof(buf_final));

    // Paso 5: transmitir al cliente
    client->text(buf_final);
}
```

El campo `"crc16"` siempre queda al **final** del JSON porque se agrega después de serializar el payload base. Este orden es determinista tanto en ArduinoJson v7 (que preserva el orden de inserción) como en `JSON.stringify()` de los navegadores modernos.

### 7.5 Verificación de CRC en mensajes recibidos de D3

```cpp
void procesarMensajeWS(AsyncWebSocketClient *client,
                       const uint8_t *payload, size_t len) {
    // 1. Deserializar
    JsonDocument doc;
    deserializeJson(doc, payload, len);

    // 2. Extraer CRC recibido
    uint16_t crc_rx = doc["crc16"].as<uint16_t>();

    // 3. Eliminar el campo crc16 del documento
    doc.remove("crc16");

    // 4. Serializar sin crc16 (replica el cálculo original de D3)
    char buf_check[300];
    size_t len_check = serializeJson(doc, buf_check, sizeof(buf_check));

    // 5. Recalcular CRC
    uint16_t crc_calc = calcularCRC16(buf_check, len_check);

    // 6. Comparar
    if (crc_rx != crc_calc) {
        // Enviar CRC_ERROR al cliente y descartar
        JsonDocument err_doc;
        err_doc["tipo"] = "CRC_ERROR";
        err_doc["info"] = "CRC-16 invalido — mensaje descartado";
        enviarMensajeWS(client, err_doc);
        return;
    }
    // 7. Procesar mensaje válido por tipo...
}
```

### 7.6 Contrato de orden de campos JSON

El CRC se calcula sobre el **string JSON exacto**. Si el orden de los campos difiere entre el emisor y el receptor al reconstruir el string base, el CRC no coincidirá aunque los datos sean semánticamente idénticos.

Contratos implementados:

- **ArduinoJson v7** preserva el orden de inserción de campos en `JsonDocument`. Cada función constructora de mensajes siempre agrega los campos en el mismo orden fijo.
- **JavaScript** en D3: `JSON.stringify()` preserva el orden de inserción en objetos literales (garantizado desde ES2015 en V8, SpiderMonkey y JavaScriptCore).
- `doc.remove("crc16")` en D2 elimina solo ese campo, preservando el orden de los restantes, replicando el estado del documento antes de que D3 agregara `crc16`.

### 7.7 Keepalive PING/PONG a nivel de aplicación

La API WebSocket del navegador no expone los frames de ping/pong del protocolo RFC 6455 (son transparentes al JavaScript). Se implementa un mecanismo equivalente a nivel de aplicación:

- D3 envía `{"tipo":"PING","crc16":N}` cada 5000 ms.
- D2 responde `{"tipo":"PONG","crc16":M}`.
- Si D3 no recibe el PONG en 3000 ms, considera la conexión muerta y cierra el socket.

D2 procesa el PING en el callback de eventos:

```cpp
if (strcmp(tipo, "PING") == 0) {
    JsonDocument pong;
    pong["tipo"] = "PONG";
    enviarMensajeWS(client, pong);
    return;
}
```

### 7.8 Handshake de desconexión voluntaria (Graceful Shutdown)

Ver sección 17.3 para la justificación completa. El flujo implementado es:

```
D3                                    D2
 │                                     │
 │── {"tipo":"CMD_DESCONECTAR"} ──────►│
 │                                     │  1. Valida CRC-16
 │                                     │  2. Prepara ACK_DESCONECTAR
 │◄── {"tipo":"ACK_DESCONECTAR"} ──────│
 │                                     │  3. client->close(1000, "...")
 │   ws.close() [disparado por D2]     │
 │                                     │  WS_EVT_DISCONNECT → LEDs actualizados
```

Código en D2:

```cpp
if (strcmp(tipo, MSG_CMD_DESCONECTAR) == 0) {
    JsonDocument ack_doc;
    ack_doc["tipo"] = "ACK_DESCONECTAR";
    enviarMensajeWS(client, ack_doc);
    client->close(1000, "Cierre solicitado por D3");
    return;
}
```

Llamar a `client->close()` desde el servidor garantiza que `WS_EVT_DISCONNECT` se dispara de forma determinística e inmediata en D2, actualizando los LEDs de estado sin latencia.

### 7.9 Throttle de comandos

Mientras hay un ciclo IR en progreso, D2 descarta los comandos CMD_COLOR, CMD_ENCENDER y CMD_APAGAR:

```cpp
if (irPendiente) {
    // Comando descartado: ciclo IR en progreso
    return;
}
```

Los mensajes PING y CMD_DESCONECTAR siempre se procesan aunque `irPendiente` sea `true`.

---

## 8. Protocolo IR con D1 (CRC-8/MAXIM)

### 8.1 Modulación NEC Raw y el periférico RMT

El protocolo IR utilizado es **NEC Raw** a 38 kHz. El periférico RMT (Remote Control Transceiver) del ESP32 genera y decodifica la modulación con precisión de microsegundos en hardware, sin consumir CPU:

- **Portadora**: 38 kHz (periodo de ~26.3 µs).
- **Modulación**: OOK (On-Off Keying) — la presencia de portadora representa un nivel lógico, la ausencia representa el otro.
- **Canal full/half-duplex**: el canal IR es físicamente half-duplex porque el mismo espectro óptico se comparte entre emisión y recepción.

### 8.2 Estructura de la trama IR de 32 bits

```
 Bit 31        Bit 24   Bit 23        Bit 16   Bit 15         Bit 8   Bit 7          Bit 0
┌─────────────────────┬─────────────────────┬─────────────────────┬─────────────────────┐
│   Componente R      │   Componente G      │   Componente B      │   Firma CRC-8       │
│      8 bits         │      8 bits         │      8 bits         │      8 bits         │
└─────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┘

Trama CMD  (D2 → D1):  bits[7:0] =  CRC8(R,G,B)    ← firma directa
Trama ACK  (D1 → D2):  bits[7:0] = ~CRC8(R,G,B)    ← firma invertida (~CRC)
```

El diseño sin campo de cabecera explícita transfiere toda la responsabilidad de integridad y autenticación al CRC. La asimetría `CRC` vs `~CRC` actúa como campo de dirección implícito.

### 8.3 CRC-8/MAXIM: implementación

Los parámetros del estándar implementado:

| Parámetro | Valor |
|-----------|-------|
| Width | 8 bits |
| Poly | `0x31` (x⁸ + x⁵ + x⁴ + 1) |
| Init | `0xFF` (modificado vs estándar, ver sección 17.1) |
| RefIn | `true` (LSB-first) |
| RefOut | `true` |
| XorOut | `0x00` |

```cpp
uint8_t calcularCRC8(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t crc = 0xFF;              // Init=0xFF: guarda contra eco nulo
    uint8_t datos[3] = { r, g, b };

    for (uint8_t i = 0; i < 3; i++) {
        crc ^= datos[i];             // XOR del byte de datos con el registro

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x01) {        // LSB=1: división en GF(2)
                crc = (crc >> 1) ^ 0x31;
            } else {
                crc >>= 1;           // LSB=0: solo desplazar
            }
        }
    }
    return crc;
}
```

Esta función es **idéntica** en D1 y D2 (mismo polinomio, mismo Init, misma lógica de bits), garantizando que ambos nodos calculan exactamente el mismo checksum para los mismos datos.

### 8.4 Construcción de la trama de COMANDO

```cpp
uint8_t crc_tx = calcularCRC8(r, g, b);

uint32_t trama = ((uint32_t)r      << 24) |
                 ((uint32_t)g      << 16) |
                 ((uint32_t)b      <<  8) |
                  (uint32_t)crc_tx;
```

Los casts explícitos a `uint32_t` antes del desplazamiento son **obligatorios en C++**: desplazar un `uint8_t` más de 7 posiciones es comportamiento indefinido (UB). El cast promueve el operando a 32 bits antes del shift.

### 8.5 Flujo de envío y espera de ACK (enviarColorIR)

```
┌──────────────────────────────────────────────────────────┐
│                    enviarColorIR(r, g, b)                │
│                                                          │
│  1. Calcular CRC8(r,g,b)                                 │
│  2. Construir trama CMD: [R][G][B][CRC8]                 │
│  3. IrReceiver.stop()     ← deshabilitar RX              │
│  4. IrSender.sendNECRaw() ← transmitir trama             │
│  5. IrReceiver.start()    ← habilitar RX para el ACK     │
│                                                          │
│  Bucle de espera (hasta TIMEOUT_ACK_MS = 2000 ms):       │
│  ┌────────────────────────────────────────────────────┐  │
│  │ IrReceiver.decode() → trama recibida               │  │
│  │ Filtrar repeticiones NEC (IRDATA_FLAGS_IS_REPEAT)  │  │
│  │ Leer decodedRawData; llamar resume()               │  │
│  │ Extraer r, g, b, crc_rx                            │  │
│  │ Calcular crc_base = CRC8(r,g,b)                    │  │
│  │ Calcular crc_esperado = ~crc_base                  │  │
│  │                                                    │  │
│  │ crc_rx == crc_base    → eco propio    → descartar  │  │
│  │ crc_rx != crc_esperado → corrupto     → descartar  │  │
│  │ crc_rx == crc_esperado → ACK legítimo              │  │
│  │   y r,g,b coinciden   → return true               │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  Agotado el timeout → return false                       │
└──────────────────────────────────────────────────────────┘
```

### 8.6 Ciclo IR con reintentos (ejecutarCicloIR)

El ciclo completo implementa hasta `MAX_REINTENTOS = 3` intentos con `DELAY_REINTENTO_MS = 300 ms` entre ellos:

```
Peor caso: 3 reintentos × (TIMEOUT_ACK_MS=2000 ms + DELAY_REINTENTO_MS=300 ms)
         = 3 × 2300 ms ≈ 6.9 segundos
```

Durante este tiempo, ESPAsyncWebServer continúa procesando eventos de red en el Core 0, de modo que las desconexiones de D3 son detectadas correctamente incluso mientras el ciclo IR está en progreso.

Si algún intento recibe ACK válido:
- `colorActual` y `encendido` son actualizados.
- `setColor(r, g, b)` aplica el color al LED RGB de D2 (espejo del estado de D1).
- `guardarNVS()` persiste el nuevo estado.
- `notificarD3Estado(true)` envía `RESULTADO_CMD` con `exito=true`.

Si todos los intentos fallan:
- El estado no cambia.
- `notificarD3Estado(false)` envía `RESULTADO_CMD` con `exito=false`.

### 8.7 Validación del ACK asimétrico

La lógica de tres vías de D2 al recibir cualquier trama durante la espera del ACK:

| Condición | Interpretación | Acción |
|-----------|---------------|--------|
| `crc_rx == CRC8(datos)` | Eco del propio CMD | Descartar, continuar esperando |
| `crc_rx != ~CRC8(datos)` | Trama corrupta o de otra fuente | Descartar, continuar esperando |
| `crc_rx == ~CRC8(datos)` AND `r==r_tx AND g==g_tx AND b==b_tx` | ACK legítimo de D1 | Aceptar, retornar `true` |

La imposibilidad matemática de que `x == ~x` para cualquier valor de 8 bits garantiza que un eco del CMD nunca pasará la validación del ACK.

---

## 9. Persistencia de estado: NVS / Preferences

### 9.1 NVS (Non-Volatile Storage) en el ESP32

El ESP32 incluye un subsistema de almacenamiento no volátil llamado **NVS** implementado sobre la flash interna. Sus características:

- **Wear-leveling automático**: el driver NVS distribuye las escrituras entre múltiples páginas de flash para extender la vida útil. No hay límite práctico de ciclos de escritura comparable al de la EEPROM de AVR.
- **Estructura clave-valor**: los datos se organizan en pares clave-valor dentro de **namespaces**. Un namespace es análogo a una sección o tabla.
- **Tipos soportados**: `uint8_t`, `uint16_t`, `uint32_t`, `int8_t`, `int16_t`, `int32_t`, `float`, `double`, `bool`, `string`, y blobs binarios.
- **Atomicidad**: cada operación `put` es atómica a nivel de página de flash.
- **Partición NVS**: ocupa una partición separada de la flash, definida en el esquema de particiones del ESP32 (por defecto en el `esp32dev`, hay 20 KB reservados para NVS).

### 9.2 API Preferences (abstracción sobre NVS)

La librería `Preferences` es la API de alto nivel del Arduino Core para acceder a la NVS:

```cpp
// Apertura del namespace en modo lectura/escritura
prefs.begin(NVS_NAMESPACE, false);

// Escritura de valores
prefs.putUChar("magic", NVS_MAGIC_VAL);  // uint8_t
prefs.putUChar("color_idx", colorActual);
prefs.putBool("encendido", encendido);

// Cierre (flush a flash)
prefs.end();
```

```cpp
// Apertura en modo solo lectura
prefs.begin(NVS_NAMESPACE, true);

// Lectura con valor por defecto
uint8_t magic = prefs.getUChar("magic", 0x00);
uint8_t idx   = prefs.getUChar("color_idx", 0);
bool    enc   = prefs.getBool("encendido", false);

prefs.end();
```

### 9.3 Esquema de datos en NVS

D2 almacena el **índice en la tabla COLORES[]** en lugar del triplete RGB completo, minimizando escrituras:

| Clave | Tipo | Valor | Propósito |
|-------|------|-------|-----------|
| `"magic"` | `uint8_t` | `0xA5` | Centinela de validez. Patrón alternante (01010101b) con baja probabilidad de corrupción aleatoria |
| `"color_idx"` | `uint8_t` | 0–11 | Índice del color activo en `COLORES[]` |
| `"encendido"` | `bool` | `true`/`false` | Estado de encendido |

El namespace de D2 es `"d2_state"` (distinto del `"d1_state"` de D1), evitando colisiones si ambos dispositivos usan el mismo ESP32 en un hipotético escenario de prueba.

### 9.4 Lógica de recuperación al arranque

```cpp
void cargarNVS() {
    prefs.begin(NVS_NAMESPACE, true);
    uint8_t magic = prefs.getUChar(NVS_KEY_MAGIC, 0x00);

    if (magic == NVS_MAGIC_VAL) {
        colorActual = prefs.getUChar(NVS_KEY_COLOR_IDX, 0);
        encendido   = prefs.getBool(NVS_KEY_ENCENDIDO, false);

        // Sanity check: evitar acceso fuera de bounds de COLORES[]
        if (colorActual >= NUM_COLORES) colorActual = 0;
    } else {
        // NVS vacía o corrompida: defaults seguros
        colorActual = 0;
        encendido   = false;
    }
    prefs.end();
}
```

El sanity check `colorActual >= NUM_COLORES` previene un acceso fuera de los límites del array `COLORES[]` si la NVS contiene un índice inválido (por ejemplo, si se cambia el número de colores entre versiones del firmware).

### 9.5 Persistencia solo en estado confirmado

`guardarNVS()` se llama **únicamente desde `ejecutarCicloIR()`**, y solo cuando D1 confirma el comando con un ACK válido. Nunca se persiste un estado que D1 no ha confirmado, garantizando que la NVS siempre refleja el último estado real del LED.

---

## 10. Control PWM del LED RGB (LEDC)

### 10.1 Periférico LEDC del ESP32

El periférico **LEDC** (LED Controller) del ESP32 proporciona 16 canales PWM independientes organizados en dos grupos de 8 (alta velocidad y baja velocidad). En el modo Arduino, se accede mediante la API `ledcSetup` / `ledcAttachPin` / `ledcWrite`.

Cada canal puede configurarse independientemente con:
- Frecuencia PWM (en Hz).
- Resolución del ciclo de trabajo (en bits, de 1 a 20).
- GPIO al que está vinculado.

### 10.2 Configuración de canales para el LED RGB de D2

```cpp
void iniciarLedc() {
    // Configurar frecuencia y resolución
    ledcSetup(LEDC_CH_R, 5000, 8);  // canal 3, 5 kHz, 8 bits
    ledcSetup(LEDC_CH_G, 5000, 8);  // canal 1
    ledcSetup(LEDC_CH_B, 5000, 8);  // canal 2

    // Vincular canal ↔ GPIO
    ledcAttachPin(PIN_R, LEDC_CH_R);  // GPIO 25 → canal 3
    ledcAttachPin(PIN_G, LEDC_CH_G);  // GPIO 26 → canal 1
    ledcAttachPin(PIN_B, LEDC_CH_B);  // GPIO 27 → canal 2
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LEDC_CH_R, r);  // ciclo de trabajo 0-255
    ledcWrite(LEDC_CH_G, g);
    ledcWrite(LEDC_CH_B, b);
}
```

### 10.3 Asignación de canales y conflicto con IRremote

IRremote usa internamente el **canal 0** del periférico LEDC para generar la portadora de 38 kHz del módulo RMT. Si el canal rojo del LED RGB se asignara también al canal 0, la escritura de `ledcWrite(0, valor)` interferiría con la portadora IR.

Solución: el canal rojo se asigna al canal 3 (`LEDC_CH_R = 3`), dejando el canal 0 exclusivamente para IRremote.

| Canal LEDC | Uso |
|------------|-----|
| 0 | Reservado internamente por IRremote (portadora 38 kHz) |
| 1 | LED RGB canal verde (GPIO 26) |
| 2 | LED RGB canal azul (GPIO 27) |
| 3 | LED RGB canal rojo (GPIO 25) |

### 10.4 Parámetros elegidos

- **Frecuencia 5000 Hz**: suficiente para evitar parpadeo visible (el umbral de percepción humana es ~50-100 Hz). A 5 kHz el parpadeo es completamente imperceptible.
- **Resolución 8 bits**: rango 0–255, compatible directamente con los valores RGB del protocolo IR (también 8 bits por canal). No es necesaria ninguna conversión.

---

## 11. LEDs de estado

D2 expone tres LEDs de indicación visual:

| LED | Color | GPIO | Indica |
|-----|-------|------|--------|
| LED_CONN | Verde | 32 | D3 tiene una sesión WebSocket activa |
| LED_DISC | Rojo | 33 | D3 se desconectó (voluntaria o abruptamente) |
| LED_AP | Amarillo | 14 | El Access Point Wi-Fi está activo y listo |

Los LEDs CONN y DISC se actualizan en el callback `onWsEvent`:

```cpp
case WS_EVT_CONNECT:
    setLedConn(true);
    setLedDisc(false);
    break;

case WS_EVT_DISCONNECT:
    setLedConn(false);
    setLedDisc(true);
    break;
```

El LED_AP se enciende al final de `setup()`, señalando que el sistema completó la inicialización y está listo para recibir conexiones.

Estos LEDs permiten verificar el estado del sistema sin necesidad del monitor serial durante la demostración.

---

## 12. Modelo de concurrencia: Core 0 y Core 1

### 12.1 Distribución de tareas

El ESP32 tiene dos núcleos Xtensa LX6. FreeRTOS (incluido en el ESP-IDF sobre el que corre el framework Arduino) asigna las tareas de la siguiente manera:

| Núcleo | Tareas |
|--------|--------|
| Core 0 | Stack Wi-Fi (esp_wifi_task), TCP/IP lwIP (tcpip_adapter), ESPAsyncWebServer callbacks, AsyncTCP |
| Core 1 | Tarea Arduino (setup + loop), ciclo IR (enviarColorIR, ejecutarCicloIR) |

### 12.2 Variables compartidas y atomicidad

Las variables `irPendiente`, `r_pendiente`, `g_pendiente`, `b_pendiente`, `colorIdx_pendiente` y `encendido_pendiente` son escritas por el callback `procesarMensajeWS` en el Core 0 y leídas por `loop()` en el Core 1.

El qualifier `volatile` garantiza que el compilador no optimice los accesos a estas variables (sin caché en registros entre lecturas). En la arquitectura Xtensa LX6 del ESP32, los accesos a `bool` y `uint8_t` son atómicos, por lo que no se requiere mutex para este patrón de un único escritor por variable.

### 12.3 Por qué el ciclo IR corre en el Core 1

El ciclo IR bloquea el Core 1 durante hasta ~6.9 segundos en el peor caso (ver sección 8.6). Si corriera en el Core 0, bloquearía el stack de red y ESPAsyncWebServer perdería eventos. Al correr en el Core 1:

- Las conexiones/desconexiones de D3 son detectadas correctamente incluso durante el ciclo IR.
- El PING/PONG de keepalive sigue siendo procesado.
- El `WS_EVT_DISCONNECT` se dispara sin latencia adicional.

### 12.4 cleanupClients y el heap de FreeRTOS

```cpp
ws.cleanupClients();
```

ESPAsyncWebServer mantiene internamente una lista de clientes WebSocket activos. Cuando un cliente se desconecta, su estructura no se libera inmediatamente: espera a que `cleanupClients()` sea llamado. Sin esta llamada periódica, el heap de FreeRTOS se fragmenta progresivamente con cada reconexión, pudiendo causar `malloc` failures y Guru Meditation Errors tras muchas reconexiones.

---

## 13. Secuencia de inicialización (setup)

```
setup()
│
├── 1. Serial.begin(115200)
│     → Puerto serie para depuración
│
├── 2. iniciarLedc()
│     → Configurar 3 canales LEDC (R=canal3, G=canal1, B=canal2)
│     → Vincular GPIO 25, 26, 27
│
├── 3. pinMode + digitalWrite para LEDs de estado
│     → PIN_LED_CONN (32), PIN_LED_DISC (33), PIN_LED_AP (14) → LOW
│
├── 4. IrReceiver.begin(IR_RECV_PIN, DISABLE_LED_FEEDBACK)
│     → Iniciar receptor IR en GPIO 5
│     → DISABLE_LED_FEEDBACK: evitar uso del pin 13
│
├── 5. cargarNVS()
│     → Leer colorActual e encendido desde flash
│     → Si NVS vacía: colorActual=0, encendido=false
│
├── 6. setColor(...)
│     → Si encendido: aplicar COLORES[colorActual].r/g/b al LED
│     → Si apagado: setColor(0,0,0)
│
├── 7. WiFi.softAP(SSID, PASSWORD, CHANNEL, 0, MAX_CONN)
│     → Crear red WPA2 en canal 6
│     → IP asignada automáticamente: 192.168.4.1
│
├── 8. ws.onEvent(onWsEvent)
│   server.addHandler(&ws)
│     → Registrar handler WebSocket en "/ws"
│
├── 9. server.on("/", HTTP_GET, ...)
│   server.onNotFound(...)
│     → Registrar ruta raíz (sirve WEB_UI desde PROGMEM)
│     → Registrar handler 404
│
├── 10. server.begin()
│      → Iniciar servidor HTTP+WS en puerto 80
│
└── 11. digitalWrite(PIN_LED_AP, HIGH)
       → Sistema completamente listo para conexiones
```

### 13.1 Orden crítico de inicialización

El orden es importante por dos razones:

1. `config.h` debe incluirse **antes** de `IRremote.hpp`: IRremote.hpp lee la macro `IR_SEND_PIN` durante el preprocesamiento para configurar el canal RMT del ESP32. Si `config.h` se incluye después, `IR_SEND_PIN` no estará definido y IRremote usará un pin por defecto incorrecto.

2. `ws.onEvent()` y `server.addHandler(&ws)` deben llamarse **antes** de `server.begin()`. Una vez iniciado el servidor, los handlers no pueden registrarse dinámicamente.

---

## 14. Ciclo principal (loop)

```cpp
void loop() {
    // Liberar recursos de clientes WS desconectados
    ws.cleanupClients();

    // Ejecutar ciclo IR si hay comando pendiente de D3
    if (irPendiente) {
        irPendiente = false;   // limpiar ANTES de ejecutar (evitar re-entrada)
        ejecutarCicloIR();
    }

    // Ceder CPU al scheduler FreeRTOS (watchdog idle Core 1)
    delay(5);
}
```

El `delay(5)` es intencional: cede la CPU al scheduler de FreeRTOS para que las tareas del sistema (watchdog idle, gestión de memoria, etc.) del Core 1 puedan ejecutarse. Sin él, el loop correría a máxima velocidad consumiendo innecesariamente la CPU.

El flag `irPendiente` se limpia **antes** de llamar a `ejecutarCicloIR()` para evitar re-entrada: si durante la ejecución del ciclo IR llega un nuevo comando de D3, `irPendiente` volvería a ponerse en `true` (escrito por el Core 0). Al limpiar antes de ejecutar, el nuevo comando quedará procesado en la siguiente iteración del loop. Si se limpiara después, el nuevo comando se perdería.

---

## 15. Paleta de colores y tabla de referencia

La tabla `COLORES[]` en `config.h` define los 12 colores disponibles:

```cpp
static const ColorRGB COLORES[NUM_COLORES] = {
    // Colores originales (TP2)
    { "ROJO",     255,   0,   0 },
    { "AMARILLO", 255, 200,   0 },
    { "VERDE",      0, 255,   0 },
    { "CELESTE",    0, 255, 255 },
    { "AZUL",       0,   0, 255 },
    { "LILA",     180,   0, 255 },
    { "BLANCO",   255, 255, 255 },
    { "ROSA",     255, 105, 180 },
    // Colores nuevos
    { "NARANJA",  255,  80,   0 },
    { "MAGENTA",  255,   0, 180 },
    { "TURQUESA",   0, 210, 140 },
    { "VIOLETA",   90,   0, 200 },
};
```

La tabla es `static const` y reside en la sección de solo lectura de la flash del ESP32. Los índices 0–7 corresponden a los colores del TP2; los índices 8–11 son nuevos.

**Sincronización requerida**: la misma tabla de colores (nombres y valores RGB) debe mantenerse sincronizada con el array `COLORES` del JavaScript en `web_ui.h`. Cualquier discrepancia causará que el CRC-16 de D3 cubra valores distintos a los que D2 buscará en su tabla.

El comando `CMD_COLOR` incluye los campos `r`, `g`, `b` además del nombre simbólico, lo que permite que el CRC-16 cubra los valores numéricos del triplete. D2 usa `r/g/b` del mensaje directamente para el ciclo IR, garantizando que los valores realmente recibidos son los que se transmiten a D1.

---

## 16. Tabla de mensajes JSON del protocolo D2 ↔ D3

### 16.1 D3 → D2 (comandos)

| `tipo` | Campos adicionales | Descripción |
|--------|--------------------|-------------|
| `CMD_COLOR` | `color`, `r`, `g`, `b`, `crc16` | Cambiar al color indicado. Incluye triplete RGB numérico para cobertura completa del CRC |
| `CMD_ENCENDER` | `crc16` | Encender el LED con el último color registrado en D2 |
| `CMD_APAGAR` | `crc16` | Apagar el LED (envía RGB={0,0,0} a D1) |
| `CMD_DESCONECTAR` | `crc16` | Iniciar handshake de desconexión voluntaria |
| `PING` | `crc16` | Keepalive de aplicación |

### 16.2 D2 → D3 (respuestas)

| `tipo` | Campos adicionales | Descripción |
|--------|--------------------|-------------|
| `ESTADO_ACTUAL` | `encendido`, `color`, `r`, `g`, `b`, `crc16` | Estado completo enviado al conectarse D3 |
| `RESULTADO_CMD` | `exito`, `encendido`, `color`, `r`, `g`, `b`, `crc16` | Resultado del ciclo IR. `exito=true` solo si D1 confirmó con ACK |
| `CRC_ERROR` | `info`, `crc16` | El mensaje recibido tenía CRC-16 inválido |
| `ACK_DESCONECTAR` | `crc16` | Confirmación de CMD_DESCONECTAR. D2 cerrará la sesión después |
| `PONG` | `crc16` | Respuesta al keepalive PING |

### 16.3 Ejemplo de intercambio completo para CMD_COLOR

```
D3 → D2:
{"tipo":"CMD_COLOR","color":"VERDE","r":0,"g":255,"b":0,"crc16":XXXX}

D2 valida CRC, ejecuta ciclo IR (hasta 3 intentos), D1 confirma con ACK ~CRC.

D2 → D3:
{"tipo":"RESULTADO_CMD","exito":true,"encendido":true,"color":"VERDE","r":0,"g":255,"b":0,"crc16":YYYY}
```

---

## 17. Problemas detectados y soluciones implementadas

### 17.1 Problema 1: Eco nulo del receptor KY-022 (tormenta de broadcast)

**Síntoma observado**: tras reactivar `IrReceiver.start()` después de una transmisión, el sistema procesaba comandos espurios con RGB={0,0,0} que hacían que el LED de D1 se apagara sin que D3 hubiera enviado ningún comando de apagado.

**Causa raíz**: al reactivar el receptor KY-022, el circuito analógico interno del TSOP generaba un pulso espurio en la transición, capturado por IRremote como la trama nula `0x00000000`. Con `Init=0x00`:

```
CRC8_init0(0x00, 0x00, 0x00) = 0x00
Trama nula = 0x00000000 → CRC_rx = 0x00 = CRC_calculado → ¡VALIDACIÓN FALSA!
```

**Solución**: cambiar `Init` de `0x00` a `0xFF` (estándar CRC-8/MAXIM). Con `Init=0xFF`:

```
CRC8_init_FF(0x00, 0x00, 0x00) ≠ 0x00
Trama nula = 0x00000000 → CRC_rx = 0x00 ≠ CRC_calculado → rechazada correctamente
```

Este cambio no agrega lógica condicional. Solo modifica el valor semilla del registro CRC, eliminando la colisión sin overhead.

### 17.2 Problema 2: Self-ACK (Falso Positivo por Eco óptico)

**Síntoma observado**: al tapar físicamente el receptor KY-022 de D1 (impidiendo que D1 reciba el comando), D2 igualmente actualizaba su estado y el LED de D2 cambiaba de color. D2 "confirmaba" un ACK que nunca existió.

**Causa raíz**: el protocolo original usaba la misma estructura `[R][G][B][CRC8]` para COMANDO y ACK. El eco óptico del propio comando de D2, reflejado por superficies del entorno, tenía exactamente el mismo formato que un ACK legítimo. D2 no podía distinguirlos matemáticamente.

**Solución**: asimetría de protocolo. D1 responde con el CRC **invertido bit a bit** (~CRC):

```
CMD  (D2 → D1): [R][G][B][ CRC8(R,G,B)]   → firma directa
ACK  (D1 → D2): [R][G][B][~CRC8(R,G,B)]   → firma invertida
```

La imposibilidad de que `x == ~x` para cualquier `x` de 8 bits garantiza que el eco del CMD (con firma `CRC8`) nunca pasará la validación del ACK (que espera `~CRC8`).

D2 implementa la validación en tres vías determinísticas:

```cpp
if (ack_crc == crc_base)     { continue; }  // eco → descartar
if (ack_crc != crc_esperado) { continue; }  // corrupto → descartar
if (r/g/b coinciden)         { return true; } // ACK válido → aceptar
```

### 17.3 Problema 3: Latencia en LEDs de estado al desconectar D3

**Síntoma observado**: al presionar "Desconectar" en D3, los LEDs de estado de D2 (LED_CONN y LED_DISC) no se actualizaban hasta varios segundos después.

**Causa raíz**: al llamar `ws.close()` desde el navegador, los navegadores envían el frame `CLOSE` del protocolo WebSocket, pero el socket TCP subyacente no se cierra inmediatamente. Esperan el frame `CLOSE` de confirmación del servidor. ESPAsyncWebServer procesaba este cierre de forma diferida (asimétrica), retrasando la generación del evento `WS_EVT_DISCONNECT` que actualiza los LEDs.

**Solución**: Graceful Shutdown iniciado desde el servidor (D2). El cierre iniciado por `client->close()` desde el servidor genera `WS_EVT_DISCONNECT` de forma síncrona e inmediata:

```
D3 → CMD_DESCONECTAR → D2 valida CRC
D2 → ACK_DESCONECTAR → D3
D2: client->close(1000) → WS_EVT_DISCONNECT inmediato → LEDs actualizados sin latencia
D3: ws.onclose() disparado → actualiza badge, banner, botones
```

---

## 18. Parámetros de compilación y configuración

### 18.1 Constantes configurables en config.h

Antes de compilar, completar obligatoriamente:

```cpp
// ── Red Wi-Fi ──────────────────────────────────────────
#define AP_SSID       "Nombre-RED"       // SSID visible en la lista de redes
#define AP_PASSWORD   "Contraseña-RED"   // WPA2, mínimo 12 caracteres
#define AP_CHANNEL    6                  // Canal no solapado (1, 6 u 11)
#define AP_MAX_CONN   1                  // Solo D3 se conecta

// ── Tiempos del protocolo IR ───────────────────────────
#define DELAY_ANTES_ACK_MS   120   // Margen para que D2 reactive su receptor
#define TIMEOUT_ACK_MS       2000  // Timeout máximo de espera por ACK
#define DELAY_REINTENTO_MS   300   // Pausa entre reintentos
#define MAX_REINTENTOS       3     // Intentos antes de reportar fallo
```

### 18.2 Build flags

```ini
build_flags =
    -DCORE_DEBUG_LEVEL=0  ; producción: sin logs del core ESP32
    ; Para depuración, cambiar a:
    ; -DCORE_DEBUG_LEVEL=3  ; verbose: logs detallados del stack Wi-Fi
```

### 18.3 Resumen de parámetros CRC

| Enlace | CRC | Poly | Init | Distancia Hamming | Longitud cubierta |
|--------|-----|------|------|-------------------|-------------------|
| D2 ↔ D1 (IR) | CRC-8/MAXIM | `0x31` | `0xFF` | HD=4 | ≤ 119 bits (3 bytes) |
| D2 ↔ D3 (WS) | CRC-16/IBM | `0x8005` | `0x0000` | HD=4 | ≤ 32.767 bits (JSON) |

### 18.4 Acceso a la interfaz web

Desde cualquier dispositivo conectado a la red Wi-Fi de D2:

```
URL: http://192.168.4.1
Protocolo: HTTP (sin HTTPS)
WebSocket: ws://192.168.4.1/ws
```

En iOS Safari: desactivar temporalmente "Limit IP Address Tracking" en la configuración de la red Wi-Fi de D2 si la interfaz no carga.

---

*Documentación elaborada en el marco del TP3 Integrador — Comunicación de Datos, Ingeniería en Computación.*  
*Bevilacqua, Francisco — Clement, Sebastián — Junio 2026.*  
*Universidad Nacional de Rafaela | Docentes: Mg. Ing. Martín Pico · Ing. Milton Pozzo*