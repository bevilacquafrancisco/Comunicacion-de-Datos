/**
 * @file    main.cpp
 * @brief   Dispositivo 1 — Esclavo IR bidireccional con CRC-8/MAXIM (ESP32).
 *
 * @section descripcion Descripción funcional
 *
 *  Este firmware implementa el nodo receptor/actuador del sistema de control
 *  cromático. El dispositivo permanece a la escucha de tramas infrarrojas NEC
 *  Raw provenientes del Dispositivo 2 (Maestro). Al recibir un comando válido:
 *
 *   1. Valida la integridad del mensaje mediante CRC-8/MAXIM por software
 *      (XOR, aritmética módulo 2, sin librerías externas).
 *   2. Aplica el color RGB al LED local mediante PWM (LEDC, 8 bits).
 *   3. Persiste el nuevo estado en la memoria no volátil (NVS / Preferences).
 *   4. Transmite un ACK infrarrojo al Maestro con la firma CRC invertida (~CRC),
 *      garantizando asimetría de protocolo para prevenir el auto-acuse de recibo.
 *
 * @section protocolo_crc Protocolo IR: trama de 32 bits con CRC-8
 *
 *  Ambas tramas —COMANDO y ACK— tienen la misma estructura de 4 bytes:
 *
 *   bits [31:24] — R (0-255)
 *   bits [23:16] — G (0-255)
 *   bits [15: 8] — B (0-255)
 *   bits [ 7: 0] — Firma CRC
 *
 *  La diferencia está en la firma:
 *   - COMANDO (D2 → D1): CRC8_normal  = CRC-8/MAXIM(R, G, B)
 *   - ACK     (D1 → D2): CRC8_invertido = ~CRC-8/MAXIM(R, G, B)
 *
 *  Esta asimetría matemática resuelve el "Falso Positivo por Auto-Acuse de
 *  Recibo" (Self-ACK / Sordera por Eco): el Maestro, al recibir un paquete,
 *  calcula el CRC normal y su inverso. Si la firma coincide con el CRC normal,
 *  el paquete es un eco de su propio comando y lo descarta. Si coincide con el
 *  inverso, es un ACK legítimo del Esclavo.
 *
 * @section crc_init Inicialización del CRC en 0xFF: guarda contra eco nulo
 *
 *  El canal IR es half-duplex y óptico. Al reactivar el receptor KY-022 tras
 *  una transmisión, el eco residual de la portadora puede capturarse como la
 *  trama 0x00000000. Si el registro CRC se inicializara en 0x00, el CRC de tres
 *  bytes cero sería 0x00 (propiedad de la operación XOR), dando un falso
 *  positivo matemáticamente válido que desencadenaría una tormenta de comandos.
 *
 *  Solución: inicializar el CRC en 0xFF (CRC-8/MAXIM estándar). Con esta
 *  inicialización, CRC(0x00, 0x00, 0x00) ≠ 0x00, eliminando la colisión.
 *
 * @section hardware Hardware
 *
 *  Módulo          │ Pin ESP32 │ Notas
 *  ────────────────┼───────────┼────────────────────────────────────────
 *  KY-022 OUT      │ GPIO 22   │ Receptor IR, activo en LOW
 *  KY-005 S        │ GPIO 23   │ Emisor IR, RMT hardware
 *  LED RGB R       │ GPIO 25   │ LEDC canal 3 (canal 0 reservado por IRremote)
 *  LED RGB G       │ GPIO 26   │ LEDC canal 1
 *  LED RGB B       │ GPIO 27   │ LEDC canal 2
 *  VCC módulos IR  │ 3.3V      │ KY-005 y KY-022 operan a 3.3 V
 *  GND             │ GND       │ Común a todos los módulos
 *
 * @section dependencias Dependencias
 *
 *  - IRremote >= 4.4.0   (z3t0/IRremote)
 *  - Preferences         (incluida en ESP32 Arduino Core)
 *
 * @authors Bevilacqua, Francisco — Clement, Sebastián
 * @date    14 de junio 2026
 */

// config.h define IR_SEND_PIN ANTES de incluir IRremote.hpp.
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

void    iniciarLedc();
void    setColor(uint8_t r, uint8_t g, uint8_t b);
uint8_t calcularCRC8(uint8_t r, uint8_t g, uint8_t b);
void    guardarNVS(uint8_t r, uint8_t g, uint8_t b);
bool    cargarNVS(uint8_t &r, uint8_t &g, uint8_t &b);
void    enviarACK(uint8_t r, uint8_t g, uint8_t b);

// ═══════════════════════════════════════════════════════════════════════════
//  CRC-8 POR SOFTWARE (aritmética módulo 2 / XOR)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Calcula el CRC-8/MAXIM sobre los tres bytes RGB.
 *
 * Implementación del estándar CRC-8/MAXIM (Dallas/1-Wire) íntegramente
 * por software mediante XOR (aritmética módulo 2). Sin tablas precalculadas
 * ni librerías externas, lo que facilita la auditoría académica del algoritmo
 * y garantiza portabilidad idéntica entre C++ (ESP32) y JavaScript (D3).
 *
 * Parámetros del estándar:
 *   Polinomio : 0x31  →  x^8 + x^5 + x^4 + 1
 *   Init      : 0xFF  →  guarda contra eco nulo (ver sección crc_init)
 *   RefIn     : true  →  procesamiento LSB-first (reflejo de entrada)
 *   RefOut    : true  →  reflejo de salida
 *   XorOut    : 0x00
 *
 * Distancia Hamming: HD=4 para mensajes ≤ 119 bits → suficiente para 3 bytes.
 *
 * @param r  Byte del canal rojo   [0-255]
 * @param g  Byte del canal verde  [0-255]
 * @param b  Byte del canal azul   [0-255]
 * @return   CRC-8 de 8 bits calculado sobre {R, G, B}.
 */
uint8_t calcularCRC8(uint8_t r, uint8_t g, uint8_t b) {
    // [BLOQUE-CRC8] Inicialización del registro en 0xFF
    // (no en 0x00, para que CRC(0,0,0) ≠ 0 y eliminar la colisión con el eco óptico)
    uint8_t crc = 0xFF;
    uint8_t datos[3] = { r, g, b };

    for (uint8_t i = 0; i < 3; i++) {
        // [BLOQUE-CRC8] XOR del byte de datos con el registro actual
        crc ^= datos[i];

        for (uint8_t bit = 0; bit < 8; bit++) {
            // [BLOQUE-CRC8] LSB = 1: desplazar a la derecha y XOR con el polinomio
            // LSB = 0: solo desplazar (no hay división en GF(2))
            if (crc & 0x01) {
                crc = (crc >> 1) ^ CRC8_POLY;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;   // [BLOQUE-CRC8] Remainder final = checksum
}

// ═══════════════════════════════════════════════════════════════════════════
//  LEDC / PWM
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Configura los tres canales LEDC para el control PWM del LED RGB.
 *
 * El módulo LEDC del ESP32 reemplaza a analogWrite() de AVR. Cada canal
 * se asocia a un GPIO; la frecuencia y resolución son compartidas dentro
 * de un mismo grupo de timers. Se usa resolución de 8 bits (0-255) para
 * compatibilidad directa con los valores RGB del protocolo IR.
 *
 * Nota: el canal rojo se asigna a LEDC_CH_R = 3, evitando el canal 0 que
 * IRremote reserva internamente para la modulación RMT de 38 kHz.
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
 * @brief Aplica un color RGB al LED mediante los canales LEDC.
 *
 * Para LED de cátodo común: valor 0 = apagado, 255 = máxima intensidad.
 *
 * @param r  Intensidad del canal rojo   [0-255]
 * @param g  Intensidad del canal verde  [0-255]
 * @param b  Intensidad del canal azul   [0-255]
 */
void setColor(uint8_t r, uint8_t g, uint8_t b) {
    // [BLOQUE-LED] Escritura del ciclo de trabajo PWM en cada canal
    ledcWrite(LEDC_CH_R, r);
    ledcWrite(LEDC_CH_G, g);
    ledcWrite(LEDC_CH_B, b);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PERSISTENCIA NVS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Persiste el estado de color actual en la NVS del ESP32.
 *
 * Escribe el valor centinela (magic = 0xA5) y los tres componentes RGB en el
 * namespace "d1_state". La NVS del ESP32 usa flash interna con wear-leveling
 * automático; no hay restricción práctica de ciclos de escritura comparable a
 * la EEPROM de AVR.
 *
 * @param r  Componente rojo   a guardar [0-255]
 * @param g  Componente verde  a guardar [0-255]
 * @param b  Componente azul   a guardar [0-255]
 */
void guardarNVS(uint8_t r, uint8_t g, uint8_t b) {
    // [BLOQUE-NVS] Apertura en modo lectura/escritura
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar(NVS_KEY_MAGIC, NVS_MAGIC_VAL);
    prefs.putUChar(NVS_KEY_R, r);
    prefs.putUChar(NVS_KEY_G, g);
    prefs.putUChar(NVS_KEY_B, b);
    prefs.end();
    // [BLOQUE-NVS] Log de confirmación
    Serial.printf("[NVS] Guardado — R=%u G=%u B=%u\n", r, g, b);
}

/**
 * @brief Recupera el último color guardado desde la NVS.
 *
 * Verifica el valor centinela (0xA5) antes de leer los componentes RGB para
 * detectar NVS vacía o corrompida. Si el centinela no coincide, los parámetros
 * de salida no se modifican y la función retorna false.
 *
 * @param[out] r  Componente rojo   recuperado
 * @param[out] g  Componente verde  recuperado
 * @param[out] b  Componente azul   recuperado
 * @return true si los datos son válidos, false si la NVS no tiene datos.
 */
bool cargarNVS(uint8_t &r, uint8_t &g, uint8_t &b) {
    // [BLOQUE-NVS] Apertura en modo solo lectura
    prefs.begin(NVS_NAMESPACE, true);
    uint8_t magic = prefs.getUChar(NVS_KEY_MAGIC, 0x00);
    bool valido = (magic == NVS_MAGIC_VAL);
    if (valido) {
        // [BLOQUE-NVS] Centinela OK: leer componentes RGB
        r = prefs.getUChar(NVS_KEY_R, 0);
        g = prefs.getUChar(NVS_KEY_G, 0);
        b = prefs.getUChar(NVS_KEY_B, 0);
    }
    prefs.end();
    return valido;
}

// ═══════════════════════════════════════════════════════════════════════════
//  PROTOCOLO IR — ACK CON FIRMA ASIMÉTRICA (~CRC)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Construye y transmite la trama de ACK al Maestro con firma CRC invertida.
 *
 * @section ack_asimetrico Asimetría de protocolo: solución al Self-ACK
 *
 *  El canal IR es compartido y óptico: cualquier trama transmitida puede ser
 *  reflejada por superficies del entorno y recapturada por el propio emisor.
 *  Si COMANDO y ACK tuvieran la misma estructura de firma, D2 no podría
 *  distinguir matemáticamente su propio eco de un ACK legítimo de D1.
 *
 *  Solución: el Esclavo (D1) envía el ACK con el CRC **invertido bit a bit**
 *  (~CRC). El Maestro (D2) calcula el CRC esperado para el eco (CRC normal)
 *  y para el ACK legítimo (~CRC). Solo acepta la trama que coincida con ~CRC.
 *
 * @section half_duplex Gestión half-duplex del canal óptico
 *
 *  El canal IR opera en half-duplex: no se puede transmitir y recibir
 *  simultáneamente. El KY-022 capturaría el eco de su propio KY-005.
 *  Secuencia de gestión:
 *   1. IrReceiver.stop()     → deshabilita el receptor durante la TX.
 *   2. delay(DELAY_ANTES_ACK_MS) → margen para que el Maestro reactive su
 *      receptor antes de que llegue el ACK (determinado experimentalmente).
 *   3. IrSender.sendNECRaw() → transmite la trama ACK.
 *   4. IrReceiver.start()    → reactiva el receptor para próximos comandos.
 *
 * @param r  Componente rojo   del color confirmado [0-255]
 * @param g  Componente verde  del color confirmado [0-255]
 * @param b  Componente azul   del color confirmado [0-255]
 */
void enviarACK(uint8_t r, uint8_t g, uint8_t b) {
    // [BLOQUE-CRC8] Calcular CRC-8/MAXIM y aplicar inversión bit a bit
    uint8_t crc_normal   = calcularCRC8(r, g, b);
    uint8_t crc_invertido = ~crc_normal;   // firma asimétrica del ACK

    // [BLOQUE-IR-TX] Construir trama ACK: [R][G][B][~CRC8]
    uint32_t trama_ack = ((uint32_t)r            << 24) |
                         ((uint32_t)g            << 16) |
                         ((uint32_t)b            <<  8) |
                          (uint32_t)crc_invertido;

    // [BLOQUE-LOG] Log detallado de la trama ACK
    Serial.printf("[IR-TX] ACK — Trama: 0x%08X\n", trama_ack);
    Serial.printf("        R=%u  G=%u  B=%u\n", r, g, b);
    Serial.printf("        CRC_normal=0x%02X  CRC_invertido=0x%02X  (firma asimetrica ~CRC)\n",
                  crc_normal, crc_invertido);

    // [BLOQUE-IR-TX] Gestión half-duplex y transmisión
    IrReceiver.stop();                       // deshabilitar RX para evitar eco propio
    delay(DELAY_ANTES_ACK_MS);              // margen para que D2 reactive su receptor
    IrSender.sendNECRaw(trama_ack, 0);      // 0 = sin repeticiones NEC automáticas
    IrReceiver.start();                      // reactivar RX para próximos comandos

    Serial.println("[IR-TX] ACK enviado correctamente");
}

// ═══════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Inicialización del sistema al encender o reiniciar el ESP32.
 *
 * Secuencia de arranque:
 *  1. Puerto serie para depuración (SERIAL_BAUD = 115200 bps).
 *  2. Canales LEDC para PWM del LED RGB (canales 1, 2 y 3; canal 0 reservado).
 *  3. Receptor IR (IrReceiver.begin) con feedback de LED deshabilitado
 *     para evitar que IRremote use el pin 13 como indicador de actividad.
 *  4. Recuperación de estado desde NVS:
 *     - NVS válida  → restaurar último color aplicado y mostrarlo en el LED.
 *     - NVS vacía   → apagar LED (estado seguro por defecto: RGB = 0, 0, 0).
 */
void setup() {
    // [BLOQUE-INIT] Serial
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n[D1] ═══════════════════════════════════════════");
    Serial.println("[D1]  Esclavo IR con CRC-8/MAXIM — Iniciando");
    Serial.println("[D1] ═══════════════════════════════════════════");
    Serial.printf( "[D1] CRC-8: polinomio=0x%02X  init=0xFF  LSB-first\n", CRC8_POLY);
    Serial.printf( "[D1] Protocolo IR: [R][G][B][CRC8] → CMD normal, ACK=~CRC\n");

    // [BLOQUE-LEDC] Configurar PWM
    iniciarLedc();
    Serial.printf("[D1] LEDC configurado — canales R=%u G=%u B=%u @ %u Hz / %u bits\n",
                  LEDC_CH_R, LEDC_CH_G, LEDC_CH_B, LEDC_FREQ_HZ, LEDC_RESOLUTION);

    // [BLOQUE-IR] Iniciar receptor IR con feedback de LED deshabilitado
    IrReceiver.begin(IR_RECV_PIN, DISABLE_LED_FEEDBACK);
    Serial.printf("[D1] Receptor IR activo — GPIO %d\n", IR_RECV_PIN);
    Serial.printf("[D1] Emisor  IR listo  — GPIO %d\n", IR_SEND_PIN);

    // [BLOQUE-NVS] Recuperar estado desde NVS
    uint8_t r = 0, g = 0, b = 0;
    if (cargarNVS(r, g, b)) {
        uint8_t crc_check = calcularCRC8(r, g, b);
        Serial.printf("[NVS] Estado recuperado — R=%u G=%u B=%u  (CRC-8 verificacion=0x%02X)\n",
                      r, g, b, crc_check);
        setColor(r, g, b);
        Serial.printf("[D1] LED restaurado a RGB(%u, %u, %u)\n", r, g, b);
    } else {
        Serial.println("[NVS] Vacía — LED apagado (estado seguro por defecto)");
        setColor(0, 0, 0);
    }

    Serial.println("[D1] Listo — esperando comandos IR del Maestro (D2)...\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  LOOP PRINCIPAL
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Ciclo principal: recepción de tramas IR, validación CRC y generación de ACK.
 *
 * @section flujo Flujo de ejecución por iteración
 *
 *  1. IrReceiver.decode() retorna true cuando hay una trama completa en el
 *     buffer interno del hardware RMT del ESP32.
 *
 *  2. Se filtra el flag IRDATA_FLAGS_IS_REPEAT para ignorar las repeticiones
 *     automáticas del protocolo NEC (el Maestro envía 0 repeticiones, pero el
 *     filtro actúa como salvaguarda defensiva).
 *
 *  3. Se copia decodedRawData ANTES de llamar a resume(), porque resume()
 *     limpia el buffer del receptor. No hacerlo haría el dato ilegible.
 *
 *  4. Se llama a resume() inmediatamente para liberar el buffer y permitir
 *     la recepción de la siguiente trama.
 *
 *  5. Se extraen R, G, B y el byte de firma CRC recibido.
 *
 *  6. Se calcula el CRC-8/MAXIM local sobre R, G, B con init=0xFF y se
 *     compara con la firma recibida:
 *     - Firma == CRC_normal → eco del propio comando de D2 → descartar.
 *     - Firma == CRC_invertido (~CRC) → ACK de D1 (no esperado en este rol).
 *     - Firma == CRC_normal y trama válida → COMANDO legítimo de D2 → procesar.
 *
 *  7. Si el COMANDO es válido: aplicar color → persistir en NVS → enviar ACK.
 */
void loop() {
    // [BLOQUE-RX] Esperar hasta que haya una trama completa en el buffer IR
    if (!IrReceiver.decode()) {
        return;   // sin datos: ceder CPU y volver al inicio del loop
    }

    // [BLOQUE-FILTRO] Ignorar repeticiones automáticas del protocolo NEC
    if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
        IrReceiver.resume();
        return;
    }

    // [BLOQUE-RX] Leer la trama ANTES de llamar a resume() para no perder el dato
    uint32_t trama = IrReceiver.decodedIRData.decodedRawData;
    IrReceiver.resume();   // liberar buffer del receptor inmediatamente

    // [BLOQUE-PARSE] Desempaquetar los 4 bytes de la trama
    uint8_t rx_r   = (trama >> 24) & 0xFF;
    uint8_t rx_g   = (trama >> 16) & 0xFF;
    uint8_t rx_b   = (trama >>  8) & 0xFF;
    uint8_t rx_crc =  trama        & 0xFF;

    // [BLOQUE-LOG] Log de trama recibida con todos los campos
    Serial.println("[IR-RX] ─────────────────────────────────────────");
    Serial.printf( "[IR-RX] Trama RAW: 0x%08X\n", trama);
    Serial.printf( "[IR-RX] Campos   — R=%u  G=%u  B=%u  CRC_rx=0x%02X\n",
                   rx_r, rx_g, rx_b, rx_crc);

    // [BLOQUE-CRC8] Calcular CRC local y sus variantes
    uint8_t crc_esperado_cmd = calcularCRC8(rx_r, rx_g, rx_b);   // firma de COMANDO
    uint8_t crc_esperado_ack = ~crc_esperado_cmd;                 // firma de ACK (~CRC)

    Serial.printf("[CRC-8] CRC_calculado=0x%02X  (cmd_normal)  CRC_ack=0x%02X  (~cmd_normal)\n",
                  crc_esperado_cmd, crc_esperado_ack);

    // [BLOQUE-FILTRO] Detectar y descartar ecos del propio ACK enviado por D1
    // (un eco tendrá firma ~CRC, que es la propia firma del ACK)
    if (rx_crc == crc_esperado_ack) {
        Serial.println("[IR-RX] Eco de ACK propio detectado (firma ~CRC) — descartado");
        Serial.println("[IR-RX] ─────────────────────────────────────────");
        return;
    }

    // [BLOQUE-CRC8] Validar que la firma corresponde al COMANDO legítimo
    if (rx_crc != crc_esperado_cmd) {
        Serial.printf("[CRC-8] ERROR — Firma 0x%02X no coincide con CRC esperado 0x%02X\n",
                      rx_crc, crc_esperado_cmd);
        Serial.println("[IR-RX] Trama descartada (CRC inválido o eco corrupto)");
        Serial.println("[IR-RX] ─────────────────────────────────────────");
        return;
    }

    // [BLOQUE-CMD] COMANDO válido recibido de D2
    Serial.println("[CRC-8] OK — Firma verificada. Comando legítimo de D2.");
    Serial.printf( "[D1]    Aplicando color RGB(%u, %u, %u)\n", rx_r, rx_g, rx_b);

    // [BLOQUE-LED] Aplicar el color al LED RGB por PWM
    setColor(rx_r, rx_g, rx_b);

    // [BLOQUE-NVS] Persistir el nuevo estado para recuperarlo ante un reinicio
    guardarNVS(rx_r, rx_g, rx_b);

    // [BLOQUE-ACK] Enviar ACK con firma asimétrica al Maestro
    enviarACK(rx_r, rx_g, rx_b);

    Serial.println("[IR-RX] ─────────────────────────────────────────");
}