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
 *    - GPIO  0 : boot mode selector; INPUT solo.
 *    - GPIO  2 : debe estar en LOW durante la programación.
 *    - GPIO  6..11 : conectados a la flash SPI interna; NUNCA usar.
 *    - GPIO 12 : configura el voltaje de la flash en boot (MTDI).
 *    - GPIO 15 : MTDO; silencia el log de boot si está en LOW.
 *    - GPIO 34..39 : INPUT ONLY; sin driver de salida ni pull-up.
 *
 * @section crc Protocolo CRC-8
 *
 *  La trama IR de 32 bits tiene la estructura:
 *
 *   bits [31:24] — Componente R (0-255)
 *   bits [23:16] — Componente G (0-255)
 *   bits [15: 8] — Componente B (0-255)
 *   bits [ 7: 0] — CRC-8 calculado sobre los bytes R, G, B
 *
 *  El CRC-8 reemplaza a las cabeceras de protocolo. La integridad se
 *  verifica calculando el CRC sobre los tres bytes de datos y comparando
 *  con el byte de CRC recibido. No se usan librerías externas: el cálculo
 *  se implementa mediante aritmética módulo 2 (XOR) por software.
 *
 *  Polinomio: CRC-8/MAXIM (0x31 — 1-Wire), ampliamente documentado y con
 *  buena distancia Hamming para mensajes cortos de 3 bytes.
 *
 * @authors Bevilacqua, Francisco — Clement, Sebastián
 * @date    14 de junio 2026
 */

#pragma once
#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════════════════
//  PINES DE HARDWARE
// ═══════════════════════════════════════════════════════════════════════════

#define IR_SEND_PIN   23   ///< KY-005 señal S  — GPIO 23 (output, RMT-capable)
#define IR_RECV_PIN   22   ///< KY-022 señal OUT — GPIO 22 (input, interrupt-capable)

#define PIN_R   25         ///< LED RGB canal rojo   — cátodo común
#define PIN_G   26         ///< LED RGB canal verde
#define PIN_B   27         ///< LED RGB canal azul

// ═══════════════════════════════════════════════════════════════════════════
//  CONFIGURACIÓN LEDC (PWM)
// ═══════════════════════════════════════════════════════════════════════════

#define LEDC_FREQ_HZ    5000   ///< Frecuencia PWM: 5 kHz (sin parpadeo visible)
#define LEDC_RESOLUTION    8   ///< Resolución: 8 bits → rango 0-255
#define LEDC_CH_R          3   ///< Canal LEDC rojo   — Canal 3 (no 0; canal 0 lo usa IRremote para RMT)
#define LEDC_CH_G          1   ///< Canal LEDC verde
#define LEDC_CH_B          2   ///< Canal LEDC azul

// ═══════════════════════════════════════════════════════════════════════════
//  PROTOCOLO IR — TRAMA DE 32 BITS CON CRC-8
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Polinomio generador del CRC-8/MAXIM (Dallas/1-Wire).
 *
 * Valor: 0x31 → polinomio x^8 + x^5 + x^4 + 1.
 * Se aplica con reflexión de entrada y salida (LSB-first), conforme al
 * estándar CRC-8/MAXIM. Este polinomio tiene buena distancia Hamming (HD=4)
 * para mensajes de hasta 119 bits, ampliamente suficiente para 3 bytes de datos.
 * La implementación es puramente por software mediante XOR (aritmética módulo 2),
 * sin uso de librerías externas.
 */
#define CRC8_POLY   0x31   ///< Polinomio CRC-8/MAXIM

/**
 * @brief Retardo antes de enviar el ACK [ms].
 *
 * Tiempo que D1 espera antes de emitir el ACK para dar margen al Maestro
 * de completar IrReceiver.stop() → transmisión → IrReceiver.start()
 * antes de que llegue el ACK. Determinado experimentalmente.
 */
#define DELAY_ANTES_ACK_MS   120

// ═══════════════════════════════════════════════════════════════════════════
//  PERSISTENCIA — Preferences (NVS)
// ═══════════════════════════════════════════════════════════════════════════

#define NVS_NAMESPACE   "d1_state"   ///< Namespace NVS del Dispositivo 1
#define NVS_KEY_MAGIC   "magic"      ///< Centinela de validez de datos
#define NVS_KEY_R       "r"          ///< Componente rojo persistido
#define NVS_KEY_G       "g"          ///< Componente verde persistido
#define NVS_KEY_B       "b"          ///< Componente azul persistido
#define NVS_MAGIC_VAL   0xA5         ///< Valor centinela (patrón alternante, baja P de corrupción)

// ═══════════════════════════════════════════════════════════════════════════
//  DEPURACIÓN
// ═══════════════════════════════════════════════════════════════════════════

#define SERIAL_BAUD   115200