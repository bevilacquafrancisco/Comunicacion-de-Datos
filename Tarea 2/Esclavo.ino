/**
 * @file    Esclavo.ino
 * @brief   Dispositivo 1 — Esclavo IR bidireccional.
 *
 * Recibe tramas RGB desde el Maestro por infrarrojo (protocolo NEC, 38 kHz),
 * actualiza el LED RGB local, persiste el nuevo estado en EEPROM y devuelve
 * un ACK IR con los mismos valores RGB para que el Maestro pueda validar la
 * recepción.
 *
 * Flujo principal:
 *  1. Al energizarse, recupera el último color de EEPROM y lo muestra.
 *  2. En el loop, espera una trama válida (cabecera 0xAA).
 *  3. Al recibirla: aplica color → guarda EEPROM → pausa receptor → envía ACK
 *     IR (0xBB + mismo RGB) → reactiva receptor.
 *
 * Protocolo de trama (32 bits, NEC Raw):
 *  [ bits 31-24 ] Cabecera : 0xAA = comando de color
 *  [ bits 23-16 ] R        : valor rojo   (0–255)
 *  [ bits 15-8  ] G        : valor verde  (0–255)
 *  [ bits  7-0  ] B        : valor azul   (0–255)
 *
 * Trama de ACK (misma estructura, cabecera 0xBB):
 *  [ bits 31-24 ] 0xBB
 *  [ bits 23-0  ] mismo RGB recibido → permite validación por comparación.
 *
 * Mapa de pines:
 *  D2  — KY-022 (receptor IR), señal OUT
 *  D3  — KY-005 (emisor IR), señal S  [fijado por IR_SEND_PIN]
 *  D9  — LED RGB canal R (PWM)
 *  D10 — LED RGB canal G (PWM)
 *  D6  — LED RGB canal B (PWM)
 *
 * Mapa EEPROM (ATmega328P, 1 KB):
 *  Addr 0 — magic byte (0xA5): indica que los datos son válidos
 *  Addr 1 — último R
 *  Addr 2 — último G
 *  Addr 3 — último B
 *
 * Dependencias:
 *  - IRremote >= 4.x  (IRremote.hpp)
 *  - EEPROM  (built-in Arduino)
 *
 * @note  IR_SEND_PIN debe definirse ANTES de incluir IRremote.hpp;
 *        de lo contrario la librería usa su pin por defecto.
 * @note  En Arduino UNO, el pin de TX IR debe ser D3 cuando el RX está en D2,
 *        porque ambos comparten el Timer2 de la librería.
 *
 * @authors Francisco Bevilacqua, Sebastián Clement
 * @date    27 de mayo de 2026
 */

/* ── Pin de transmisión IR — debe declararse antes de incluir IRremote ── */
#define IR_SEND_PIN 3   ///< KY-005: pin de señal del emisor IR (D3, Timer2 OC2B)

#include <IRremote.hpp> ///< Librería IRremote v4.x: manejo de RX/TX infrarrojo
#include <EEPROM.h>     ///< Librería built-in: acceso a memoria no volátil

/* ── Pines de hardware ──────────────────────────────────────────────────── */
#define PIN_RX  2   ///< KY-022: pin OUT del receptor IR (INT0, requerido por IRremote)
#define PIN_R   9   ///< LED RGB: canal rojo   — PWM (Timer1 OC1A)
#define PIN_G   10  ///< LED RGB: canal verde  — PWM (Timer1 OC1B)
#define PIN_B   6   ///< LED RGB: canal azul   — PWM (Timer0 OC0A)

/* ── Direcciones y constante de validación EEPROM ───────────────────────── */
#define EEPROM_VALID 0      ///< Dirección del magic byte de validación
#define EEPROM_R     1      ///< Dirección del último valor R persistido
#define EEPROM_G     2      ///< Dirección del último valor G persistido
#define EEPROM_B     3      ///< Dirección del último valor B persistido
#define EEPROM_MAGIC 0xA5   ///< Valor centinela: indica que la EEPROM tiene datos válidos


/* ══════════ FUNCIONES  ════════════════════════════════════════════════ */

/**
 * @brief Aplica un color al LED RGB mediante señales PWM.
 *
 * Escribe los tres canales directamente con analogWrite().
 * El LED es de cátodo común: valor 0 = apagado, 255 = máxima intensidad.
 * Para LED de ánodo común, invertir los valores antes de llamar.
 *
 * @param r  Intensidad canal rojo   [0–255]
 * @param g  Intensidad canal verde  [0–255]
 * @param b  Intensidad canal azul   [0–255]
 */
void setColor(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_R, r);
  analogWrite(PIN_G, g);
  analogWrite(PIN_B, b);
}

/**
 * @brief Persiste el color actual en EEPROM.
 *
 * Escribe el magic byte primero para marcar los datos como válidos,
 * luego los tres componentes RGB en posiciones consecutivas.
 * Se usa EEPROM.write() (no .update()); en una optimización futura
 * se podría cambiar a .update() para reducir ciclos de escritura,
 * dado que la EEPROM del ATmega328P soporta ~100.000 escrituras por celda.
 *
 * @param r  Valor rojo   a guardar [0–255]
 * @param g  Valor verde  a guardar [0–255]
 * @param b  Valor azul   a guardar [0–255]
 */
void guardarEEPROM(uint8_t r, uint8_t g, uint8_t b) {
  EEPROM.write(EEPROM_VALID, EEPROM_MAGIC); ///< Marca los datos como válidos
  EEPROM.write(EEPROM_R, r);
  EEPROM.write(EEPROM_G, g);
  EEPROM.write(EEPROM_B, b);
}

/* ════════ SETUP ════════════════════════════════════════════════════════════ */

/**
 * @brief Inicialización del sistema.
 *
 * Secuencia de arranque:
 *  1. Serial a 9600 bps para depuración.
 *  2. Pines RGB como salida.
 *  3. Receptor IR en PIN_RX con feedback de LED deshabilitado
 *     (DISABLE_LED_FEEDBACK evita parpadeo del LED de la placa durante RX).
 *  4. Lectura del magic byte de EEPROM:
 *     - Si es válido: recupera y muestra el último color guardado.
 *     - Si no: apaga el LED (estado seguro por defecto).
 */
void setup() {
  Serial.begin(9600);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);

  /* Inicia el receptor IR; DISABLE_LED_FEEDBACK = no parpadea el pin 13 */
  IrReceiver.begin(PIN_RX, DISABLE_LED_FEEDBACK);

  /* Recuperación de estado post-energización */
  if (EEPROM.read(EEPROM_VALID) == EEPROM_MAGIC) {
    /* EEPROM válida: restaurar último color conocido */
    uint8_t r = EEPROM.read(EEPROM_R);
    uint8_t g = EEPROM.read(EEPROM_G);
    uint8_t b = EEPROM.read(EEPROM_B);
    setColor(r, g, b);
    Serial.print("Color recuperado: R=");
    Serial.print(r); Serial.print(" G=");
    Serial.print(g); Serial.print(" B=");
    Serial.println(b);
  } else {
    /* EEPROM vacía o corrompida: estado seguro = LED apagado */
    setColor(0, 0, 0);
    Serial.println("EEPROM vacia, LED apagado");
  }

  Serial.println("Esclavo listo");
}

/* ════════════════════════════════════════════════════════════════════════ */
/*  LOOP PRINCIPAL                                                          */
/* ════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Ciclo principal: recepción de tramas IR y generación de ACK.
 *
 * Lógica de recepción:
 *  - IrReceiver.decode() retorna true cuando hay una trama completa en el buffer.
 *  - Se filtra el flag IRDATA_FLAGS_IS_REPEAT para ignorar las repeticiones
 *    automáticas que envía NEC mientras el botón permanece presionado.
 *  - IrReceiver.resume() debe llamarse en cuanto se termina de leer
 *    decodedRawData; de lo contrario el buffer no se libera y se pierden
 *    tramas siguientes.
 *
 * Extracción de campos de la trama de 32 bits:
 *  - Cabecera : bits [31:24] → desplazamiento >>24, máscara 0xFF
 *  - R        : bits [23:16] → desplazamiento >>16, máscara 0xFF
 *  - G        : bits [15:8]  → desplazamiento >>8,  máscara 0xFF
 *  - B        : bits [7:0]   → sin desplazamiento,  máscara 0xFF
 *
 * Gestión del canal half-duplex:
 *  - IrReceiver.stop() pausa la interrupción del receptor para evitar
 *    que el KY-022 capture el eco del propio KY-005 durante la transmisión.
 *  - delay(100) da tiempo al Maestro de reactivar su receptor antes de
 *    que llegue el ACK.
 *  - IrReceiver.start() reactiva el receptor tras completar el envío.
 *
 * Construcción del ACK (32 bits):
 *  - Cabecera 0xBB en bits [31:24] identifica la trama como ACK.
 *  - Se reutilizan exactamente los mismos R, G, B recibidos; esto permite
 *    al Maestro validar la integridad comparando byte a byte.
 */
void loop() {
  if (IrReceiver.decode()) {

    /* Filtrar repeticiones NEC (se generan si el emisor mantiene la señal) */
    if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
      IrReceiver.resume(); ///< Liberar buffer del receptor para la siguiente trama
      return;
    }

    /* Leer la trama raw de 32 bits antes de llamar a resume() */
    uint32_t trama = IrReceiver.decodedIRData.decodedRawData;
    IrReceiver.resume(); ///< Liberar buffer inmediatamente tras copiar el dato

    /* Desempaquetar los 4 bytes de la trama */
    uint8_t cabecera = (trama >> 24) & 0xFF; ///< Byte identificador del tipo de trama
    uint8_t r = (trama >> 16) & 0xFF;        ///< Componente rojo
    uint8_t g = (trama >>  8) & 0xFF;        ///< Componente verde
    uint8_t b =  trama        & 0xFF;        ///< Componente azul

    Serial.print("Trama: 0x"); Serial.print(trama, HEX);
    Serial.print(" R="); Serial.print(r);
    Serial.print(" G="); Serial.print(g);
    Serial.print(" B="); Serial.println(b);

    if (cabecera == 0xAA) {
      /* Trama de comando válida: actualizar LED y persistir */
      setColor(r, g, b);
      guardarEEPROM(r, g, b);

      /*
       * Construcción del ACK:
       *  - Cabecera 0xBB en los 8 bits más significativos.
       *  - R, G, B del comando recibido en los 24 bits restantes.
       *  - El Maestro comparará estos valores con los que envió
       *    para confirmar coherencia de estado.
       */
      uint32_t ack = ((uint32_t)0xBB << 24) |
                     ((uint32_t)r    << 16) |
                     ((uint32_t)g    <<  8) | b;

      /* Gestión half-duplex: pausar RX → transmitir ACK → reanudar RX */
      IrReceiver.stop();
      delay(100); ///< Margen para que el Maestro active su receptor antes del ACK
      IrSender.sendNECRaw(ack, 0); ///< 0 repeticiones: envío único sin auto-repeat
      IrReceiver.start();

      Serial.print("ACK enviado: 0x"); Serial.println(ack, HEX);
    } else {
      /* Cabecera desconocida: descartar trama silenciosamente */
      Serial.println("Cabecera invalida, ignorando.");
    }
  }
}
