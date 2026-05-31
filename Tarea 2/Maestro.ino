/**
 * @file    Maestro.ino
 * @brief   Dispositivo 2 — Maestro IR bidireccional.
 *
 * Cicla a través de 8 colores predefinidos mediante un pulsador.
 * Al presionar, calcula el siguiente color, arma una trama IR [0xAA][R][G][B]
 * y la envía al Esclavo hasta MAX_REINTENTOS veces. Actualiza su propio LED
 * y persiste el nuevo índice en EEPROM únicamente si recibe un ACK IR válido
 * del Esclavo (cabecera 0xBB + mismo RGB enviado).
 *
 * Flujo principal:
 *  1. Al energizarse, recupera el índice de color de EEPROM y muestra el color.
 *  2. En el loop, detecta flanco descendente del pulsador con debounce por software.
 *  3. Calcula el siguiente índice en la secuencia circular (módulo 8).
 *  4. Llama a enviarColor() hasta 3 veces; si algún intento retorna true:
 *     - Avanza colorActual al nuevo índice.
 *     - Aplica el color al LED propio.
 *     - Guarda el índice en EEPROM.
 *  5. Si ningún intento tiene ACK: colorActual permanece sin cambios.
 *
 * Protocolo de trama (32 bits, NEC Raw):
 *  [ bits 31-24 ] Cabecera CMD : 0xAA
 *  [ bits 23-16 ] R            : valor rojo   (0–255)
 *  [ bits 15-8  ] G            : valor verde  (0–255)
 *  [ bits  7-0  ] B            : valor azul   (0–255)
 *
 * Validación del ACK:
 *  El Esclavo responde con cabecera 0xBB y los mismos R, G, B.
 *  El Maestro extrae los tres bytes del ACK y los compara contra lo enviado;
 *  solo retorna true si las tres comparaciones son exitosas.
 *
 * Mapa de pines:
 *  D4  — KY-005 (emisor IR), señal S  [fijado por IR_SEND_PIN]
 *  D11 — KY-022 (receptor IR), señal OUT
 *  D7  — Pulsador (INPUT_PULLUP, activo en LOW)
 *  D9  — LED RGB canal R (PWM)
 *  D10 — LED RGB canal G (PWM)
 *  D6  — LED RGB canal B (PWM)
 *
 * Mapa EEPROM (ATmega328P, 1 KB):
 *  Addr 0 — magic byte (0xA5): indica que los datos son válidos
 *  Addr 1 — índice del último color aplicado exitosamente [0–7]
 *
 * Dependencias:
 *  - IRremote >= 4.x  (IRremote.hpp)
 *  - EEPROM  (built-in Arduino)
 *
 * @note  IR_SEND_PIN debe definirse ANTES de incluir IRremote.hpp.
 * @note  El Maestro usa D2 para RX (en lugar de D11) para liberar INT0
 *        y evitar interferencias con el Timer2 que usa el emisor en D4.
 *
 * @authors Francisco Bevilacqua, Sebastián Clement
 * @date    27 de mayo de 2026
 */

/* ── Pin de transmisión IR — debe declararse antes de incluir IRremote ── */
#define IR_SEND_PIN 4   ///< KY-005: pin de señal del emisor IR (D4)

#include <IRremote.hpp> ///< Librería IRremote v4.x: manejo de RX/TX infrarrojo
#include <EEPROM.h>     ///< Librería built-in: acceso a memoria no volátil

/* ── Pines de hardware ──────────────────────────────────────────────────── */
#define PIN_RX  2   ///< KY-022: pin OUT del receptor IR
#define PIN_BTN  7  ///< Pulsador: activo en LOW (usa resistencia pull-up interna)
#define PIN_R    9  ///< LED RGB: canal rojo   — PWM (Timer1 OC1A)
#define PIN_G   10  ///< LED RGB: canal verde  — PWM (Timer1 OC1B)
#define PIN_B    6  ///< LED RGB: canal azul   — PWM (Timer0 OC0A)

/* ── Direcciones y constante de validación EEPROM ───────────────────────── */
#define EEPROM_VALID 0      ///< Dirección del magic byte de validación
#define EEPROM_INDEX 1      ///< Dirección del último índice de color persistido
#define EEPROM_MAGIC 0xA5   ///< Valor centinela: indica que la EEPROM tiene datos válidos

/* ── Parámetro de control de flujo ─────────────────────────────────────── */
#define MAX_REINTENTOS 3    ///< Cantidad máxima de intentos de envío antes de abandonar

/* ── Tabla de colores predefinidos (R, G, B) ────────────────────────────── */
/**
 * Secuencia de 8 colores en orden de ciclo.
 * Dimensión [8][3]: 8 entradas, 3 componentes por entrada (R, G, B).
 * Los valores son de tipo uint8_t (0–255), compatibles con analogWrite().
 * La tabla es const: reside en SRAM pero es inmutable en tiempo de ejecución.
 */
const uint8_t COLORES[8][3] = {
  {255,   0,   0},  // 0: Rojo
  {255, 200,   0},  // 1: Amarillo
  {  0, 255,   0},  // 2: Verde
  {  0, 255, 255},  // 3: Celeste
  {  0,   0, 255},  // 4: Azul
  {180,   0, 255},  // 5: Lila
  {255, 255, 255},  // 6: Blanco
  {255, 105, 180}   // 7: Rosa
};

/* ── Estado global ──────────────────────────────────────────────────────── */
int colorActual = 0; ///< Índice activo en COLORES[]; avanza en loop solo con ACK exitoso

/* ──────── FUNCIONES   ─────────────────────────────────────────────────────── */

/**
 * @brief Aplica un color al LED RGB local mediante señales PWM.
 *
 * Refleja en el LED del Maestro el último color confirmado exitosamente,
 * es decir, se llama solo tras recibir ACK válido del Esclavo.
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
 * @brief Persiste el índice del color actual en EEPROM.
 *
 * El Maestro guarda solo el índice (1 byte) en lugar de los tres componentes RGB,
 * ya que puede reconstruir el color completo desde la tabla COLORES[].
 * Esto reduce las escrituras en EEPROM a 2 bytes por evento (magic + índice).
 * La EEPROM del ATmega328P soporta ~100.000 escrituras por celda.
 *
 * @param indice  Índice en COLORES[] a persistir [0–7]
 */
void guardarEEPROM(int indice) {
  EEPROM.write(EEPROM_VALID, EEPROM_MAGIC); ///< Marca los datos como válidos
  EEPROM.write(EEPROM_INDEX, indice);        ///< Persiste el índice del color
}

/**
 * @brief Envía la trama de color al Esclavo y espera su ACK IR.
 *
 * Implementa el mecanismo de handshaking del protocolo:
 *  1. Arma la trama de 32 bits con cabecera 0xAA y los tres bytes RGB.
 *  2. Pausa el receptor (IrReceiver.stop()) para evitar eco propio.
 *  3. Transmite la trama con sendNECRaw() (0 repeticiones).
 *  4. Reactiva el receptor (IrReceiver.start()) para escuchar el ACK.
 *  5. Entra en un bucle de espera de hasta 2000 ms:
 *     - Si llega una trama, filtra repeticiones y extrae cabecera + RGB.
 *     - Valida: cabecera == 0xBB y los tres bytes coinciden con los enviados.
 *     - Retorna true en el primer ACK válido; false si expira el timeout.
 *
 * La comparación byte a byte del ACK garantiza que el Esclavo aplicó
 * exactamente el color solicitado, no un color residual de otro ciclo.
 *
 * @param r  Componente rojo   del color a enviar [0–255]
 * @param g  Componente verde  del color a enviar [0–255]
 * @param b  Componente azul   del color a enviar [0–255]
 * @return   true si ACK válido recibido antes del timeout; false en caso contrario.
 */
bool enviarColor(uint8_t r, uint8_t g, uint8_t b) {
  /*
   * Construcción de la trama de 32 bits:
   *  - 0xAA se desplaza 24 bits a la izquierda → ocupa bits [31:24].
   *  - r se desplaza 16 bits → bits [23:16].
   *  - g se desplaza  8 bits → bits [15:8].
   *  - b sin desplazamiento  → bits [7:0].
   *  Cast a uint32_t obligatorio antes del desplazamiento para evitar
   *  overflow en un int de 16 bits (arquitectura AVR).
   */
  uint32_t trama = ((uint32_t)0xAA << 24) |
                   ((uint32_t)r    << 16) |
                   ((uint32_t)g    <<  8) | b;

  Serial.print("Trama: 0x"); Serial.println(trama, HEX);

  /* Gestión half-duplex: pausar RX → transmitir → reanudar RX */
  IrReceiver.stop();
  IrSender.sendNECRaw(trama, 0); ///< 0 = sin repeticiones automáticas NEC
  IrReceiver.start(); ///< Reactivar receptor para capturar el ACK del Esclavo

  /* Bucle de espera del ACK con timeout de 2 segundos */
  unsigned long t = millis(); ///< Marca de tiempo de inicio de la espera
  while (millis() - t < 2000) {
    if (IrReceiver.decode()) {

      /* Filtrar repeticiones NEC antes de procesar */
      if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
        IrReceiver.resume();
        continue;
      }

      uint32_t respuesta = IrReceiver.decodedIRData.decodedRawData; ///< Trama ACK cruda
      uint8_t cab = (respuesta >> 24) & 0xFF; ///< Cabecera del ACK (esperado: 0xBB)
      Serial.print("Respuesta: 0x"); Serial.println(respuesta, HEX);
      IrReceiver.resume(); ///< Liberar buffer antes de evaluar (evita perder la siguiente trama)

      /*
       * Validación del ACK:
       *  - cab == 0xBB : identifica la trama como ACK del Esclavo.
       *  - Bits [23:16] == r : el Esclavo confirmó el canal rojo correcto.
       *  - Bits [15:8]  == g : el Esclavo confirmó el canal verde correcto.
       *  - Bits [7:0]   == b : el Esclavo confirmó el canal azul correcto.
       *  Las tres condiciones deben cumplirse simultáneamente.
       */
      if (cab == 0xBB &&
          ((respuesta >> 16) & 0xFF) == r &&
          ((respuesta >>  8) & 0xFF) == g &&
          ( respuesta        & 0xFF) == b) {
        return true; ///< ACK válido: color confirmado por el Esclavo
      }
    }
  }
  return false; ///< Timeout: no se recibió ACK válido en 2 segundos
}

/* ═════ SETUP    ═══════════════════════════════════════════════════ */

/**
 * @brief Inicialización del sistema.
 *
 * Secuencia de arranque:
 *  1. Serial a 9600 bps para depuración.
 *  2. Pulsador como entrada con pull-up interno (reposo = HIGH, activo = LOW).
 *  3. Pines RGB como salida.
 *  4. Receptor IR en PIN_RX con feedback de LED deshabilitado.
 *  5. Lectura del magic byte de EEPROM:
 *     - Si es válido: recupera el índice y aplica el color correspondiente.
 *     - Si no: inicia en índice 0 (Rojo).
 *     - Guarda el índice en colorActual para continuar la secuencia desde ahí.
 */
void setup() {
  Serial.begin(9600);
  pinMode(PIN_BTN, INPUT_PULLUP); ///< Pull-up interno: no requiere resistencia externa
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);

  /* Inicia el receptor IR; DISABLE_LED_FEEDBACK = no parpadea el pin 13 */
  IrReceiver.begin(PIN_RX, DISABLE_LED_FEEDBACK);

  /* Recuperación de estado post-energización */
  if (EEPROM.read(EEPROM_VALID) == EEPROM_MAGIC) {
    /* EEPROM válida: restaurar índice del último color confirmado */
    colorActual = EEPROM.read(EEPROM_INDEX);
    if (colorActual >= 8) colorActual = 0; ///< Sanity check: índice fuera de rango → reset
    Serial.print("Color recuperado, indice: ");
    Serial.println(colorActual);
  } else {
    /* EEPROM vacía o corrompida: iniciar desde el primer color de la secuencia */
    colorActual = 0;
    Serial.println("EEPROM vacia, iniciando en Rojo");
  }

  /* Mostrar el color recuperado o inicial en el LED */
  setColor(COLORES[colorActual][0], COLORES[colorActual][1], COLORES[colorActual][2]);
  Serial.println("Maestro listo");
}

/* ═══════════ LOOP PRINCIPAL  ═════════════════════════════════════════ */

/**
 * @brief Ciclo principal: detección de pulsador, envío IR y actualización de estado.
 *
 * Debounce por software (doble lectura con delay de 50 ms):
 *  - Primera lectura: detecta el nivel LOW inicial.
 *  - delay(50): espera a que el rebote mecánico se estabilice.
 *  - Segunda lectura: confirma que la señal sigue en LOW (pulsación real).
 *  Este método es suficiente para pulsadores mecánicos estándar.
 *
 * Cálculo del siguiente color:
 *  - Operación módulo 8 garantiza la ciclicidad: índice 7 → índice 0.
 *  - Los componentes RGB se leen directamente de la tabla COLORES[].
 *
 * Lógica de reintento:
 *  - El bucle for itera hasta MAX_REINTENTOS veces.
 *  - La condición !ack cortocircuita el bucle en el primer éxito (lazy evaluation).
 *  - delay(300) entre reintentos da tiempo al canal IR de despejarse.
 *
 * Actualización de estado (solo con ACK válido):
 *  - colorActual avanza al nuevo índice.
 *  - setColor() refleja el nuevo color en el LED del Maestro.
 *  - guardarEEPROM() persiste el índice para sobrevivir ciclos de energía.
 *  Si no hay ACK, colorActual se mantiene: el Maestro no avanza,
 *  preservando la coherencia con el estado del Esclavo.
 *
 * Bloqueo al soltar el pulsador:
 *  - while(PIN_BTN == LOW) espera a que el usuario libere el pulsador
 *    para evitar múltiples envíos por una sola pulsación larga.
 */
void loop() {
  if (digitalRead(PIN_BTN) == LOW) {
    delay(50); ///< Espera de debounce: deja que el rebote mecánico se estabilice
    if (digitalRead(PIN_BTN) == LOW) { ///< Confirmación: pulsación real, no ruido

      /* Calcular índice del siguiente color en la secuencia circular */
      int siguienteColor = (colorActual + 1) % 8;
      uint8_t r = COLORES[siguienteColor][0];
      uint8_t g = COLORES[siguienteColor][1];
      uint8_t b = COLORES[siguienteColor][2];

      Serial.print("Enviando -> R="); Serial.print(r);
      Serial.print(" G="); Serial.print(g);
      Serial.print(" B="); Serial.println(b);

      bool ack = false; ///< Resultado acumulado del proceso de envío + verificación
      for (int intento = 0; intento < MAX_REINTENTOS && !ack; intento++) {
        if (intento > 0) {
          /* Solo imprime y espera en reintentos, no en el primer intento */
          Serial.print("Reintento "); Serial.println(intento);
          delay(300); ///< Pausa entre reintentos para liberar el canal IR
        }
        ack = enviarColor(r, g, b);
      }

      if (ack) {
        /* ACK recibido: actualizar estado local y persistir */
        colorActual = siguienteColor;  ///< Avanzar el puntero de color
        setColor(r, g, b);             ///< Reflejar el nuevo color en el LED propio
        guardarEEPROM(colorActual);    ///< Persistir índice en EEPROM
        Serial.println("ACK OK! Color actualizado.");
      } else {
        /* Sin ACK tras todos los reintentos: estado permanece sin cambios */
        Serial.println("Sin ACK. Color sin cambios.");
      }

      delay(500); ///< Pausa post-acción: evita re-entrada inmediata al soltar el botón
      while (digitalRead(PIN_BTN) == LOW); ///< Esperar liberación física del pulsador
    }
  }
}
