/**
 * @file    config.h
 * @brief   Configuracion centralizada del Dispositivo 2 — Maestro IR + Servidor Wi-Fi (ESP32).
 *
 * @section secrets CREDENCIALES — COMPLETAR ANTES DE COMPILAR
 *
 *  Las macros marcadas con [CONFIGURAR] deben ser completadas antes de compilar.
 *
 *  Requisitos de la contrasena WPA2:
 *   - Minimo 12 caracteres.
 *   - Combinar mayusculas, minusculas, numeros y simbolos.
 *   - No usar informacion personal ni secuencias predecibles.
 *
 * @section crc_ir CRC-8 en el protocolo IR (D1 — D2)
 *
 *  La trama IR de 32 bits no lleva cabecera; la integridad se verifica con CRC-8:
 *
 *   bits [31:24] — R (0-255)
 *   bits [23:16] — G (0-255)
 *   bits [15: 8] — B (0-255)
 *   bits [ 7: 0] — CRC-8/MAXIM calculado sobre R, G, B
 *
 *  Implementacion: XOR (aritmetica modulo 2) por software. Sin librerias.
 *  Polinomio: 0x31 (CRC-8/MAXIM, Dallas/1-Wire). LSB-first.
 *
 * @section crc_ws CRC-16 en el protocolo WebSocket (D2 — D3)
 *
 *  Cada mensaje JSON enviado o recibido por WebSocket lleva un campo
 *  adicional "crc16" con el CRC-16 calculado sobre el payload JSON sin
 *  ese campo. El receptor valida el CRC antes de procesar el mensaje.
 *
 *  Implementacion: XOR por software. Polinomio: CRC-16/IBM (0x8005). Sin librerias.
 *
 * @section pines Criterios de seleccion de pines
 *
 *  Se evitan: GPIO 0, 2, 6-11, 12, 15, 34-39.
 *
 * @authors Bevilacqua, Francisco — Clement, Sebastian
 * @date    14 de junio 2026
 */

#pragma once
#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════════════════
//  [CONFIGURAR] CREDENCIALES WI-FI ACCESS POINT
// ═══════════════════════════════════════════════════════════════════════════

#define AP_SSID       "Nombre-RED"                        ///< SSID de la red Wi-Fi de D2
#define AP_PASSWORD   "Contraseña-RED"                    ///< Contrasena WPA2 (>= 12 chars)
#define AP_CHANNEL    6                                   ///< Canal 1, 6 u 11 (no solapados)
#define AP_MAX_CONN   1                                   ///< Solo D3 se conecta

// ═══════════════════════════════════════════════════════════════════════════
//  CONFIGURACION DE RED
// ═══════════════════════════════════════════════════════════════════════════
// El dispositivo 3 accede a la GUI mediante http://192.168.4.1
#define AP_IP            "192.168.4.1"   ///< IP por defecto del AP mode del ESP32
#define SERVER_PORT      80              ///< Puerto HTTP + WebSocket
#define WS_PATH          "/ws"           ///< Endpoint WebSocket en el servidor HTTP
#define WS_PING_INTERVAL_MS   10000      ///< Intervalo de ping del servidor [ms]

// ═══════════════════════════════════════════════════════════════════════════
//  PINES DE HARDWARE
// ═══════════════════════════════════════════════════════════════════════════

#define IR_SEND_PIN   4    ///< KY-005 S   → GPIO  4 (output, RMT-capable)
#define IR_RECV_PIN   5    ///< KY-022 OUT → GPIO  5 (input, interrupt-capable)

#define PIN_R   25         ///< LED RGB rojo   — LEDC canal 3
#define PIN_G   26         ///< LED RGB verde  — LEDC canal 1
#define PIN_B   27         ///< LED RGB azul   — LEDC canal 2

#define PIN_LED_CONN   32  ///< LED verde    : D3 conectado con sesion WS activa
#define PIN_LED_DISC   33  ///< LED rojo     : desconexion de D3 detectada
#define PIN_LED_AP     14  ///< LED amarillo : Access Point activo

// ═══════════════════════════════════════════════════════════════════════════
//  CONFIGURACION LEDC (PWM)
// ═══════════════════════════════════════════════════════════════════════════

#define LEDC_FREQ_HZ    5000   ///< 5 kHz: sin parpadeo visible
#define LEDC_RESOLUTION    8   ///< 8 bits: rango 0-255
#define LEDC_CH_R          3   ///< Canal 3 para rojo (canal 0 reservado por IRremote/RMT)
#define LEDC_CH_G          1
#define LEDC_CH_B          2

// ═══════════════════════════════════════════════════════════════════════════
//  CRC-8 — PROTOCOLO IR (D1 — D2)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Polinomio CRC-8/MAXIM (Dallas/1-Wire).
 *
 * Valor: 0x31 = x^8 + x^5 + x^4 + 1.
 * Distancia Hamming HD=4 para mensajes de hasta 119 bits.
 * Procesamiento LSB-first con reflejo de entrada y salida.
 */
#define CRC8_POLY        0x31   ///< Polinomio generador CRC-8/MAXIM

/// Retardo en D2 antes de reactivar el receptor para escuchar el ACK [ms].
#define DELAY_ANTES_ACK_MS   120

/// Timeout maximo de espera por ACK de D1 por intento [ms].
#define TIMEOUT_ACK_MS   2000

/// Pausa entre reintentos de envio IR [ms].
#define DELAY_REINTENTO_MS   300

/// Maximo de intentos de envio IR antes de reportar fallo.
#define MAX_REINTENTOS   3

// ═══════════════════════════════════════════════════════════════════════════
//  CRC-16 — PROTOCOLO WEBSOCKET (D2 — D3)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Polinomio CRC-16/IBM (CRC-16/ARC).
 *
 * Valor: 0x8005 = x^16 + x^15 + x^2 + 1.
 * Estandar industrial ampliamente documentado.
 * HD=4 para mensajes de hasta 32.767 bits; suficiente para los payloads JSON
 * de este sistema (< 300 bytes).
 * Procesamiento LSB-first con reflejo de entrada y salida.
 * Valor inicial: 0x0000. XOR final: 0x0000.
 */
#define CRC16_POLY   0x8005   ///< Polinomio generador CRC-16/IBM

// ═══════════════════════════════════════════════════════════════════════════
//  TABLA DE COLORES
// ═══════════════════════════════════════════════════════════════════════════

/** Numero de colores en la paleta. */
#define NUM_COLORES   12

/**
 * @brief Entrada de la tabla de colores: nombre simbolico y componentes RGB.
 *
 * El nombre se usa en los mensajes JSON entre D2 y D3.
 * La tabla es constante (reside en ROM del ESP32).
 */
struct ColorRGB {
    const char* nombre;
    uint8_t     r, g, b;
};

/**
 * @brief Paleta de colores disponibles para control desde D3.
 *
 * 12 colores: los 8 originales del TP2 mas 4 nuevos (NARANJA, MAGENTA,
 * TURQUESA, VIOLETA) para mayor expresividad cromatica y cumplir la
 * consigna de eleccion de color RGB variado.
 */
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
    { "NARANJA",  255,  80,   0 },   ///< Naranja saturado
    { "MAGENTA",  255,   0, 180 },   ///< Magenta / fucsia
    { "TURQUESA",   0, 210, 140 },   ///< Turquesa (verde-azulado)
    { "VIOLETA",   90,   0, 200 },   ///< Violeta profundo
};

// ═══════════════════════════════════════════════════════════════════════════
//  PERSISTENCIA — Preferences (NVS)
// ═══════════════════════════════════════════════════════════════════════════

#define NVS_NAMESPACE        "d2_state"
#define NVS_KEY_MAGIC        "magic"
#define NVS_KEY_COLOR_IDX    "color_idx"
#define NVS_KEY_ENCENDIDO    "encendido"
#define NVS_MAGIC_VAL        0xA5

// ═══════════════════════════════════════════════════════════════════════════
//  MENSAJES JSON — TIPOS DE TRAMA (protocolo D2 — D3)
// ═══════════════════════════════════════════════════════════════════════════

// D2 → D3
#define MSG_ESTADO_ACTUAL   "ESTADO_ACTUAL"    ///< Estado completo al conectar D3
#define MSG_RESULTADO_CMD   "RESULTADO_CMD"    ///< Resultado del ciclo IR hacia D1
#define MSG_CRC_ERROR       "CRC_ERROR"        ///< CRC-16 invalido en mensaje recibido

// D3 → D2
#define MSG_CMD_COLOR       "CMD_COLOR"        ///< Cambiar a color especifico
#define MSG_CMD_ENCENDER    "CMD_ENCENDER"     ///< Encender LED con ultimo color
#define MSG_CMD_APAGAR      "CMD_APAGAR"       ///< Apagar LED (RGB = 0,0,0)
#define MSG_CMD_DESCONECTAR "CMD_DESCONECTAR"  ///< Desconexion voluntaria de D3

// ═══════════════════════════════════════════════════════════════════════════
//  DEPURACION
// ═══════════════════════════════════════════════════════════════════════════

#define SERIAL_BAUD   115200