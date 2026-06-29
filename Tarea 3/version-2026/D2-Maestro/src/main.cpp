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
 * @date    29 de junio 2026
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
#include <ctype.h>    // isdigit() — usado en formatearJsonParaLog()

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
volatile unsigned long ultimo_msj_ws = 0;  ///< Tiempo del último mensaje recibido de D3

// CRC-16 del comando de D3 que disparó el ciclo IR en curso (Etapa ① del
// flujo D3→D2→D1→D2→D3). Se capturan en procesarMensajeWS() al validar el
// comando y se conservan durante todo el ciclo IR para poder mostrarlos,
// junto a los CRC-8 de D1 y el CRC-16 de la respuesta, en un resumen final
// comparativo (ejecutarCicloIR()) — sin esto, el CRC-16 de D3 sólo se vería
// en el log de recepción, separado por todo el bloque IR del resto del ciclo.
volatile uint16_t crc16_rx_pendiente   = 0;   ///< CRC-16 recibido en el JSON de D3
volatile uint16_t crc16_calc_pendiente = 0;   ///< CRC-16 recalculado por D2 sobre el mismo payload

// CRC-8 de la última trama COMANDO enviada (Etapa ②) y del último ACK
// válido recibido de D1 (Etapa ③). Se actualizan dentro de enviarColorIR()
// en cada intento; al finalizar el ciclo (con o sin éxito) reflejan los
// valores del último intento, para el resumen comparativo de 4 firmas.
volatile uint8_t  crc8_cmd_ultimo = 0;   ///< CRC-8/MAXIM de la trama COMANDO (D2→D1)
volatile uint8_t  crc8_ack_ultimo = 0;   ///< CRC-8/MAXIM invertido del ACK (D1→D2)

// ═══════════════════════════════════════════════════════════════════════════
//  PROTOTIPOS
// ═══════════════════════════════════════════════════════════════════════════

void     iniciarLedc();
void     setColor(uint8_t r, uint8_t g, uint8_t b);
uint8_t  calcularCRC8(uint8_t r, uint8_t g, uint8_t b);
uint16_t calcularCRC16(const char *payload, size_t len);
void     formatearJsonParaLog(const char *json_in, uint16_t crc,
                               char *out, size_t out_size);
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

/**
 * @brief Reescribe el campo "crc16":<decimal> de un JSON ya serializado a
 *        formato hexadecimal SOLO para fines de log por monitor serial.
 *
 * @section motivo Por qué existe esta función
 *
 *  ArduinoJson serializa los campos numéricos en decimal (ej. "crc16":12582),
 *  mientras que el resto de los logs de este firmware muestran el CRC-16 en
 *  hexadecimal (ej. CRC-16=0x3126). Esa mezcla decimal/hex dentro de una
 *  misma línea de log dificulta contrastar a simple vista el valor mostrado
 *  en D2 contra el que muestra la consola del navegador de D3 (que también
 *  loguea en hex). Esta función NO altera el JSON real transmitido por
 *  WebSocket — solo genera una copia de texto para Serial.print, dejando
 *  buf_final intacto para que D3 lo parsee con JSON.parse() sin cambios.
 *
 * @section implementacion Implementación
 *
 *  Busca la subcadena literal `"crc16":` dentro de json_in, copia todo lo
 *  anterior a out tal cual, y en lugar del número decimal que sigue escribe
 *  `"0xXXXX"` (como string, entre comillas, para que quede visualmente
 *  inequívoco que es hexadecimal). Luego copia el resto del JSON (la llave
 *  de cierre) sin modificar. Si la subcadena no se encuentra (no debería
 *  ocurrir, ya que todo mensaje saliente de D2 lleva "crc16"), se copia el
 *  JSON original sin cambios como salvaguarda defensiva.
 *
 * @param json_in   JSON final ya serializado, con "crc16":<decimal> incluido.
 * @param crc       Valor de CRC-16 conocido (evita tener que parsear json_in).
 * @param out       Buffer de salida donde escribir el JSON con CRC en hex.
 * @param out_size  Tamaño total del buffer out.
 */
void formatearJsonParaLog(const char *json_in, uint16_t crc,
                           char *out, size_t out_size) {
    const char *clave   = "\"crc16\":";
    const char *pos_clave = strstr(json_in, clave);

    if (!pos_clave) {
        // [BLOQUE-LOG-HEX] Salvaguarda: sin "crc16" en el JSON, copiar tal cual
        snprintf(out, out_size, "%s", json_in);
        return;
    }

    // [BLOQUE-LOG-HEX] Posición donde empieza el valor numérico tras la clave
    const char *inicio_valor = pos_clave + strlen(clave);

    // [BLOQUE-LOG-HEX] Avanzar hasta el primer caracter que no sea parte del
    // número decimal (la llave de cierre '}' o una coma si hay mas campos)
    const char *fin_valor = inicio_valor;
    while (*fin_valor && (isdigit((unsigned char)*fin_valor))) {
        fin_valor++;
    }

    size_t len_prefijo = (size_t)(pos_clave - json_in) + strlen(clave);

    // [BLOQUE-LOG-HEX] Reconstruir: prefijo + clave + "0xXXXX" + resto del JSON
    int escrito = snprintf(out, out_size, "%.*s\"0x%04X\"%s",
                            (int)len_prefijo, json_in, crc, fin_valor);

    if (escrito < 0 || (size_t)escrito >= out_size) {
        // [BLOQUE-LOG-HEX] Buffer insuficiente: salvaguarda con el JSON original
        snprintf(out, out_size, "%s", json_in);
    }
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
    crc8_cmd_ultimo = crc_tx;   // [RESUMEN-4-FIRMAS] Etapa ② — CRC-8 de la trama COMANDO

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
            crc8_ack_ultimo = ack_crc;   // [RESUMEN-4-FIRMAS] Etapa ③ — CRC-8 (~CRC) del ACK
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
 * @section filtro_log_keepalive Filtro de logs para PING/PONG
 *
 *  La lógica de cálculo y envío del CRC-16 es idéntica para todos los tipos
 *  de mensaje, incluido PONG. Lo único que se suprime es la línea de log
 *  "[WS-TX] → cliente ... " cuando el mensaje a enviar es PONG, para no
 *  saturar el monitor serial con el tráfico periódico de keepalive. Los
 *  mensajes de comando/resultado (ESTADO_ACTUAL, RESULTADO_CMD, CRC_ERROR,
 *  ACK_DESCONECTAR) siempre se loguean con su CRC-16 enviado.
 *
 * @param client  Cliente WebSocket destinatario.
 * @param doc     Documento JSON a enviar (se modifica in-place agregando "crc16").
 */
void enviarMensajeWS(AsyncWebSocketClient *client, JsonDocument &doc) {
    // [BLOQUE-FILTRO-LOG] PONG es la única respuesta de keepalive que D2 ENVÍA;
    // se silencia el log de "mensaje de comando" pero el CRC-16 se calcula y
    // se transmite exactamente igual, sin ninguna rama distinta de código.
    const char *tipo_tx = doc["tipo"] | "";
    bool es_keepalive_tx = (strcmp(tipo_tx, "PONG") == 0);

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

    // [BLOQUE-LOG] Traza de envío — silenciada solo para PONG, nunca para
    // mensajes de comando/resultado (ESTADO_ACTUAL, RESULTADO_CMD, CRC_ERROR,
    // ACK_DESCONECTAR), que siempre muestran su CRC-16 enviado.
    //
    // El JSON que se imprime (buf_log) NO es el mismo que se transmitió
    // (buf_final): en buf_log el campo "crc16" se reescribe en hexadecimal
    // solo para que el log sea legible y comparable con el de D3 (que
    // también logea en hex). El dato real enviado por WebSocket sigue
    // siendo buf_final, con "crc16" numérico estándar.
    if (!es_keepalive_tx) {
        char buf_log[340];
        formatearJsonParaLog(buf_final, crc, buf_log, sizeof(buf_log));
        Serial.printf("[WS-TX] → cliente #%u | CRC-16=0x%04X | %s\n",
                      client->id(), crc, buf_log);
    }
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

    // [BLOQUE-LOG] buf_log reescribe "crc16" en hexadecimal solo para el log
    // (ver formatearJsonParaLog); el dato real transmitido es buf_final.
    char buf_log[340];
    formatearJsonParaLog(buf_final, crc, buf_log, sizeof(buf_log));
    Serial.printf("[WS-TX] Broadcast | CRC-16=0x%04X | %s\n", crc, buf_log);
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

    // [REFACTORIZACION LOGS] Pre-calcular el CRC-16 de la respuesta
    // para armar el bloque de resumen teórico (Flujo_CRC) antes de enviar
    char buf_base[300];
    size_t len_base = serializeJson(doc, buf_base, sizeof(buf_base));
    uint16_t crc16_tx = calcularCRC16(buf_base, len_base);

    // ------------------------------------------------------------------
    //  BLOQUE INTEGRAL DE RESUMEN DE FIRMAS (Flujo D3 → D2 → D1 → D2 → D3)
    // ------------------------------------------------------------------
    Serial.println("\n[D2] ═════════ RESUMEN DE FIRMAS CRC (3 NODOS) ═════════");
    Serial.printf( "[CRC-16] *1* Recibido en D2 desde D3 (Comando WS): 0x%04X\n", crc16_rx_pendiente);
    
    if (exito) {
        Serial.printf( "[CRC-8]  *2* Enviado de D2 a D1 (Comando IR)   : 0x%02X\n", crc8_cmd_ultimo);
        Serial.printf( "[CRC-8]  *3* Recibido en D2 desde D1 (ACK ~CRC) : 0x%02X\n", crc8_ack_ultimo);
    } else {
        Serial.printf( "[CRC-8]  *2* Enviado de D2 a D1 (Comando IR)   : 0x%02X\n", crc8_cmd_ultimo);
        Serial.println("[CRC-8]  *3* Recibido en D2 desde D1 (ACK ~CRC) : FALLO / TIMEOUT");
    }
    
    Serial.printf( "[CRC-16] *4* Enviado de D2 a D3 (Respuesta WS): 0x%04X\n", crc16_tx);
    Serial.println("[D2] ════════════════════════════════════════════════════\n");
    // ------------------------------------------------------------------

    Serial.printf("[WS] Notificando resultado → exito=%s  enc=%s  color=%s  RGB(%u,%u,%u)\n",
                  exito ? "SI" : "NO",
                  encendido ? "SI" : "NO",
                  encendido ? COLORES[colorActual].nombre : "-",
                  encendido ? COLORES[colorActual].r : 0,
                  encendido ? COLORES[colorActual].g : 0,
                  encendido ? COLORES[colorActual].b : 0);

    // Enviar por WebSocket aprovechando el crc16_tx ya calculado (evita doble cálculo)
    doc["crc16"] = crc16_tx;
    char buf_final[320];
    serializeJson(doc, buf_final, sizeof(buf_final));
    ws.textAll(buf_final);

    // Loguear el JSON enviado con formato hex
    char buf_log[340];
    formatearJsonParaLog(buf_final, crc16_tx, buf_log, sizeof(buf_log));
    Serial.printf("[WS-TX] Broadcast | CRC-16=0x%04X | %s\n", crc16_tx, buf_log);
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
 * @section filtro_log_keepalive Filtro de logs para PING/PONG
 *
 *  Los mensajes PING (recibidos) y PONG (respondidos, ver enviarMensajeWS())
 *  no generan NINGÚN log en el monitor serial: ni el payload crudo recibido,
 *  ni la línea de verificación "[CRC-16] rx=... calc=...", ni "tipo=PING",
 *  ni "Integridad verificada", ni la traza de envío del PONG. El heartbeat
 *  queda completamente silencioso para no saturar la consola con tráfico
 *  periódico. La lógica de chequeo de conexión PING-PONG en sí no se
 *  modifica: D2 sigue calculando y validando el CRC-16 de cada PING, y
 *  sigue respondiendo PONG con su propio CRC-16 calculado correctamente —
 *  simplemente no se imprime nada de eso por Serial.
 *
 *  Para cualquier otro tipo de mensaje (CMD_COLOR, CMD_ENCENDER, CMD_APAGAR,
 *  CMD_DESCONECTAR, y los descartados por CRC inválido) todos los logs se
 *  mantienen exactamente igual que antes, incluyendo siempre el CRC-16
 *  recibido y calculado.
 *
 * @param client   Cliente WS que envió el mensaje.
 * @param payload  Buffer del mensaje JSON recibido.
 * @param len      Longitud del payload en bytes.
 */
void procesarMensajeWS(AsyncWebSocketClient *client,
                       const uint8_t *payload, size_t len) {
    // [BLOQUE-PARSE] Copiar el payload a un buffer local null-terminado.
    // El buffer que entrega ESPAsyncWebServer (payload/len) no garantiza
    // terminador nulo; formatearJsonParaLog() y los logs con "%s" requieren
    // un C-string válido, de modo que se trabaja sobre esta copia segura
    // en lugar del puntero crudo del framework.
    char payload_str[300];
    size_t len_copia = (len < sizeof(payload_str) - 1) ? len : sizeof(payload_str) - 1;
    memcpy(payload_str, payload, len_copia);
    payload_str[len_copia] = '\0';

    // [BLOQUE-PARSE] Deserializar JSON recibido
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        Serial.printf("[WS-RX] Error JSON: %s — payload: %s\n",
                      err.c_str(), payload_str);
        return;
    }

    // [BLOQUE-FILTRO-LOG] Determinar si es trafico de keepalive (PING).
    // Este flag silencia TODOS los logs asociados a este mensaje, incluida
    // la linea de verificacion de CRC-16. El calculo y la comparacion
    // matematica del CRC (crc_rx == crc_calc) se siguen ejecutando igual
    // para PING; solo se omite la impresion por Serial.
    const char *tipo_preliminar = doc["tipo"] | "";
    bool es_keepalive = (strcmp(tipo_preliminar, "PING") == 0);

    // [BLOQUE-CRC16] Extraer campo CRC-16 del mensaje. Se hace ANTES del log
    // del payload crudo (más abajo) para poder reescribir ese log con el
    // CRC en hexadecimal en lugar del decimal que envía JSON.stringify en D3.
    if (!doc["crc16"].is<uint16_t>()) {
        if (!es_keepalive) {
            Serial.printf("[WS-RX] ← cliente #%u | %s\n",
                          client->id(), payload_str);
        }
        Serial.println("[CRC-16] Mensaje sin campo crc16 — descartado");
        return;
    }
    uint16_t crc_rx = doc["crc16"].as<uint16_t>();

    // [BLOQUE-LOG] Log del mensaje crudo recibido (silenciado para PING).
    //
    // buf_log_rx reescribe "crc16":<decimal> (tal como lo serializa
    // JSON.stringify en D3) a "crc16":"0xXXXX" SOLO para este log, de forma
    // que el valor recibido sea directamente comparable, en el mismo
    // formato hexadecimal, con el que muestra la consola del navegador de
    // D3 y con el resto de los logs de CRC-16 de D2. El payload real ya fue
    // parseado en doc/deserializeJson más arriba; esta copia es puramente
    // cosmética para el monitor serial.
    if (!es_keepalive) {
        char buf_log_rx[340];
        formatearJsonParaLog(payload_str, crc_rx, buf_log_rx, sizeof(buf_log_rx));
        Serial.printf("[WS-RX] ← cliente #%u | %s\n",
                      client->id(), buf_log_rx);
    }

    // [BLOQUE-CRC16] Eliminar crc16, serializar y recalcular para verificar
    doc.remove("crc16");
    char buf_check[300];
    size_t len_check = serializeJson(doc, buf_check, sizeof(buf_check));
    uint16_t crc_calc = calcularCRC16(buf_check, len_check);

    // [BLOQUE-LOG] Log de verificación CRC-16 — silenciado para PING/PONG.
    // A pedido explicito, el heartbeat queda con CERO logs en el monitor
    // serial: ni el payload crudo, ni esta linea de CRC, ni "tipo=", ni
    // "Integridad verificada". El chequeo matematico de crc_rx == crc_calc
    // SI se sigue ejecutando igual para PING (si fallara, simplemente no se
    // responde el PONG y el timeout del lado de D3 detecta la conexion
    // muerta); lo unico que cambia es que no se imprime nada por Serial.
    if (!es_keepalive) {
        Serial.printf("[CRC-16] rx=0x%04X  calc=0x%04X  base=%s\n",
                      crc_rx, crc_calc, buf_check);
    }

    if (crc_rx != crc_calc) {
        if (!es_keepalive) {
            Serial.printf("[CRC-16] ERROR — Integridad comprometida. Mensaje descartado.\n");
        }
        // [BLOQUE-CRC16] Notificar el error al cliente con su propio CRC-16
        JsonDocument err_doc;
        err_doc["tipo"] = MSG_CRC_ERROR;
        err_doc["info"] = "CRC-16 invalido — mensaje descartado";
        enviarMensajeWS(client, err_doc);
        return;
    }

    const char *tipo = doc["tipo"] | "";

    // [BLOQUE-PING] Responder keepalive con PONG (también con CRC-16).
    // Se retorna ANTES de loguear "Integridad verificada" / "tipo=" para que
    // el trafico periodico PING-PONG no aparezca como mensaje de comando.
    if (es_keepalive) {
        JsonDocument pong;
        pong["tipo"] = "PONG";
        enviarMensajeWS(client, pong);
        return;
    }

    Serial.println("[CRC-16] OK — Integridad verificada.");
    Serial.printf("[WS-RX] tipo=%s\n", tipo);

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

    // [BLOQUE-CRC16-PENDIENTE] Capturar el CRC-16 de este comando (Etapa ①
    // del flujo D3→D2→D1→D2→D3) para poder mostrarlo en el resumen
    // comparativo de las 4 firmas al final de ejecutarCicloIR(). En este
    // punto crc_rx y crc_calc ya fueron validados como iguales más arriba;
    // se guardan ambos para loguear el mismo par "recibido/recalculado"
    // que documenta la Etapa 2 del flujo teórico (Flujo_CRC_VERDE).
    crc16_rx_pendiente   = crc_rx;
    crc16_calc_pendiente = crc_calc;

    if (strcmp(tipo, MSG_CMD_COLOR) == 0) {
        // [CORRECCION] Control de estado: Si está apagado, se rechaza el comando de color
        if (!encendido) {
            Serial.println("[D2] CMD_COLOR descartado: El sistema se encuentra apagado.");
            
            // Re-enviamos el estado actual (apagado) a D3 para que la interfaz web deshaga
            // el intento de cambio y vuelva a sincronizarse correctamente sin encenderse.
            JsonDocument doc_estado;
            doc_estado["tipo"]      = MSG_ESTADO_ACTUAL;
            doc_estado["encendido"] = false;
            doc_estado["color"]     = "";
            doc_estado["r"]         = 0;
            doc_estado["g"]         = 0;
            doc_estado["b"]         = 0;
            enviarMensajeWS(client, doc_estado);
            return;  // Abortamos la ejecución para que no inicie el ciclo IR
        }
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
            ultimo_msj_ws = millis();  // Iniciar contador al conectar
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
            ultimo_msj_ws = millis();  //Resetear contador al recibir datos
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
    // [NUEVO] Watchdog de WebSocket (Detector de Conexiones Zombi)
    // D3 envía un PING periódico. Si pasan 10 segundos sin recibir NADA,
    // asumimos que la conexión de red se cayó de forma abrupta.
    if (ws.count() > 0 && (millis() - ultimo_msj_ws > 10000)) {
        Serial.println("\n[WS] Timeout: Se perdió el contacto con D3. Forzando cierre...");
        ws.closeAll(); // Esto dispara automáticamente WS_EVT_DISCONNECT
    }

    // [BLOQUE-CICLO-IR] Ejecutar ciclo IR si hay comando pendiente de D3
    if (irPendiente) {
        irPendiente = false;   // limpiar ANTES de ejecutar (evitar re-entrada)
        ejecutarCicloIR();
    }

    delay(5);   // ceder CPU al scheduler FreeRTOS (watchdog idle Core 1)
}