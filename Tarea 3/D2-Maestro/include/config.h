/**
 * @file    config.h
 * @brief   Configuración centralizada del Dispositivo 2 — Maestro IR + Servidor Wi-Fi (ESP32).
 *
 * @section secrets CREDENCIALES — LEER ANTES DE COMPILAR
 *
 *  Las secciones marcadas con [CONFIGURAR] DEBEN ser completadas antes
 *  de compilar.
 *
 *  Requisitos mínimos de seguridad:
 *   - Contraseña WPA2: mínimo 12 caracteres, mezclar mayúsculas,
 *     minúsculas, números y símbolos. Ejemplo: "Rf4el@Lab2025!"
 *   - No usar contraseñas por defecto ni secuencias predecibles.
 *   - El SSID no debe revelar información del equipo o institución.
 *
 * @section pines Criterios de selección de pines
 *
 *  Se evitan: GPIO 0, 2, 6-11, 12, 15, 34-39.
 *  Los pines IR del D2 son distintos a los del D1 para evitar confusión
 *  en el montaje, aunque eléctricamente son equivalentes.
 *
 * @authors Bevilacqua, Francisco — Clement, Sebastián
 * @date    7 de junio 2026
 */

#pragma once
#include <Arduino.h>
// ═══════════════════════════════════════════════════════════════════════════
//  [CONFIGURAR] CREDENCIALES WI-FI ACCESS POINT
//  ⚠ Completar estos valores antes de compilar.
//  ⚠ No subir a repositorios públicos con valores reales.
// ═══════════════════════════════════════════════════════════════════════════

// SSID de la red Wi-Fi creada por D2.
#define AP_SSID       "Nombre-RED"

// Contraseña WPA2 del Access Point.
#define AP_PASSWORD   "Contraseña-RED"

/** Canal Wi-Fi del Access Point (1-13).
 *  Usar canal 1, 6 o 11 para evitar solapamiento con redes vecinas. */
#define AP_CHANNEL    6

/** Número máximo de clientes simultáneos permitidos en el AP.
 *  Se limita a 1 para este proyecto (solo D3 se conecta). */
#define AP_MAX_CONN   1

// ═══════════════════════════════════════════════════════════════════════════
//  CONFIGURACIÓN DE RED
// ═══════════════════════════════════════════════════════════════════════════

/** IP estática del Access Point.
 *  192.168.4.1 es la IP por defecto del AP mode de ESP32.
 *  D3 accederá a http://192.168.4.1 para abrir la interfaz web. */
#define AP_IP         "192.168.4.1"

/** Puerto del servidor HTTP + WebSocket. */
#define SERVER_PORT   80

/** Ruta del endpoint WebSocket. */
#define WS_PATH       "/ws"

/** Intervalo del heartbeat WebSocket en el servidor [ms].
 *  ESPAsyncWebServer envía ping automático a los clientes cada este intervalo
 *  para detectar conexiones muertas (TCP zombie). */
#define WS_PING_INTERVAL_MS   10000

// ═══════════════════════════════════════════════════════════════════════════
//  PINES DE HARDWARE
// ═══════════════════════════════════════════════════════════════════════════

// ── Enlace infrarrojo ──────────────────────────────────────────────────────
#define IR_SEND_PIN   4    ///< KY-005 señal S  → GPIO  4 (output, RMT-capable)
#define IR_RECV_PIN   5    ///< KY-022 señal OUT → GPIO  5 (input, interrupt-capable)

// ── LED RGB (color actual del sistema) ────────────────────────────────────
#define PIN_R   25   ///< LED RGB canal rojo   — LEDC canal 3
#define PIN_G   26   ///< LED RGB canal verde  — LEDC canal 1
#define PIN_B   27   ///< LED RGB canal azul   — LEDC canal 2

// ── LEDs de estado de conectividad ────────────────────────────────────────
/** LED verde: encendido cuando D3 tiene sesión WebSocket activa. */
#define PIN_LED_CONN   32

/** LED rojo: encendido cuando D3 se desconecta abruptamente.
 *  Se apaga al reconectarse D3. */
#define PIN_LED_DISC   33

/** LED azul/blanco/amarillo : encendido cuando el Access Point Wi-Fi está activo
 *  y aceptando conexiones. Permanece encendido durante toda la operación
 *  normal del sistema. */
#define PIN_LED_AP     14

// ═══════════════════════════════════════════════════════════════════════════
//  CONFIGURACIÓN LEDC (PWM)
// ═══════════════════════════════════════════════════════════════════════════

#define LEDC_FREQ_HZ    5000
#define LEDC_RESOLUTION    8
#define LEDC_CH_R          3
#define LEDC_CH_G          1
#define LEDC_CH_B          2

// ═══════════════════════════════════════════════════════════════════════════
//  PROTOCOLO IR
// ═══════════════════════════════════════════════════════════════════════════

#define HDR_CMD   0xAA   ///< Cabecera de trama de comando (D2 → D1)
#define HDR_ACK   0xBB   ///< Cabecera de trama de ACK     (D1 → D2)

/** Cantidad máxima de intentos de envío IR antes de reportar fallo. */
#define MAX_REINTENTOS     3

/** Tiempo máximo de espera por el ACK de D1 por intento [ms]. */
#define TIMEOUT_ACK_MS  2000

/** Pausa entre reintentos para que el canal IR se libere [ms]. */
#define DELAY_REINTENTO_MS  300

// ═══════════════════════════════════════════════════════════════════════════
//  TABLA DE COLORES (secuencia del TP2)
// ═══════════════════════════════════════════════════════════════════════════

/** Número de colores en la secuencia circular. */
#define NUM_COLORES   8

/**
 * @brief Estructura que representa un color con su nombre simbólico y componentes RGB.
 *
 * El nombre se usa en los mensajes JSON hacia D3 para que la interfaz
 * web pueda mostrar texto descriptivo además del color visual.
 */
struct ColorRGB {
    const char* nombre;
    uint8_t     r, g, b;
};

/** Secuencia de colores del TP2, en orden de ciclo.
 *  La tabla es constante y reside en flash (PROGMEM no es necesario en ESP32,
 *  pero la declaración const asegura que el compilador la coloque en ROM). */
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

// ═══════════════════════════════════════════════════════════════════════════
//  PERSISTENCIA — Preferences (NVS)
// ═══════════════════════════════════════════════════════════════════════════

#define NVS_NAMESPACE        "d2_state"
#define NVS_KEY_MAGIC        "magic"
#define NVS_KEY_COLOR_IDX    "color_idx"
#define NVS_KEY_ENCENDIDO    "encendido"
#define NVS_MAGIC_VAL        0xA5

// ═══════════════════════════════════════════════════════════════════════════
//  MENSAJES JSON — TIPOS DE TRAMA (protocolo D2 ↔ D3)
// ═══════════════════════════════════════════════════════════════════════════

// Enviados por D2 → D3
#define MSG_ESTADO_ACTUAL   "ESTADO_ACTUAL"    ///< Estado completo al conectar D3
#define MSG_RESULTADO_CMD   "RESULTADO_CMD"    ///< Resultado de un comando enviado

// Recibidos de D3 → D2
#define MSG_CMD_COLOR       "CMD_COLOR"        ///< Cambiar a un color específico
#define MSG_CMD_ENCENDER    "CMD_ENCENDER"     ///< Encender LED con último color
#define MSG_CMD_APAGAR      "CMD_APAGAR"       ///< Apagar LED (RGB = 0,0,0)

// ═══════════════════════════════════════════════════════════════════════════
//  DEPURACIÓN
// ═══════════════════════════════════════════════════════════════════════════

#define SERIAL_BAUD   115200
