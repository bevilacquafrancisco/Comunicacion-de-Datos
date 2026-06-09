/**
 * @file    config.h
 * @brief   Configuración centralizada del Dispositivo 1 — Esclavo IR (ESP32).
 *
 * Todas las constantes de hardware, protocolo y persistencia del D1 están
 * declaradas aquí. Modificar este archivo es suficiente para adaptar el
 * firmware a distintos pinouts o parámetros de protocolo sin tocar la
 * lógica de main.cpp.
 *
 * @section pines Criterios de selección de pines (ESP32 DevKit v1)
 *
 *  Los siguientes GPIO están PROHIBIDOS o son problemáticos en ESP32:
 *    - GPIO  0 : boot mode selector (pull-down en flash); INPUT solo.
 *    - GPIO  2 : debe estar en LOW durante la programación.
 *    - GPIO  6..11 : conectados a la flash SPI interna; NUNCA usar.
 *    - GPIO 12 : configura el voltaje de la flash en boot (MTDI); evitar OUTPUT.
 *    - GPIO 15 : MTDO; silencia el log de boot si está en LOW.
 *    - GPIO 34..39 : INPUT ONLY (no tienen driver de salida ni pull-up).
 *
 *  Pines elegidos para D1: todos son GPIO de propósito general con soporte
 *  de interrupción, PWM (LEDC) y niveles de 3.3 V tolerados por KY-005/022.
 *
 * @authors Bevilacqua, Francisco — Clement, Sebastián
 * @date    7 de junio 2026
 */

#pragma once

// ═══════════════════════════════════════════════════════════════════════════
//  PINES DE HARDWARE
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @defgroup pines_ir Pines del enlace infrarrojo
 * @{
 *  IR_SEND_PIN : GPIO de salida hacia el módulo KY-005 (emisor IR).
 *               IRremote usa el canal RMT del ESP32 para generar la portadora
 *               de 38 kHz con precisión hardware; cualquier GPIO de salida
 *               es válido para RMT.
 *
 *  IR_RECV_PIN : GPIO de entrada desde el módulo KY-022 (receptor IR).
 *               Requiere soporte de interrupción (todos los GPIO del ESP32
 *               lo soportan excepto 34-39 que son solo input sin pull).
 *               GPIO 22 es seguro: no tiene función especial en boot.
 * @}
 */
#define IR_SEND_PIN   23    ///< KY-005 señal S  → GPIO 23 (output, RMT-capable)
#define IR_RECV_PIN   22    ///< KY-022 señal OUT → GPIO 22 (input, interrupt-capable)

/**
 * @defgroup pines_rgb Pines del LED RGB (cátodo común)
 * @{
 *  El ESP32 maneja PWM a través del módulo LEDC (16 canales independientes,
 *  resolución configurable hasta 16 bits). Cualquier GPIO de salida puede
 *  usarse como canal LEDC. Se eligen GPIO en el rango 25-27 por estar lejos
 *  de los pines de arranque y ser completamente de propósito general.
 * @}
 */
#define PIN_R   25   ///< LED RGB canal rojo   — LEDC channel 3
#define PIN_G   26   ///< LED RGB canal verde  — LEDC channel 1
#define PIN_B   27   ///< LED RGB canal azul   — LEDC channel 2

// ═══════════════════════════════════════════════════════════════════════════
//  CONFIGURACIÓN LEDC (PWM)
// ═══════════════════════════════════════════════════════════════════════════

#define LEDC_FREQ_HZ    5000   ///< Frecuencia PWM: 5 kHz (evita parpadeo visible)
#define LEDC_RESOLUTION    8   ///< Resolución en bits: 0-255 (compatible con analogWrite)
#define LEDC_CH_R          3   ///< Canal LEDC para rojo
#define LEDC_CH_G          1   ///< Canal LEDC para verde
#define LEDC_CH_B          2   ///< Canal LEDC para azul

// ═══════════════════════════════════════════════════════════════════════════
//  PROTOCOLO IR — TRAMA DE 32 BITS (NEC RAW)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @defgroup protocolo_ir Cabeceras del protocolo IR propietario
 * @{
 *  La trama de 32 bits tiene la siguiente estructura:
 *
 *   bits [31:24] — Cabecera (1 byte):
 *                  0xAA = comando de color (D2 → D1)
 *                  0xBB = confirmación ACK  (D1 → D2)
 *   bits [23:16] — Componente R (0-255)
 *   bits [15: 8] — Componente G (0-255)
 *   bits [ 7: 0] — Componente B (0-255)
 *
 *  El uso de cabeceras distintas permite al receptor filtrar sus propios
 *  ecos y distinguir comandos de ACKs en el medio compartido.
 * @}
 */
#define HDR_CMD   0xAA   ///< Cabecera de trama de comando   (D2 → D1)
#define HDR_ACK   0xBB   ///< Cabecera de trama de respuesta (D1 → D2)

/// Retardo antes de enviar el ACK para dar tiempo al Maestro
/// de reactivar su receptor tras detener la transmisión [ms].
#define DELAY_ANTES_ACK_MS   120

// ═══════════════════════════════════════════════════════════════════════════
//  PERSISTENCIA — Preferences (NVS)
// ═══════════════════════════════════════════════════════════════════════════

#define NVS_NAMESPACE   "d1_state"   ///< Namespace NVS del Dispositivo 1
#define NVS_KEY_MAGIC   "magic"      ///< Clave centinela de validación
#define NVS_KEY_R       "r"          ///< Clave del componente rojo persistido
#define NVS_KEY_G       "g"          ///< Clave del componente verde persistido
#define NVS_KEY_B       "b"          ///< Clave del componente azul persistido
#define NVS_MAGIC_VAL   0xA5         ///< Valor centinela: datos NVS válidos

// ═══════════════════════════════════════════════════════════════════════════
//  DEPURACIÓN
// ═══════════════════════════════════════════════════════════════════════════

#define SERIAL_BAUD   115200   ///< Velocidad del puerto serie para logs
