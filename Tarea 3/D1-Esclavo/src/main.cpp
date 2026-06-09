/**
 * @file    main.cpp
 * @brief   Dispositivo 1 — Esclavo IR bidireccional (ESP32).
 *
 * @section descripcion Descripción funcional
 *
 *  Este firmware implementa el nodo receptor del sistema de control cromático.
 *  El dispositivo permanece a la escucha de tramas infrarrojas provenientes del
 *  Dispositivo 2 (Maestro). Al recibir un comando válido:
 *
 *   1. Aplica el color RGB al LED local mediante PWM (LEDC).
 *   2. Persiste el nuevo estado en la memoria no volátil (NVS / Preferences).
 *   3. Transmite un ACK infrarrojo al Maestro con la cabecera 0xBB y los mismos
 *      valores RGB, permitiendo que el Maestro valide la coherencia de estado.
 *
 *  El D1 no posee lógica de control propia: su estado depende exclusivamente
 *  de los comandos recibidos por infrarrojo. Al energizarse, recupera el último
 *  color persistido en NVS y lo aplica de inmediato.
 *
 * @section protocolo Protocolo IR (NEC Raw, 32 bits)
 *
 *  Trama de COMANDO recibida desde D2:
 *   ┌────────────┬───────────┬───────────┬───────────┐
 *   │ bits 31-24 │ bits 23-16│ bits 15-8 │ bits 7-0  │
 *   │  0xAA (CMD)│     R     │     G     │     B     │
 *   └────────────┴───────────┴───────────┴───────────┘
 *
 *  Trama de ACK enviada hacia D2:
 *   ┌────────────┬───────────┬───────────┬───────────┐
 *   │ bits 31-24 │ bits 23-16│ bits 15-8 │ bits 7-0  │
 *   │  0xBB (ACK)│     R     │     G     │     B     │
 *   └────────────┴───────────┴───────────┴───────────┘
 *
 *  El uso de NEC Raw (sendNECRaw / decodedRawData) evita las inversiones de
 *  bits que aplica el protocolo NEC estándar, preservando los bytes tal como
 *  fueron construidos en la aplicación.
 *
 * @section hardware Hardware
 *
 *  Módulo          │ Pin ESP32 │ Notas
 *  ────────────────┼───────────┼──────────────────────────────────────
 *  KY-022 OUT      │ GPIO 22   │ Receptor IR, activo en LOW
 *  KY-005 S        │ GPIO 23   │ Emisor IR, RMT hardware
 *  LED RGB R       │ GPIO 25   │ LEDC canal 3, cátodo común
 *  LED RGB G       │ GPIO 26   │ LEDC canal 1
 *  LED RGB B       │ GPIO 27   │ LEDC canal 2
 *  VCC módulos IR  │ 3.3V      │ KY-005 y KY-022 operan a 3.3 V
 *  GND             │ GND       │ Común a todos los módulos
 *
 * @section dependencias Dependencias
 *
 *  - IRremote >= 4.4.0   (IRremote/IRremote)
 *  - Preferences         (incluida en ESP32 Arduino Core)
 *
 * @authors Bevilacqua, Francisco — Clement, Sebastián
 * @date    7 de junio 2026
 */

// ── config.h define IR_SEND_PIN ANTES de incluir IRremote.hpp ────────────
// IRremote.hpp lee IR_SEND_PIN en tiempo de preprocesamiento para configurar
// el canal RMT del ESP32; config.h debe ir primero en el orden de includes.
#include "config.h"

#include <Arduino.h>
#include <IRremote.hpp>
#include <Preferences.h>

// ═══════════════════════════════════════════════════════════════════════════
//  OBJETOS GLOBALES
// ═══════════════════════════════════════════════════════════════════════════

/// Objeto de acceso a la memoria no volátil (NVS).
Preferences prefs;

// ═══════════════════════════════════════════════════════════════════════════
//  PROTOTIPOS
// ═══════════════════════════════════════════════════════════════════════════

void     iniciarLedc();
void     setColor(uint8_t r, uint8_t g, uint8_t b);
void     guardarNVS(uint8_t r, uint8_t g, uint8_t b);
bool     cargarNVS(uint8_t &r, uint8_t &g, uint8_t &b);
void     enviarACK(uint8_t r, uint8_t g, uint8_t b);

// ═══════════════════════════════════════════════════════════════════════════
//  IMPLEMENTACIÓN — FUNCIONES AUXILIARES
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Configura los canales LEDC para el control PWM del LED RGB.
 *
 * El módulo LEDC del ESP32 reemplaza a analogWrite() de AVR. Cada canal
 * se asocia a un GPIO; la frecuencia y resolución son compartidas dentro
 * de un mismo grupo de timers. Se usa resolución de 8 bits (0-255) para
 * mantener compatibilidad directa con los valores RGB del protocolo.
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
 * @brief Aplica un color RGB al LED mediante los canales LEDC.
 *
 * Para LED de cátodo común: valor 0 = apagado, 255 = máxima intensidad.
 * Para LED de ánodo común: invertir los tres valores antes de llamar.
 *
 * @param r  Intensidad del canal rojo   [0-255]
 * @param g  Intensidad del canal verde  [0-255]
 * @param b  Intensidad del canal azul   [0-255]
 */
void setColor(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LEDC_CH_R, r);
    ledcWrite(LEDC_CH_G, g);
    ledcWrite(LEDC_CH_B, b);
}

/**
 * @brief Persiste el estado de color actual en la NVS del ESP32.
 *
 * Escribe el valor centinela (magic) y los tres componentes RGB en el
 * namespace "d1_state". La NVS del ESP32 utiliza flash interna con wear
 * leveling automático; no hay restricción práctica de ciclos de escritura
 * comparable a la EEPROM de AVR.
 *
 * @param r  Componente rojo   a guardar [0-255]
 * @param g  Componente verde  a guardar [0-255]
 * @param b  Componente azul   a guardar [0-255]
 */
void guardarNVS(uint8_t r, uint8_t g, uint8_t b) {
    prefs.begin(NVS_NAMESPACE, false);   // false = lectura/escritura
    prefs.putUChar(NVS_KEY_MAGIC, NVS_MAGIC_VAL);
    prefs.putUChar(NVS_KEY_R, r);
    prefs.putUChar(NVS_KEY_G, g);
    prefs.putUChar(NVS_KEY_B, b);
    prefs.end();
    Serial.printf("[NVS] Guardado R=%u G=%u B=%u\n", r, g, b);
}

/**
 * @brief Recupera el último color guardado desde la NVS.
 *
 * Verifica el valor centinela antes de leer los componentes RGB para
 * detectar NVS vacía o corrompida. Si el centinela no coincide, los
 * parámetros de salida no son modificados.
 *
 * @param[out] r  Componente rojo   recuperado
 * @param[out] g  Componente verde  recuperado
 * @param[out] b  Componente azul   recuperado
 * @return true si los datos son válidos, false si la NVS no tiene datos.
 */
bool cargarNVS(uint8_t &r, uint8_t &g, uint8_t &b) {
    prefs.begin(NVS_NAMESPACE, true);   // true = solo lectura
    uint8_t magic = prefs.getUChar(NVS_KEY_MAGIC, 0x00);
    bool valido = (magic == NVS_MAGIC_VAL);
    if (valido) {
        r = prefs.getUChar(NVS_KEY_R, 0);
        g = prefs.getUChar(NVS_KEY_G, 0);
        b = prefs.getUChar(NVS_KEY_B, 0);
    }
    prefs.end();
    return valido;
}

/**
 * @brief Construye y transmite la trama de ACK al Maestro por IR.
 *
 * Implementa la gestión half-duplex del canal IR compartido:
 *  1. Detiene el receptor (IrReceiver.stop()) para evitar que el KY-022
 *     capture el eco del propio KY-005 durante la transmisión.
 *  2. Aguarda DELAY_ANTES_ACK_MS para dar tiempo al Maestro de
 *     reactivar su receptor antes de que llegue el ACK.
 *  3. Transmite la trama ACK con cabecera 0xBB + mismo RGB.
 *  4. Reactiva el receptor (IrReceiver.start()).
 *
 * @param r  Componente rojo   del color confirmado
 * @param g  Componente verde  del color confirmado
 * @param b  Componente azul   del color confirmado
 */
void enviarACK(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t trama_ack = ((uint32_t)HDR_ACK << 24) |
                         ((uint32_t)r        << 16) |
                         ((uint32_t)g        <<  8) |
                          (uint32_t)b;

    IrReceiver.stop();
    delay(DELAY_ANTES_ACK_MS);
    IrSender.sendNECRaw(trama_ack, 0);   // 0 = sin repeticiones automáticas
    IrReceiver.start();

    Serial.printf("[IR-TX] ACK enviado: 0x%08X\n", trama_ack);
}

// ═══════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Inicialización del sistema al encender o reiniciar el ESP32.
 *
 * Secuencia de arranque:
 *  1. Puerto serie para depuración (115200 bps).
 *  2. Canales LEDC para PWM del LED RGB.
 *  3. Receptor IR (IrReceiver.begin) con feedback de LED deshabilitado.
 *  4. Recuperación de estado desde NVS:
 *     - NVS válida  → restaurar último color y aplicarlo al LED.
 *     - NVS vacía   → apagar LED (estado seguro por defecto).
 */
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);   // tiempo para que el monitor serie se conecte
    Serial.println("\n[D1] === Esclavo IR — Iniciando ===");

    // ── Configurar PWM ─────────────────────────────────────────────────
    iniciarLedc();
    Serial.println("[D1] LEDC configurado");

    // ── Iniciar receptor IR ────────────────────────────────────────────
    // DISABLE_LED_FEEDBACK evita que IRremote use el LED del pin 13
    // como indicador de actividad, lo que interferiría con otros usos del GPIO.
    IrReceiver.begin(IR_RECV_PIN, DISABLE_LED_FEEDBACK);
    Serial.printf("[D1] Receptor IR activo en GPIO %d\n", IR_RECV_PIN);

    // ── Recuperar estado desde NVS ─────────────────────────────────────
    uint8_t r = 0, g = 0, b = 0;
    if (cargarNVS(r, g, b)) {
        Serial.printf("[D1] Estado recuperado de NVS: R=%u G=%u B=%u\n", r, g, b);
        setColor(r, g, b);
    } else {
        Serial.println("[D1] NVS vacía — LED apagado");
        setColor(0, 0, 0);
    }

    Serial.println("[D1] Listo, esperando comandos IR...");
}

// ═══════════════════════════════════════════════════════════════════════════
//  LOOP PRINCIPAL
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Ciclo principal: recepción de tramas IR y generación de ACK.
 *
 * @section flujo Flujo de ejecución
 *
 *  1. IrReceiver.decode() retorna true cuando hay una trama completa
 *     en el buffer interno del receptor.
 *  2. Se filtra el flag IRDATA_FLAGS_IS_REPEAT para ignorar las
 *     repeticiones automáticas que NEC genera mientras el emisor mantiene
 *     la señal activa (el Maestro usa 0 repeticiones, pero el filtro es
 *     una salvaguarda defensiva).
 *  3. Se copia decodedRawData ANTES de llamar a resume(), porque resume()
 *     limpia el buffer y haría el dato ilegible.
 *  4. Se llama a resume() inmediatamente para liberar el buffer y permitir
 *     la recepción de la siguiente trama (importante: no hacerlo bloqueará
 *     el receptor indefinidamente).
 *  5. Se extrae la cabecera y se verifica que sea 0xAA (comando válido).
 *  6. Se aplica el color, se persiste en NVS y se envía el ACK.
 */
void loop() {
    if (!IrReceiver.decode()) {
        return;   // sin datos: ceder CPU y volver a verificar
    }

    // ── Filtrar repeticiones NEC ───────────────────────────────────────
    if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
        IrReceiver.resume();
        return;
    }

    // ── Leer la trama ANTES de llamar a resume() ───────────────────────
    uint32_t trama = IrReceiver.decodedIRData.decodedRawData;
    IrReceiver.resume();   // liberar buffer del receptor inmediatamente

    uint8_t cabecera = (trama >> 24) & 0xFF;
    uint8_t r        = (trama >> 16) & 0xFF;
    uint8_t g        = (trama >>  8) & 0xFF;
    uint8_t b        =  trama        & 0xFF;

    Serial.printf("[IR-RX] Trama: 0x%08X | cab=0x%02X R=%u G=%u B=%u\n",
                  trama, cabecera, r, g, b);

    if (cabecera != HDR_CMD) {
        // Cabecera desconocida: puede ser eco o trama de otro dispositivo.
        // Se descarta silenciosamente para no generar ACK espurio.
        Serial.printf("[IR-RX] Cabecera inválida (0x%02X) — descartada\n", cabecera);
        return;
    }

    // ── Comando válido: aplicar color → persistir → confirmar ─────────
    Serial.printf("[D1] Aplicando color R=%u G=%u B=%u\n", r, g, b);
    setColor(r, g, b);
    guardarNVS(r, g, b);
    enviarACK(r, g, b);
}
