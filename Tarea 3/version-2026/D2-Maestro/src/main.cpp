/**
 * @file    main.cpp
 * @brief   Dispositivo 2 — Maestro IR + Servidor Wi-Fi/WebSocket con CRC (ESP32).
 *
 * @section descripcion Descripción funcional
 *
 *  Nodo central del sistema TP3. Opera simultáneamente como:
 *
 *  a) MAESTRO IR con CRC-8/MAXIM: genera tramas NEC Raw de 32 bits con
 *     estructura [R][G][B][CRC8]. El CRC se calcula íntegramente por software
 *     (XOR, aritmética módulo 2, polinomio 0x31, init=0xFF). Gestiona el
 *     handshaking con reintentos y valida el ACK asimétrico recibido desde D1.
 *
 *  b) SERVIDOR WI-FI con CRC-16/IBM: Access Point WPA2, servidor HTTP que
 *     sirve la interfaz web de D3, y servidor WebSocket. Todos los mensajes
 *     JSON llevan un campo "crc16" calculado por software (polinomio 0x8005,
 *     init=0x0000, LSB-first). Los mensajes con CRC inválido son descartados
 *     y se notifica CRC_ERROR al cliente.
 *
 * @section problemas_resueltos Problemas resueltos y sus soluciones
 *
 *  PROBLEMA 1 — Eco nulo / tormenta de broadcast:
 *   Al reactivar el receptor KY-022 tras una TX, el eco residual de la
 *   portadora IR se capturaba como 0x00000000. Con CRC init=0x00,
 *   CRC(0,0,0)=0x00 era matemáticamente válido, generando falsos comandos.
 *   SOLUCIÓN: inicializar el CRC en 0xFF (CRC-8/MAXIM estándar). Con esta
 *   inicialización, CRC(0,0,0) ≠ 0, eliminando la colisión con el eco nulo.
 *
 *  PROBLEMA 2 — Desconexión asimétrica (latencia en LEDs de estado):
 *   Al presionar "Desconectar" en D3, el navegador cerraba el socket con
 *   ws.close(), pero ESPAsyncWebServer procesaba el cierre de forma diferida
 *   (asimétrica), retrasando la actualización de los LEDs de estado en D2.
 *   SOLUCIÓN: desconexión limpia (Graceful Shutdown) mediante handshake
 *   CMD_DESCONECTAR / ACK_DESCONECTAR. D3 envía CMD_DESCONECTAR con CRC-16;
 *   D2 responde ACK_DESCONECTAR y llama a client->close(1000), cerrando la
 *   sesión desde el lado del servidor. El evento WS_EVT_DISCONNECT en D2
 *   se dispara de forma determinística, actualizando los LEDs inmediatamente.
 *
 *  PROBLEMA 3 — Falso positivo por Auto-Acuse de Recibo (Self-ACK / Sordera por Eco):
 *   El protocolo IR original usaba la misma estructura para COMANDO y ACK
 *   ([R][G][B][CRC8]). Sin un campo de dirección, D2 no podía distinguir
 *   si una trama recibida era el ACK legítimo de D1 o el eco de su propio
 *   comando rebotando en el entorno. Al tapar D1, D2 igual "confirmaba" el
 *   color porque su eco propio pasaba la validación CRC.
 *   SOLUCIÓN: asimetría de protocolo. D1 responde con CRC invertido bit a bit
 *   (~CRC). D2 calcula el CRC normal y su inverso; descarta la trama si la
 *   firma coincide con el CRC normal (= eco propio) y acepta solo si coincide
 *   con ~CRC (= ACK legítimo de D1).
 *
 * @section protocolo_ir Protocolo IR: estructura de tramas
 *
 *   COMANDO D2→D1:  [R][G][B][CRC8_normal]     bits [31:0]
 *   ACK     D1→D2:  [R][G][B][~CRC8_normal]    bits [31:0]
 *
 *   El Maestro valida el ACK comprobando que la firma == ~CRC8(R,G,B).
 *   Si la firma == CRC8(R,G,B), la trama es un eco propio y se descarta.
 *
 * @section protocolo_ws Protocolo WebSocket: mensajes JSON con CRC-16
 *
 *   D3 → D2 (comandos con r, g, b explícitos en CMD_COLOR):
 *    {"tipo":"CMD_COLOR",      "color":"NOMBRE", "r":n, "g":n, "b":n, "crc16":<u16>}
 *    {"tipo":"CMD_ENCENDER",                                           "crc16":<u16>}
 *    {"tipo":"CMD_APAGAR",                                             "crc16":<u16>}
 *    {"tipo":"CMD_DESCONECTAR",                                        "crc16":<u16>}
 *    {"tipo":"PING",                                                   "crc16":<u16>}
 *
 *   D2 → D3 (respuestas):
 *    {"tipo":"ESTADO_ACTUAL", "encendido":bool, "color":str, "r":n, "g":n, "b":n, "crc16":<u16>}
 *    {"tipo":"RESULTADO_CMD", "exito":bool, "encendido":bool, "color":str,
 *                             "r":n, "g":n, "b":n, "crc16":<u16>}
 *    {"tipo":"CRC_ERROR",     "info":str, "crc16":<u16>}
 *    {"tipo":"ACK_DESCONECTAR",            "crc16":<u16>}
 *    {"tipo":"PONG",                       "crc16":<u16>}
 *
 *   El campo "r","g","b" en CMD_COLOR permite que el CRC-16 cubra también
 *   los valores numéricos del color, no solo el nombre simbólico. Esto
 *   garantiza integridad completa del triplete de color transmitido.
 *
 * @section concurrencia Modelo de concurrencia
 *
 *  ESPAsyncWebServer gestiona eventos de red en el Core 0 (tarea lwIP).
 *  loop() corre en el Core 1 (tarea Arduino). Las variables compartidas
 *  usan el qualifier volatile; la atomicidad de bool/uint8_t en ESP32
 *  garantiza coherencia sin mutex (un solo escritor por variable).
 *
 * @section hardware Hardware
 *
 *  Módulo               │ Pin   │ Notas
 *  ─────────────────────┼───────┼──────────────────────────────────────────
 *  KY-005 S             │ GPIO 4│ Emisor IR, RMT hardware
 *  KY-022 OUT           │ GPIO 5│ Receptor IR, interrupt-capable
 *  LED RGB R            │GPIO 25│ LEDC canal 3 (canal 0 reservado por IRremote)
 *  LED RGB G            │GPIO 26│ LEDC canal 1
 *  LED RGB B            │GPIO 27│ LEDC canal 2
 *  LED_CONN (verde)     │GPIO 32│ D3 conectado con sesión WS activa
 *  LED_DISC (rojo)      │GPIO 33│ Desconexión de D3 detectada
 *  LED_AP   (amarillo)  │GPIO 14│ Access Point Wi-Fi activo
 *
 * @section dependencias Dependencias (platformio.ini)
 *
 *  - z3t0/IRremote >= 4.7.1
 *  - me-no-dev/AsyncTCP >= 1.1.1
 *  - me-no-dev/ESPAsyncWebServer >= 1.2.3
 *  - bblanchon/ArduinoJson >= 7.2.0
 *  - Preferences (ESP32 Arduino Core)
 *
 * @authors Bevilacqua, Francisco — Clement, Sebastián
 * @date    14 de junio 2026
 */

// config.h debe ser el PRIMER include: define IR_SEND_PIN antes de IRremote.hpp
#include "config.h"

#include <Arduino.h>
#include <IRremote.hpp>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "web_ui.h"   // Interfaz web D3 embebida en flash

// ═══════════════════════════════════════════════════════════════════════════
//  OBJETOS GLOBALES
// ═══════════════════════════════════════════════════════════════════════════

AsyncWebServer  server(SERVER_PORT);
AsyncWebSocket  ws(WS_PATH);
Preferences     prefs;

// ═══════════════════════════════════════════════════════════════════════════
//  ESTADO DEL SISTEMA
// ═══════════════════════════════════════════════════════════════════════════

volatile uint8_t  colorActual  = 0;      ///< Índice en COLORES[] del último color confirmado por D1
volatile bool     encendido    = false;  ///< true = LED de D1 encendido con colorActual

// Variables de comando pendiente: escritas por el callback WS (Core 0),
// leídas y limpiadas por loop() (Core 1).
volatile bool    irPendiente         = false;
volatile uint8_t r_pendiente         = 0;
volatile uint8_t g_pendiente         = 0;
volatile uint8_t b_pendiente         = 0;
volatile uint8_t colorIdx_pendiente  = 0;
volatile bool    encendido_pendiente = false;

// ═══════════════════════════════════════════════════════════════════════════
//  PROTOTIPOS
// ═══════════════════════════════════════════════════════════════════════════

void     iniciarLedc();
void     setColor(uint8_t r, uint8_t g, uint8_t b);
uint8_t  calcularCRC8(uint8_t r, uint8_t g, uint8_t b);
uint16_t calcularCRC16(const char *payload, size_t len);
void     guardarNVS();
void     cargarNVS();
bool     enviarColorIR(uint8_t r, uint8_t g, uint8_t b);
void     ejecutarCicloIR();
void     notificarD3Estado(bool exito);
void     enviarEstadoInicial(AsyncWebSocketClient *client);
void     enviarMensajeWS(AsyncWebSocketClient *client, JsonDocument &doc);
void     enviarMensajeWSBroadcast(JsonDocument &doc);
void     procesarMensajeWS(AsyncWebSocketClient *client,
                            const uint8_t *payload, size_t len);
void     onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len);
void     setLedConn(bool on);
void     setLedDisc(bool on);

// ═══════════════════════════════════════════════════════════════════════════
//  CRC-8 POR SOFTWARE (aritmética módulo 2 / XOR)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Calcula el CRC-8/MAXIM sobre los tres bytes RGB.
 *
 * Implementación íntegra por software mediante XOR (aritmética módulo 2).
 * Sin tablas precalculadas ni librerías externas, favoreciendo transparencia
 * académica y portabilidad simétrica con la implementación en JavaScript de D3.
 *
 * Parámetros del estándar CRC-8/MAXIM (Dallas/1-Wire):
 *   Polinomio : 0x31  →  x^8 + x^5 + x^4 + 1
 *   Init      : 0xFF  →  guarda contra eco nulo (ver @section problemas_resueltos)
 *   RefIn     : true  →  procesamiento LSB-first
 *   RefOut    : true
 *   XorOut    : 0x00
 *
 * Distancia Hamming: HD=4 para mensajes ≤ 119 bits → adecuada para 3 bytes.
 *
 * @param r  Byte del canal rojo   [0-255]
 * @param g  Byte del canal verde  [0-255]
 * @param b  Byte del canal azul   [0-255]
 * @return   CRC-8 calculado sobre {R, G, B}.
 */
uint8_t calcularCRC8(uint8_t r, uint8_t g, uint8_t b) {
    // [BLOQUE-CRC8] Inicialización en 0xFF: invalida matemáticamente el eco nulo
    uint8_t crc = 0xFF;
    uint8_t datos[3] = { r, g, b };

    for (uint8_t i = 0; i < 3; i++) {
        crc ^= datos[i];   // [BLOQUE-CRC8] XOR del byte de datos con el registro CRC

        for (uint8_t bit = 0; bit < 8; bit++) {
            // [BLOQUE-CRC8] LSB=1: desplazar y XOR con polinomio (división en GF(2))
            // LSB=0: solo desplazar (no hay término que cancelar)
            if (crc & 0x01) {
                crc = (crc >> 1) ^ CRC8_POLY;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;   // [BLOQUE-CRC8] Remainder = checksum de 8 bits
}

// ═══════════════════════════════════════════════════════════════════════════
//  CRC-16 POR SOFTWARE (aritmética módulo 2 / XOR)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Calcula el CRC-16/IBM sobre un payload de texto (JSON serializado).
 *
 * Implementación íntegra por software mediante XOR. Misma lógica que la
 * función calcularCRC16() en JavaScript del cliente D3, garantizando
 * compatibilidad cruzada sin necesidad de librerías intermediarias.
 *
 * Parámetros del estándar CRC-16/IBM (CRC-16/ARC):
 *   Polinomio : 0x8005  →  x^16 + x^15 + x^2 + 1
 *   Init      : 0x0000
 *   RefIn     : true    →  procesamiento LSB-first
 *   RefOut    : true
 *   XorOut    : 0x0000
 *
 * Distancia Hamming: HD=4 para mensajes ≤ 32.767 bits → suficiente para los
 * payloads JSON de este sistema (< 300 bytes / ~2400 bits).
 *
 * Aplicación: el CRC se calcula sobre el JSON serializado SIN el campo "crc16"
 * para evitar dependencia circular. El proceso es:
 *   1. Serializar doc → buf_base (sin "crc16").
 *   2. CRC = calcularCRC16(buf_base).
 *   3. doc["crc16"] = CRC.
 *   4. Serializar doc → buf_final (con "crc16").
 *   5. Enviar buf_final.
 *
 * @param payload  Puntero al buffer de texto sobre el cual calcular el CRC.
 * @param len      Número de bytes a procesar.
 * @return         CRC-16 de 16 bits.
 */
uint16_t calcularCRC16(const char *payload, size_t len) {
    // [BLOQUE-CRC16] Inicialización del registro de 16 bits
    uint16_t crc = 0x0000;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint8_t)payload[i];   // [BLOQUE-CRC16] XOR con byte actual del payload

        for (uint8_t bit = 0; bit < 8; bit++) {
            // [BLOQUE-CRC16] LSB=1: desplazar y XOR con polinomio
            // LSB=0: solo desplazar
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ CRC16_POLY;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;   // [BLOQUE-CRC16] Resultado de 16 bits
}

// ═══════════════════════════════════════════════════════════════════════════
//  LEDC / PWM
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Configura los tres canales LEDC para el LED RGB de D2.
 *
 * Canal rojo asignado a LEDC_CH_R = 3 (no al 0) para evitar colisión con
 * el canal que IRremote reserva internamente para la modulación RMT de 38 kHz.
 */
void iniciarLedc() {
    // [BLOQUE-LEDC] Configuración de frecuencia y resolución por canal
    ledcSetup(LEDC_CH_R, LEDC_FREQ_HZ, LEDC_RESOLUTION);
    ledcSetup(LEDC_CH_G, LEDC_FREQ_HZ, LEDC_RESOLUTION);
    ledcSetup(LEDC_CH_B, LEDC_FREQ_HZ, LEDC_RESOLUTION);

    // [BLOQUE-LEDC] Vinculación canal ↔ GPIO
    ledcAttachPin(PIN_R, LEDC_CH_R);
    ledcAttachPin(PIN_G, LEDC_CH_G);
    ledcAttachPin(PIN_B, LEDC_CH_B);
}

/**
 * @brief Aplica un color RGB al LED de D2 mediante LEDC.
 * @param r  Intensidad rojo   [0-255]
 * @param g  Intensidad verde  [0-255]
 * @param b  Intensidad azul   [0-255]
 */
void setColor(uint8_t r, uint8_t g, uint8_t b) {
    // [BLOQUE-LED] Escritura del ciclo de trabajo PWM en cada canal
    ledcWrite(LEDC_CH_R, r);
    ledcWrite(LEDC_CH_G, g);
    ledcWrite(LEDC_CH_B, b);
}

// ═══════════════════════════════════════════════════════════════════════════
//  LEDS DE ESTADO
// ═══════════════════════════════════════════════════════════════════════════

/** @brief Controla el LED verde (D3 conectado con sesión WS activa). */
void setLedConn(bool on) { digitalWrite(PIN_LED_CONN, on ? HIGH : LOW); }

/** @brief Controla el LED rojo (desconexión de D3 detectada). */
void setLedDisc(bool on) { digitalWrite(PIN_LED_DISC, on ? HIGH : LOW); }

// ═══════════════════════════════════════════════════════════════════════════
//  PERSISTENCIA NVS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Persiste el estado actual (índice de color + encendido) en NVS.
 *
 * Se guarda el índice en COLORES[] en lugar del triplete RGB completo,
 * minimizando escrituras en flash. Los valores RGB se reconstruyen desde
 * la tabla en tiempo de ejecución. El centinela 0xA5 (patrón alternante)
 * permite detectar NVS vacía o corrompida al arrancar.
 */
void guardarNVS() {
    // [BLOQUE-NVS] Escritura del estado con centinela de validez
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar(NVS_KEY_MAGIC,     NVS_MAGIC_VAL);
    prefs.putUChar(NVS_KEY_COLOR_IDX, colorActual);
    prefs.putBool (NVS_KEY_ENCENDIDO, encendido);
    prefs.end();
    Serial.printf("[NVS] Guardado — idx=%u (%s)  encendido=%s  RGB(%u,%u,%u)\n",
                  colorActual,
                  COLORES[colorActual].nombre,
                  encendido ? "SI" : "NO",
                  COLORES[colorActual].r,
                  COLORES[colorActual].g,
                  COLORES[colorActual].b);
}

/**
 * @brief Recupera el estado desde NVS al arrancar.
 *
 * Si el centinela no coincide (NVS vacía o corrompida), inicializa con
 * idx=0 (ROJO), LED apagado. Incluye sanity-check del índice para no
 * acceder fuera de los límites de la tabla COLORES[].
 */
void cargarNVS() {
    // [BLOQUE-NVS] Lectura con validación de centinela
    prefs.begin(NVS_NAMESPACE, true);
    uint8_t magic = prefs.getUChar(NVS_KEY_MAGIC, 0x00);
    if (magic == NVS_MAGIC_VAL) {
        colorActual = prefs.getUChar(NVS_KEY_COLOR_IDX, 0);
        encendido   = prefs.getBool (NVS_KEY_ENCENDIDO,  false);
        if (colorActual >= NUM_COLORES) colorActual = 0;   // sanity check
        Serial.printf("[NVS] Recuperado — idx=%u (%s)  encendido=%s  RGB(%u,%u,%u)\n",
                      colorActual,
                      COLORES[colorActual].nombre,
                      encendido ? "SI" : "NO",
                      COLORES[colorActual].r,
                      COLORES[colorActual].g,
                      COLORES[colorActual].b);
    } else {
        colorActual = 0;
        encendido   = false;
        Serial.println("[NVS] Vacía — defaults aplicados (idx=0 ROJO, apagado)");
    }
    prefs.end();
}

// ═══════════════════════════════════════════════════════════════════════════
//  PROTOCOLO IR — ENVÍO CON CRC-8 Y VALIDACIÓN DE ACK ASIMÉTRICO
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Envía una trama IR con CRC-8 y espera el ACK asimétrico de D1.
 *
 * @section trama_cmd Construcción de la trama de COMANDO
 *
 *  Estructura [R][G][B][CRC8_normal] empaquetada en 32 bits (NEC Raw):
 *   bits [31:24] = R
 *   bits [23:16] = G
 *   bits [15: 8] = B
 *   bits [ 7: 0] = CRC-8/MAXIM(R, G, B)  con init=0xFF
 *
 * @section validacion_ack Validación del ACK asimétrico
 *
 *  Para resolver el problema del Self-ACK (eco del propio COMANDO), D1
 *  responde con la firma CRC **invertida bit a bit** (~CRC). D2 aplica el
 *  siguiente algoritmo de discriminación sobre cada trama recibida:
 *
 *   crc_base     = CRC-8/MAXIM(R_rx, G_rx, B_rx)   // firma esperada para COMANDO/eco
 *   crc_esperado = ~crc_base                         // firma esperada para ACK legítimo
 *
 *   Si  firma_rx == crc_base     → la trama es un eco del propio COMANDO → descartar.
 *   Si  firma_rx == crc_esperado → ACK legítimo de D1 → verificar datos → retornar true.
 *   Otro caso                    → trama corrupta o de otra fuente → descartar.
 *
 * @section half_duplex Gestión half-duplex
 *
 *  El canal IR es óptico y half-duplex. IrReceiver.stop() deshabilita el
 *  receptor durante la transmisión para evitar la auto-captura del eco.
 *  IrReceiver.start() lo reactiva inmediatamente tras la TX para poder
 *  escuchar el ACK de D1.
 *
 * @param r  Componente rojo   a transmitir [0-255]
 * @param g  Componente verde  a transmitir [0-255]
 * @param b  Componente azul   a transmitir [0-255]
 * @return   true si se recibe y valida el ACK asimétrico de D1; false por timeout.
 */
bool enviarColorIR(uint8_t r, uint8_t g, uint8_t b) {
    // [BLOQUE-CRC8] Calcular CRC-8/MAXIM con init=0xFF
    uint8_t crc_tx = calcularCRC8(r, g, b);

    // [BLOQUE-IR-TX] Construir trama de COMANDO: [R][G][B][CRC8_normal]
    uint32_t trama = ((uint32_t)r     << 24) |
                     ((uint32_t)g     << 16) |
                     ((uint32_t)b     <<  8) |
                      (uint32_t)crc_tx;

    // [BLOQUE-LOG] Log detallado de la trama a transmitir
    Serial.println("[IR-TX] ─────────────────────────────────────────");
    Serial.printf( "[IR-TX] Trama COMANDO: 0x%08X\n", trama);
    Serial.printf( "        R=%u  G=%u  B=%u\n", r, g, b);
    Serial.printf( "        CRC_tx=0x%02X  (init=0xFF, poly=0x%02X, firma normal)\n",
                   crc_tx, CRC8_POLY);
    Serial.printf( "        ACK esperado: R=%u G=%u B=%u CRC=0x%02X  (~CRC_tx)\n",
                   r, g, b, (uint8_t)~crc_tx);

    // [BLOQUE-IR-TX] Gestión half-duplex: deshabilitar RX antes de TX
    IrReceiver.stop();
    IrSender.sendNECRaw(trama, 0);   // 0 = sin repeticiones NEC automáticas
    IrReceiver.start();              // reactivar RX para escuchar el ACK
    Serial.println("[IR-TX] Trama enviada. Esperando ACK...");
    Serial.println("[IR-TX] ─────────────────────────────────────────");

    // [BLOQUE-IR-RX] Bucle de espera del ACK con timeout
    unsigned long t0 = millis();
    while ((millis() - t0) < TIMEOUT_ACK_MS) {
        if (!IrReceiver.decode()) continue;

        // [BLOQUE-FILTRO] Descartar repeticiones automáticas del protocolo NEC
        if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
            IrReceiver.resume();
            continue;
        }

        // [BLOQUE-IR-RX] Leer y liberar el buffer del receptor
        uint32_t resp  = IrReceiver.decodedIRData.decodedRawData;
        IrReceiver.resume();

        uint8_t ack_r   = (resp >> 24) & 0xFF;
        uint8_t ack_g   = (resp >> 16) & 0xFF;
        uint8_t ack_b   = (resp >>  8) & 0xFF;
        uint8_t ack_crc =  resp        & 0xFF;

        // [BLOQUE-CRC8] Calcular referencias para discriminación de eco vs ACK
        uint8_t crc_base     = calcularCRC8(ack_r, ack_g, ack_b);   // firma de eco/COMANDO
        uint8_t crc_esperado = ~crc_base;                            // firma de ACK legítimo

        // [BLOQUE-LOG] Log detallado de la trama recibida
        Serial.println("[IR-RX] ─────────────────────────────────────────");
        Serial.printf( "[IR-RX] Trama recibida: 0x%08X\n", resp);
        Serial.printf( "        R=%u  G=%u  B=%u  CRC_rx=0x%02X\n",
                       ack_r, ack_g, ack_b, ack_crc);
        Serial.printf( "[CRC-8] CRC_normal=0x%02X  (firma esperada si eco)\n", crc_base);
        Serial.printf( "[CRC-8] CRC_~inv  =0x%02X  (firma esperada si ACK)\n", crc_esperado);

        // [BLOQUE-FILTRO-ECO] Si la firma == CRC_normal → eco del propio COMANDO
        if (ack_crc == crc_base) {
            Serial.println("[CRC-8] Eco propio detectado (firma == CRC_normal) — descartado");
            Serial.println("[IR-RX] ─────────────────────────────────────────");
            continue;
        }

        // [BLOQUE-CRC8] Validar firma asimétrica del ACK
        if (ack_crc != crc_esperado) {
            Serial.printf("[CRC-8] Firma inválida: rx=0x%02X  ≠  esperado_ack=0x%02X — descartado\n",
                          ack_crc, crc_esperado);
            Serial.println("[IR-RX] ─────────────────────────────────────────");
            continue;
        }

        // [BLOQUE-IR-RX] Firma ACK correcta: verificar coherencia de datos RGB
        if (ack_r == r && ack_g == g && ack_b == b) {
            Serial.println("[CRC-8] Firma ACK válida (~CRC). Datos RGB coinciden.");
            Serial.printf( "[IR-RX] ACK confirmado: R=%u  G=%u  B=%u  CRC=0x%02X\n",
                           ack_r, ack_g, ack_b, ack_crc);
            Serial.println("[IR-RX] ─────────────────────────────────────────");
            return true;
        }

        Serial.printf("[IR-RX] ACK con datos RGB incorrectos (rx: R=%u G=%u B=%u ≠ tx: R=%u G=%u B=%u) — descartado\n",
                      ack_r, ack_g, ack_b, r, g, b);
        Serial.println("[IR-RX] ─────────────────────────────────────────");
    }

    Serial.printf("[IR] Timeout (%u ms) — sin ACK válido de D1\n", TIMEOUT_ACK_MS);
    return false;
}

/**
 * @brief Ejecuta el ciclo completo de envío IR con reintentos.
 *
 * Llamado desde loop() cuando irPendiente == true (disparado por el
 * callback WS al recibir un comando válido de D3).
 *
 * Si algún intento recibe ACK válido:
 *   - Actualiza colorActual y encendido.
 *   - Aplica el color al LED de D2 (espejo visual del estado de D1).
 *   - Persiste en NVS.
 *   - Notifica a D3 con RESULTADO_CMD exito=true.
 *
 * Si todos los intentos fallan (timeout en cada uno):
 *   - El estado del sistema no cambia.
 *   - Notifica a D3 con RESULTADO_CMD exito=false.
 */
void ejecutarCicloIR() {
    // [BLOQUE-CICLO-IR] Capturar valores pendientes en variables locales
    uint8_t r   = r_pendiente;
    uint8_t g   = g_pendiente;
    uint8_t b   = b_pendiente;
    uint8_t ci  = colorIdx_pendiente;
    bool    enc = encendido_pendiente;

    Serial.println("\n[D2] ══════════ CICLO IR ══════════");
    Serial.printf( "[D2] Comando: RGB(%u,%u,%u)  color=%s  encendido=%s\n",
                   r, g, b,
                   (ci < NUM_COLORES) ? COLORES[ci].nombre : "?",
                   enc ? "SI" : "NO");
    Serial.printf( "[D2] Máximo reintentos: %d  Timeout/intento: %u ms\n",
                   MAX_REINTENTOS, TIMEOUT_ACK_MS);

    bool ack = false;
    for (int i = 0; i < MAX_REINTENTOS && !ack; i++) {
        if (i > 0) {
            Serial.printf("\n[IR] ── Reintento %d / %d ──\n", i, MAX_REINTENTOS - 1);
            delay(DELAY_REINTENTO_MS);
        }
        ack = enviarColorIR(r, g, b);
    }

    if (ack) {
        // [BLOQUE-CICLO-IR] ACK válido: actualizar estado del sistema
        colorActual = ci;
        encendido   = enc;
        setColor(r, g, b);
        guardarNVS();
        Serial.printf("[D2] Estado confirmado — idx=%u (%s)  enc=%s  RGB(%u,%u,%u)\n",
                      colorActual,
                      COLORES[colorActual].nombre,
                      encendido ? "SI" : "NO",
                      r, g, b);
    } else {
        Serial.println("[D2] Sin ACK tras todos los reintentos — estado sin cambios");
    }

    Serial.println("[D2] ══════════════════════════════\n");
    notificarD3Estado(ack);
}

// ═══════════════════════════════════════════════════════════════════════════
//  WEBSOCKET — ENVÍO DE MENSAJES CON CRC-16
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Serializa un JsonDocument, agrega el campo CRC-16 y envía a un cliente.
 *
 * Proceso de firma:
 *  1. Serializar doc → buf_base (sin "crc16").
 *  2. Calcular CRC-16/IBM sobre buf_base (el payload "puro").
 *  3. doc["crc16"] = CRC.
 *  4. Serializar doc → buf_final (ahora con "crc16" incluido).
 *  5. Transmitir buf_final al cliente WebSocket.
 *
 * El CRC se calcula sobre el JSON sin "crc16" para evitar dependencia
 * circular (no se puede incluir el CRC en el dato sobre el que se calcula).
 *
 * @param client  Cliente WebSocket destinatario.
 * @param doc     Documento JSON a enviar (se modifica in-place agregando "crc16").
 */
void enviarMensajeWS(AsyncWebSocketClient *client, JsonDocument &doc) {
    // [BLOQUE-CRC16] Serialización sin campo CRC para calcular
    char buf_base[300];
    size_t len_base = serializeJson(doc, buf_base, sizeof(buf_base));

    // [BLOQUE-CRC16] Cálculo del CRC-16/IBM sobre el payload base
    uint16_t crc = calcularCRC16(buf_base, len_base);

    // [BLOQUE-CRC16] Agregar campo CRC y serializar versión final
    doc["crc16"] = crc;
    char buf_final[320];
    serializeJson(doc, buf_final, sizeof(buf_final));

    client->text(buf_final);
    Serial.printf("[WS-TX] → cliente #%u | CRC-16=0x%04X | %s\n",
                  client->id(), crc, buf_final);
}

/**
 * @brief Serializa un JsonDocument, agrega CRC-16 y hace broadcast a todos los clientes.
 *
 * @param doc  Documento JSON a enviar (se modifica in-place agregando "crc16").
 */
void enviarMensajeWSBroadcast(JsonDocument &doc) {
    // [BLOQUE-CRC16] Cálculo y serialización con CRC-16
    char buf_base[300];
    size_t len_base = serializeJson(doc, buf_base, sizeof(buf_base));
    uint16_t crc = calcularCRC16(buf_base, len_base);
    doc["crc16"] = crc;
    char buf_final[320];
    serializeJson(doc, buf_final, sizeof(buf_final));

    ws.textAll(buf_final);
    Serial.printf("[WS-TX] Broadcast | CRC-16=0x%04X | %s\n", crc, buf_final);
}

/**
 * @brief Envía el estado actual al cliente recién conectado.
 *
 * Llamado en WS_EVT_CONNECT para que D3 muestre el estado real del sistema
 * inmediatamente al conectarse, sin necesidad de enviar un comando de consulta.
 * El mensaje incluye CRC-16 para que D3 valide la integridad.
 *
 * @param client  Cliente WebSocket recién conectado.
 */
void enviarEstadoInicial(AsyncWebSocketClient *client) {
    // [BLOQUE-WS-CONN] Construcción del mensaje ESTADO_ACTUAL
    JsonDocument doc;
    doc["tipo"]      = MSG_ESTADO_ACTUAL;
    doc["encendido"] = (bool)encendido;
    doc["color"]     = encendido ? COLORES[colorActual].nombre : "";
    doc["r"]         = encendido ? COLORES[colorActual].r : 0;
    doc["g"]         = encendido ? COLORES[colorActual].g : 0;
    doc["b"]         = encendido ? COLORES[colorActual].b : 0;

    Serial.printf("[WS] Estado inicial → cliente #%u | enc=%s  color=%s  RGB(%u,%u,%u)\n",
                  client->id(),
                  encendido ? "SI" : "NO",
                  encendido ? COLORES[colorActual].nombre : "-",
                  encendido ? COLORES[colorActual].r : 0,
                  encendido ? COLORES[colorActual].g : 0,
                  encendido ? COLORES[colorActual].b : 0);

    enviarMensajeWS(client, doc);
}

/**
 * @brief Notifica a todos los clientes el resultado del último ciclo IR.
 *
 * Envía RESULTADO_CMD con el estado actual y el campo "exito" que indica si
 * D1 confirmó el comando. Incluye CRC-16.
 *
 * @param exito  true si D1 confirmó el comando con ACK asimétrico válido;
 *               false si todos los reintentos agotaron el timeout.
 */
void notificarD3Estado(bool exito) {
    // [BLOQUE-WS-NOTIF] Construcción del mensaje de resultado
    JsonDocument doc;
    doc["tipo"]      = MSG_RESULTADO_CMD;
    doc["exito"]     = exito;
    doc["encendido"] = (bool)encendido;
    doc["color"]     = encendido ? COLORES[colorActual].nombre : "";
    doc["r"]         = encendido ? COLORES[colorActual].r : 0;
    doc["g"]         = encendido ? COLORES[colorActual].g : 0;
    doc["b"]         = encendido ? COLORES[colorActual].b : 0;

    Serial.printf("[WS] Notificando resultado → exito=%s  enc=%s  color=%s  RGB(%u,%u,%u)\n",
                  exito ? "SI" : "NO",
                  encendido ? "SI" : "NO",
                  encendido ? COLORES[colorActual].nombre : "-",
                  encendido ? COLORES[colorActual].r : 0,
                  encendido ? COLORES[colorActual].g : 0,
                  encendido ? COLORES[colorActual].b : 0);

    enviarMensajeWSBroadcast(doc);
}

// ═══════════════════════════════════════════════════════════════════════════
//  WEBSOCKET — PROCESAMIENTO DE MENSAJES ENTRANTES CON CRC-16
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Valida el CRC-16 de un mensaje JSON entrante y lo procesa.
 *
 * @section validacion_crc16_rx Validación del CRC-16 recibido
 *
 *  El receptor (D2) aplica el proceso inverso al del emisor (D3):
 *   1. Parsear el JSON del payload recibido.
 *   2. Leer el campo "crc16" del mensaje recibido (CRC_rx).
 *   3. Remover el campo "crc16" del documento parseado.
 *   4. Serializar el documento SIN "crc16" para replicar el cálculo original de D3.
 *   5. Calcular CRC-16 sobre esa serialización (CRC_calc).
 *   6. Si CRC_rx ≠ CRC_calc → rechazar con CRC_ERROR y retornar.
 *   7. Si CRC_rx == CRC_calc → mensaje íntegro → procesar por tipo.
 *
 *  Nota: el paso 4 requiere que el serializador de D2 (ArduinoJson) y el
 *  de D3 (JSON.stringify) produzcan exactamente el mismo string para el mismo
 *  objeto. Se logró estandarizando el orden de campos en ambos lados mediante
 *  el patrón Envelope (campos en orden canónico fijo antes de agregar "crc16").
 *
 * @section cmd_color_rgb CMD_COLOR con campos r, g, b
 *
 *  El comando CMD_COLOR incluye los campos numéricos "r", "g", "b" además del
 *  nombre simbólico "color". Esto permite que el CRC-16 cubra también los valores
 *  numéricos del triplete, no solo el string del nombre. D2 usa directamente los
 *  valores r/g/b del mensaje sin necesidad de buscar en la tabla COLORES[], aunque
 *  igualmente busca el índice por nombre para mantener la coherencia con colorActual.
 *
 * @section handshake_desconexion Handshake de desconexión (Graceful Shutdown)
 *
 *  Al recibir CMD_DESCONECTAR:
 *   1. D2 envía ACK_DESCONECTAR con CRC-16 al cliente.
 *   2. D2 llama a client->close(1000, "Cierre solicitado por D3").
 *   3. El cierre desde el servidor dispara WS_EVT_DISCONNECT de forma
 *      determinística, actualizando los LEDs de estado inmediatamente.
 *  Sin este handshake, el cierre iniciado por D3 (ws.close() en el navegador)
 *  era procesado de forma asimétrica por ESPAsyncWebServer, retrasando el
 *  evento DISCONNECT y causando latencia en los LEDs de D2.
 *
 * @param client   Cliente WS que envió el mensaje.
 * @param payload  Buffer del mensaje JSON recibido.
 * @param len      Longitud del payload en bytes.
 */
void procesarMensajeWS(AsyncWebSocketClient *client,
                       const uint8_t *payload, size_t len) {
    // [BLOQUE-PARSE] Deserializar JSON recibido
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        Serial.printf("[WS-RX] Error JSON: %s — payload: %.*s\n",
                      err.c_str(), (int)len, (const char *)payload);
        return;
    }

    // [BLOQUE-LOG] Log del mensaje crudo recibido
    Serial.printf("[WS-RX] ← cliente #%u | %.*s\n",
                  client->id(), (int)len, (const char *)payload);

    // [BLOQUE-CRC16] Extraer campo CRC-16 del mensaje
    if (!doc["crc16"].is<uint16_t>()) {
        Serial.println("[CRC-16] Mensaje sin campo crc16 — descartado");
        return;
    }
    uint16_t crc_rx = doc["crc16"].as<uint16_t>();

    // [BLOQUE-CRC16] Eliminar crc16, serializar y recalcular para verificar
    doc.remove("crc16");
    char buf_check[300];
    size_t len_check = serializeJson(doc, buf_check, sizeof(buf_check));
    uint16_t crc_calc = calcularCRC16(buf_check, len_check);

    // [BLOQUE-LOG] Log de verificación CRC-16
    Serial.printf("[CRC-16] rx=0x%04X  calc=0x%04X  base=%s\n",
                  crc_rx, crc_calc, buf_check);

    if (crc_rx != crc_calc) {
        Serial.printf("[CRC-16] ERROR — Integridad comprometida. Mensaje descartado.\n");
        // [BLOQUE-CRC16] Notificar el error al cliente con su propio CRC-16
        JsonDocument err_doc;
        err_doc["tipo"] = MSG_CRC_ERROR;
        err_doc["info"] = "CRC-16 invalido — mensaje descartado";
        enviarMensajeWS(client, err_doc);
        return;
    }
    Serial.println("[CRC-16] OK — Integridad verificada.");

    const char *tipo = doc["tipo"] | "";
    Serial.printf("[WS-RX] tipo=%s\n", tipo);

    // [BLOQUE-PING] Responder keepalive con PONG (también con CRC-16)
    if (strcmp(tipo, "PING") == 0) {
        JsonDocument pong;
        pong["tipo"] = "PONG";
        enviarMensajeWS(client, pong);
        return;
    }

    // [BLOQUE-DESCONECTAR] Cierre voluntario solicitado por D3
    // Handshake: D2 responde ACK_DESCONECTAR y cierra la sesión desde el servidor,
    // garantizando que WS_EVT_DISCONNECT se dispare de forma determinística
    // para actualizar los LEDs de estado sin latencia.
    if (strcmp(tipo, MSG_CMD_DESCONECTAR) == 0) {
        Serial.printf("[WS] Cliente #%u solicitó desconexión voluntaria\n", client->id());
        JsonDocument ack_doc;
        ack_doc["tipo"] = "ACK_DESCONECTAR";
        enviarMensajeWS(client, ack_doc);
        // Cierre desde el servidor con código 1000 (cierre normal RFC 6455)
        client->close(1000, "Cierre solicitado por D3");
        return;
    }

    // [BLOQUE-CMD] Rechazar nuevo comando si ya hay un ciclo IR en progreso
    if (irPendiente) {
        Serial.println("[D2] Comando descartado: ciclo IR en progreso");
        return;
    }

    if (strcmp(tipo, MSG_CMD_COLOR) == 0) {
        // [BLOQUE-CMD-COLOR] Cambiar al color indicado
        // D3 envía r, g, b junto al nombre simbólico; el CRC-16 cubre los valores numéricos.
        const char *nombre = doc["color"] | "";

        // Buscar el índice en la tabla para mantener coherencia con colorActual
        int idx = -1;
        for (int i = 0; i < NUM_COLORES; i++) {
            if (strcmp(COLORES[i].nombre, nombre) == 0) { idx = i; break; }
        }
        if (idx < 0) {
            Serial.printf("[D2] Color desconocido: '%s' — comando ignorado\n", nombre);
            return;
        }

        // Usar los valores r/g/b del mensaje (cubiertos por CRC-16)
        uint8_t cmd_r = doc["r"] | (uint8_t)COLORES[idx].r;
        uint8_t cmd_g = doc["g"] | (uint8_t)COLORES[idx].g;
        uint8_t cmd_b = doc["b"] | (uint8_t)COLORES[idx].b;

        Serial.printf("[D2] CMD_COLOR → idx=%u (%s)  RGB(%u,%u,%u)\n",
                      idx, nombre, cmd_r, cmd_g, cmd_b);

        colorIdx_pendiente   = (uint8_t)idx;
        r_pendiente          = cmd_r;
        g_pendiente          = cmd_g;
        b_pendiente          = cmd_b;
        encendido_pendiente  = true;
        irPendiente          = true;

    } else if (strcmp(tipo, MSG_CMD_ENCENDER) == 0) {
        // [BLOQUE-CMD-ENCENDER] Encender con el último color conocido
        Serial.printf("[D2] CMD_ENCENDER → reutilizando idx=%u (%s)  RGB(%u,%u,%u)\n",
                      colorActual,
                      COLORES[colorActual].nombre,
                      COLORES[colorActual].r,
                      COLORES[colorActual].g,
                      COLORES[colorActual].b);

        colorIdx_pendiente   = colorActual;
        r_pendiente          = COLORES[colorActual].r;
        g_pendiente          = COLORES[colorActual].g;
        b_pendiente          = COLORES[colorActual].b;
        encendido_pendiente  = true;
        irPendiente          = true;

    } else if (strcmp(tipo, MSG_CMD_APAGAR) == 0) {
        // [BLOQUE-CMD-APAGAR] Apagar: RGB = {0, 0, 0}; preservar colorActual para re-encendido
        Serial.printf("[D2] CMD_APAGAR → RGB(0,0,0)  colorActual preservado: %s\n",
                      COLORES[colorActual].nombre);

        colorIdx_pendiente   = colorActual;   // preservar para CMD_ENCENDER posterior
        r_pendiente          = 0;
        g_pendiente          = 0;
        b_pendiente          = 0;
        encendido_pendiente  = false;
        irPendiente          = true;

    } else {
        Serial.printf("[D2] Tipo de mensaje desconocido: '%s' — ignorado\n", tipo);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  WEBSOCKET — MANEJADOR DE EVENTOS (callback asíncrono, Core 0)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Callback del servidor WebSocket para todos los eventos de red.
 *
 * Ejecutado por ESPAsyncWebServer en el contexto de la tarea de red del
 * Core 0 (lwIP). Las operaciones aquí deben ser rápidas y no bloqueantes;
 * el ciclo IR se delega al loop() del Core 1 mediante el flag irPendiente.
 *
 * WS_EVT_CONNECT    → log, actualizar LEDs, enviar estado inicial al cliente.
 * WS_EVT_DISCONNECT → log, actualizar LEDs.
 *                     Disparado de forma determinística cuando D2 cierra la
 *                     sesión vía client->close() (Graceful Shutdown), o ante
 *                     pérdida de conexión detectada por el heartbeat de ping.
 * WS_EVT_DATA       → solo mensajes de texto completos (no fragmentados).
 * WS_EVT_ERROR      → log del código y descripción del error.
 */
void onWsEvent(AsyncWebSocket       *server,
               AsyncWebSocketClient *client,
               AwsEventType          type,
               void                 *arg,
               uint8_t              *data,
               size_t                len) {
    switch (type) {

        case WS_EVT_CONNECT:
            // [BLOQUE-WS-CONN] Nueva conexión de D3
            Serial.printf("\n[WS] ── CONNECT ── Cliente #%u desde %s\n",
                          client->id(), client->remoteIP().toString().c_str());
            setLedConn(true);
            setLedDisc(false);
            enviarEstadoInicial(client);
            break;

        case WS_EVT_DISCONNECT:
            // [BLOQUE-WS-DISC] Desconexión de D3 (voluntaria vía Graceful Shutdown o abrupta)
            Serial.printf("[WS] ── DISCONNECT ── Cliente #%u\n", client->id());
            setLedConn(false);
            setLedDisc(true);
            break;

        case WS_EVT_DATA: {
            // [BLOQUE-WS-DATA] Procesar solo mensajes de texto completos (no fragmentados)
            AwsFrameInfo *info = (AwsFrameInfo *)arg;
            if (info->final && info->index == 0 &&
                info->len == len && info->opcode == WS_TEXT) {
                procesarMensajeWS(client, data, len);
            }
            break;
        }

        case WS_EVT_ERROR:
            Serial.printf("[WS] Error cliente #%u: código=%u  desc=%s\n",
                          client->id(), *((uint16_t *)arg), (char *)data);
            break;

        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Secuencia completa de inicialización del sistema D2.
 *
 * Orden:
 *  1. Serial para depuración (115200 bps).
 *  2. Hardware: canales LEDC, LEDs de estado, receptor IR.
 *  3. Recuperar estado desde NVS y aplicar al LED de D2.
 *  4. Iniciar Access Point Wi-Fi WPA2 (sin salida a Internet).
 *     - IP del AP: 192.168.4.1 (valor por defecto del modo AP del ESP32).
 *     - D3 accede a http://192.168.4.1 para obtener la interfaz web.
 *  5. Registrar el handler WebSocket y las rutas HTTP:
 *     - GET "/" : serve WEB_UI (interfaz de D3 embebida en flash, ~10 KB).
 *     - WS "/ws": endpoint WebSocket full-duplex con CRC-16.
 *  6. Iniciar servidor HTTP (puerto 80).
 *  7. Encender LED_AP: sistema listo para conexiones.
 */
void setup() {
    // [BLOQUE-INIT] Serial
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n[D2] ═══════════════════════════════════════════════════");
    Serial.println("[D2]  Maestro IR + Servidor Wi-Fi con CRC — Iniciando");
    Serial.println("[D2] ═══════════════════════════════════════════════════");
    Serial.printf( "[D2] CRC-8:  poly=0x%02X  init=0xFF  LSB-first\n",   CRC8_POLY);
    Serial.printf( "[D2] CRC-16: poly=0x%04X  init=0x0000  LSB-first\n", CRC16_POLY);
    Serial.printf( "[D2] Protocolo IR: [R][G][B][CRC8_normal] → ACK=[R][G][B][~CRC8]\n");

    // [BLOQUE-INIT] Hardware: PWM, LEDs de estado, receptor IR
    iniciarLedc();
    Serial.printf("[D2] LEDC — canales R=%u G=%u B=%u @ %u Hz / %u bits\n",
                  LEDC_CH_R, LEDC_CH_G, LEDC_CH_B, LEDC_FREQ_HZ, LEDC_RESOLUTION);

    pinMode(PIN_LED_CONN, OUTPUT);
    pinMode(PIN_LED_DISC, OUTPUT);
    pinMode(PIN_LED_AP,   OUTPUT);
    setLedConn(false);
    setLedDisc(false);
    digitalWrite(PIN_LED_AP, LOW);

    IrReceiver.begin(IR_RECV_PIN, DISABLE_LED_FEEDBACK);
    Serial.printf("[D2] Receptor IR activo — GPIO %d\n", IR_RECV_PIN);
    Serial.printf("[D2] Emisor  IR listo  — GPIO %d\n", IR_SEND_PIN);

    // [BLOQUE-NVS] Recuperar estado y aplicar al LED de D2
    cargarNVS();
    if (encendido) {
        setColor(COLORES[colorActual].r,
                 COLORES[colorActual].g,
                 COLORES[colorActual].b);
        Serial.printf("[D2] LED restaurado a %s RGB(%u,%u,%u)\n",
                      COLORES[colorActual].nombre,
                      COLORES[colorActual].r,
                      COLORES[colorActual].g,
                      COLORES[colorActual].b);
    } else {
        setColor(0, 0, 0);
        Serial.println("[D2] LED apagado (estado inicial)");
    }

    // [BLOQUE-WIFI] Iniciar Access Point WPA2
    Serial.printf("\n[Wi-Fi] Iniciando AP: SSID='%s'  canal=%d  max_conn=%d\n",
                  AP_SSID, AP_CHANNEL, AP_MAX_CONN);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[Wi-Fi] AP activo — IP: %s\n", ip.toString().c_str());
    Serial.printf("[Wi-Fi] Interfaz D3 disponible en: http://%s\n",
                  ip.toString().c_str());

    // [BLOQUE-WS] Registrar handler del WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // [BLOQUE-HTTP] Ruta raíz: servir interfaz web D3 desde flash
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", WEB_UI);
    });
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });

    // [BLOQUE-HTTP] Iniciar servidor
    server.begin();
    Serial.println("[D2] Servidor HTTP + WebSocket iniciado (puerto 80)");

    // [BLOQUE-INIT] LED AP encendido: sistema completamente listo
    digitalWrite(PIN_LED_AP, HIGH);
    Serial.println("[D2] Sistema listo. Esperando conexión de D3...\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  LOOP PRINCIPAL
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Ciclo principal: mantenimiento del servidor WS y ejecución del ciclo IR.
 *
 * ws.cleanupClients() libera recursos de clientes desconectados que el
 * servidor aún no removió de su lista interna. Sin esta llamada periódica,
 * la memoria heap del ESP32 se fragmenta progresivamente con cada reconexión.
 *
 * El ciclo IR se ejecuta SOLO cuando irPendiente == true (activado por el
 * callback WS al recibir un comando válido de D3). Durante el ciclo IR
 * (hasta ~6.9 s en el peor caso: 3 reintentos × TIMEOUT_ACK_MS + delays),
 * ESPAsyncWebServer continúa procesando eventos de red en el Core 0, por
 * lo que las desconexiones de D3 son detectadas correctamente.
 *
 * delay(5): cede CPU al scheduler de FreeRTOS para evitar inanición del
 * watchdog idle en el Core 1. Suficiente para que las tareas del sistema
 * de red del Core 0 procesen eventos pendientes.
 */
void loop() {
    // [BLOQUE-LOOP] Liberar recursos de clientes WS desconectados
    ws.cleanupClients();

    // [BLOQUE-CICLO-IR] Ejecutar ciclo IR si hay comando pendiente de D3
    if (irPendiente) {
        irPendiente = false;   // limpiar ANTES de ejecutar (evitar re-entrada)
        ejecutarCicloIR();
    }

    delay(5);   // ceder CPU al scheduler FreeRTOS (watchdog idle Core 1)
}