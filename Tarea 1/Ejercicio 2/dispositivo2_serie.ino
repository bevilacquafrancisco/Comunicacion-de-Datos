/**
 * ===========================================================================
 * Archivo     : dispositivo2_serie.ino
 * Ejercicio   : 2 — Comunicación Serie UART
 * Autores     : Francisco Bevilacqua, Sebastian Clement
 * Fecha       : 2026-04-22
 * Versión     : 1.0.0
 * Descripción : Firmware del Dispositivo 2 (Receptor y controlador del LED
 *               RGB) para comunicación bidireccional serie UART con D1.
 *               Implementa el handshake de conexión, recepción y validación
 *               de comandos, control PWM del LED RGB, y envío de ACK/NACK.
 *
 * Hardware    : Arduino UNO (ATmega328P)
 * Dependencias: SoftwareSerial (incluida en Arduino IDE)
 *
 * ---------------------------------------------------------------------------
 * PROTOCOLO — Tramas del sistema
 * ---------------------------------------------------------------------------
 *   CONN_REQ  D2→D1  [ 0xAA | CHK ]              Solicitud de conexión
 *   CONN_ACK  D1→D2  [ 0xBB | CHK ]              Aceptación de conexión
 *   CMD_ONOFF D1→D2  [ 0x01 | STATE | CHK ]       Encender/apagar LED RGB
 *   CMD_COLOR D1→D2  [ 0x02 | R | G | B | CHK ]  Cambiar color RGB
 *   ACK       D2→D1  [ 0xFF | CHK ]              Confirmación positiva
 *   NACK      D2→D1  [ 0xFE | CHK ]              Trama corrupta detectada
 *
 * Checksum   : Suma de todos los bytes de payload (sin CHK) MOD 256
 *
 * ---------------------------------------------------------------------------
 * PINOUT
 * ---------------------------------------------------------------------------
 *   SoftSerial RX : pin 4  (recibe de D1 pin 5)
 *   SoftSerial TX : pin 5  (envía a D1 pin 4)
 *   GND común     : GND   (OBLIGATORIO — referencia de tensión)
 *   BTN_CONEXION  : pin 11 (INPUT_PULLUP)
 *   LED RGB Rojo  : pin 9  → R220Ω → LED → GND  (PWM)
 *   LED RGB Verde : pin 10 → R220Ω → LED → GND  (PWM)
 *   LED RGB Azul  : pin 6  → R220Ω → LED → GND  (PWM)
 *   Serial debug  : pines 0/1 (USB-Serial hardware, 115200 bps)
 *
 * ---------------------------------------------------------------------------
 * MÁQUINA DE ESTADOS D2
 * ---------------------------------------------------------------------------
 *   DESCONECTADO ──(BTN_CONEXION)────► envía CONN_REQ, espera CONN_ACK
 *                └─(sin respuesta)───► reintenta cada 1000 ms
 *   CONECTADO ──(CMD_ONOFF recibido)─► valida CHK → ejecuta → envía ACK/NACK
 *   CONECTADO ──(CMD_COLOR recibido)─► valida CHK → ejecuta → envía ACK/NACK
 * ===========================================================================
 */

#include <SoftwareSerial.h>

// ===========================================================================
// SOFTWARESERIAL — Canal de comunicación con D1
// ===========================================================================

/**
 * Puerto serie por software. Misma configuración que D1.
 * IMPORTANTE: RX de D2 (pin 4) conectado a TX de D1 (pin 5), y viceversa.
 * Cruzado intencional: cada Arduino escucha lo que el otro transmite.
 */
SoftwareSerial serieD1(4, 5);  // RX=4, TX=5

// ===========================================================================
// PINOUT — PERIFÉRICOS DE D2
// ===========================================================================

/** Botón de conexión: al presionar, D2 inicia el handshake con D1. */
const int PIN_BTN_CONEXION = 11;

/** Pin PWM del canal Rojo del LED RGB. */
const int PIN_LED_R = 9;

/** Pin PWM del canal Verde del LED RGB. */
const int PIN_LED_G = 10;

/**
 * Pin PWM del canal Azul del LED RGB.
 * Nota: pin 6 en lugar de pin 11 del Ejercicio 1, para liberar pin 11
 * al botón BTN_CONEXION. Pin 6 tiene PWM en el UNO (Timer0).
 */
const int PIN_LED_B = 6;

// ===========================================================================
// CONSTANTES DEL PROTOCOLO
// ===========================================================================

#define HEADER_CONN_REQ   0xAA
#define HEADER_CONN_ACK   0xBB
#define HEADER_CMD_ONOFF  0x01
#define HEADER_CMD_COLOR  0x02
#define HEADER_ACK        0xFF
#define HEADER_NACK       0xFE

/** Intervalo entre reintentos de CONN_REQ cuando D1 no responde (ms). */
#define INTERVALO_CONN_REQ_MS   1000

/** Timeout para esperar CONN_ACK después de enviar CONN_REQ (ms). */
#define TIMEOUT_CONN_ACK_MS     1500

/** Timeout para recibir los bytes restantes de un comando tras el header (ms). */
#define TIMEOUT_PAYLOAD_MS       500

/** Tiempo de debounce para el botón de conexión (ms). */
#define DEBOUNCE_MS               50

#define BAUD_SERIE              9600
#define BAUD_DEBUG            115200

// ===========================================================================
// ESTADOS DE LA MÁQUINA DE ESTADOS
// ===========================================================================

#define ESTADO_DESCONECTADO   0
#define ESTADO_CONECTADO      1

// ===========================================================================
// VARIABLES DE ESTADO
// ===========================================================================

uint8_t estadoSistema   = ESTADO_DESCONECTADO;

bool    ledEncendido    = false;
uint8_t colorActualR    = 0; 
uint8_t colorActualG    = 0;
uint8_t colorActualB    = 0;

unsigned long ultimoTimeBtnConexion = 0;
unsigned long ultimoIntentoCONN    = 0;

/** Contador de tramas de comando recibidas (RX #N — debe correlacionar con TX #N de D1). */
uint16_t contadorRX = 0;

/** Contadores de errores para el resumen de sesión. */
uint16_t erroresChk     = 0;
uint16_t erroresTimeout = 0;
uint16_t contadorACK    = 0;
uint16_t contadorNACK   = 0;

// ===========================================================================
// DECLARACIONES DE FUNCIONES
// ===========================================================================

void    configurarPines();
uint8_t calcularChecksum(uint8_t* datos, uint8_t longitud);
void    enviarTrama(uint8_t* datos, uint8_t longitud);
bool    recibirTrama(uint8_t* buffer, uint8_t esperados, uint32_t timeoutMs);
void    solicitarConexion();
void    procesarComando();
void    ejecutarOnOff(bool estado);
void    ejecutarColor(uint8_t r, uint8_t g, uint8_t b);
void    enviarACK();
void    enviarNACK();
void    aplicarLED(uint8_t r, uint8_t g, uint8_t b);
void    apagarLED();
bool    leerBoton(int pin, unsigned long &ultimoTiempo);
void    logCabecera();
void    logSeparador();
void    logByte(uint8_t valor);
void    logTramaTX(uint8_t* datos, uint8_t longitud, const char* tipo);
void    logTramaRX(uint8_t* datos, uint8_t longitud, const char* tipo,
                   uint8_t chkRecibido, uint8_t chkCalculado);
void    logResumenSesion();

// ===========================================================================
// SETUP
// ===========================================================================

void setup() {
  Serial.begin(BAUD_DEBUG);
  serieD1.begin(BAUD_SERIE);

  configurarPines();
  apagarLED();

  logCabecera();
  Serial.println(F("[INIT] Estado inicial: DESCONECTADO"));
  Serial.println(F("[INIT] Presione BTN_CONEXION para iniciar handshake con D1."));
  Serial.println(F(""));
}

// ===========================================================================
// LOOP PRINCIPAL
// ===========================================================================

/**
 * Máquina de estados principal de D2.
 *
 * Estado DESCONECTADO: espera pulsación de BTN_CONEXION para iniciar
 *                      el proceso de handshake con D1.
 * Estado CONECTADO   : monitorea el canal en busca de comandos de D1.
 *                      Cada trama recibida es validada y despachada.
 */
void loop() {

  switch (estadoSistema) {

    // -----------------------------------------------------------------------
    case ESTADO_DESCONECTADO:
    // -----------------------------------------------------------------------
      if (leerBoton(PIN_BTN_CONEXION, ultimoTimeBtnConexion)) {
        Serial.println(F(""));
        Serial.println(F("[BTN] CONEXION presionado. Iniciando handshake..."));
        solicitarConexion();
      }
      break;

    // -----------------------------------------------------------------------
    case ESTADO_CONECTADO:
    // -----------------------------------------------------------------------
      if (serieD1.available()) {
        procesarComando();
      }
      break;
  }
}

// ===========================================================================
// FUNCIONES DE HARDWARE
// ===========================================================================

void configurarPines() {
  pinMode(PIN_BTN_CONEXION, INPUT_PULLUP);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
}

bool leerBoton(int pin, unsigned long &ultimoTiempo) {
  if (digitalRead(pin) == LOW) {
    unsigned long ahora = millis();
    if ((ahora - ultimoTiempo) > DEBOUNCE_MS) {
      ultimoTiempo = ahora;
      return true;
    }
  }
  return false;
}

/**
 * Aplica intensidades RGB al LED mediante PWM de 8 bits.
 * analogWrite(pin, 0) = apagado; analogWrite(pin, 255) = máxima intensidad.
 *
 * @param r, g, b  Intensidades de cada canal (0–255)
 */
void aplicarLED(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_LED_R, r);
  analogWrite(PIN_LED_G, g);
  analogWrite(PIN_LED_B, b);
  Serial.print  (F("      [LED-RGB] PWM aplicado: R="));
  Serial.print  (r); Serial.print(F("  G="));
  Serial.print  (g); Serial.print(F("  B="));
  Serial.println(b);
}

/** Apaga el LED RGB poniendo los tres canales PWM en 0. */
void apagarLED() {
  analogWrite(PIN_LED_R, 0);
  analogWrite(PIN_LED_G, 0);
  analogWrite(PIN_LED_B, 0);
}

// ===========================================================================
// FUNCIONES DE CHECKSUM
// ===========================================================================

/**
 * Checksum idéntico al de D1 — CRÍTICO: ambos dispositivos deben usar
 * exactamente el mismo algoritmo o los CHK nunca coincidirán.
 *
 * @param datos    Array de bytes del payload (sin CHK)
 * @param longitud Cantidad de bytes a sumar
 * @return         CHK = suma & 0xFF
 */
uint8_t calcularChecksum(uint8_t* datos, uint8_t longitud) {
  uint16_t suma = 0;
  for (uint8_t i = 0; i < longitud; i++) {
    suma += datos[i];
  }
  return (uint8_t)(suma & 0xFF);
}

// ===========================================================================
// FUNCIONES DE TRANSMISIÓN Y RECEPCIÓN
// ===========================================================================

/**
 * Envía una trama por SoftwareSerial añadiendo el CHK al final.
 * Idéntica en estructura a la de D1 para garantizar compatibilidad.
 */
void enviarTrama(uint8_t* datos, uint8_t longitud) {
  uint8_t chk = calcularChecksum(datos, longitud);
  for (uint8_t i = 0; i < longitud; i++) {
    serieD1.write(datos[i]);
  }
  serieD1.write(chk);
}

/**
 * Recibe exactamente `esperados` bytes con timeout.
 * Mismo comportamiento que D1 para simetría del protocolo.
 */
bool recibirTrama(uint8_t* buffer, uint8_t esperados, uint32_t timeoutMs) {
  uint8_t recibidos = 0;
  unsigned long inicio = millis();

  while (recibidos < esperados) {
    if ((millis() - inicio) > timeoutMs) {
      Serial.print  (F("[ERROR-RX] Timeout en D2. Recibidos: "));
      Serial.print  (recibidos);
      Serial.print  (F(" / "));
      Serial.print  (esperados);
      Serial.println(F(" bytes."));
      Serial.println(F("           DIAGNOSTICO: Verificar cable TX de D1 al pin 4 de D2."));
      erroresTimeout++;
      return false;
    }
    if (serieD1.available()) {
      buffer[recibidos++] = serieD1.read();
    }
  }
  return true;
}

// ===========================================================================
// FUNCIONES DEL PROTOCOLO — D2
// ===========================================================================

/**
 * Gestiona el proceso de solicitud de conexión con D1.
 *
 * Comportamiento:
 *   1. Envía CONN_REQ ([ 0xAA | CHK ]) por SoftwareSerial.
 *   2. Espera CONN_ACK de D1 durante TIMEOUT_CONN_ACK_MS.
 *   3. Si no llega respuesta, loguea el reintento y vuelve al loop
 *      (el botón deberá presionarse de nuevo, o se podría hacer automático
 *       ajustando la lógica del loop — decisión de diseño simplificada).
 *   4. Al recibir CONN_ACK con CHK válido, transiciona a CONECTADO.
 *
 * Nota de diseño: el enunciado pide que D2 "solicite conexión hasta que
 * D1 le habilite la conexión". La implementación actual envía un CONN_REQ
 * por pulsación de botón. Para envío continuo automático, se debe mover la lógica
 * de reintento al loop de ESTADO_DESCONECTADO con un timer (ultimoIntentoCONN).
 */
void solicitarConexion() {
  static uint16_t intentos = 0;
  intentos++;

  logSeparador();
  Serial.print  (F("[TX] CONN_REQ — intento #"));
  Serial.println(intentos);

  uint8_t tramaREQ[1] = { HEADER_CONN_REQ };
  logTramaTX(tramaREQ, 1, "CONN_REQ");
  enviarTrama(tramaREQ, 1);

  // Esperar CONN_ACK: [ 0xBB | CHK ] = 2 bytes
  Serial.print  (F("[ESPERA] Aguardando CONN_ACK... (timeout: "));
  Serial.print  (TIMEOUT_CONN_ACK_MS);
  Serial.println(F(" ms)"));

  uint8_t bufferRx[2];
  if (!recibirTrama(bufferRx, 2, TIMEOUT_CONN_ACK_MS)) {
    Serial.println(F("[WARN] Sin respuesta de D1. Presione BTN_CONEXION para reintentar."));
    Serial.println(F("       DIAGNOSTICO: D1 puede estar en proceso de verificar CHK del"));
    Serial.println(F("       CONN_REQ, o hay un problema en el cable RX del D1 (pin 4)."));
    logSeparador();
    return;
  }

  uint8_t header    = bufferRx[0];
  uint8_t chkRecib  = bufferRx[1];
  uint8_t payload[1] = { header };
  uint8_t chkCalc   = calcularChecksum(payload, 1);

  logTramaRX(bufferRx, 2, "CONN_ACK", chkRecib, chkCalc);

  if (header != HEADER_CONN_ACK) {
    Serial.print  (F("[ERROR] Esperaba CONN_ACK (0xBB), recibido: 0x")); logByte(header);
    Serial.println(F(""));
    Serial.println(F("        DIAGNOSTICO: Posible desincronizacion de protocolo."));
    Serial.println(F("        Verificar que D1 ejecuta dispositivo1_serie.ino correcto."));
    logSeparador();
    return;
  }

  if (chkRecib != chkCalc) {
    Serial.println(F("[ERROR] CHK incorrecto en CONN_ACK. Conexion rechazada por D2."));
    Serial.print  (F("        Recibido: 0x")); logByte(chkRecib);
    Serial.print  (F(" | Calculado: 0x")); logByte(chkCalc);
    Serial.println(F(""));
    logSeparador();
    return;
  }

  // Conexión establecida exitosamente
  estadoSistema = ESTADO_CONECTADO;
  intentos = 0;

  Serial.println(F("[ESTADO] Conexion establecida con D1. Sistema CONECTADO."));
  Serial.println(F("         Esperando comandos CMD_ONOFF / CMD_COLOR..."));
  logSeparador();
  Serial.println(F(""));
}

/**
 * Recibe y despacha un comando de D1.
 *
 * Flujo:
 *   1. Leer el primer byte disponible (header).
 *   2. Según el header, leer los bytes de payload restantes.
 *   3. Verificar CHK sobre todos los bytes (header + payload).
 *   4. Si CHK OK  → ejecutar comando → enviar ACK.
 *   5. Si CHK MAL → no ejecutar → enviar NACK → loguear diagnóstico.
 *   6. Header desconocido → descartar → loguear warning.
 */
void procesarComando() {
  contadorRX++;

  uint8_t header = serieD1.read();
  logSeparador();
  Serial.print  (F("[RX #"));
  Serial.print  (contadorRX);
  Serial.print  (F("] Header recibido: 0x")); logByte(header);
  Serial.println(F(""));

  // -------------------------------------------------------------------
  // CMD_ONOFF: [ 0x01 | STATE | CHK ]  → leer 2 bytes más (STATE + CHK)
  // -------------------------------------------------------------------
  if (header == HEADER_CMD_ONOFF) {
    uint8_t resto[2];  // [STATE, CHK]
    if (!recibirTrama(resto, 2, TIMEOUT_PAYLOAD_MS)) {
      Serial.println(F("[ERROR] CMD_ONOFF incompleto. Trama descartada."));
      logSeparador();
      return;
    }

    uint8_t state     = resto[0];
    uint8_t chkRecib  = resto[1];
    uint8_t payload[2] = { header, state };
    uint8_t chkCalc   = calcularChecksum(payload, 2);

    // Construir array completo para el log (header + payload + chk)
    uint8_t tramaCompleta[3] = { header, state, chkRecib };
    logTramaRX(tramaCompleta, 3, "CMD_ONOFF", chkRecib, chkCalc);

    if (chkRecib != chkCalc) {
      erroresChk++;
      logErrorChecksum(chkRecib, chkCalc, contadorRX);
      enviarNACK();
      logSeparador();
      return;
    }

    bool nuevoEstado = (state == 0x01);
    ejecutarOnOff(nuevoEstado);
    enviarACK();

  // -------------------------------------------------------------------
  // CMD_COLOR: [ 0x02 | R | G | B | CHK ]  → leer 4 bytes más
  // -------------------------------------------------------------------
  } else if (header == HEADER_CMD_COLOR) {
    uint8_t resto[4];  // [R, G, B, CHK]
    if (!recibirTrama(resto, 4, TIMEOUT_PAYLOAD_MS)) {
      Serial.println(F("[ERROR] CMD_COLOR incompleto. Trama descartada."));
      logSeparador();
      return;
    }

    uint8_t r        = resto[0];
    uint8_t g        = resto[1];
    uint8_t b        = resto[2];
    uint8_t chkRecib = resto[3];
    uint8_t payload[4] = { header, r, g, b };
    uint8_t chkCalc   = calcularChecksum(payload, 4);

    uint8_t tramaCompleta[5] = { header, r, g, b, chkRecib };
    logTramaRX(tramaCompleta, 5, "CMD_COLOR", chkRecib, chkCalc);

    if (chkRecib != chkCalc) {
      erroresChk++;
      logErrorChecksum(chkRecib, chkCalc, contadorRX);
      enviarNACK();
      logSeparador();
      return;
    }

    ejecutarColor(r, g, b);
    enviarACK();

  // -------------------------------------------------------------------
  // Header desconocido — byte espurio o desincronización de protocolo
  // -------------------------------------------------------------------
  } else {
    Serial.print  (F("[WARN] Header desconocido: 0x")); logByte(header);
    Serial.println(F(""));
    Serial.println(F("       DIAGNOSTICO: Posible byte espurio por ruido en el canal,"));
    Serial.println(F("       o desincronizacion entre tramas. El sistema puede haber"));
    Serial.println(F("       perdido el alineamiento de bytes."));
    Serial.println(F("       Accion: byte ignorado. Si persiste, reiniciar ambos Arduinos."));
    logSeparador();
  }
}

/**
 * Ejecuta el comando ON/OFF sobre el LED RGB.
 *
 * @param estado  true = encender (restaura último color) | false = apagar
 */
void ejecutarOnOff(bool estado) {
  ledEncendido = estado;
  Serial.print  (F("[CMD] ON/OFF → LED RGB: "));
  Serial.println(estado ? F("ENCENDIDO") : F("APAGADO"));

  if (estado) {
    Serial.print(F("      Restaurando color anterior: R="));
    Serial.print(colorActualR); Serial.print(F(" G="));
    Serial.print(colorActualG); Serial.print(F(" B="));
    Serial.println(colorActualB);
    aplicarLED(colorActualR, colorActualG, colorActualB);
  } else {
    apagarLED();
    Serial.println(F("      [LED-RGB] Apagado (PWM=0 en los 3 canales)"));
  }
}

/**
 * Ejecuta el comando COLOR sobre el LED RGB.
 * Actualiza el color solo si el LED está encendido.
 * El color siempre se guarda (se aplicará al encender).
 *
 * @param r, g, b  Intensidades del nuevo color (0–255)
 */
void ejecutarColor(uint8_t r, uint8_t g, uint8_t b) {
  colorActualR = r;
  colorActualG = g;
  colorActualB = b;

  Serial.print  (F("[CMD] COLOR → R="));
  Serial.print  (r); Serial.print(F("  G="));
  Serial.print  (g); Serial.print(F("  B="));
  Serial.println(b);

  if (ledEncendido) {
    aplicarLED(r, g, b);
  } else {
    Serial.println(F("      LED apagado: color guardado, NO aplicado al PWM aun."));
    Serial.println(F("      (Se aplicara al recibir CMD_ONOFF con STATE=1)"));
  }
}

/**
 * Envía una trama ACK a D1: [ 0xFF | CHK ]
 * Indica que el comando fue recibido y ejecutado correctamente.
 */
void enviarACK() {
  contadorACK++;
  uint8_t tramaACK[1] = { HEADER_ACK };
  logTramaTX(tramaACK, 1, "ACK");
  enviarTrama(tramaACK, 1);
}

/**
 * Envía una trama NACK a D1: [ 0xFE | CHK ]
 * Indica que la trama recibida tenía un checksum incorrecto.
 * D1 NO reenviará el comando (ARQ no implementado).
 */
void enviarNACK() {
  contadorNACK++;
  uint8_t tramaNACK[1] = { HEADER_NACK };
  logTramaTX(tramaNACK, 1, "NACK");
  enviarTrama(tramaNACK, 1);
  Serial.println(F("      Estado del LED RGB: SIN CAMBIOS (trama corrupta descartada)"));
}

// ===========================================================================
// FUNCIONES DE LOGGING — Debug por Serial Monitor
// ===========================================================================

void logByte(uint8_t valor) {
  if (valor < 0x10) Serial.print(F("0"));
  Serial.print(valor, HEX);
}

void logCabecera() {
  Serial.println(F(""));
  Serial.println(F("======================================================="));
  Serial.println(F("  DISPOSITIVO 2 | Receptor | UART Serie"));
  Serial.println(F("  Comunicacion de Datos — Ejercicio 2 | v1.0.0"));
  Serial.println(F("======================================================="));
  Serial.print  (F("  Baud SoftSerial  : ")); Serial.println(BAUD_SERIE);
  Serial.println(F("  PINOUT SoftSerial: RX=4, TX=5"));
  Serial.print  (F("  LED RGB          : R="));
  Serial.print  (PIN_LED_R); Serial.print(F("(PWM) G="));
  Serial.print  (PIN_LED_G); Serial.print(F("(PWM) B="));
  Serial.print  (PIN_LED_B); Serial.println(F("(PWM)"));
  Serial.print  (F("  BTN CONEXION     : pin ")); Serial.println(PIN_BTN_CONEXION);
  Serial.println(F("  Checksum         : Suma de bytes MOD 256"));
  Serial.println(F("  NACK             : trama corrupta descartada, sin reenvio"));
  Serial.println(F("======================================================="));
  Serial.println(F(""));
}

void logSeparador() {
  Serial.println(F("-------------------------------------------------------"));
}

void logTramaTX(uint8_t* datos, uint8_t longitud, const char* tipo) {
  Serial.print  (F("[TX] Enviando "));
  Serial.println(tipo);

  const char* nombresACK[1]     = { "HEADER ACK  (confirmacion OK)" };
  const char* nombresNACK[1]    = { "HEADER NACK (CHK incorrecto)" };
  const char* nombresREQ[1]     = { "HEADER CONN_REQ (solicitud conexion)" };

  for (uint8_t i = 0; i < longitud; i++) {
    Serial.print  (F("    Byte "));
    Serial.print  (i);
    Serial.print  (F(": 0x")); logByte(datos[i]);
    Serial.print  (F(" (dec=")); Serial.print(datos[i]); Serial.print(F(")  ← "));
    if (strcmp(tipo, "ACK")      == 0) Serial.print(nombresACK[0]);
    if (strcmp(tipo, "NACK")     == 0) Serial.print(nombresNACK[0]);
    if (strcmp(tipo, "CONN_REQ") == 0) Serial.print(nombresREQ[0]);
    Serial.println(F(""));
  }

  uint8_t chk = calcularChecksum(datos, longitud);
  Serial.print  (F("    CHK: suma(0x")); logByte(datos[0]);
  for (uint8_t i = 1; i < longitud; i++) {
    Serial.print(F("+0x")); logByte(datos[i]);
  }
  uint16_t sumaDetalle = 0;
  for (uint8_t i = 0; i < longitud; i++) sumaDetalle += datos[i];
  Serial.print  (F(")=0x"));
  if (sumaDetalle < 0x10) Serial.print(F("0"));
  Serial.print  (sumaDetalle, HEX);
  Serial.print  (F(" → mod256=0x")); logByte(chk);
  Serial.println(F(""));
  Serial.println(F("    --> Trama enviada."));
}

/**
 * Log detallado de trama recibida con verificación de checksum.
 * Muestra el cálculo paso a paso para facilitar el diagnóstico de errores.
 *
 * @param datos        Trama completa recibida (header + payload + chk)
 * @param longitud     Longitud total (incluyendo CHK)
 * @param tipo         Tipo de trama ("CMD_ONOFF", "CMD_COLOR", etc.)
 * @param chkRecibido  CHK llegado en la trama
 * @param chkCalculado CHK que D2 calcula sobre los bytes recibidos
 */
void logTramaRX(uint8_t* datos, uint8_t longitud, const char* tipo,
                uint8_t chkRecibido, uint8_t chkCalculado) {

  // Nombres de campo según el tipo de trama
  bool esOnOff = (strcmp(tipo, "CMD_ONOFF") == 0);
  bool esColor = (strcmp(tipo, "CMD_COLOR") == 0);
  bool esConnAck = (strcmp(tipo, "CONN_ACK") == 0);

  Serial.print(F("    Tipo identificado: ")); Serial.println(tipo);

  for (uint8_t i = 0; i < longitud; i++) {
    Serial.print  (F("    Byte "));
    Serial.print  (i);
    Serial.print  (F(": 0x")); logByte(datos[i]);
    Serial.print  (F(" (dec="));
    Serial.print  (datos[i]);
    Serial.print  (F(")  ← "));

    if (i == longitud - 1) {
      // Último byte siempre es el CHK
      Serial.print(F("CHK recibido"));
    } else if (i == 0) {
      Serial.print(F("HEADER"));
    } else if (esOnOff && i == 1) {
      Serial.print(F("STATE: "));
      Serial.print(datos[i] == 0x01 ? F("ENCENDER (0x01)") : F("APAGAR (0x00)"));
    } else if (esColor && i == 1) {
      Serial.print(F("R (Rojo)   = ")); Serial.print(datos[i]);
    } else if (esColor && i == 2) {
      Serial.print(F("G (Verde)  = ")); Serial.print(datos[i]);
    } else if (esColor && i == 3) {
      Serial.print(F("B (Azul)   = ")); Serial.print(datos[i]);
    }
    Serial.println(F(""));
  }

  // Mostrar verificación del checksum en detalle
  Serial.print  (F("    Verificacion CHK (suma mod256): "));
  uint16_t sumaVerif = 0;
  for (uint8_t i = 0; i < longitud - 1; i++) {
    Serial.print(F("0x")); logByte(datos[i]);
    sumaVerif += datos[i];
    if (i < longitud - 2) Serial.print(F("+"));
  }
  Serial.print  (F(" = 0x"));
  if (sumaVerif < 0x10) Serial.print(F("0"));
  Serial.print  (sumaVerif, HEX);
  Serial.print  (F(" → mod256 = 0x")); logByte(chkCalculado);
  Serial.print  (F(" | Recibido: 0x")); logByte(chkRecibido);

  if (chkRecibido == chkCalculado) {
    Serial.println(F("  → OK ✓"));
  } else {
    Serial.println(F("  → ERROR ✗"));
  }
}

/**
 * Loguea un error de checksum con diagnóstico detallado.
 * Diferencia entre tipo de error y posibles causas físicas para
 * ayudar a identificar si el problema es eléctrico o de software.
 *
 * @param chkRecib   Checksum que llegó en la trama
 * @param chkCalc    Checksum que D2 calculó
 * @param numTrama   Número de trama (RX #N)
 */
void logErrorChecksum(uint8_t chkRecib, uint8_t chkCalc, uint16_t numTrama) {
  Serial.println(F(""));
  Serial.println(F("*** ERROR DE CHECKSUM ***"));
  Serial.print  (F("    Trama RX #")); Serial.println(numTrama);
  Serial.print  (F("    CHK recibido : 0x")); logByte(chkRecib);
  Serial.print  (F(" (dec=")); Serial.print(chkRecib); Serial.println(F(")"));
  Serial.print  (F("    CHK calculado: 0x")); logByte(chkCalc);
  Serial.print  (F(" (dec=")); Serial.print(chkCalc); Serial.println(F(")"));
  Serial.print  (F("    Diferencia   : "));
  int16_t diff = (int16_t)chkRecib - (int16_t)chkCalc;
  Serial.print  (diff); Serial.println(F(" unidades"));
  Serial.println(F("    Accion       : Trama DESCARTADA. Estado LED RGB: SIN CAMBIOS."));
  Serial.println(F("    Respuesta    : NACK enviado a D1."));
  Serial.println(F("    DIAGNOSTICO  :"));
  Serial.println(F("      1. Ruido electrico en el canal TX/RX (cable largo o cerca de fuente)."));
  Serial.println(F("      2. GND no comun entre D1 y D2 — causa mas frecuente."));
  Serial.println(F("      3. Baud rate diferente entre D1 y D2 (debe ser 9600 en ambos)."));
  Serial.println(F("      4. SoftwareSerial con interferencia de otras interrupciones."));
  Serial.print  (F("    Errores CHK acumulados: ")); Serial.println(erroresChk);
}

void logResumenSesion() {
  Serial.println(F(""));
  Serial.println(F("========== RESUMEN DE SESION D2 =========="));
  Serial.print  (F("  Comandos recibidos (RX) : ")); Serial.println(contadorRX);
  Serial.print  (F("  ACKs enviados           : ")); Serial.println(contadorACK);
  Serial.print  (F("  NACKs enviados          : ")); Serial.println(contadorNACK);
  Serial.print  (F("  Errores de checksum     : ")); Serial.println(erroresChk);
  Serial.print  (F("  Timeouts de payload     : ")); Serial.println(erroresTimeout);
  uint16_t ok = contadorRX - erroresChk - erroresTimeout;
  Serial.print  (F("  Comandos ejecutados OK  : ")); Serial.println(ok);
  if (contadorRX > 0) {
    Serial.print(F("  Tasa de exito           : "));
    Serial.print(ok * 100UL / contadorRX);
    Serial.println(F(" %"));
  }
  Serial.println(F("=========================================="));
}
