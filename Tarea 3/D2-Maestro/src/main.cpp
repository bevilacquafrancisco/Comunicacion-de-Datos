/**
 * @file    main.cpp
 * @brief   Dispositivo 2 - Maestro IR + Servidor Wi-Fi/WebSocket (ESP32).
 *
 * @section descripcion Descripcion funcional
 *
 *  Este firmware implementa el nodo central del sistema TP3. Actua
 *  simultaneamente como:
 *
 *  a) MAESTRO IR: genera tramas de color en formato NEC Raw de 32 bits,
 *     las envia al D1 por infrarrojo y gestiona el protocolo de ACK
 *     con reintentos, tal como se implemento en el TP2.
 *
 *  b) SERVIDOR WI-FI: crea un Access Point WPA2 propio, levanta un
 *     servidor HTTP que sirve la interfaz web del D3, y gestiona una
 *     conexion WebSocket persistente para comunicacion full-duplex
 *     en tiempo real con el cliente (D3).
 *
 * @section flujo Flujo general
 *
 *  1. setup(): inicializar hardware -> recuperar NVS -> levantar AP -> servidor.
 *  2. loop(): limpiar clientes WS + ejecutar ciclo IR cuando hay comando pendiente.
 *  3. onWsEvent() [callback asincrono]:
 *     - CONNECT    -> enviar ESTADO_ACTUAL, actualizar LEDs.
 *     - DISCONNECT -> actualizar LEDs.
 *     - DATA       -> parsear JSON -> marcar irPendiente.
 *
 * @section concurrencia Modelo de concurrencia
 *
 *  ESPAsyncWebServer gestiona los eventos de red en el Core 0 del ESP32
 *  (tarea de la pila lwIP/TCP), mientras que loop() corre en el Core 1
 *  (tarea de Arduino). Los accesos a las variables compartidas
 *  (irPendiente, colorPendiente, etc.) se coordinan mediante flags atomicos.
 *  Para esta version v1 no se requiere mutex explicito porque:
 *   - El callback WS solo ESCRIBE irPendiente = true y los pendientes.
 *   - El loop solo LEE irPendiente y luego lo pone en false.
 *   - El ESP32 garantiza atomicidad de lectura/escritura en bool/uint8_t.
 *
 * @section hardware Hardware
 *
 *  Modulo               | Pin ESP32 |         Notas
 *  ---------------------+-----------+----------------------------------------
 *  KY-005 S             | GPIO  4   | Emisor IR, RMT hardware
 *  KY-022 OUT           | GPIO  5   | Receptor IR, interrupt-capable
 *  LED RGB R            | GPIO 25   | LEDC canal 3
 *  LED RGB G            | GPIO 26   | LEDC canal 1
 *  LED RGB B            | GPIO 27   | LEDC canal 2
 *  LED_CONN (verde)     | GPIO 32   | D3 conectado
 *  LED_DISC (rojo)      | GPIO 33   | D3 desconectado
 *  LED_AP   (amarillo)  | GPIO 14   | AP activo
 *  VCC modulos IR       | 3.3V      |
 *  GND                  | GND       | Comun a todos
 *
 * @section dependencias Dependencias (platformio.ini)
 *
 *  - IRremote >= 4.4.0        (IRremote/IRremote)
 *  - AsyncTCP >= 1.1.1        (me-no-dev/AsyncTCP)
 *  - ESPAsyncWebServer >= 1.2 (me-no-dev/ESPAsyncWebServer)
 *  - ArduinoJson >= 7.0       (bblanchon/ArduinoJson)
 *  - Preferences              (incluida en ESP32 Arduino Core)
 *
 * @authors Bevilacqua, Francisco - Clement, Sebastian
 * @date    7 de junio 2026
 */

// config.h debe ser el PRIMER include: define IR_SEND_PIN que IRremote.hpp
// lee en tiempo de preprocesamiento para configurar el canal RMT del ESP32.
#include "config.h"

#include <Arduino.h>
#include <IRremote.hpp>        // despues de config.h (necesita IR_SEND_PIN)
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "web_ui.h"            // interfaz D3 embebida en flash

// =============================================================================
//  OBJETOS GLOBALES
// =============================================================================

AsyncWebServer  server(SERVER_PORT);
AsyncWebSocket  ws(WS_PATH);
Preferences     prefs;

// =============================================================================
//  ESTADO DEL SISTEMA
// =============================================================================

volatile uint8_t  colorActual  = 0;       // Indice en COLORES[] del ultimo color confirmado
volatile bool     encendido    = false;   // true = LED D1 encendido con colorActual

// Variables de comando pendiente: escritas por el callback WS,
// leidas y limpiadas por el loop principal.
volatile bool    irPendiente        = false;
volatile uint8_t r_pendiente        = 0;
volatile uint8_t g_pendiente        = 0;
volatile uint8_t b_pendiente        = 0;
volatile uint8_t colorIdx_pendiente = 0;
volatile bool    encendido_pendiente = false;

// =============================================================================
//  PROTOTIPOS
// =============================================================================

void  iniciarLedc();
void  setColor(uint8_t r, uint8_t g, uint8_t b);
void  guardarNVS();
void  cargarNVS();
bool  enviarColorIR(uint8_t r, uint8_t g, uint8_t b);
void  ejecutarCicloIR();
void  notificarD3Estado(bool exito);
void  enviarEstadoInicial(AsyncWebSocketClient *client);
void  onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len);
void  procesarMensajeWS(AsyncWebSocketClient *client,
                          uint8_t *payload, size_t len);
void  setLedConn(bool on);
void  setLedDisc(bool on);

// =============================================================================
//  LEDC / PWM
// =============================================================================

/**
 * @brief Configura los tres canales LEDC para el LED RGB.
 *
 * El modulo LEDC del ESP32 reemplaza a analogWrite() de AVR.
 * Se usa resolucion de 8 bits (0-255) para compatibilidad directa
 * con los valores RGB del protocolo.
 */
void iniciarLedc() {
    ledcSetup(LEDC_CH_R, LEDC_FREQ_HZ, LEDC_RESOLUTION);
    ledcSetup(LEDC_CH_G, LEDC_FREQ_HZ, LEDC_RESOLUTION);
    ledcSetup(LEDC_CH_B, LEDC_FREQ_HZ, LEDC_RESOLUTION);
    ledcAttachPin(PIN_R, LEDC_CH_R);
    ledcAttachPin(PIN_G, LEDC_CH_G);
    ledcAttachPin(PIN_B, LEDC_CH_B);
}

/**
 * @brief Aplica un color RGB al LED del D2 mediante LEDC.
 * @param r  [0-255] canal rojo
 * @param g  [0-255] canal verde
 * @param b  [0-255] canal azul
 */
void setColor(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LEDC_CH_R, r);
    ledcWrite(LEDC_CH_G, g);
    ledcWrite(LEDC_CH_B, b);
}

// =============================================================================
//  LEDS DE ESTADO
// =============================================================================

/** @brief Controla el LED verde (D3 conectado). */
void setLedConn(bool on) { digitalWrite(PIN_LED_CONN, on ? HIGH : LOW); }

/** @brief Controla el LED rojo (desconexion detectada). */
void setLedDisc(bool on) { digitalWrite(PIN_LED_DISC, on ? HIGH : LOW); }

// =============================================================================
//  PERSISTENCIA NVS
// =============================================================================

/**
 * @brief Persiste el estado actual (colorActual + encendido) en NVS.
 *
 * Solo se guarda el indice de color y el booleano de encendido.
 * Los valores RGB se reconstruyen desde COLORES[colorActual],
 * minimizando escrituras en flash.
 */
void guardarNVS() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar(NVS_KEY_MAGIC,     NVS_MAGIC_VAL);
    prefs.putUChar(NVS_KEY_COLOR_IDX, colorActual);
    prefs.putBool (NVS_KEY_ENCENDIDO, encendido);
    prefs.end();
    Serial.printf("[NVS] Guardado idx=%u encendido=%s\n",
                  colorActual, encendido ? "SI" : "NO");
}

/**
 * @brief Recupera el estado desde NVS al inicio.
 *
 * Si la NVS esta vacia o corrupta (magic incorrecto), inicializa
 * con valores por defecto: indice 0 (Rojo), LED apagado.
 */
void cargarNVS() {
    prefs.begin(NVS_NAMESPACE, true);
    uint8_t magic = prefs.getUChar(NVS_KEY_MAGIC, 0x00);
    if (magic == NVS_MAGIC_VAL) {
        colorActual = prefs.getUChar(NVS_KEY_COLOR_IDX, 0);
        encendido   = prefs.getBool (NVS_KEY_ENCENDIDO,  false);
        if (colorActual >= NUM_COLORES) colorActual = 0;  // sanity check
        Serial.printf("[NVS] Recuperado idx=%u encendido=%s\n",
                      colorActual, encendido ? "SI" : "NO");
    } else {
        colorActual = 0;
        encendido   = false;
        Serial.println("[NVS] Vacia - iniciando con defaults");
    }
    prefs.end();
}

// =============================================================================
//  PROTOCOLO IR - ENVIO Y ACK
// =============================================================================

/**
 * @brief Envia una trama IR y espera el ACK del Esclavo.
 *
 * Protocolo de handshaking:
 *  1. Construye la trama de 32 bits: [0xAA][R][G][B].
 *  2. Detiene el receptor (half-duplex): evita eco propio en KY-022.
 *  3. Transmite la trama con sendNECRaw (sin repeticiones automaticas).
 *  4. Reactiva el receptor para capturar el ACK del Esclavo.
 *  5. Bucle de espera hasta TIMEOUT_ACK_MS:
 *     - Filtra repeticiones NEC.
 *     - Valida cabecera 0xBB y coincidencia byte a byte de R, G, B.
 *     - Retorna true en el primer ACK valido.
 *  6. Retorna false si el timeout expira sin ACK valido.
 *
 * @param r  Componente rojo   [0-255]
 * @param g  Componente verde  [0-255]
 * @param b  Componente azul   [0-255]
 * @return   true si ACK valido recibido; false si timeout.
 */
bool enviarColorIR(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t trama = ((uint32_t)HDR_CMD << 24) |
                     ((uint32_t)r        << 16) |
                     ((uint32_t)g        <<  8) |
                      (uint32_t)b;

    Serial.printf("[IR-TX] Enviando trama: 0x%08X\n", trama);

    IrReceiver.stop();
    IrSender.sendNECRaw(trama, 0);   // 0 = sin repeticiones automaticas
    IrReceiver.start();

    unsigned long t0 = millis();
    while ((millis() - t0) < TIMEOUT_ACK_MS) {
        if (!IrReceiver.decode()) continue;

        // Filtrar repeticiones automaticas NEC
        if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
            IrReceiver.resume();
            continue;
        }

        uint32_t resp  = IrReceiver.decodedIRData.decodedRawData;
        uint8_t  cab   = (resp >> 24) & 0xFF;
        uint8_t  ack_r = (resp >> 16) & 0xFF;
        uint8_t  ack_g = (resp >>  8) & 0xFF;
        uint8_t  ack_b =  resp        & 0xFF;
        IrReceiver.resume();

        Serial.printf("[IR-RX] Respuesta: 0x%08X | cab=0x%02X R=%u G=%u B=%u\n",
                      resp, cab, ack_r, ack_g, ack_b);

        if (cab == HDR_ACK && ack_r == r && ack_g == g && ack_b == b) {
            Serial.println("[IR] ACK valido recibido");
            return true;
        }
        Serial.println("[IR] ACK invalido - ignorado");
    }

    Serial.println("[IR] Timeout - sin ACK");
    return false;
}

/**
 * @brief Ejecuta el ciclo completo de envio IR con reintentos.
 *
 * Llamado desde el loop() cuando irPendiente == true.
 * Intenta enviar el color pendiente hasta MAX_REINTENTOS veces.
 *
 * Si algún intento recibe ACK valido:
 *   - Actualiza colorActual y encendido.
 *   - Aplica el color al LED del D2.
 *   - Persiste en NVS.
 *   - Notifica a D3 con RESULTADO_CMD exito=true.
 *
 * Si ningun intento tiene ACK:
 *   - El estado no cambia.
 *   - Notifica a D3 con RESULTADO_CMD exito=false.
 */
void ejecutarCicloIR() {
    uint8_t r   = r_pendiente;
    uint8_t g   = g_pendiente;
    uint8_t b   = b_pendiente;
    uint8_t ci  = colorIdx_pendiente;
    bool    enc = encendido_pendiente;

    bool ack = false;

    for (int i = 0; i < MAX_REINTENTOS && !ack; i++) {
        if (i > 0) {
            Serial.printf("[IR] Reintento %d/%d\n", i, MAX_REINTENTOS - 1);
            delay(DELAY_REINTENTO_MS);
        }
        ack = enviarColorIR(r, g, b);
    }

    if (ack) {
        colorActual = ci;
        encendido   = enc;
        setColor(r, g, b);
        guardarNVS();
        Serial.printf("[D2] Estado actualizado: idx=%u enc=%s R=%u G=%u B=%u\n",
                      colorActual, encendido ? "SI" : "NO", r, g, b);
    } else {
        Serial.println("[D2] Sin ACK - estado sin cambios");
    }

    notificarD3Estado(ack);
}

// =============================================================================
//  WEBSOCKET - NOTIFICACIONES HACIA D3
// =============================================================================

/**
 * @brief Serializa el estado y lo envia a todos los clientes WS conectados.
 *
 * Construye un mensaje JSON RESULTADO_CMD con los campos:
 * tipo, exito, encendido, color (nombre), r, g, b.
 *
 * Usa JsonDocument para evitar fragmentacion de heap en el ESP32.
 *
 * @param exito  true si el ultimo comando IR fue confirmado por D1.
 */
void notificarD3Estado(bool exito) {
    JsonDocument doc;
    doc["tipo"]      = MSG_RESULTADO_CMD;
    doc["exito"]     = exito;
    doc["encendido"] = (bool)encendido;
    doc["color"]     = encendido ? COLORES[colorActual].nombre : "";
    doc["r"]         = encendido ? COLORES[colorActual].r : 0;
    doc["g"]         = encendido ? COLORES[colorActual].g : 0;
    doc["b"]         = encendido ? COLORES[colorActual].b : 0;

    char buf[200];
    serializeJson(doc, buf, sizeof(buf));
    ws.textAll(buf);
    Serial.printf("[WS-TX] RESULTADO_CMD -> %s\n", buf);
}

/**
 * @brief Envia el estado actual al cliente recien conectado.
 *
 * Llamado en WS_EVT_CONNECT para que D3 muestre el estado real
 * del sistema inmediatamente al conectarse.
 *
 * @param client  Puntero al cliente WebSocket recien conectado.
 */
void enviarEstadoInicial(AsyncWebSocketClient *client) {
    JsonDocument doc;
    doc["tipo"]      = MSG_ESTADO_ACTUAL;
    doc["encendido"] = (bool)encendido;
    doc["color"]     = encendido ? COLORES[colorActual].nombre : "";
    doc["r"]         = encendido ? COLORES[colorActual].r : 0;
    doc["g"]         = encendido ? COLORES[colorActual].g : 0;
    doc["b"]         = encendido ? COLORES[colorActual].b : 0;

    char buf[200];
    serializeJson(doc, buf, sizeof(buf));
    client->text(buf);
    Serial.printf("[WS-TX] ESTADO_ACTUAL -> cliente #%u: %s\n",
                  client->id(), buf);
}

// =============================================================================
//  WEBSOCKET - PROCESAMIENTO DE MENSAJES ENTRANTES
// =============================================================================

/**
 * @brief Parsea y procesa un mensaje JSON recibido desde D3.
 *
 * Tipos de mensaje reconocidos:
 *  - CMD_COLOR    : cambiar al color indicado en el campo "color".
 *  - CMD_ENCENDER : encender LED con el ultimo colorActual.
 *  - CMD_APAGAR   : apagar LED (RGB = 0,0,0).
 *  - PING         : responder PONG para el keepalive del cliente.
 *
 * Tras determinar el comando, carga las variables pendientes y activa
 * irPendiente para que el loop() ejecute el ciclo IR.
 *
 * @param client   Cliente WS que envio el mensaje.
 * @param payload  Buffer con el texto JSON.
 * @param len      Longitud del payload.
 */
void procesarMensajeWS(AsyncWebSocketClient *client,
                        uint8_t *payload, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        Serial.printf("[WS-RX] Error JSON: %s\n", err.c_str());
        return;
    }

    const char *tipo = doc["tipo"] | "";
    Serial.printf("[WS-RX] tipo=%s\n", tipo);

    // Keepalive: responder PONG
    if (strcmp(tipo, "PING") == 0) {
        client->text("{\"tipo\":\"PONG\"}");
        return;
    }

    // Si ya hay un comando en vuelo, descartar el nuevo
    if (irPendiente) {
        Serial.println("[D2] Comando descartado: ciclo IR en progreso");
        return;
    }

    if (strcmp(tipo, MSG_CMD_COLOR) == 0) {
        // Buscar el color en la tabla por nombre
        const char *nombreColor = doc["color"] | "";
        int idx = -1;
        for (int i = 0; i < NUM_COLORES; i++) {
            if (strcmp(COLORES[i].nombre, nombreColor) == 0) {
                idx = i; break;
            }
        }
        if (idx < 0) {
            Serial.printf("[D2] Color desconocido: %s\n", nombreColor);
            return;
        }
        colorIdx_pendiente   = (uint8_t)idx;
        r_pendiente          = COLORES[idx].r;
        g_pendiente          = COLORES[idx].g;
        b_pendiente          = COLORES[idx].b;
        encendido_pendiente  = true;
        irPendiente          = true;

    } else if (strcmp(tipo, MSG_CMD_ENCENDER) == 0) {
        // Encender con el ultimo color conocido
        colorIdx_pendiente   = colorActual;
        r_pendiente          = COLORES[colorActual].r;
        g_pendiente          = COLORES[colorActual].g;
        b_pendiente          = COLORES[colorActual].b;
        encendido_pendiente  = true;
        irPendiente          = true;

    } else if (strcmp(tipo, MSG_CMD_APAGAR) == 0) {
        // Apagar: enviar RGB={0,0,0}, preservar colorActual para reencendido
        colorIdx_pendiente   = colorActual;
        r_pendiente          = 0;
        g_pendiente          = 0;
        b_pendiente          = 0;
        encendido_pendiente  = false;
        irPendiente          = true;

    } else {
        Serial.printf("[D2] Tipo desconocido: %s\n", tipo);
    }
}

// =============================================================================
//  WEBSOCKET - MANEJADOR DE EVENTOS (callback asincrono, Core 0)
// =============================================================================

/**
 * @brief Callback principal del servidor WebSocket.
 *
 * Llamado por ESPAsyncWebServer desde el contexto de la tarea de red
 * (Core 0 / lwIP). Las operaciones aqui deben ser rapidas y no bloqueantes;
 * el ciclo IR se delega al loop() principal (Core 1).
 *
 * WS_EVT_CONNECT    -> log, LEDs, enviar estado inicial.
 * WS_EVT_DISCONNECT -> log, LEDs.
 * WS_EVT_DATA       -> acumular fragmentos, procesar mensaje completo.
 * WS_EVT_ERROR      -> log.
 */
void onWsEvent(AsyncWebSocket       *server,
               AsyncWebSocketClient *client,
               AwsEventType          type,
               void                 *arg,
               uint8_t              *data,
               size_t                len) {
    switch (type) {

        case WS_EVT_CONNECT:
            Serial.printf("[WS] Cliente #%u conectado desde %s\n",
                          client->id(),
                          client->remoteIP().toString().c_str());
            setLedConn(true);
            setLedDisc(false);
            enviarEstadoInicial(client);
            break;

        case WS_EVT_DISCONNECT:
            Serial.printf("[WS] Cliente #%u desconectado\n", client->id());
            setLedConn(false);
            setLedDisc(true);
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo *info = (AwsFrameInfo *)arg;
            // Solo procesar mensajes de texto completos (no fragmentados)
            if (info->final && info->index == 0 &&
                info->len == len && info->opcode == WS_TEXT) {
                // Imprimimos el payload limitando la longitud (%.*s) porque data no tiene \0 al final
                Serial.printf("[WS-RX] #%u: %.*s\n", client->id(), len, data);
                // Pasamos el puntero raw directamente (ArduinoJson lo lee de forma segura)
                procesarMensajeWS(client, data, len);
            }
            break;
        }

        case WS_EVT_ERROR:
            Serial.printf("[WS] Error cliente #%u: %u %s\n",
                          client->id(),
                          *((uint16_t *)arg),
                          (char *)data);
            break;

        default:
            break;
    }
}

// =============================================================================
//  SETUP
// =============================================================================

/**
 * @brief Secuencia de inicializacion completa del sistema.
 *
 * Orden:
 *  1. Serial para depuracion.
 *  2. Hardware: LEDC, LEDs de estado, receptor IR.
 *  3. Recuperar estado desde NVS; aplicar color al LED.
 *  4. Iniciar Access Point Wi-Fi con credenciales de config.h.
 *  5. Configurar rutas del servidor HTTP:
 *     - GET "/"    -> servir WEB_UI (interfaz D3).
 *     - WS  "/ws"  -> WebSocket para comandos y estados.
 *  6. Iniciar servidor HTTP.
 *  7. Encender LED_AP.
 */
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n[D2] === Maestro IR + Servidor Wi-Fi - Iniciando ===");

    // -- Hardware ----------------------------------------------------------
    iniciarLedc();

    pinMode(PIN_LED_CONN, OUTPUT);
    pinMode(PIN_LED_DISC, OUTPUT);
    pinMode(PIN_LED_AP,   OUTPUT);
    setLedConn(false);
    setLedDisc(false);
    digitalWrite(PIN_LED_AP, LOW);

    IrReceiver.begin(IR_RECV_PIN, DISABLE_LED_FEEDBACK);
    Serial.printf("[D2] Receptor IR activo en GPIO %d\n", IR_RECV_PIN);

    // -- NVS ---------------------------------------------------------------
    cargarNVS();
    if (encendido) {
        setColor(COLORES[colorActual].r,
                 COLORES[colorActual].g,
                 COLORES[colorActual].b);
    } else {
        setColor(0, 0, 0);
    }

    // -- Access Point Wi-Fi ------------------------------------------------
    Serial.printf("[Wi-Fi] Iniciando AP: SSID='%s'\n", AP_SSID);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[Wi-Fi] AP activo. IP: %s\n", ip.toString().c_str());
    Serial.printf("[Wi-Fi] Interfaz D3: http://%s\n", ip.toString().c_str());

    // -- WebSocket ---------------------------------------------------------
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // -- Rutas HTTP --------------------------------------------------------
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", WEB_UI);
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });

    server.begin();
    Serial.println("[D2] Servidor HTTP+WS iniciado");

    // -- LED AP encendido --------------------------------------------------
    digitalWrite(PIN_LED_AP, HIGH);
    Serial.println("[D2] Listo. Esperando conexion de D3...");
}

// =============================================================================
//  LOOP PRINCIPAL
// =============================================================================

/**
 * @brief Ciclo principal: mantenimiento del servidor WS + ejecucion del ciclo IR.
 *
 * ws.cleanupClients() libera recursos de clientes desconectados que el
 * servidor aun no removio de la lista interna. Sin esta llamada periodica,
 * la memoria del servidor se fragmenta con el tiempo.
 *
 * El ciclo IR se ejecuta SOLO cuando irPendiente == true (disparado por
 * el callback WS al recibir un comando de D3). Durante la ejecucion del
 * ciclo IR (hasta ~6.9 s en el peor caso con 3 reintentos + timeouts),
 * ESPAsyncWebServer sigue procesando eventos de red en el Core 0, por lo
 * que los eventos de desconexion de D3 son detectados correctamente.
 *
 * El delay(5) al final del loop cede CPU al scheduler de FreeRTOS,
 * permitiendo que las tareas de red del Core 0 se ejecuten sin hambre
 * de CPU (aunque en ESP32 los cores son independientes, la tarea idle
 * del watchdog requiere al menos un yield periodico desde el Core 1).
 */
void loop() {
    ws.cleanupClients();

    if (irPendiente) {
        irPendiente = false;  // limpiar ANTES de ejecutar para evitar re-entrada
        ejecutarCicloIR();
    }

    delay(5);
}
