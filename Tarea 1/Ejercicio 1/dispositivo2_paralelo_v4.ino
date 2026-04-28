/**
 * ===========================================================================
 * Archivo     : dispositivo2_paralelo_v4.ino
 * Ejercicio   : 1 - Receptor — Bus paralelo 8 bits
 * Autores     : Francisco Bevilacqua, Sebastian Clement
 * Materia     : Comunicación de Datos — 4to año, Ingeniería en Computación-UNRaf
 * Fecha       : 2026-04-26
 * Versión     : 4.0.0
 *
 * Descripción : Firmware del Dispositivo 2 (Receptor) — Bus paralelo 8 bits(Ejercicio 1).
 *               Recibe tramas de 5 bytes desde el Dispositivo 1 (Emisor)
 *               y controla un LED RGB mediante señales PWM según el protocolo
 *               definido en la consigna.
 *
 * ===========================================================================
 * ANÁLISIS DE LOGS — CAUSA RAÍZ DEL ERROR (v3 → v4)
 * ===========================================================================
 *
 * SÍNTOMA OBSERVADO EN LOGS (lado receptor):
 *   - Error siempre en T4 (CHK), datos T0–T3 llegan "correctos".
 *   - Diferencia siempre = 0x06 (bits D1=pin3 y D2=pin4 del bus).
 *   - Timing inter-trama regular (~31 ms), descartando causa de timing.
 *   - Errores solo en tramas COLOR (no en ON/OFF con datos 0x00).
 *
 * CAUSA RAÍZ — Crosstalk capacitivo PWM → pines del bus:
 *   Cuando este dispositivo ejecuta analogWrite(PIN_LED_R/G/B) después de
 *   una trama COLOR, los Timer1 (pines 9/10) y Timer2 (pines 9/11) generan
 *   señales PWM a ~490–980 Hz. Las corrientes de conmutación crean un campo
 *   magnético en los cables del protoboard que se acopla inductivamente a los
 *   cables del bus de datos. Los pines más afectados son los más cercanos a
 *   los cables PWM en el protoboard: D1=pin3 y D2=pin4.
 *
 *   El mecanismo exacto del error:
 *     1. D1 envía T4 (CHK) con el bus correcto.
 *     2. Mientras D1 ejecuta el setup time antes de activar STR, este D2
 *        está ejecutando analogWrite() del LED (acción de la trama anterior).
 *     3. El PWM del Timer2 (OC2B = pin3 en D1, OC2A = pin11 en D2) inyecta
 *        glitches en los cables del bus vía crosstalk capacitivo.
 *     4. Cuando D2 ejecuta leerBus() durante el strobe, los pines D1 y D2
 *        (pines 3 y 4) están levemente perturbados → lectura incorrecta.
 *
 * SOLUCIÓN PRINCIPAL (idéntica en D1 y D2):
 *   Reasignación del bus para usar pines sin función de timer:
 *     D1 → A2  (antes pin3 = OC2B del Timer2 → ¡era el pin del PWM!)
 *     D2 → A3  (antes pin4 = adyacente a OC2B)
 *   Los pines A2 y A3 son GPIO puro en el ATmega328P, sin ningún timer.
 *   Esto elimina físicamente la fuente de interferencia.
 *
 * SOLUCIÓN COMPLEMENTARIA (nuevo en v4 receptor):
 *   Doble lectura de verificación en leerBus():
 *     Después de leer el bus, espera 50 µs y vuelve a leer. Si ambas
 *     lecturas coinciden, el dato es válido. Si difieren, reintenta hasta
 *     READ_RETRIES veces. Esto captura y descarta lecturas en transición.
 *
 * ---------------------------------------------------------------------------
 * PROTOCOLO — Estructura de tramas recibidas (5 transferencias)
 * ---------------------------------------------------------------------------
 *
 *   T0 — Trama de control (1 byte):
 *        Bit 7 (MASK_CMD)  : 0 = CMD ON/OFF  |  1 = CMD COLOR
 *        Bit 0 (MASK_STATE): 1 = encender    |  0 = apagar (solo si CMD=0)
 *        Bits 6–1          : reservados
 *
 *   T1 — Intensidad Rojo   (0–255) | 0x00 si CMD=ON/OFF
 *   T2 — Intensidad Verde  (0–255) | 0x00 si CMD=ON/OFF
 *   T3 — Intensidad Azul   (0–255) | 0x00 si CMD=ON/OFF
 *   T4 — Checksum XOR: debe coincidir con T0 ^ T1 ^ T2 ^ T3
 *
 *   Detección de trama:
 *     STR=LOW  → bus en idle, ignorar.
 *     STR HIGH (flanco ascendente) → bus estable, leer inmediatamente.
 *
 * ---------------------------------------------------------------------------
 * TIMING ESPERADO (configurado en D1 v4.0.0)
 * ---------------------------------------------------------------------------
 *
 *   SETUP_TIME_MS   = 15 ms  → bus estable antes del strobe
 *   STROBE_HIGH_MS  =  8 ms  → ventana de lectura
 *   HOLD_TIME_MS    =  8 ms  → margen post-lectura
 *   INTER_FRAME_MS  =  5 ms  → pausa entre tramas
 *   Duración por trama: ~36 ms
 *   Transmisión completa (5 tramas): ~180 ms
 *   Timeout por trama configurado en este dispositivo: 1000 ms (~27×)
 *
 * ---------------------------------------------------------------------------
 * PINOUT v4.0.0
 * ---------------------------------------------------------------------------
 *
 *   Bus datos (entradas) : D0=2, D1=A2, D2=A3, D3=5, D4=6, D5=7, D6=8, D7=A0
 *   Strobe (entrada)     : A1  (STR — detección por flanco en loop)
 *   LED RGB (salidas PWM): R=9(PWM/Timer1), G=10(PWM/Timer1), B=11(PWM/Timer2)
 *   GND común con D1     : GND (OBLIGATORIO — cable dedicado)
 *   Serial debug         : UART USB-Serial hardware, 115200 bps
 *
 * ---------------------------------------------------------------------------
 * CONEXIÓN ELÉCTRICA (protoboard)
 * ---------------------------------------------------------------------------
 *
 *   D1 → D2 (9 cables de datos + 1 GND):
 *     D1.pin2  → D2.pin2   (D0 del bus)
 *     D1.pinA2 → D2.pinA2  (D1 del bus) ← cable NUEVO, antes pin3
 *     D1.pinA3 → D2.pinA3  (D2 del bus) ← cable NUEVO, antes pin4
 *     D1.pin5  → D2.pin5   (D3 del bus)
 *     D1.pin6  → D2.pin6   (D4 del bus)
 *     D1.pin7  → D2.pin7   (D5 del bus)
 *     D1.pin8  → D2.pin8   (D6 del bus)
 *     D1.pinA0 → D2.pinA0  (D7 del bus)
 *     D1.pinA1 → D2.pinA1  (STR — strobe)
 *     D1.GND   → D2.GND    (referencia común — CRÍTICO)
 *
 *   LED RGB en Dispositivo 2:
 *     Pin 9  → R (ánodo vía resistencia 220Ω) → GND (LED cátodo común)
 *     Pin 10 → G (ánodo vía resistencia 220Ω) → GND
 *     Pin 11 → B (ánodo vía resistencia 220Ω) → GND
 *     Si el LED es cátodo común: conectar cátodo a GND.
 *     Si el LED es ánodo común: conectar ánodo a 5V y NEGAR la señal PWM.
 *
 * ===========================================================================
 */

// ===========================================================================
// PINOUT — BUS DE DATOS v4.0.0 (entradas desde Dispositivo 1)
// ===========================================================================
// D1 y D2 movidos a A2/A3 (GPIO puro, sin timer) para eliminar interferencia.

const int PIN_D0 = 2;
const int PIN_D1 = A2; 
const int PIN_D2 = A3; 
const int PIN_D3 = 5;
const int PIN_D4 = 6;
const int PIN_D5 = 7;
const int PIN_D6 = 8;
const int PIN_D7 = A0;

/** Array ordenado LSB→MSB para lectura de bus. */
const int BUS_PINS[8] = {
  PIN_D0, PIN_D1, PIN_D2, PIN_D3,
  PIN_D4, PIN_D5, PIN_D6, PIN_D7
};

/** Señal de strobe: flanco ascendente indica dato válido en el bus. */
const int PIN_STR = A1;

// ===========================================================================
// PINOUT — LED RGB (salidas PWM)
// ===========================================================================

const int PIN_LED_R = 9;   // PWM — Timer1/OC1A
const int PIN_LED_G = 10;  // PWM — Timer1/OC1B
const int PIN_LED_B = 11;  // PWM — Timer2/OC2A

// ===========================================================================
// CONSTANTES DEL PROTOCOLO
// ===========================================================================

/** Máscara para extraer el bit de comando del T0. */
#define MASK_CMD        0x80

/** Máscara para extraer el bit de estado del T0 (solo válido si CMD=ON/OFF). */
#define MASK_STATE      0x01

/** Valor de CMD cuando el bit 7 de T0 es 0 → comando ON/OFF. */
#define CMD_ONOFF       0x00

/** Valor de CMD cuando el bit 7 de T0 es 1 → comando COLOR. */
#define CMD_COLOR       0x80

/** Número de tramas en una transmisión completa. */
#define NUM_TRAMAS      5

/**
 * Timeout máximo entre tramas consecutivas (ms).
 * D1 v4 tarda ~36 ms por trama. 1000 ms = ~27× el tiempo esperado.
 * Lo suficientemente largo para absorber variaciones, suficientemente
 * corto para detectar desconexión real de D1 sin bloquear el sistema.
 */
#define TIMEOUT_TRAMA_MS  1000

/**
 * Número de reintentos de lectura si la doble verificación falla.
 * Mecanismo de defensa adicional contra glitches de lectura en protoboard.
 */
#define READ_RETRIES    3

// ===========================================================================
// VARIABLES DE ESTADO
// ===========================================================================

/** Estado lógico actual del LED. */
bool    ledEncendido  = false;

/** Color actual del LED (preservado para restaurar al encender). 
 *  Inicia en 0 (apagado) y se actualiza solo con comandos COLOR, no con ON/OFF.
 *  uint8_t es un dato entero sin signo de 8 bits para preservar el rango 0–255 de intensidad.
*/
uint8_t colorActualR  = 0; 
uint8_t colorActualG  = 0;
uint8_t colorActualB  = 0;

/**
 * Último estado de STR en el polling del loop.
 * Permite detectar el flanco ascendente LOW→HIGH.
 */
bool strobeAnterior = false;

/** Buffer de recepción: almacena las 5 tramas de una transmisión completa. */
uint8_t bufferRx[NUM_TRAMAS];

/**
 * Timestamps de recepción de cada trama (ms desde boot).
 * Permite calcular intervalos inter-trama para diagnóstico de timing de D1.
 */
unsigned long tiempoRx[NUM_TRAMAS];

/** Contadores de diagnóstico de sesión. */
uint16_t contadorRX       = 0;
uint16_t erroresChk       = 0;
uint16_t erroresTimeout   = 0;
uint16_t contadorReintentos = 0;

// ===========================================================================
// DECLARACIONES DE FUNCIONES
// ===========================================================================

void    configurarPines();
uint8_t leerBus();
bool    esperarStrobe(unsigned long timeoutMs);
bool    recibirTransmision();
uint8_t calcularChecksum(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3);
void    procesarTrama();
void    aplicarLED(uint8_t r, uint8_t g, uint8_t b);
void    apagarLED();
void    imprimirByte(uint8_t valor);
void    imprimirBinario(uint8_t valor);
void    logTablaRecepcion();
void    logChecksum(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3,
                    uint8_t chkRecibido, uint8_t chkCalculado);
void    logResumenSesion();

// ===========================================================================
// SETUP
// ===========================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  Serial.println(F(""));
  Serial.println(F("======================================================="));
  Serial.println(F("  DISPOSITIVO 2 | Receptor | Bus paralelo 8 bits"));
  Serial.println(F("  Comunicacion de Datos — v4.0.0"));
  Serial.println(F("======================================================="));
  Serial.println(F("  PINOUT v4 (REASIGNADO):"));
  Serial.println(F("    Bus datos : D0=2 D1=A2 D2=A3 D3=5 D4=6 D5=7 D6=8 D7=A0"));
  Serial.println(F("    *** D1 y D2 movidos a A2/A3 — sin interferencia PWM ***"));
  Serial.println(F("    Strobe    : A1  (STR — deteccion por flanco)"));
  Serial.println(F("    LED RGB   : R=9(PWM) G=10(PWM) B=11(PWM)"));
  Serial.println(F("  PROTOCOLO:"));
  Serial.println(F("    5 tramas: T0(ctrl) T1(R) T2(G) T3(B) T4(CHK=XOR)"));
  Serial.print  (F("  Timeout por trama: ")); Serial.print(TIMEOUT_TRAMA_MS);
  Serial.println(F(" ms"));
  Serial.println(F("  CORRECCIONES v4 (sobre v3):"));
  Serial.println(F("    [FIX-1] D1=A2, D2=A3: elimina interferencia Timer2/PWM."));
  Serial.println(F("    [FIX-2] Doble lectura de verificacion en leerBus()."));
  Serial.println(F("    [FIX-3] Conteo de reintentos de lectura para diagnostico."));
  Serial.println(F("======================================================="));
  Serial.println(F(""));

  configurarPines();
  apagarLED();

  Serial.println(F("[INIT] Sistema listo. Esperando transmision de D1 v4..."));
  Serial.println(F("[INIT] Verificar que D1 tenga firmware v4 y misma asignacion de pines."));
  Serial.println(F(""));
}

// ===========================================================================
// LOOP PRINCIPAL
// ===========================================================================

/**
 * Detección de flanco ascendente de STR por polling.
 *
 * El loop ejecuta digitalRead(PIN_STR) continuamente comparando con el
 * estado anterior. Al detectar LOW→HIGH, el bus contiene T0 estable y
 * válido (D1 ya esperó SETUP_TIME_MS antes de subir STR).
 *
 * DISEÑO v3/v4: sin Serial.print() en el loop para que el polling de STR
 * sea lo más rápido posible y no perder flancos de corta duración.
 */
void loop() {
  const bool strobeActual = (digitalRead(PIN_STR) == HIGH);

  if (strobeActual && !strobeAnterior) {
    // --- Flanco ascendente: bus contiene T0 estable ---
    bufferRx[0]  = leerBus();
    tiempoRx[0]  = millis();

    // Recibir T1..T4 (función puramente de hardware, sin Serial)
    if (recibirTransmision()) {
      // Todas las tramas llegaron → procesar y loguear
      procesarTrama();
    } else {
      // Timeout en alguna trama intermedia
      erroresTimeout++;
      Serial.println(F(""));
      Serial.println(F("-------------------------------------------------------"));
      Serial.println(F("[ERROR] TIMEOUT — Transmision incompleta."));
      Serial.print  (F("        T0 recibido antes del timeout: "));
      imprimirByte(bufferRx[0]);
      Serial.println(F(""));
      Serial.print  (F("        Timeouts acumulados: ")); Serial.println(erroresTimeout);
      Serial.println(F("        DIAGNOSTICO: STR espurio o D1 detuvo la transmision."));
      Serial.println(F("        Verificar cable STR (A1) y alimentacion de D1."));
      Serial.println(F("-------------------------------------------------------"));
      Serial.println(F(""));
    }
  }

  strobeAnterior = strobeActual;
}

// ===========================================================================
// FUNCIONES DE HARDWARE
// ===========================================================================

/**
 * Configura los pines del bus como entradas (sin pull-up — D1 maneja el nivel),
 * STR como entrada, y los pines del LED como salidas PWM.
 */
void configurarPines() {
  for (int i = 0; i < 8; i++) {
    pinMode(BUS_PINS[i], INPUT);
  }
  pinMode(PIN_STR, INPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  Serial.println(F("[INIT] Pines configurados: bus=INPUT, STR=INPUT, LED=OUTPUT."));
}

/**
 * Lee el bus de datos con doble verificación para detectar glitches.
 *
 * Mecanismo de doble lectura (nuevo en v4):
 *   1. Lee los 8 pines y arma el byte (lectura A).
 *   2. Espera 50 µs (tiempo de settling para carga RC del bus).
 *   3. Lee nuevamente los 8 pines (lectura B).
 *   4. Si A == B → dato estable, retornar A.
 *   5. Si A != B → bus en transición o glitch, reintentar (max READ_RETRIES).
 *   6. Si tras READ_RETRIES intentos sigue inestable, retornar la última lectura
 *      y loguear el evento para diagnóstico.
 *
 * @return  Byte reconstruido desde los 8 pines del bus.
 */
uint8_t leerBus() {
  for (int reintento = 0; reintento < READ_RETRIES; reintento++) {

    // Lectura A
    uint8_t lecturaA = 0;
    for (int i = 0; i < 8; i++) {
      if (digitalRead(BUS_PINS[i]) == HIGH) {
        lecturaA |= (1 << i);
      }
    }

    // Esperar settling time
    delayMicroseconds(50);

    // Lectura B (verificación)
    uint8_t lecturaB = 0;
    for (int i = 0; i < 8; i++) {
      if (digitalRead(BUS_PINS[i]) == HIGH) {
        lecturaB |= (1 << i);
      }
    }

    if (lecturaA == lecturaB) {
      // Dato estable y verificado
      if (reintento > 0) {
        contadorReintentos++;
        // No imprimir aquí (zona de recepción, sin Serial)
        // El evento se refleja en contadorReintentos que se logua en procesarTrama
      }
      return lecturaA;
    }
    // Si no coinciden, reintentar (el bus aún está transitando)
  }

  // Tras READ_RETRIES intentos: retornar última lectura y marcar para log
  contadorReintentos++;
  uint8_t lecturaFinal = 0;
  for (int i = 0; i < 8; i++) {
    if (digitalRead(BUS_PINS[i]) == HIGH) {
      lecturaFinal |= (1 << i);
    }
  }
  return lecturaFinal;
}

// ===========================================================================
// FUNCIONES DE RECEPCIÓN
// ===========================================================================

/**
 * Espera un flanco ascendente de STR dentro del timeout especificado.
 *
 * El polling corre sin ningún Serial.print() para maximizar la velocidad
 * de detección y no perder flancos de corta duración (STROBE_HIGH = 8 ms).
 *
 * @param timeoutMs  Tiempo máximo de espera en milisegundos.
 * @return           true si se detectó flanco ascendente antes del timeout,
 *                   false si se agotó el tiempo de espera.
 */
bool esperarStrobe(unsigned long timeoutMs) {
  const unsigned long inicio = millis();
  bool previo = (digitalRead(PIN_STR) == HIGH);

  while ((millis() - inicio) < timeoutMs) {
    const bool actual = (digitalRead(PIN_STR) == HIGH);
    if (actual && !previo) {
      return true;  // Flanco ascendente detectado
    }
    previo = actual;
    // Sin delay: polling a máxima velocidad para no perder el flanco
  }
  return false;  // Timeout
}

/**
 * Recibe las tramas T1..T4 sin imprimir nada por Serial.
 *
 * DISEÑO v3/v4 — Zona silenciosa de recepción:
 *   Esta función es exclusivamente de hardware. No llama a Serial.print().
 *   Espera el flanco de STR para cada trama, lee el bus con doble verificación,
 *   y almacena el resultado en bufferRx[] junto con su timestamp.
 *   Todo el logging se delega a procesarTrama().
 *
 * @return  true si T1..T4 llegaron dentro del timeout, false si hubo timeout.
 */
bool recibirTransmision() {
  for (int i = 1; i < NUM_TRAMAS; i++) {
    if (!esperarStrobe(TIMEOUT_TRAMA_MS)) {
      bufferRx[i] = 0xFF;  // Marca de trama no recibida por timeout
      tiempoRx[i] = millis();
      return false;
    }
    bufferRx[i] = leerBus();   // Lectura con doble verificación
    tiempoRx[i] = millis();
  }
  return true;
}

// ===========================================================================
// FUNCIONES DEL PROTOCOLO
// ===========================================================================

/**
 * Calcula el checksum XOR de las cuatro tramas de datos.
 *
 * @param t0,t1,t2,t3  Bytes de datos recibidos.
 * @return             XOR local para comparar con T4 recibido.
 */
uint8_t calcularChecksum(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3) {
  return t0 ^ t1 ^ t2 ^ t3;
}

/**
 * Procesa el buffer de recepción completo (bufferRx[0..4]).
 *
 * DISEÑO v3/v4: todo el Serial.print() está aquí, ejecutándose cuando
 * el bus ya está libre y no hay riesgo de interferencia UART→GPIO.
 *
 * Pasos:
 *   1. Imprimir tabla completa de las 5 tramas recibidas con timestamps.
 *   2. Calcular y verificar checksum XOR.
 *   3. Si OK → interpretar comando y actuar sobre el LED.
 *   4. Si error → descartar, loguear diagnóstico con bits erróneos identificados.
 */
void procesarTrama() {
  logTablaRecepcion();

  const uint8_t t0           = bufferRx[0];
  const uint8_t t1           = bufferRx[1];
  const uint8_t t2           = bufferRx[2];
  const uint8_t t3           = bufferRx[3];
  const uint8_t chkRecibido  = bufferRx[4];
  const uint8_t chkCalculado = calcularChecksum(t0, t1, t2, t3);

  logChecksum(t0, t1, t2, t3, chkRecibido, chkCalculado);

  if (contadorReintentos > 0) {
    Serial.print  (F("  [DIAG] Reintentos de lectura de bus en esta sesion: "));
    Serial.println(contadorReintentos);
  }

  if (chkRecibido != chkCalculado) {
    // -----------------------------------------------------------------------
    // CHECKSUM INCORRECTO — Trama descartada
    // -----------------------------------------------------------------------
    erroresChk++;
    const uint8_t diferencia = chkRecibido ^ chkCalculado;

    Serial.println(F(""));
    Serial.println(F("  [ERROR] CHECKSUM INCORRECTO → Trama DESCARTADA."));
    Serial.print  (F("          CHK recibido  (T4)  : ")); imprimirByte(chkRecibido);  Serial.println(F(""));
    Serial.print  (F("          CHK calculado (local): ")); imprimirByte(chkCalculado); Serial.println(F(""));
    Serial.print  (F("          Diferencia            : ")); imprimirByte(diferencia);

    Serial.print(F("  bin="));
    imprimirBinario(diferencia);
    Serial.println(F(""));

    Serial.print(F("          Bits en error: "));
    bool hayError = false;
    for (int b = 7; b >= 0; b--) {
      if ((diferencia >> b) & 0x01) {
        Serial.print(F("D")); Serial.print(b); Serial.print(F(" "));
        hayError = true;
      }
    }
    if (!hayError) Serial.print(F("ninguno"));
    Serial.println(F(""));

    Serial.println(F("          Estado del LED NO modificado."));
    Serial.print  (F("          Errores de checksum acumulados: ")); Serial.println(erroresChk);

    Serial.println(F("  DIAGNOSTICO v4 (si este error persiste):"));
    Serial.println(F("    1. Confirmar que D1 tambien tiene firmware v4 (pines A2/A3)."));
    Serial.println(F("    2. Verificar que los cables D1 y D2 esten en pinA2 y pinA3"));
    Serial.println(F("       en AMBOS dispositivos (no en pin3/pin4 como en v3)."));
    Serial.println(F("    3. Verificar calidad de soldadura/contacto del LED RGB."));
    Serial.println(F("    4. En protoboard: usar cables cortos y separar bus de PWM."));
    Serial.println(F("    5. Si error es DIFERENTE a 0x06, revisar cable GND comun."));
    Serial.println(F("-------------------------------------------------------"));
    Serial.println(F(""));
    return;
  }

  // -----------------------------------------------------------------------
  // CHECKSUM CORRECTO — interpretar y ejecutar comando
  // -----------------------------------------------------------------------
  Serial.println(F("  [CHK] OK → Ejecutando comando..."));
  Serial.println(F(""));

  const uint8_t cmd = t0 & MASK_CMD;

  if (cmd == CMD_ONOFF) {
    // Comando ON/OFF: bit 0 de T0 indica el nuevo estado
    const bool nuevoEstado = (t0 & MASK_STATE) == 0x01;
    ledEncendido = nuevoEstado;

    Serial.print(F("  [CMD] ON/OFF → LED: "));
    Serial.println(ledEncendido ? F("ENCENDIDO") : F("APAGADO"));

    if (ledEncendido) {
      Serial.print(F("        Restaurando ultimo color: R="));
      Serial.print(colorActualR); Serial.print(F(" G="));
      Serial.print(colorActualG); Serial.print(F(" B="));
      Serial.println(colorActualB);
      aplicarLED(colorActualR, colorActualG, colorActualB);
    } else {
      apagarLED();
      Serial.println(F("        PWM R=0 G=0 B=0 → LED apagado."));
    }

  } else if (cmd == CMD_COLOR) {
    // Comando COLOR: T1=R, T2=G, T3=B
    colorActualR = t1;
    colorActualG = t2;
    colorActualB = t3;

    Serial.print(F("  [CMD] COLOR → R=")); Serial.print(colorActualR);
    Serial.print(F("  G=")); Serial.print(colorActualG);
    Serial.print(F("  B=")); Serial.println(colorActualB);

    if (ledEncendido) {
      aplicarLED(colorActualR, colorActualG, colorActualB);
    } else {
      Serial.println(F("        LED apagado: color guardado. Presiona ON/OFF para aplicarlo."));
    }

  } else {
    // Comando no reconocido: bit 7 ni 0 ni 1 → trama corrupta no detectada por CHK
    Serial.print  (F("  [WARN] Comando no reconocido: T0="));
    imprimirByte(t0); Serial.println(F(""));
    Serial.println(F("         Trama ignorada. Verificar cable D7 (A0)."));
  }

  Serial.println(F("-------------------------------------------------------"));
  Serial.println(F(""));
}

// ===========================================================================
// FUNCIONES DE CONTROL DEL LED RGB
// ===========================================================================

/**
 * Aplica los valores de intensidad RGB al LED mediante PWM.
 * @param r,g,b  Intensidades de cada canal (0=apagado, 255=máximo).
 */
void aplicarLED(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_LED_R, r);
  analogWrite(PIN_LED_G, g);
  analogWrite(PIN_LED_B, b);
  Serial.print(F("        [LED] PWM aplicado: R="));
  Serial.print(r);
  Serial.print(F(" G=")); Serial.print(g);
  Serial.print(F(" B=")); Serial.println(b);
}

/**
 * Apaga el LED configurando todos los canales PWM a 0.
 */
void apagarLED() {
  analogWrite(PIN_LED_R, 0);
  analogWrite(PIN_LED_G, 0);
  analogWrite(PIN_LED_B, 0);
}

// ===========================================================================
// FUNCIONES DE LOGGING
// ===========================================================================

/**
 * Imprime un byte en formato hexadecimal (siempre 2 dígitos, ej: 0x0A).
 */
void imprimirByte(uint8_t valor) {
  Serial.print(F("0x"));
  if (valor < 0x10) Serial.print(F("0"));
  Serial.print(valor, HEX);
}

/**
 * Imprime un byte en formato binario MSB→LSB con separador de nibbles.
 */
void imprimirBinario(uint8_t valor) {
  for (int b = 7; b >= 0; b--) {
    Serial.print((valor >> b) & 0x01);
    if (b == 4) Serial.print(F(" "));
  }
}

/**
 * Imprime la tabla completa de las 5 tramas recibidas con timestamps.
 *
 * Incluye:
 *   - Número de recepción (para correlación con TX de D1).
 *   - Valor en hex, decimal y binario de cada trama.
 *   - Tiempo relativo desde T0 (en ms) para diagnóstico de timing.
 *   - Intervalos inter-trama con alertas si son inusuales.
 */
void logTablaRecepcion() {
  contadorRX++;
  const unsigned long tiempoBase = tiempoRx[0];

  Serial.println(F("-------------------------------------------------------"));
  Serial.print  (F("  RX #")); Serial.print(contadorRX);
  Serial.println(F(" | Tabla de tramas recibidas:"));
  Serial.println(F("  Trama        | Hex   | Dec | Binario   | t_rel(ms)"));
  Serial.println(F("  -------------|-------|-----|-----------|----------"));

  const char* etiq[5] = {
    "T0-Control   ",
    "T1-Rojo      ",
    "T2-Verde     ",
    "T3-Azul      ",
    "T4-Checksum  "
  };

  for (int i = 0; i < NUM_TRAMAS; i++) {
    Serial.print(F("  ")); Serial.print(etiq[i]);
    Serial.print(F("| ")); imprimirByte(bufferRx[i]);
    Serial.print(F(" | "));
    if (bufferRx[i] < 100) Serial.print(F(" "));
    if (bufferRx[i] < 10)  Serial.print(F(" "));
    Serial.print(bufferRx[i]);
    Serial.print(F(" | "));
    imprimirBinario(bufferRx[i]);
    Serial.print(F(" | +"));
    Serial.print(tiempoRx[i] - tiempoBase);
    Serial.println(F(" ms"));
  }

  // Intervalos inter-trama para diagnóstico de timing en D1
  Serial.println(F("  Intervalos entre tramas (diagnostico timing D1):"));
  for (int i = 1; i < NUM_TRAMAS; i++) {
    const unsigned long intervalo = tiempoRx[i] - tiempoRx[i - 1];
    Serial.print(F("    T")); Serial.print(i - 1);
    Serial.print(F(" → T")); Serial.print(i);
    Serial.print(F(" : ")); Serial.print(intervalo); Serial.print(F(" ms"));
    if (intervalo > 70) {
      Serial.print(F("  ← LARGO (esperado ~36ms para v4)"));
    } else if (intervalo < 15) {
      Serial.print(F("  ← CORTO (posible trama perdida)"));
    }
    Serial.println(F(""));
  }
}

/**
 * Loguea el cálculo del checksum XOR con comparación bit a bit.
 * Muestra exactamente qué recibió D2 y qué calcula localmente para
 * identificar el/los bit(s) en error si hay discrepancia.
 *
 * @param t0..t3         Bytes de datos recibidos.
 * @param chkRecibido    T4 tal cual llegó por el bus.
 * @param chkCalculado   XOR local de T0..T3.
 */
void logChecksum(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3,
                 uint8_t chkRecibido, uint8_t chkCalculado) {

  Serial.print(F("  CHK calculado local : "));
  imprimirByte(t0); Serial.print(F(" ^ "));
  imprimirByte(t1); Serial.print(F(" ^ "));
  imprimirByte(t2); Serial.print(F(" ^ "));
  imprimirByte(t3); Serial.print(F(" = "));
  imprimirByte(chkCalculado); Serial.println(F(""));

  // Tabla bit a bit: T0..T3 + CHK calculado + CHK recibido
  Serial.println(F("        Bit a bit (MSB→LSB):"));
  const uint8_t vals[6]  = { t0, t1, t2, t3, chkCalculado, chkRecibido };
  const char*   etiq[6]  = { "T0 ", "T1 ", "T2 ", "T3 ", "CHK(calc)", "CHK(recv)" };
  for (int v = 0; v < 6; v++) {
    Serial.print(F("        ")); Serial.print(etiq[v]); Serial.print(F(" = "));
    imprimirBinario(vals[v]);
    if (v >= 4) {
      Serial.print(F("  ("));
      imprimirByte(vals[v]);
      Serial.print(F(")"));
    }
    Serial.println(F(""));
  }

  // Fila de diferencias: marca con ^ los bits que difieren
  const uint8_t diff = chkCalculado ^ chkRecibido;
  Serial.print(F("        Diferencia      = "));
  for (int b = 7; b >= 0; b--) {
    Serial.print(((diff >> b) & 0x01) ? F("^") : F("."));
    if (b == 4) Serial.print(F(" "));
  }
  if (diff == 0) {
    Serial.println(F("  (sin diferencia — OK)"));
  } else {
    Serial.print(F("  (bits alterados: "));
    imprimirByte(diff);
    Serial.println(F(")"));
  }

  Serial.print(F("  CHK recibido  (T4)   : ")); imprimirByte(chkRecibido);  Serial.println(F(""));
  Serial.print(F("  CHK calculado (local): ")); imprimirByte(chkCalculado); Serial.println(F(""));
  Serial.println(F(""));
}

/**
 * Imprime un resumen de la sesión de recepción con estadísticas de calidad.
 * Disponible para llamar manualmente desde el monitor serial si se necesita.
 */
void logResumenSesion() {
  const uint16_t ok = contadorRX - erroresChk - erroresTimeout;

  Serial.println(F(""));
  Serial.println(F("======= RESUMEN DE SESION D2 ======="));
  Serial.print  (F("  Recepciones totales : ")); Serial.println(contadorRX);
  Serial.print  (F("  Tramas OK           : ")); Serial.println(ok);
  Serial.print  (F("  Errores de checksum : ")); Serial.println(erroresChk);
  Serial.print  (F("  Errores de timeout  : ")); Serial.println(erroresTimeout);
  Serial.print  (F("  Reintentos de lectura: ")); Serial.println(contadorReintentos);
  if (contadorRX > 0) {
    Serial.print(F("  Tasa de exito       : "));
    Serial.print(ok * 100UL / contadorRX);
    Serial.println(F(" %"));
  }
  Serial.println(F("====================================="));
}
