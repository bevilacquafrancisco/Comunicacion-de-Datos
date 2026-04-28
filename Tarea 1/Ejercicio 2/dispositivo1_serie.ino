/**
 * ===========================================================================
 * Archivo     : dispositivo1_serie.ino
 * Ejercicio   : 2 — Comunicación Serie UART
 * Autores     : Francisco Bevilacqua, Sebastian Clement
 * Fecha       : 2026-04-22
 * Versión     : 1.0.0
 * Descripción : Firmware del Dispositivo 1 (Maestro de comandos) para
 *               comunicación bidireccional serie UART con el Dispositivo 2.
 *               Implementa el protocolo de handshake, comandos ON/OFF y
 *               COLOR RGB, recepción de ACK/NACK y logging exhaustivo
 *               por Serial hardware para debugging.
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
 *   SoftSerial RX : pin 4  (recibe de D2 pin 5)
 *   SoftSerial TX : pin 5  (envía a D2 pin 4)
 *   GND común     : GND   (OBLIGATORIO — referencia de tensión)
 *   BTN_ONOFF     : pin 11 (INPUT_PULLUP)
 *   BTN_CAMBIAR   : pin 12 (INPUT_PULLUP)
 *   LED1 (ACK)    : pin 8  → R220Ω → LED → GND
 *   LED2 (CONEXION): pin 9 → R220Ω → LED → GND
 *   Serial debug  : pines 0/1 (USB-Serial hardware, 115200 bps)
 *
 * ---------------------------------------------------------------------------
 * MÁQUINA DE ESTADOS D1
 * ---------------------------------------------------------------------------
 *   IDLE ──(CONN_REQ recibido)──► envía CONN_ACK, enciende LED2
 *        └──► CONECTADO
 *   CONECTADO ──(BTN_ONOFF)────► envía CMD_ONOFF, espera ACK/NACK
 *   CONECTADO ──(BTN_CAMBIAR)──► envía CMD_COLOR, espera ACK/NACK
 *   cualquier ──(ACK válido)───► LED1 enciende 2 s
 *   cualquier ──(NACK recibido)► loguea error, LED1 NO enciende
 * ===========================================================================
 */

#include <SoftwareSerial.h>

// ===========================================================================
// SOFTWARESERIAL — Canal de comunicación con D2
// ===========================================================================

/**
 * Puerto serie por software en pines 4 (RX) y 5 (TX).
 *
 * Justificación de pines: el Arduino UNO tiene un único puerto UART hardware
 * (pines 0/RX y 1/TX) compartido con el USB-Serial del IDE. Usar SoftwareSerial
 * en pines alternativos libera el puerto hardware exclusivamente para el monitor
 * de debug, replicando la metodología del Ejercicio 1 (bus paralelo + Serial).
 *
 * Baud rate: 9600 bps — suficiente para las tramas cortas del protocolo
 * (máximo 5 bytes) y compatible con SoftwareSerial sin pérdida de datos.
 */
SoftwareSerial serieD2(4, 5);   // RX=4, TX=5

// ===========================================================================
// PINOUT — PERIFÉRICOS DE D1
// ===========================================================================

/** Botón ON/OFF: alterna el estado del LED RGB en D2. */
const int PIN_BTN_ONOFF   = 11;

/** Botón CAMBIAR: genera un color aleatorio y lo envía a D2. */
const int PIN_BTN_CAMBIAR = 12;

/**
 * LED1 — Indicador de ACK recibido.
 * Enciende durante LED1_DURACION_MS al recibir ACK positivo de D2.
 */
const int PIN_LED1 = 8;

/**
 * LED2 — Indicador de conexión activa con D2.
 * Enciende al aceptar el CONN_REQ de D2 y permanece encendido.
 */
const int PIN_LED2 = 9;

// ===========================================================================
// CONSTANTES DEL PROTOCOLO
// ===========================================================================

/** Header del mensaje CONN_REQ (D2 solicita conexión). */
#define HEADER_CONN_REQ   0xAA

/** Header del mensaje CONN_ACK (D1 acepta la conexión). */
#define HEADER_CONN_ACK   0xBB

/** Header del comando ON/OFF enviado a D2. */
#define HEADER_CMD_ONOFF  0x01

/** Header del comando de cambio de color enviado a D2. */
#define HEADER_CMD_COLOR  0x02

/**
 * Header del ACK positivo (D2 confirma recepción correcta).
 * 0xFF elegido por ser el valor máximo de un byte (fácil de identificar
 * visualmente en el log hexadecimal y difícil de confundir con datos).
 */
#define HEADER_ACK        0xFF

/**
 * Header del NACK (D2 reporta checksum incorrecto).
 * 0xFE = 0xFF - 1, adyacente al ACK, diferenciable en una sola comparación.
 */
#define HEADER_NACK       0xFE

/** Tiempo que LED1 permanece encendido al recibir ACK (ms). */
#define LED1_DURACION_MS  2000

/** Timeout máximo para esperar ACK/NACK de D2 tras enviar un comando (ms). */
#define TIMEOUT_ACK_MS    3000

/** Tiempo de debounce para botones (ms). */
#define DEBOUNCE_MS       50

/** Baud rate del canal SoftwareSerial con D2. */
#define BAUD_SERIE        9600

/** Baud rate del Serial hardware para debug. */
#define BAUD_DEBUG        115200

// ===========================================================================
// ESTADOS DE LA MÁQUINA DE ESTADOS
// ===========================================================================

/** D1 está en reposo, esperando CONN_REQ de D2. */
#define ESTADO_IDLE       0

/** D1 tiene conexión activa con D2 y puede enviar comandos. */
#define ESTADO_CONECTADO  1

// ===========================================================================
// VARIABLES DE ESTADO
// ===========================================================================

/** Estado actual de la máquina de estados de D1. */
uint8_t estadoSistema = ESTADO_IDLE;

/** Estado actual del LED RGB en D2 (local para tracking). */
bool ledRgbEncendido = false;

/** Último color RGB enviado (para referencia en logs). */
uint8_t ultimoR = 0, ultimoG = 0, ultimoB = 0; 

/** Timestamps para debounce de botones. */
unsigned long ultimoTimeBtnOnOff  = 0;
unsigned long ultimoTimeBtnCambiar = 0;

/** Contador global de comandos enviados (TX #N). */
uint16_t contadorTX = 0;

/** Contador de ACKs recibidos en la sesión. */
uint16_t contadorACK = 0;

/** Contador de NACKs recibidos en la sesión. */
uint16_t contadorNACK = 0;

/** Contador de timeouts de ACK en la sesión. */
uint16_t contadorTimeoutACK = 0;

// ===========================================================================
// DECLARACIONES DE FUNCIONES
// ===========================================================================

void    configurarPines();
uint8_t calcularChecksum(uint8_t* datos, uint8_t longitud);
void    enviarTrama(uint8_t* datos, uint8_t longitud);
bool    recibirTrama(uint8_t* buffer, uint8_t esperados, uint32_t timeoutMs);
void    esperarCONN_REQ();
void    enviarCMD_ONOFF(bool estado);
void    enviarCMD_COLOR(uint8_t r, uint8_t g, uint8_t b);
bool    procesarRespuesta(const char* contexto);
bool    leerBoton(int pin, unsigned long &ultimoTiempo);
void    encenderLED1();
void    logCabecera();
void    logSeparador();
void    logTramaTX(uint8_t* datos, uint8_t longitud, const char* tipo);
void    logTramaRX(uint8_t* datos, uint8_t longitud, const char* tipo,
                   uint8_t chkRecibido, uint8_t chkCalculado);
void    logResumenSesion();
void    logByte(uint8_t valor);

// ===========================================================================
// SETUP
// ===========================================================================

/**
 * Inicializa el hardware, abre los puertos serie y muestra la cabecera
 * de debug. Queda en estado IDLE esperando CONN_REQ de D2.
 */
void setup() {
  // Serial hardware: exclusivo para debug, máxima velocidad posible
  Serial.begin(BAUD_DEBUG);

  // SoftwareSerial: canal de datos con D2
  serieD2.begin(BAUD_SERIE);

  configurarPines();
  randomSeed(analogRead(A0));  // Semilla analógica para colores aleatorios

  logCabecera();

  Serial.println(F("[INIT] Estado inicial: IDLE"));
  Serial.println(F("[INIT] Esperando solicitud de conexion de D2..."));
  Serial.println(F(""));
}

// ===========================================================================
// LOOP PRINCIPAL
// ===========================================================================

/**
 * Máquina de estados principal de D1.
 *
 * Estado IDLE    : Escucha CONN_REQ de D2. Al recibirlo, envía CONN_ACK,
 *                  enciende LED2 y transiciona a CONECTADO.
 * Estado CONECTADO: Monitorea botones. Cada pulsación construye y envía
 *                  la trama correspondiente, luego espera ACK/NACK de D2.
 */
void loop() {

  switch (estadoSistema) {

    // -----------------------------------------------------------------------
    case ESTADO_IDLE:
    // -----------------------------------------------------------------------
      // Escuchar pasivamente el canal hasta detectar un CONN_REQ
      esperarCONN_REQ();
      break;

    // -----------------------------------------------------------------------
    case ESTADO_CONECTADO:
    // -----------------------------------------------------------------------
      // Botón ON/OFF
      if (leerBoton(PIN_BTN_ONOFF, ultimoTimeBtnOnOff)) {
        ledRgbEncendido = !ledRgbEncendido; // Alternar estado del LED RGB

        Serial.println(F(""));
        Serial.println(F("[BTN] ON/OFF presionado"));
        Serial.print  (F("      Estado nuevo del LED RGB: "));
        Serial.println(ledRgbEncendido ? F("ENCENDIDO") : F("APAGADO"));

        enviarCMD_ONOFF(ledRgbEncendido);
        procesarRespuesta("CMD_ONOFF");
      }

      // Botón CAMBIAR
      if (leerBoton(PIN_BTN_CAMBIAR, ultimoTimeBtnCambiar)) {
        ultimoR = random(0, 256);
        ultimoG = random(0, 256);
        ultimoB = random(0, 256);

        Serial.println(F(""));
        Serial.println(F("[BTN] CAMBIAR presionado"));
        Serial.print  (F("      Color aleatorio generado: R="));
        Serial.print  (ultimoR);
        Serial.print  (F("  G="));
        Serial.print  (ultimoG);
        Serial.print  (F("  B="));
        Serial.println(ultimoB);

        enviarCMD_COLOR(ultimoR, ultimoG, ultimoB);
        procesarRespuesta("CMD_COLOR");
      }
      break;
  }
}

// ===========================================================================
// FUNCIONES DE HARDWARE
// ===========================================================================

/**
 * Configura todos los pines de D1.
 * LEDs como OUTPUT en LOW, botones como INPUT_PULLUP.
 */
void configurarPines() {
  pinMode(PIN_BTN_ONOFF,   INPUT_PULLUP);
  pinMode(PIN_BTN_CAMBIAR, INPUT_PULLUP);
  pinMode(PIN_LED1, OUTPUT);  digitalWrite(PIN_LED1, LOW);
  pinMode(PIN_LED2, OUTPUT);  digitalWrite(PIN_LED2, LOW);
}

/**
 * Lee un botón con debounce por software.
 * Lógica invertida: LOW = presionado (pull-up interno activo).
 *
 * @param pin          Pin del botón a leer
 * @param ultimoTiempo Timestamp del último evento válido (referencia)
 * @return             true si se detectó pulsación válida
 */
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
 * Enciende LED1 durante LED1_DURACION_MS y lo apaga.
 * Llamado únicamente al recibir un ACK válido de D2.
 * El delay bloquea el loop durante 2 s — aceptable porque el sistema
 * no deberia procesar nuevos comandos mientras confirma la recepción.
 */
void encenderLED1() {
  Serial.print  (F("[LED1] Encendido por "));
  Serial.print  (LED1_DURACION_MS);
  Serial.println(F(" ms (ACK confirmado)"));
  digitalWrite(PIN_LED1, HIGH);
  delay(LED1_DURACION_MS);
  digitalWrite(PIN_LED1, LOW);
  Serial.println(F("[LED1] Apagado"));
}

// ===========================================================================
// FUNCIONES DE CHECKSUM
// ===========================================================================

/**
 * Calcula el checksum de una trama como la suma de sus bytes módulo 256.
 *
 * Algoritmo: CHK = (B0 + B1 + ... + Bn) & 0xFF
 *
 * Ventajas frente al XOR del Ejercicio 1:
 *   - Detecta todos los errores de un solo byte alterado.
 *   - Detecta la mayoría de errores de doble byte (excepto cuando la
 *     suma de los cambios es múltiplo de 256, caso extremadamente raro).
 *   - Es el mecanismo base de Modbus RTU (LRC = complemento de la suma).
 *   - Permite mostrar el cálculo paso a paso en el log, facilitando
 *     la comprensión del algoritmo.
 *
 * Limitación: no detecta reordenamiento de bytes (el XOR tampoco),
 * para lo cual se requeriría CRC-16 o superior.
 *
 * @param datos    Puntero al array de bytes (sin incluir el CHK)
 * @param longitud Cantidad de bytes a sumar
 * @return         Byte de checksum resultante (0–255)
 */
uint8_t calcularChecksum(uint8_t* datos, uint8_t longitud) {
  uint16_t suma = 0;
  for (uint8_t i = 0; i < longitud; i++) {
    suma += datos[i];
  }
  return (uint8_t)(suma & 0xFF);
}

// ===========================================================================
// FUNCIONES DE TRANSMISIÓN
// ===========================================================================

/**
 * Envía una trama por SoftwareSerial: primero los bytes de datos,
 * luego el checksum calculado sobre esos bytes.
 *
 * El checksum se calcula internamente y se adjunta como último byte,
 * por lo que el array `datos` NO debe incluirlo.
 *
 * @param datos    Array de bytes del payload (HEADER + campos)
 * @param longitud Cantidad de bytes en `datos` (sin CHK)
 */
void enviarTrama(uint8_t* datos, uint8_t longitud) {
  uint8_t chk = calcularChecksum(datos, longitud);
  for (uint8_t i = 0; i < longitud; i++) {
    serieD2.write(datos[i]);
  }
  serieD2.write(chk);
}

/**
 * Recibe exactamente `esperados` bytes por SoftwareSerial con timeout.
 *
 * Estrategia: polling con ventana de tiempo. Si el canal está silencioso
 * más de `timeoutMs` ms desde el inicio de la espera, se considera que
 * la respuesta no llegará (D2 desconectado o trama perdida).
 *
 * @param buffer    Buffer de destino (debe tener al menos `esperados` bytes)
 * @param esperados Cantidad exacta de bytes a recibir
 * @param timeoutMs Tiempo máximo de espera total (ms)
 * @return          true si se recibieron todos los bytes a tiempo
 */
bool recibirTrama(uint8_t* buffer, uint8_t esperados, uint32_t timeoutMs) {
  uint8_t recibidos = 0;
  unsigned long inicio = millis();

  while (recibidos < esperados) {
    if ((millis() - inicio) > timeoutMs) {
      Serial.print  (F("[ERROR-RX] Timeout. Recibidos: "));
      Serial.print  (recibidos);
      Serial.print  (F(" / "));
      Serial.print  (esperados);
      Serial.println(F(" bytes esperados."));
      Serial.println(F("           DIAGNOSTICO: Verificar cable TX de D2 al pin 4 de D1,"));
      Serial.println(F("           GND comun, y que D2 este ejecutando el firmware correcto."));
      return false;
    }
    if (serieD2.available()) {
      buffer[recibidos++] = serieD2.read();
    }
  }
  return true;
}

// ===========================================================================
// FUNCIONES DEL PROTOCOLO — D1
// ===========================================================================

/**
 * Escucha el canal en busca de un CONN_REQ de D2.
 * Cuando lo recibe, verifica el checksum, responde con CONN_ACK,
 * enciende LED2 y transiciona el sistema a ESTADO_CONECTADO.
 *
 * Si el checksum del CONN_REQ es incorrecto, loguea el error y
 * permanece en IDLE (no acepta la conexión con una trama corrupta).
 */
void esperarCONN_REQ() {
  if (!serieD2.available()) return;

  // CONN_REQ tiene 2 bytes: [ 0xAA | CHK ]
  uint8_t bufferRx[2];
  bufferRx[0] = serieD2.read();

  if (bufferRx[0] != HEADER_CONN_REQ) {
    Serial.print  (F("[WARN] Byte inesperado en canal (IDLE): 0x"));
    logByte(bufferRx[0]);
    Serial.println(F(" — ignorado."));
    return;
  }

  // Leer el CHK, que debe llegar inmediatamente después del header
  if (!recibirTrama(&bufferRx[1], 1, 500)) {
    Serial.println(F("[ERROR] CONN_REQ incompleto (timeout en CHK)."));
    return;
  }

  uint8_t payload[1] = { HEADER_CONN_REQ };
  uint8_t chkCalc    = calcularChecksum(payload, 1);
  uint8_t chkRecib   = bufferRx[1];

  Serial.println(F(""));
  logSeparador();
  Serial.print(F("[RX] CONN_REQ recibido: "));
  logByte(bufferRx[0]); Serial.print(F(" ")); logByte(chkRecib);
  Serial.println(F(""));

  logTramaRX(bufferRx, 2, "CONN_REQ", chkRecib, chkCalc);

  if (chkRecib != chkCalc) {
    Serial.println(F("[ERROR] CHK incorrecto en CONN_REQ. Conexion rechazada."));
    Serial.println(F("        DIAGNOSTICO: Ruido en el cable RX (pin 4). Verificar conexiones."));
    logSeparador();
    return;
  }

  // CHK OK → enviar CONN_ACK
  uint8_t tramaCONN_ACK[1] = { HEADER_CONN_ACK };
  logTramaTX(tramaCONN_ACK, 1, "CONN_ACK");
  enviarTrama(tramaCONN_ACK, 1);

  // Activar LED2 (conexión activa permanente)
  digitalWrite(PIN_LED2, HIGH);
  estadoSistema = ESTADO_CONECTADO;

  Serial.println(F("[LED2] Encendido — conexion activa"));
  Serial.println(F("[ESTADO] Sistema CONECTADO. Esperando botones..."));
  logSeparador();
  Serial.println(F(""));
}

/**
 * Construye y envía el comando CMD_ONOFF a D2.
 * Trama: [ 0x01 | STATE | CHK ]  (3 bytes en total)
 *
 * @param estado  true = encender LED RGB | false = apagar LED RGB
 */
void enviarCMD_ONOFF(bool estado) {
  contadorTX++;
  uint8_t trama[2] = { HEADER_CMD_ONOFF, (uint8_t)(estado ? 0x01 : 0x00) }; //trama de 2 bytes: header + estado
  logTramaTX(trama, 2, "CMD_ONOFF");
  enviarTrama(trama, 2);
}

/**
 * Construye y envía el comando CMD_COLOR a D2.
 * Trama: [ 0x02 | R | G | B | CHK ]  (5 bytes en total)
 *
 * @param r  Intensidad del canal Rojo  (0–255)
 * @param g  Intensidad del canal Verde (0–255)
 * @param b  Intensidad del canal Azul  (0–255)
 */
void enviarCMD_COLOR(uint8_t r, uint8_t g, uint8_t b) {
  contadorTX++;
  uint8_t trama[4] = { HEADER_CMD_COLOR, r, g, b }; //trama de 4 bytes: header + R + G + B
  logTramaTX(trama, 4, "CMD_COLOR");
  enviarTrama(trama, 4);
}

/**
 * Espera la respuesta (ACK o NACK) de D2 tras un comando.
 * Interpreta el header recibido y actualiza los contadores.
 *
 * Respuesta ACK : [ 0xFF | CHK ]  → enciende LED1 2 s
 * Respuesta NACK: [ 0xFE | CHK ]  → loguea el error, LED1 no enciende
 * Timeout       : D2 no respondió → loguea el problema
 *
 * @param contexto  Nombre del comando que originó la espera (para el log)
 * @return          true si se recibió ACK válido, false en cualquier otro caso
 */
bool procesarRespuesta(const char* contexto) {
  Serial.print  (F("[ESPERA] Aguardando ACK/NACK de D2... (timeout: "));
  Serial.print  (TIMEOUT_ACK_MS);
  Serial.println(F(" ms)"));

  uint8_t bufferRx[2];
  if (!recibirTrama(bufferRx, 2, TIMEOUT_ACK_MS)) {
    contadorTimeoutACK++;
    Serial.print  (F("[ERROR] Timeout esperando respuesta a "));
    Serial.println(contexto);
    Serial.print  (F("        Timeouts acumulados: "));
    Serial.println(contadorTimeoutACK);
    Serial.println(F("        DIAGNOSTICO: D2 puede haber descartado la trama por CHK"));
    Serial.println(F("        incorrecto sin enviar NACK, o hay problema en el cable RX."));
    return false;
  }

  uint8_t header    = bufferRx[0];
  uint8_t chkRecib  = bufferRx[1];
  uint8_t payload[1] = { header };
  uint8_t chkCalc   = calcularChecksum(payload, 1);

  // Log de la trama recibida
  const char* tipoRespuesta = (header == HEADER_ACK) ? "ACK" :
                              (header == HEADER_NACK) ? "NACK" : "DESCONOCIDO";
  logTramaRX(bufferRx, 2, tipoRespuesta, chkRecib, chkCalc);

  // Verificar integridad del ACK/NACK recibido
  if (chkRecib != chkCalc) {
    Serial.println(F("[ERROR] CHK incorrecto en respuesta de D2."));
    Serial.print  (F("        Recibido: 0x")); logByte(chkRecib);
    Serial.print  (F(" | Calculado: 0x")); logByte(chkCalc);
    Serial.println(F(""));
    Serial.println(F("        DIAGNOSTICO: Ruido en el canal. La respuesta de D2 llegó"));
    Serial.println(F("        corrupta. LED1 NO enciende por seguridad."));
    return false;
  }

  if (header == HEADER_ACK) {
    contadorACK++;
    Serial.print(F("[RX] ACK valido recibido para ")); Serial.println(contexto);
    encenderLED1();
    return true;

  } else if (header == HEADER_NACK) {
    contadorNACK++;
    Serial.print  (F("[RX] NACK recibido para ")); Serial.println(contexto);
    Serial.println(F("     D2 detecto checksum incorrecto en el comando enviado."));
    Serial.println(F("     LED1 NO enciende. El estado del LED RGB en D2 NO cambio."));
    Serial.println(F("     (Reenvio automatico no implementado — ARQ fuera de scope)"));
    Serial.print  (F("     NACKs acumulados esta sesion: "));
    Serial.println(contadorNACK);
    return false;

  } else {
    Serial.print  (F("[ERROR] Header de respuesta desconocido: 0x")); logByte(header);
    Serial.println(F(""));
    Serial.println(F("        DIAGNOSTICO: Byte espurio en el canal o desincronizacion"));
    Serial.println(F("        de tramas. Verificar que D2 usa el mismo protocolo."));
    return false;
  }
}

// ===========================================================================
// FUNCIONES DE LOGGING — Debug por Serial Monitor
// ===========================================================================

/**
 * Imprime un byte en formato hexadecimal con cero a la izquierda si < 0x10.
 * Función auxiliar usada en todos los logs de tramas.
 *
 * @param valor  Byte a imprimir
 */
void logByte(uint8_t valor) {
  if (valor < 0x10) Serial.print(F("0"));
  Serial.print(valor, HEX);
}

/**
 * Imprime la cabecera de inicio del sistema con todos los parámetros
 * de configuración para referencia rápida durante el debug.
 */
void logCabecera() {
  Serial.println(F(""));
  Serial.println(F("======================================================="));
  Serial.println(F("  DISPOSITIVO 1 | Maestro | UART Serie"));
  Serial.println(F("  Comunicacion de Datos — Ejercicio 2 | v1.0.0"));
  Serial.println(F("======================================================="));
  Serial.print  (F("  Baud SoftSerial  : ")); Serial.println(BAUD_SERIE);
  Serial.println(F("  PINOUT SoftSerial: RX=4, TX=5"));
  Serial.print  (F("  BTN ON/OFF       : pin ")); Serial.println(PIN_BTN_ONOFF);
  Serial.print  (F("  BTN CAMBIAR      : pin ")); Serial.println(PIN_BTN_CAMBIAR);
  Serial.print  (F("  LED1 (ACK)       : pin ")); Serial.println(PIN_LED1);
  Serial.print  (F("  LED2 (CONEXION)  : pin ")); Serial.println(PIN_LED2);
  Serial.println(F("  Checksum         : Suma de bytes MOD 256"));
  Serial.println(F("  NACK             : sin reenvio automatico (ARQ no implementado)"));
  Serial.println(F("======================================================="));
  Serial.println(F(""));
}

/** Imprime una línea separadora para delimitar bloques en el log. */
void logSeparador() {
  Serial.println(F("-------------------------------------------------------"));
}

/**
 * Loguea una trama que se está enviando, byte por byte, con el
 * cálculo del checksum detallado.
 *
 * @param datos    Array de bytes del payload (sin CHK)
 * @param longitud Cantidad de bytes en el payload
 * @param tipo     Nombre del tipo de trama (para el encabezado del log)
 */
void logTramaTX(uint8_t* datos, uint8_t longitud, const char* tipo) {
  logSeparador();
  Serial.print  (F("[TX #"));
  Serial.print  (contadorTX);
  Serial.print  (F("] Enviando "));
  Serial.println(tipo);

  // Nombres descriptivos de cada byte según el protocolo
  const char* nombresOnOff[2] = { "HEADER CMD_ONOFF", "STATE (1=ENC, 0=APG)" };
  const char* nombresColor[4] = { "HEADER CMD_COLOR", "R (Rojo)", "G (Verde)", "B (Azul)" };
  const char* nombresConnAck[1] = { "HEADER CONN_ACK" };

  for (uint8_t i = 0; i < longitud; i++) {
    Serial.print  (F("    Byte "));
    Serial.print  (i);
    Serial.print  (F(": 0x")); logByte(datos[i]);
    Serial.print  (F(" (dec=")); Serial.print(datos[i]); Serial.print(F(")"));
    Serial.print  (F("  ← "));
    // Descripción según tipo de trama
    if (strcmp(tipo, "CMD_ONOFF") == 0 && i < 2) {
      Serial.print(nombresOnOff[i]);
      if (i == 1) {
        Serial.print(F(" → "));
        Serial.print(datos[i] == 0x01 ? F("ENCENDER") : F("APAGAR"));
      }
    } else if (strcmp(tipo, "CMD_COLOR") == 0 && i < 4) {
      Serial.print(nombresColor[i]);
    } else if (strcmp(tipo, "CONN_ACK") == 0) {
      Serial.print(nombresConnAck[0]);
    }
    Serial.println(F(""));
  }

  // Mostrar cálculo del checksum
  uint8_t chk = calcularChecksum(datos, longitud);
  uint16_t sumaDetalle = 0;
  Serial.print  (F("    CHK (Suma mod256): "));
  for (uint8_t i = 0; i < longitud; i++) {
    Serial.print(F("0x")); logByte(datos[i]);
    sumaDetalle += datos[i];
    if (i < longitud - 1) Serial.print(F(" + "));
  }
  Serial.print  (F(" = 0x"));
  if (sumaDetalle < 0x10) Serial.print(F("0"));
  Serial.print  (sumaDetalle, HEX);
  Serial.print  (F(" → mod256 = 0x")); logByte(chk);
  Serial.println(F(""));

  Serial.print  (F("    Byte CHK: 0x")); logByte(chk);
  Serial.println(F(" (adjuntado al final de la trama)"));
  Serial.println(F("    --> Trama enviada por SoftwareSerial."));
}

/**
 * Loguea una trama recibida con verificación de checksum detallada.
 * Permite identificar exactamente qué llegó vs. qué se esperaba.
 *
 * @param datos        Array de bytes recibidos (incluyendo CHK en último lugar)
 * @param longitud     Total de bytes recibidos
 * @param tipo         Nombre del tipo de trama
 * @param chkRecibido  Byte CHK recibido
 * @param chkCalculado CHK que D1 calculó sobre los bytes de payload
 */
void logTramaRX(uint8_t* datos, uint8_t longitud, const char* tipo,
                uint8_t chkRecibido, uint8_t chkCalculado) {
  Serial.print  (F("[RX] Trama "));
  Serial.print  (tipo);
  Serial.println(F(":"));
  for (uint8_t i = 0; i < longitud; i++) {
    Serial.print  (F("    Byte "));
    Serial.print  (i);
    Serial.print  (F(": 0x")); logByte(datos[i]);
    Serial.print  (F(" (dec="));
    Serial.print  (datos[i]);
    Serial.println(F(")"));
  }
  Serial.print  (F("    Verificacion CHK: recibido=0x")); logByte(chkRecibido);
  Serial.print  (F(" | calculado=0x"));   logByte(chkCalculado);
  if (chkRecibido == chkCalculado) {
    Serial.println(F(" | OK ✓"));
  } else {
    Serial.println(F(" | ERROR ✗"));
    Serial.println(F("    DIAGNOSTICO: Un byte fue alterado durante la transmision."));
    Serial.println(F("    Posibles causas: ruido electrico, GND no comun, cable defectuoso."));
  }
}

/**
 * Imprime un resumen de la sesión completa.
 * Útil para evaluar la tasa de errores del canal al finalizar una prueba.
 * (Llamar manualmente desde el monitor serie no es necesario; se puede
 * agregar un botón dedicado si se desea en futuras versiones.)
 */
void logResumenSesion() {
  Serial.println(F(""));
  Serial.println(F("========== RESUMEN DE SESION D1 =========="));
  Serial.print  (F("  Comandos enviados (TX)  : ")); Serial.println(contadorTX);
  Serial.print  (F("  ACKs recibidos          : ")); Serial.println(contadorACK);
  Serial.print  (F("  NACKs recibidos         : ")); Serial.println(contadorNACK);
  Serial.print  (F("  Timeouts ACK            : ")); Serial.println(contadorTimeoutACK);
  uint16_t errTotal = contadorNACK + contadorTimeoutACK;
  Serial.print  (F("  Errores totales         : ")); Serial.println(errTotal);
  if (contadorTX > 0) {
    Serial.print(F("  Tasa de exito           : "));
    Serial.print(contadorACK * 100UL / contadorTX);
    Serial.println(F(" %"));
  }
  Serial.println(F("=========================================="));
}
