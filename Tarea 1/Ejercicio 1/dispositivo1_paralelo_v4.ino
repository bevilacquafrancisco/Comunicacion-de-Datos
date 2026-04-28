/**
 * ===========================================================================
 * Archivo     : dispositivo1_paralelo_v4.ino
 * Ejercicio   : 1 - Emisor — Bus paralelo 8 bits
 * Autores     : Francisco Bevilacqua, Sebastian Clement
 * Materia     : Comunicación de Datos — 4to año, Ingeniería en Computación-UNRaf
 * Fecha       : 2026-04-26
 * Versión     : 4.0.0
 *
 * Descripción : Firmware del Dispositivo 1 (Emisor) — Bus paralelo 8 bits(Ejercicio 1).
 *               Emite tramas de 5 bytes hacia el Dispositivo 2 (Receptor)
 *               usando el protocolo definido en la consigna. Controla el
 *               encendido/apagado y el color del LED RGB conectado a D2.
 *
 * ===========================================================================
 * ANÁLISIS DE LOGS — CAUSA RAÍZ DEL ERROR (v3 → v4)
 * ===========================================================================
 *
 * SÍNTOMA OBSERVADO EN LOGS:
 *   - Error siempre en T4 (CHK), nunca en T0–T3.
 *   - Diferencia siempre = 0x06 (bits D1 y D2, es decir pines 3 y 4).
 *   - Timing inter-trama regular (~31 ms), descartando causa de timing.
 *   - Errores solo en tramas COLOR (no en ON/OFF con datos 0x00).
 *
 * CAUSA RAÍZ IDENTIFICADA — Interferencia PWM sobre pines del bus:
 *   Los pines Arduino 3 y 11 están asociados al Timer2 (OC2A/OC2B).
 *   Los pines 9 y 10 están asociados al Timer1 (OC1A/OC1B).
 *   En la asignación de bus v3, D1=pin3 y D2=pin4 eran los afectados.
 *
 *   Cuando el Dispositivo 2 ejecuta analogWrite() sobre los pines del LED
 *   RGB (9, 10, 11), el PWM de los Timer1/Timer2 genera conmutaciones a
 *   ~490 Hz (Timer2) y ~980 Hz (Timer1). Estos flancos se acoplan
 *   capacitivamente/inductivamente a los cables del bus, introduciendo
 *   glitches de hasta 100–200 ns en pines adyacentes o compartidos de
 *   puerto. En el lado EMISOR (D1), el pin 3 (OC2B) puede ver reflexiones
 *   de su propio PWM si D2 tiene el mismo pin como salida PWM, aunque el
 *   pin 3 de D1 está configurado como salida. En el lado RECEPTOR (D2), la
 *   causa más directa es que los pines 3 y 4 (D1 y D2 del bus) se ven
 *   afectados por el campo magnético generado por las corrientes del PWM
 *   en los cables adyacentes.
 *
 *   Evidencia clave: el error aparece SOLO en datos con bits D1/D2
 *   alternados (CHK con patrón 0x06=0b00000110), y NUNCA en tramas
 *   ON/OFF donde T1..T3 son todos 0x00 y CHK tiene valores simples.
 *   Esto confirma que el error depende del ESTADO del cable de datos
 *   en ese momento (transición HIGH→LOW o LOW→HIGH), que es exactamente
 *   el comportamiento de crosstalk capacitivo.
 *
 * CORRECCIONES IMPLEMENTADAS EN v4:
 *
 *   1. REASIGNACIÓN DEL BUS DE DATOS (corrección principal):
 *      El bus se reasigna para evitar completamente los pines de Timer:
 *        D0 = pin 2  (sin timer)      → sin cambio
 *        D1 = pin A2 (sin timer)      → antes pin 3 (OC2B!) ← CAUSA RAÍZ
 *        D2 = pin A3 (sin timer)      → antes pin 4 (adyacente a OC2B)
 *        D3 = pin 5  (sin timer)      → sin cambio
 *        D4 = pin 6  (sin timer)      → sin cambio
 *        D5 = pin 7  (sin timer)      → sin cambio
 *        D6 = pin 8  (sin timer)      → sin cambio
 *        D7 = pin A0 (sin timer)      → sin cambio
 *        STR= pin A1 (sin timer)      → sin cambio
 *      Pines A2 y A3 no tienen función de timer en el ATmega328P.
 *      Son puramente GPIO digital, eliminando toda interferencia PWM.
 *
 *   2. DOBLE LECTURA DE VERIFICACIÓN EN ESCRITURA (defensa adicional):
 *      escribirByte() verifica que todos los pines del bus se establecieron
 *      correctamente leyendo de vuelta el estado del pin. Si la lectura de
 *      verificación no coincide, reintenta la escritura hasta WRITE_RETRIES
 *      veces. Esto detecta y corrige glitches de escritura por carga
 *      capacitiva en cables largos de protoboard.
 *
 *   3. HOLD TIME AUMENTADO: 5 ms → 8 ms
 *      Margen adicional para que D2 capture el dato en cualquier condición
 *      de carga del loop (procesamiento de log, PWM, etc).
 *
 *   4. INTER_FRAME AUMENTADO: 3 ms → 5 ms
 *      Da más tiempo a D2 para completar su ciclo de actualización del LED
 *      (analogWrite) antes de que el bus cambie al siguiente dato.
 *
 * ---------------------------------------------------------------------------
 * PROTOCOLO — Estructura de tramas (5 transferencias por transmisión)
 * ---------------------------------------------------------------------------
 *
 *   T0 — Trama de control (1 byte):
 *        Bit 7 (CMD)   : 0 = ON/OFF  |  1 = COLOR
 *        Bit 0 (STATE) : 1 = encender |  0 = apagar  (solo si CMD=0)
 *        Bits 6–1      : reservados, siempre 0
 *
 *   T1 — Intensidad Rojo   (0–255) | 0x00 si CMD=ON/OFF
 *   T2 — Intensidad Verde  (0–255) | 0x00 si CMD=ON/OFF
 *   T3 — Intensidad Azul   (0–255) | 0x00 si CMD=ON/OFF
 *   T4 — Checksum XOR: T0 ^ T1 ^ T2 ^ T3
 *
 *   Señalización:
 *     STR=LOW  → bus sin datos (idle o transición)
 *     STR=HIGH → bus estable, dato válido, D2 debe leer
 *
 * ---------------------------------------------------------------------------
 * TIMING POR TRAMA (v4.0.0)
 * ---------------------------------------------------------------------------
 *
 *   |<-- SETUP_TIME_MS -->|<-- STROBE_HIGH_MS -->|<-- HOLD_TIME_MS -->|<-- INTER_FRAME_MS -->|
 *   Bus escrito            STR sube               STR baja             Bus limpiado
 *   (datos estables)       (D2 lee bus)           (fin de ventana)     (siguiente trama)
 *
 *   SETUP_TIME_MS   = 15 ms  (estabilización eléctrica del bus)
 *   STROBE_HIGH_MS  =  8 ms  (ventana de lectura de D2)
 *   HOLD_TIME_MS    =  8 ms  (margen post-lectura antes de limpiar bus)
 *   INTER_FRAME_MS  =  5 ms  (pausa entre tramas consecutivas)
 *   Duración por trama: 15+8+8+5 = 36 ms
 *   Transmisión completa (5 tramas): ~180 ms
 *
 * ---------------------------------------------------------------------------
 * PINOUT v4.0.0
 * ---------------------------------------------------------------------------
 *
 *   Bus datos (salidas) : D0=2, D1=A2, D2=A3, D3=5, D4=6, D5=7, D6=8, D7=A0
 *   Strobe (salida)     : A1
 *   BTN ON/OFF          : pin 11  (INPUT_PULLUP, activo en LOW)
 *   BTN CAMBIAR         : pin 12  (INPUT_PULLUP, activo en LOW)
 *   GND común con D2    : GND  (OBLIGATORIO — cable dedicado)
 *   Serial debug        : UART USB-Serial hardware, 115200 bps
 *
 * ---------------------------------------------------------------------------
 * CONEXIÓN ELÉCTRICA (protoboard)
 * ---------------------------------------------------------------------------
 *
 *   Dispositivo 1 → Dispositivo 2 (9 cables de datos + 1 GND):
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
 *   Botones en Dispositivo 1:
 *     BTN_ONOFF  : entre pin11 y GND (INPUT_PULLUP: LOW = pulsado)
 *     BTN_CAMBIAR: entre pin12 y GND (INPUT_PULLUP: LOW = pulsado)
 *     No se necesitan resistencias externas (pull-up interno habilitado).
 *
 * ===========================================================================
 */

// ===========================================================================
// PINOUT — BUS DE DATOS v4.0.0
// ===========================================================================
// D1 movido de pin3 (OC2B/Timer2) a A2 (GPIO puro, sin timer).
// D2 movido de pin4 (adyacente a OC2B) a A3 (GPIO puro, sin timer).
// Esto elimina la interferencia PWM que causaba los errores en D1/D2.

const int PIN_D0 = 2;
const int PIN_D1 = A2; 
const int PIN_D2 = A3;  
const int PIN_D3 = 5;
const int PIN_D4 = 6;
const int PIN_D5 = 7;
const int PIN_D6 = 8;
const int PIN_D7 = A0;

/** Array ordenado LSB→MSB para escritura de bus. */
const int BUS_PINS[8] = {
  PIN_D0, PIN_D1, PIN_D2, PIN_D3,
  PIN_D4, PIN_D5, PIN_D6, PIN_D7
};

/** Señal de strobe: sube para indicar que el bus tiene dato válido. */
const int PIN_STR = A1;

// ===========================================================================
// PINOUT — BOTONES
// ===========================================================================

const int PIN_BTN_ONOFF   = 11;
const int PIN_BTN_CAMBIAR = 12;

// ===========================================================================
// CONSTANTES DEL PROTOCOLO
// ===========================================================================

/** Bit 7 del T0 = 0 → comando ON/OFF. */
#define CMD_ONOFF   0x00

/** Bit 7 del T0 = 1 → comando cambio de color. */
#define CMD_COLOR   0x80

// ===========================================================================
// CONSTANTES DE TIMING v4.0.0
// ===========================================================================

/**
 * Tiempo desde que el bus se escribe hasta que STR sube (ms).
 * Asegura estabilización eléctrica de los 8 pines del bus antes de que
 * D2 sea habilitado para leer. 15 ms es conservador para protoboard/jumpers.
 */
#define SETUP_TIME_MS     15

/**
 * Duración del pulso STR=HIGH (ms).
 * Ventana durante la cual D2 detecta el flanco y ejecuta leerBus().
 * 8 ms garantiza detección incluso si el loop de D2 está procesando logs.
 */
#define STROBE_HIGH_MS     8

/**
 * Tiempo desde que STR baja hasta que el bus se limpia (ms).
 * Aumentado a 8 ms en v4 para mayor margen de seguridad post-lectura.
 * D2 puede tardar hasta ~3 ms en registrar el dato internamente después
 * del flanco descendente de STR.
 */
#define HOLD_TIME_MS       8

/**
 * Pausa entre limpiarBus() de la trama N y escribirByte() de la trama N+1 (ms).
 * Aumentado a 5 ms en v4 para dar tiempo a D2 a ejecutar analogWrite()
 * después de procesar el CHK, antes de que el bus cambie.
 */
#define INTER_FRAME_MS     5

/** Tiempo mínimo entre dos presiones válidas del mismo botón (ms). */
#define DEBOUNCE_MS       50

/**
 * Número de reintentos de escritura si la verificación de vuelta falla.
 * Mecanismo de defensa adicional contra glitches de escritura en protoboard.
 */
#define WRITE_RETRIES      3

// ===========================================================================
// VARIABLES DE ESTADO
// ===========================================================================

/** Estado lógico del LED en D2 (espejado en D1 para coherencia de protocolo). */
bool    ledEncendido        = false;

/** Último color generado (se mantiene para consistencia de estado). 
 *  Se inicia en (0,0,0) = apagado, aunque el LED en D2 también se inicia apagado por protocolo.
 *  uint8_t es un tipo de dato entero sin signo de 8 bits, ideal para representar valores de 0 a 255.
*/
uint8_t colorR              = 0; // dato entero sin signo de 8 bits
uint8_t colorG              = 0;
uint8_t colorB              = 0;

/** Estado de debounce de botones. */
unsigned long ultimoTimeBtnOnOff   = 0;
unsigned long ultimoTimeBtnCambiar = 0;
bool estadoEstableOnOff    = HIGH;
bool estadoEstableCambiar  = HIGH;

/** Contador de transmisiones para correlación con logs de D2. */
uint16_t contadorTX        = 0;

/** Contador de reintentos de escritura para diagnóstico. */
uint16_t contadorReintentos = 0;

// ===========================================================================
// DECLARACIONES DE FUNCIONES
// ===========================================================================

void    configurarBus();
bool    escribirByte(uint8_t dato);
void    limpiarBus();
void    activarStrobe();
void    transmitirTrama(uint8_t* bytes, uint8_t cantidad);
uint8_t calcularChecksum(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3);
void    enviarTramaOnOff(bool estado);
void    enviarTramaColor(uint8_t r, uint8_t g, uint8_t b);
bool    leerBoton(int pin, unsigned long &ultimoTiempo, bool &estadoEstable);

void    imprimirByte(uint8_t valor);
void    imprimirBinario(uint8_t valor);
void    logCabeceraTX(const char* tipo);
void    logFilaTrama(uint8_t idx, uint8_t valor, const char* desc);
void    logChecksum(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3, uint8_t chk);
void    logTransmisionCompleta(uint8_t* bytes, uint8_t cantidad);
void    logPieTX();
void    logResumenSesion();

// ===========================================================================
// SETUP
// ===========================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }  // Esperar apertura del puerto (necesario en algunos clones)

  Serial.println(F(""));
  Serial.println(F("======================================================="));
  Serial.println(F("  DISPOSITIVO 1 | Emisor | Bus paralelo 8 bits"));
  Serial.println(F("  Comunicacion de Datos — v4.0.0"));
  Serial.println(F("======================================================="));
  Serial.println(F("  PINOUT v4 (REASIGNADO):"));
  Serial.println(F("    Bus datos : D0=2 D1=A2 D2=A3 D3=5 D4=6 D5=7 D6=8 D7=A0"));
  Serial.println(F("    *** D1 y D2 movidos a A2/A3 — sin interferencia PWM ***"));
  Serial.println(F("    Strobe    : A1  (STR)"));
  Serial.println(F("    BTN ON/OFF: pin 11  (INPUT_PULLUP)"));
  Serial.println(F("    BTN COLOR : pin 12  (INPUT_PULLUP)"));
  Serial.println(F("  PROTOCOLO:"));
  Serial.println(F("    5 tramas: T0(ctrl) T1(R) T2(G) T3(B) T4(CHK=XOR)"));
  Serial.print  (F("  TIMING v4.0.0: Setup="));
  Serial.print(SETUP_TIME_MS);
  Serial.print  (F("ms | STR_HIGH="));
  Serial.print(STROBE_HIGH_MS);
  Serial.print  (F("ms | Hold="));
  Serial.print(HOLD_TIME_MS);
  Serial.print  (F("ms | InterFrame="));
  Serial.print(INTER_FRAME_MS);
  Serial.println(F("ms"));
  Serial.println(F("  CORRECCIONES v4 (sobre v3):"));
  Serial.println(F("    [FIX-1] D1=A2, D2=A3: elimina interferencia Timer2/PWM."));
  Serial.println(F("    [FIX-2] Hold time: 5ms → 8ms."));
  Serial.println(F("    [FIX-3] InterFrame: 3ms → 5ms."));
  Serial.println(F("    [FIX-4] Verificacion de escritura con reintento."));
  Serial.println(F("======================================================="));
  Serial.println(F(""));

  configurarBus();
  pinMode(PIN_BTN_ONOFF,   INPUT_PULLUP);
  pinMode(PIN_BTN_CAMBIAR, INPUT_PULLUP);

  // Semilla aleatoria desde pin análogico flotante (ruido térmico)
  randomSeed(analogRead(A4)); // A4 no está conectado al bus, es seguro usarlo para ruido

  Serial.println(F("[INIT] Sistema listo. Presiona un boton para transmitir."));
  Serial.println(F("[INIT] Verificar que D2 tenga firmware v4 y misma asignacion de pines."));
  Serial.println(F(""));
}

// ===========================================================================
// LOOP PRINCIPAL
// ===========================================================================

void loop() {

  // --- Botón ON/OFF: alterna el estado del LED en D2 ---
  if (leerBoton(PIN_BTN_ONOFF, ultimoTimeBtnOnOff, estadoEstableOnOff)) {
    ledEncendido = !ledEncendido; // alternar estado
    Serial.print(F("[BTN] ON/OFF detectado | Nuevo estado: "));
    Serial.println(ledEncendido ? F("ENCENDIDO") : F("APAGADO"));
    enviarTramaOnOff(ledEncendido);
  }

  // --- Botón CAMBIAR: genera un color RGB aleatorio y lo envía ---
  if (leerBoton(PIN_BTN_CAMBIAR, ultimoTimeBtnCambiar, estadoEstableCambiar)) {
    colorR = (uint8_t)random(0, 256);
    colorG = (uint8_t)random(0, 256);
    colorB = (uint8_t)random(0, 256);
    Serial.print(F("[BTN] CAMBIAR detectado | Color RGB: R="));
    Serial.print(colorR);
    Serial.print(F("  G="));
    Serial.print(colorG);
    Serial.print(F("  B="));
    Serial.println(colorB);
    enviarTramaColor(colorR, colorG, colorB);
  }
}

// ===========================================================================
// FUNCIONES DE CONTROL DEL BUS
// ===========================================================================

/**
 * Configura todos los pines del bus como salidas y los inicializa en LOW.
 * También configura el pin de strobe como salida en LOW (idle).
 */
void configurarBus() {
  for (int i = 0; i < 8; i++) {
    pinMode(BUS_PINS[i], OUTPUT);
    digitalWrite(BUS_PINS[i], LOW);
  }
  pinMode(PIN_STR, OUTPUT);
  digitalWrite(PIN_STR, LOW);
  Serial.println(F("[INIT] Bus configurado: 8 pines OUTPUT, STR=LOW."));
}

/**
 * Escribe un byte en el bus de datos (LSB primero) con verificación de vuelta.
 *
 * Mecanismo de verificación (nuevo en v4):
 *   Después de llamar a digitalWrite() en cada pin, realiza una lectura de
 *   vuelta (digitalRead del mismo pin de salida). Si el valor leído no
 *   coincide con el esperado, reintenta la escritura hasta WRITE_RETRIES
 *   veces. Esto detecta y corrige glitches de escritura por capacitancia
 *   parásita en cables de protoboard/jumpers.
 *
 * @param  dato   Byte a escribir en el bus (bits 0–7 → pines D0–D7).
 * @return true   si la escritura fue verificada correctamente.
 *         false  si después de WRITE_RETRIES intentos el bus no es consistente.
 */
bool escribirByte(uint8_t dato) {
  for (int reintento = 0; reintento < WRITE_RETRIES; reintento++) {

    // Fase 1: escribir todos los bits
    for (int i = 0; i < 8; i++) {
      digitalWrite(BUS_PINS[i], (dato >> i) & 0x01); // dato >> i para obtener el bit i, & 0x01 para asegurar solo 0 o 1
    }

    // Fase 2: verificar lectura de vuelta (pequeño delay para carga RC)
    delayMicroseconds(50);
    bool ok = true;
    for (int i = 0; i < 8; i++) {
      const int bitEsperado = (dato >> i) & 0x01;
      const int bitLeido    = digitalRead(BUS_PINS[i]);
      if (bitEsperado != bitLeido) {
        ok = false;
        break;
      }
    }
    // Si la verificación es exitosa, retornar true. Si no, reintentar.
    if (ok) {
      if (reintento > 0) {
        contadorReintentos++;
        Serial.print(F("[WARN] Escritura verificada en reintento #"));
        Serial.print(reintento);
        Serial.print(F(" | Dato: "));
        imprimirByte(dato);
        Serial.print(F(" | Reintentos acumulados: "));
        Serial.println(contadorReintentos);
      }
      return true;
    }
  }

  // Si se llega a aquí, la escritura falló repetidamente (problema eléctrico grave)
  Serial.print(F("[ERROR] FALLO DE ESCRITURA en bus tras "));
  Serial.print(WRITE_RETRIES);
  Serial.print(F(" reintentos. Dato: "));
  imprimirByte(dato);
  Serial.println(F(" | Verificar cables y GND comun."));
  return false;
}

/**
 * Limpia el bus de datos (todos los pines a LOW) y asegura STR=LOW.
 * Se llama después de cada ciclo de strobe para liberar el canal.
 */
void limpiarBus() {
  for (int i = 0; i < 8; i++) {
    digitalWrite(BUS_PINS[i], LOW);
  }
  digitalWrite(PIN_STR, LOW);
}

/**
 * Genera el pulso de strobe sin ningún Serial.print() interno.
 *
 * Secuencia de timing (zona crítica — sin Serial durante toda la función):
 *   1. delay(SETUP_TIME_MS)  → bus estable, D2 ignora (STR=LOW)
 *   2. STR = HIGH            → D2 habilitado para leer
 *   3. delay(STROBE_HIGH_MS) → D2 ejecuta leerBus()
 *   4. STR = LOW             → fin de ventana de lectura
 *   5. delay(HOLD_TIME_MS)   → D2 termina de registrar el dato
 *
 * DISEÑO: ningún Serial.print() dentro de esta función para no introducir
 * jitter de UART en los GPIO durante la ventana crítica de lectura de D2.
 */
void activarStrobe() {
  delay(SETUP_TIME_MS); // estabilización del bus antes de subir STR
  digitalWrite(PIN_STR, HIGH); // STR sube: D2 debe leer el bus en este momento
  delay(STROBE_HIGH_MS); // ventana de lectura de D2 (D2 ejecuta leerBus())
  digitalWrite(PIN_STR, LOW); // STR baja: fin de ventana de lectura
  delay(HOLD_TIME_MS); // margen post-lectura para que D2 registre el dato internamente antes de limpiar el bus
}

/**
 * Transmite un array de bytes por el bus paralelo, uno por strobe.
 *
 * DISEÑO — Separación estricta de transmisión y logging:
 *   FASE 1 (crítica, sin Serial): ejecuta toda la transmisión hardware.
 *     Para cada byte: escribirByte → activarStrobe → limpiarBus → pausa.
 *   FASE 2 (logging): imprime el resumen DESPUÉS de que el bus quedó libre.
 *
 * @param bytes     Array de bytes a transmitir (T0..T4).
 * @param cantidad  Número de bytes en el array.
 */
void transmitirTrama(uint8_t* bytes, uint8_t cantidad) {

  // ==========================================================================
  // FASE 1 — TRANSMISIÓN HARDWARE (zona crítica: sin Serial.print aquí)
  // ==========================================================================
  for (uint8_t i = 0; i < cantidad; i++) {
    escribirByte(bytes[i]);   // Escritura + verificación de vuelta
    activarStrobe();          // Pulso STR: setup → HIGH → LOW → hold
    limpiarBus();             // Bus a 0x00, STR=LOW

    // Pausa inter-trama: da tiempo a D2 para procesar la trama anterior
    // (log + analogWrite del LED) antes de que el bus cambie.
    if (i < cantidad - 1) {
      delay(INTER_FRAME_MS);
    }
  }

  // ==========================================================================
  // FASE 2 — LOGGING (bus libre, sin riesgo de interferencia UART→GPIO)
  // ==========================================================================
  logTransmisionCompleta(bytes, cantidad);
}

// ===========================================================================
// FUNCIONES DEL PROTOCOLO
// ===========================================================================

/**
 * Calcula el checksum XOR de las cuatro tramas de datos.
 *
 * @param t0,t1,t2,t3  Bytes de datos de la transmisión.
 * @return             XOR de los cuatro bytes.
 */
uint8_t calcularChecksum(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3) {
  return t0 ^ t1 ^ t2 ^ t3;
}

/**
 * Construye y transmite una trama de comando ON/OFF.
 *
 * Estructura de T0:
 *   Bit 7 = 0 (CMD_ONOFF)
 *   Bit 0 = 1 si estado=true (ENCENDER), 0 si estado=false (APAGAR)
 *
 * @param estado  true = encender LED RGB en D2, false = apagar.
 */
void enviarTramaOnOff(bool estado) {
  const uint8_t t0  = CMD_ONOFF | (estado ? 0x01 : 0x00); // Bit 7=0, Bit 0=estado, Bits 6-1=0
  const uint8_t t1  = 0x00;
  const uint8_t t2  = 0x00;
  const uint8_t t3  = 0x00;
  const uint8_t chk = calcularChecksum(t0, t1, t2, t3);

  // Log pre-transmisión (descripción de lo que se va a enviar)
  logCabeceraTX("ON/OFF");
  char descT0[40]; // Descripción detallada de T0 con interpretación de bits
  snprintf(descT0, sizeof(descT0), "CMD=ON/OFF | STATE=%s",
           estado ? "ENCENDIDO(1)" : "APAGADO(0)"); 
  logFilaTrama(0, t0,  descT0);
  logFilaTrama(1, t1,  "Sin dato (ON/OFF no usa RGB)");
  logFilaTrama(2, t2,  "Sin dato (ON/OFF no usa RGB)");
  logFilaTrama(3, t3,  "Sin dato (ON/OFF no usa RGB)");
  logChecksum (t0, t1, t2, t3, chk);
  logFilaTrama(4, chk, "Checksum XOR(T0^T1^T2^T3)");
  Serial.println(F("  --- Iniciando transmision (logging suspendido) ---"));

  uint8_t trama[5] = { t0, t1, t2, t3, chk };
  transmitirTrama(trama, 5);

  logPieTX();
}

/**
 * Construye y transmite una trama de cambio de color RGB.
 *
 * Estructura de T0:
 *   Bit 7 = 1 (CMD_COLOR)
 *   Bits 6–0 = 0 (reservados)
 *
 * @param r  Intensidad del canal rojo   (0–255).
 * @param g  Intensidad del canal verde  (0–255).
 * @param b  Intensidad del canal azul   (0–255).
 */
void enviarTramaColor(uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t t0  = CMD_COLOR; // Bit 7=1, Bits 6-0=0
  const uint8_t chk = calcularChecksum(t0, r, g, b);

  // Log pre-transmisión
  logCabeceraTX("COLOR RGB");
  char descR[32], descG[32], descB[32]; // Descripciones detalladas de cada canal con su valor decimal
  snprintf(descR, sizeof(descR), "Canal Rojo   R = %3d", r);
  snprintf(descG, sizeof(descG), "Canal Verde  G = %3d", g);
  snprintf(descB, sizeof(descB), "Canal Azul   B = %3d", b);
  logFilaTrama(0, t0,  "CMD=COLOR (bit7=1)");
  logFilaTrama(1, r,   descR);
  logFilaTrama(2, g,   descG);
  logFilaTrama(3, b,   descB);
  logChecksum (t0, r, g, b, chk);
  logFilaTrama(4, chk, "Checksum XOR(T0^T1^T2^T3)");
  Serial.println(F("  --- Iniciando transmision (logging suspendido) ---"));

  uint8_t trama[5] = { t0, r, g, b, chk };
  transmitirTrama(trama, 5);

  logPieTX();
}

// ===========================================================================
// FUNCIONES DE BOTONES
// ===========================================================================

/**
 * Detecta un flanco descendente (LOW) validado por debounce en un botón
 * configurado como INPUT_PULLUP.
 *
 * Lógica:
 *   - La lectura del pin es HIGH en reposo y LOW cuando se presiona.
 *   - Solo devuelve true en el primer flanco de LOW después de DEBOUNCE_MS.
 *   - Una vez confirmado el flanco, actualiza el estado estable.
 *
 * @param pin           Pin Arduino configurado como INPUT_PULLUP.
 * @param ultimoTiempo  Referencia al timestamp de la última detección.
 * @param estadoEstable Referencia al estado registrado en la última lectura.
 * @return              true si se detectó presión validada, false si no.
 */
bool leerBoton(int pin, unsigned long &ultimoTiempo, bool &estadoEstable) {
  const bool lecturaActual = (bool)digitalRead(pin);
  bool disparoValidado = false;

  if ((millis() - ultimoTiempo) > DEBOUNCE_MS) {
    if (lecturaActual != estadoEstable) {
      ultimoTiempo  = millis();
      estadoEstable = lecturaActual;
      if (lecturaActual == LOW) {
        disparoValidado = true;
      }
    }
  }
  return disparoValidado;
}

// ===========================================================================
// FUNCIONES DE LOGGING
// ===========================================================================

/**
 * Imprime un byte en formato hexadecimal (siempre 2 dígitos, ej: 0x0A).
 * @param valor  Byte a imprimir.
 */
void imprimirByte(uint8_t valor) {
  Serial.print(F("0x"));
  if (valor < 0x10) Serial.print(F("0"));
  Serial.print(valor, HEX);
}

/**
 * Imprime un byte en formato binario MSB→LSB con separador de nibbles.
 * Ejemplo: 0xB2 → "1011 0010"
 * @param valor  Byte a imprimir.
 */
void imprimirBinario(uint8_t valor) {
  for (int b = 7; b >= 0; b--) {
    Serial.print((valor >> b) & 0x01); // bit b-ésimo, desplazado a LSB y enmascarado
    if (b == 4) Serial.print(F(" ")); // Separador entre nibbles
  }
}

/**
 * Imprime la cabecera de una transmisión con número y tipo de trama.
 * @param tipo  Descripción del tipo ("ON/OFF" o "COLOR RGB").
 */
void logCabeceraTX(const char* tipo) {
  contadorTX++;
  Serial.println(F(""));
  Serial.println(F("-------------------------------------------------------"));
  Serial.print  (F("  TX #")); Serial.print(contadorTX);
  Serial.print  (F(" | Tipo: ")); Serial.println(tipo);
  Serial.println(F("  Trama | Hex   | Dec | Binario   | Descripcion"));
  Serial.println(F("  ------|-------|-----|-----------|-------------------"));
}

/**
 * Imprime una fila de la tabla de trama (una trama individual).
 * @param idx    Índice de la trama (0–4).
 * @param valor  Valor del byte.
 * @param desc   Descripción del campo.
 */
void logFilaTrama(uint8_t idx, uint8_t valor, const char* desc) {
  Serial.print(F("  T")); Serial.print(idx);
  Serial.print(F("    | ")); imprimirByte(valor);
  Serial.print(F(" | "));
  if (valor < 100) Serial.print(F(" "));
  if (valor < 10)  Serial.print(F(" "));
  Serial.print(valor);
  Serial.print(F(" | "));
  imprimirBinario(valor);
  Serial.print(F(" | ")); Serial.println(desc);
}

/**
 * Imprime el cálculo completo del checksum XOR con representación bit a bit.
 * @param t0..t3  Bytes de datos.
 * @param chk     Resultado del XOR calculado.
 */
void logChecksum(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3, uint8_t chk) {
  Serial.print  (F("  [CHK] XOR: "));
  imprimirByte(t0); Serial.print(F(" ^ "));
  imprimirByte(t1); Serial.print(F(" ^ "));
  imprimirByte(t2); Serial.print(F(" ^ "));
  imprimirByte(t3); Serial.print(F(" = "));
  imprimirByte(chk); Serial.println(F(""));

  Serial.println(F("         Bit a bit (MSB a LSB):"));
  const uint8_t vals[5] = { t0, t1, t2, t3, chk };
  const char* etiq[5]   = { "T0 ", "T1 ", "T2 ", "T3 ", "CHK" };
  for (int v = 0; v < 5; v++) {
    Serial.print(F("         ")); Serial.print(etiq[v]); Serial.print(F("= "));
    imprimirBinario(vals[v]);
    Serial.println(F(""));
  }
}

/**
 * Loguea la confirmación de cada trama DESPUÉS de completada la transmisión.
 * Se imprime cuando el bus ya está en 0x00 y STR en LOW.
 *
 * @param bytes     Array de bytes transmitidos (T0..T4).
 * @param cantidad  Número de bytes transmitidos.
 */
void logTransmisionCompleta(uint8_t* bytes, uint8_t cantidad) {
  Serial.println(F("  --- Tramas enviadas (log post-transmision) ---"));
  Serial.print  (F("  Timing: Setup="));  Serial.print(SETUP_TIME_MS);
  Serial.print  (F("ms | STR="));         Serial.print(STROBE_HIGH_MS);
  Serial.print  (F("ms | Hold="));        Serial.print(HOLD_TIME_MS);
  Serial.print  (F("ms | Gap="));         Serial.print(INTER_FRAME_MS);
  Serial.println(F("ms"));

  for (uint8_t i = 0; i < cantidad; i++) {
    Serial.print(F("      [T")); Serial.print(i);
    Serial.print(F("] Valor enviado: ")); imprimirByte(bytes[i]);
    Serial.print(F(" | ")); imprimirBinario(bytes[i]);
    if (i == cantidad - 1) {
      Serial.println(F(" | (ultima trama — CHK)"));
    } else {
      Serial.println(F(""));
    }
  }

  if (contadorReintentos > 0) {
    Serial.print(F("  [WARN] Reintentos de escritura acumulados en sesion: "));
    Serial.println(contadorReintentos);
  }
}

/**
 * Imprime el pie del log de transmisión indicando que el canal quedó libre.
 */
void logPieTX() {
  Serial.println(F("  --- Transmision completada ---"));
  Serial.println(F("  Bus = 0x00 | STR = LOW | Canal libre"));
  Serial.println(F("-------------------------------------------------------"));
  Serial.println(F(""));
}

/**
 * Imprime un resumen de la sesión de transmisión.
 * Llamar manualmente si se necesita en el monitor serial.
 */
void logResumenSesion() {
  Serial.println(F(""));
  Serial.println(F("======= RESUMEN DE SESION D1 ======="));
  Serial.print  (F("  Transmisiones realizadas: ")); Serial.println(contadorTX);
  Serial.print  (F("  Reintentos de escritura : ")); Serial.println(contadorReintentos);
  Serial.println(F("====================================="));
}
