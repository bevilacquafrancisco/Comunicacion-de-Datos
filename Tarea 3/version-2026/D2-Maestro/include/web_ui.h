/**
 * @file    web_ui.h
 * @brief   Interfaz web del Dispositivo 3 — embebida en flash del ESP32 (D2).
 *
 * La página completa (HTML + CSS + JavaScript) se almacena como string
 * literal PROGMEM y es servida por ESPAsyncWebServer en la ruta raíz ("/").
 * Al no depender de Internet, funciona en el AP aislado que crea D2.
 *
 * @section arquitectura Arquitectura de la interfaz
 *
 *  - HTML semántico de una sola página (SPA mínima).
 *  - CSS con variables custom para theming; sin frameworks externos.
 *  - JavaScript vanilla con la API WebSocket nativa del navegador.
 *  - Compatible con Chrome, Firefox, Safari y Edge en Android, iOS,
 *    Windows, macOS y Linux.
 *  - Sin CDN ni recursos externos: crítico dado que D2 opera como AP
 *    sin salida a la red pública.
 *
 * @section protocolo_ws Protocolo WebSocket con CRC-16/IBM
 *
 *  Todos los mensajes JSON —en ambas direcciones— incluyen el campo
 *  "crc16" calculado íntegramente por software (XOR, aritmética módulo 2,
 *  polinomio 0x8005, init=0x0000, LSB-first) sobre el payload JSON
 *  sin ese campo.
 *
 *  D3 -> D2 (comandos):
 *   {"tipo":"CMD_COLOR", "color":"NOMBRE", "r":n, "g":n, "b":n, "crc16":<u16>}
 *   {"tipo":"CMD_ENCENDER",                                       "crc16":<u16>}
 *   {"tipo":"CMD_APAGAR",                                         "crc16":<u16>}
 *   {"tipo":"CMD_DESCONECTAR",                                    "crc16":<u16>}
 *   {"tipo":"PING",                                               "crc16":<u16>}
 *
 *  D2 -> D3 (respuestas):
 *   {"tipo":"ESTADO_ACTUAL",  "encendido":bool, "color":str, "r":n, "g":n, "b":n, "crc16":<u16>}
 *   {"tipo":"RESULTADO_CMD",  "exito":bool, "encendido":bool, "color":str,
 *                             "r":n, "g":n, "b":n, "crc16":<u16>}
 *   {"tipo":"CRC_ERROR",      "info":str,                           "crc16":<u16>}
 *   {"tipo":"ACK_DESCONECTAR",                                      "crc16":<u16>}
 *   {"tipo":"PONG",                                                  "crc16":<u16>}
 *
 * @section cmd_color_rgb CMD_COLOR con campos r, g, b
 *
 *  El comando CMD_COLOR incluye los valores numericos r, g, b ademas del
 *  nombre simbolico del color. Esto garantiza que el CRC-16 cubra el triplete
 *  completo y no solo el string del nombre. D2 puede asi verificar la
 *  integridad de los valores numericos transmitidos independientemente de la
 *  tabla de colores local.
 *
 * @section crc_orden Orden canonico de campos y compatibilidad JSON
 *
 *  Para que el CRC-16 calculado en D3 (JSON.stringify) sea identico al
 *  recalculado en D2 (ArduinoJson serializeJson), el orden de los campos
 *  en el objeto debe ser determinista. JavaScript garantiza el orden de
 *  insercion en objetos literales desde ES2015. ArduinoJson v7 preserva
 *  el orden de insercion. La funcion serializarConCRC() construye el objeto
 *  siempre en el mismo orden de campos, garantizando serializacion canonica.
 *
 * @section problemas_resueltos Problemas resueltos en esta version
 *
 *  PROBLEMA 2 - Desconexion asimetrica (LEDs sin actualizar):
 *   Llamar a ws.close() desde el navegador cerraba el socket de forma
 *   asimetrica en ESPAsyncWebServer, retrasando el evento DISCONNECT en D2
 *   y dejando los LEDs de estado sin actualizar hasta que el timeout del
 *   heartbeat lo detectaba.
 *   SOLUCION: Graceful Shutdown mediante handshake CMD_DESCONECTAR /
 *   ACK_DESCONECTAR. D3 envia CMD_DESCONECTAR y espera el ACK_DESCONECTAR
 *   de D2; solo entonces cierra el socket con ws.close(1000). El cierre se
 *   inicia desde el servidor (D2 llama a client->close()), disparando el
 *   evento WS_EVT_DISCONNECT en D2 de forma deterministica e inmediata.
 *
 * @section paleta Paleta de 12 colores
 *
 *  8 originales (TP2): ROJO, AMARILLO, VERDE, CELESTE, AZUL, LILA, BLANCO, ROSA.
 *  4 nuevos: NARANJA, MAGENTA, TURQUESA, VIOLETA.
 *  Debe mantenerse sincronizada con la tabla COLORES[] en config.h de D2.
 *
 * @authors Bevilacqua, Francisco - Clement, Sebastian
 * @date    29 de junio 2026
 */

#pragma once

static const char WEB_UI[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TP3 · Control RGB</title>
<style>
  /* ── Variables y reset ──────────────────────────────────── */
  :root {
    --bg:      #0d0d0f;
    --surface: #18181c;
    --border:  #2a2a30;
    --text:    #e8e8ed;
    --muted:   #6b6b78;
    --accent:  #7c6af7;
    --conn:    #34d399;
    --disc:    #f87171;
    --warn:    #fbbf24;
    --radius:  14px;
    --shadow:  0 4px 32px rgba(0,0,0,.45);
    --font:    'Courier New','Lucida Console',monospace;
  }
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: var(--font);
    min-height: 100dvh;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 24px 16px 40px;
    gap: 20px;
  }

  /* ── Encabezado ─────────────────────────────────────────── */
  header {
    width: 100%; max-width: 480px;
    display: flex; justify-content: space-between; align-items: center;
    border-bottom: 1px solid var(--border); padding-bottom: 14px;
  }
  header h1 { font-size: 1rem; letter-spacing: .12em; text-transform: uppercase; color: var(--muted); }
  header h1 span { color: var(--accent); }

  /* ── Badge de conexion ──────────────────────────────────── */
  #badge {
    font-size: .72rem; letter-spacing: .08em; text-transform: uppercase;
    padding: 4px 12px; border-radius: 99px; border: 1px solid currentColor;
    transition: color .3s, border-color .3s;
  }
  #badge.conectado    { color: var(--conn); }
  #badge.desconectado { color: var(--disc); }
  #badge.conectando   { color: var(--warn); }

  /* ── Tarjeta principal ──────────────────────────────────── */
  .card {
    width: 100%; max-width: 480px;
    background: var(--surface); border: 1px solid var(--border);
    border-radius: var(--radius); padding: 24px; box-shadow: var(--shadow);
  }

  /* ── Preview del color ──────────────────────────────────── */
  .color-preview {
    width: 100%; height: 110px;
    border-radius: 10px; border: 1px solid var(--border);
    transition: background .5s ease;
    display: flex; align-items: center; justify-content: center;
    margin-bottom: 18px; position: relative; overflow: hidden;
  }
  .color-preview span {
    font-size: .78rem; letter-spacing: .14em; text-transform: uppercase;
    font-weight: 700; text-shadow: 0 1px 4px rgba(0,0,0,.6); opacity: .85;
  }
  .color-preview.apagado { background: #111113 !important; }
  .color-preview.apagado span::before { content: "· APAGADO ·"; }
  .color-preview:not(.apagado) span::before { content: attr(data-nombre); }

  /* ── Chips RGB numericos ────────────────────────────────── */
  .rgb-info { display: flex; gap: 10px; margin-bottom: 22px; }
  .rgb-info .chip {
    flex: 1; background: var(--bg);
    border: 1px solid var(--border); border-radius: 8px;
    padding: 8px 6px; text-align: center; font-size: .75rem;
  }
  .chip .label { color: var(--muted); font-size: .65rem; letter-spacing: .1em; }
  .chip .val   { font-size: 1rem; margin-top: 2px; }
  #chip-r .val { color: #f87171; }
  #chip-g .val { color: #34d399; }
  #chip-b .val { color: #60a5fa; }

  /* ── Controles de encendido ─────────────────────────────── */
  .power-row { display: flex; gap: 10px; margin-bottom: 22px; }
  .btn {
    flex: 1; padding: 13px 0;
    border: 1px solid var(--border); border-radius: 10px;
    background: var(--bg); color: var(--text);
    font-family: var(--font); font-size: .78rem;
    letter-spacing: .1em; text-transform: uppercase;
    cursor: pointer;
    transition: background .2s, border-color .2s, opacity .2s, transform .1s;
  }
  .btn:active  { transform: scale(.97); }
  .btn:disabled { opacity: .3; cursor: not-allowed; transform: none; }
  .btn.on  { border-color: var(--conn); color: var(--conn); }
  .btn.off { border-color: var(--disc); color: var(--disc); }
  .btn.on:hover  { background: rgba(52,211,153,.08); }
  .btn.off:hover { background: rgba(248,113,113,.08); }

  /* ── Grid de 4 columnas para 12 colores ─────────────────── */
  .color-section-title {
    font-size: .68rem; letter-spacing: .14em;
    text-transform: uppercase; color: var(--muted); margin-bottom: 12px;
  }
  .color-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 8px;
  }
  .color-btn {
    aspect-ratio: 1; border-radius: 10px;
    border: 2px solid transparent; cursor: pointer;
    transition: transform .15s, border-color .2s, box-shadow .2s;
    position: relative; overflow: hidden;
  }
  .color-btn:hover  { transform: scale(1.08); }
  .color-btn:active { transform: scale(.95); }
  .color-btn:disabled { opacity: .3; cursor: not-allowed; transform: none; }
  .color-btn.activo {
    border-color: white;
    box-shadow: 0 0 14px rgba(255,255,255,.35);
  }
  .color-btn .cn {
    position: absolute; bottom: 4px; left: 0; right: 0;
    text-align: center; font-size: .55rem; letter-spacing: .05em;
    text-transform: uppercase;
    text-shadow: 0 1px 3px rgba(0,0,0,.8); color: #fff;
  }

  /* ── Banner de notificaciones ───────────────────────────── */
  #banner {
    width: 100%; max-width: 480px; border-radius: 10px;
    padding: 14px 18px; font-size: .8rem; letter-spacing: .06em;
    display: none; border: 1px solid;
  }
  #banner.error { display: block; background: rgba(248,113,113,.08); border-color: var(--disc); color: var(--disc); }
  #banner.info  { display: block; background: rgba(124,106,247,.08); border-color: var(--accent); color: var(--accent); }
  #banner.warn  { display: block; background: rgba(251,191,36,.08);  border-color: var(--warn);   color: var(--warn); }

  /* ── Indicador de integridad CRC ────────────────────────── */
  #crc-status {
    width: 100%; max-width: 480px;
    font-size: .65rem; color: var(--muted);
    letter-spacing: .08em; text-align: right;
    min-height: 1.2em;
  }
  #crc-status.ok  { color: var(--conn); }
  #crc-status.err { color: var(--disc); }

  /* ── Fila de botones de conexion ────────────────────────── */
  .conn-row { width: 100%; max-width: 480px; display: flex; gap: 10px; }
  #btn-connect { flex: 1; }
  #btn-connect.reconectar { border-color: var(--accent); color: var(--accent); }
  #btn-connect.reconectar:hover { background: rgba(124,106,247,.1); }
  #btn-disconnect {
    flex: 1;
    border-color: var(--disc); color: var(--disc);
    display: none;
  }
  #btn-disconnect:hover { background: rgba(248,113,113,.08); }

  /* ── Footer ─────────────────────────────────────────────── */
  footer {
    font-size: .65rem; color: var(--muted);
    letter-spacing: .08em; text-align: center; margin-top: auto;
  }
</style>
</head>
<body>

<header>
  <h1>TP3 · <span>RGB</span> Control</h1>
  <div id="badge" class="desconectado">Desconectado</div>
</header>

<!-- Tarjeta principal de estado y control -->
<div class="card">

  <!-- Preview del color activo -->
  <div class="color-preview apagado" id="preview">
    <span data-nombre=""></span>
  </div>

  <!-- Chips RGB numericos -->
  <div class="rgb-info">
    <div class="chip" id="chip-r"><div class="label">R</div><div class="val" id="val-r">—</div></div>
    <div class="chip" id="chip-g"><div class="label">G</div><div class="val" id="val-g">—</div></div>
    <div class="chip" id="chip-b"><div class="label">B</div><div class="val" id="val-b">—</div></div>
  </div>

  <!-- Encender / Apagar -->
  <div class="power-row">
    <button class="btn on"  id="btn-on"  disabled onclick="cmdEncender()">▶ Encender</button>
    <button class="btn off" id="btn-off" disabled onclick="cmdApagar()">■ Apagar</button>
  </div>

  <!-- Paleta de 12 colores generada dinamicamente -->
  <div class="color-section-title">Seleccionar color</div>
  <div class="color-grid" id="color-grid"></div>
</div>

<!-- Banner de notificaciones -->
<div id="banner"></div>

<!-- Indicador de integridad CRC del ultimo mensaje -->
<div id="crc-status"></div>

<!-- Botones de conexion / desconexion voluntaria -->
<div class="conn-row">
  <button class="btn" id="btn-connect"    onclick="conectar()">Conectar a D2</button>
  <button class="btn" id="btn-disconnect" onclick="desconectar()">✕ Desconectar</button>
</div>

<footer>CdD · TP3 Integrador · Bevilacqua &amp; Clement · 2026</footer>

<script>
// ═══════════════════════════════════════════════════════════════
//  PALETA DE COLORES
//  Debe mantenerse sincronizada con COLORES[] en config.h de D2.
//  Los valores r, g, b se incluyen en CMD_COLOR para que el CRC-16
//  cubra el triplete numerico completo, no solo el nombre simbolico.
// ═══════════════════════════════════════════════════════════════

const COLORES = [
  // Colores originales (TP2)
  { nombre:"ROJO",     r:255, g:0,   b:0   },
  { nombre:"AMARILLO", r:255, g:200, b:0   },
  { nombre:"VERDE",    r:0,   g:255, b:0   },
  { nombre:"CELESTE",  r:0,   g:255, b:255 },
  { nombre:"AZUL",     r:0,   g:0,   b:255 },
  { nombre:"LILA",     r:180, g:0,   b:255 },
  { nombre:"BLANCO",   r:255, g:255, b:255 },
  { nombre:"ROSA",     r:255, g:105, b:180 },
  // Colores nuevos
  { nombre:"NARANJA",  r:255, g:80,  b:0   },
  { nombre:"MAGENTA",  r:255, g:0,   b:180 },
  { nombre:"TURQUESA", r:0,   g:210, b:140 },
  { nombre:"VIOLETA",  r:90,  g:0,   b:200 },
];

const WS_URL    = `ws://${location.hostname}/ws`;
const PING_MS   = 5000;   // intervalo de envio del keepalive PING [ms]
const PONG_WAIT = 3000;   // timeout para considerar la conexion muerta [ms]

// ═══════════════════════════════════════════════════════════════
//  CRC-16/IBM POR SOFTWARE (aritmetica modulo 2 / XOR)
//
//  Parametros del estandar CRC-16/IBM (CRC-16/ARC):
//    Polinomio : 0x8005  ->  x^16 + x^15 + x^2 + 1
//    Init      : 0x0000
//    RefIn     : true    ->  procesamiento LSB-first
//    RefOut    : true
//    XorOut    : 0x0000
//
//  Implementacion simetrica a la de D2 (C++): misma logica de
//  XOR bit a bit, misma inicializacion y mismo polinomio.
//  Portabilidad garantizada sin librerias externas en ningun lado.
// ═══════════════════════════════════════════════════════════════

/**
 * Calcula el CRC-16/IBM sobre un string (payload JSON serializado).
 *
 * El string se procesa byte a byte como codigos ASCII (charCodeAt).
 * El operador >>> garantiza desplazamiento logico sin signo en JS.
 * El enmascarado & 0xFFFF mantiene el resultado en 16 bits.
 *
 * @param {string} str  Payload sobre el que calcular el CRC.
 * @returns {number}    CRC de 16 bits (entero sin signo, 0 a 65535).
 */
function calcularCRC16(str) {
  const POLY = 0x8005;
  let crc = 0x0000;                        // Init = 0x0000

  for (let i = 0; i < str.length; i++) {
    crc ^= str.charCodeAt(i);             // XOR con el byte actual del payload

    for (let bit = 0; bit < 8; bit++) {
      if (crc & 0x0001) {
        crc = ((crc >>> 1) ^ POLY) & 0xFFFF; // LSB=1: desplazar y XOR con polinomio
      } else {
        crc = (crc >>> 1) & 0xFFFF;          // LSB=0: solo desplazar
      }
    }
  }
  return crc;                              // Remainder de 16 bits = checksum
}

/**
 * Reescribe el campo "crc16":<decimal> de un JSON ya serializado a formato
 * hexadecimal "crc16":"0xXXXX", SOLO para que los logs de consola sean
 * directamente comparables con los del monitor serial de D2 (que tambien
 * logea el CRC-16 en hexadecimal). Funcion simetrica a la de D2 (C++):
 * misma logica de busqueda de la clave "crc16": y reemplazo del valor.
 *
 * IMPORTANTE: esta funcion es puramente cosmetica para console.log/debug.
 * Nunca se usa sobre el string que efectivamente se envia por WebSocket
 * (ws.send siempre recibe el JSON original, con crc16 numerico estandar).
 *
 * @param {string} jsonStr  JSON ya serializado, con "crc16":<decimal>.
 * @returns {string}        Mismo JSON con "crc16" reescrito en hexadecimal.
 */
function formatearJsonParaLog(jsonStr) {
  const clave = '"crc16":';
  const pos = jsonStr.indexOf(clave);
  if (pos === -1) return jsonStr;   // sin campo crc16: devolver tal cual

  const inicioValor = pos + clave.length;
  let finValor = inicioValor;
  while (finValor < jsonStr.length && jsonStr[finValor] >= '0' && jsonStr[finValor] <= '9') {
    finValor++;
  }

  const valorDecimal = jsonStr.slice(inicioValor, finValor);
  if (valorDecimal.length === 0) return jsonStr;   // formato inesperado: no tocar

  const crcNum = parseInt(valorDecimal, 10);
  return jsonStr.slice(0, inicioValor) + '"' + crcHex(crcNum) + '"' + jsonStr.slice(finValor);
}

/**
 * Serializa un objeto JS con el campo crc16 firmado.
 *
 * Proceso:
 *  1. Serializar el objeto sin crc16 -> string base (orden de campos =
 *     orden de insercion en el objeto literal, determinista en ES2015+).
 *  2. Calcular CRC-16/IBM sobre el string base.
 *  3. Agregar obj.crc16 = CRC.
 *  4. Serializar nuevamente -> string final con crc16 incluido.
 *  5. Retornar el string final para enviarlo por WebSocket.
 *
 * El paso 1->2 evita la dependencia circular (no se puede calcular el
 * CRC sobre un string que ya contiene el campo crc16).
 *
 * @param {Object} obj  Objeto a serializar y firmar.
 * @returns {string}    JSON con campo crc16 al final.
 */
function serializarConCRC(obj) {
  const base  = JSON.stringify(obj);       // JSON sin crc16
  const crc   = calcularCRC16(base);      // CRC sobre payload puro
  obj.crc16   = crc;                      // agregar campo CRC
  const final = JSON.stringify(obj);      // JSON final con crc16
  console.debug('[CRC-16] TX | base=' + base +
                ' | crc=0x' + crc.toString(16).toUpperCase().padStart(4,'0') +
                ' | final=' + formatearJsonParaLog(final));
  return final;
}

/**
 * Valida el CRC-16 de un mensaje JSON recibido de D2.
 *
 * Proceso inverso a serializarConCRC():
 *  1. Leer y guardar crc_rx = msg.crc16.
 *  2. Crear una copia del objeto sin el campo crc16.
 *  3. Serializar la copia -> misma representacion que uso D2 al firmar.
 *  4. Recalcular el CRC-16 sobre esa serializacion.
 *  5. Comparar crc_rx con el recalculado.
 *
 * Object.assign() copia los campos en el mismo orden de insercion
 * que tiene el objeto original, manteniendo compatibilidad con el
 * orden canonico que usa ArduinoJson en D2.
 *
 * @param {Object} msg  Objeto JS ya parseado (JSON.parse del mensaje WS).
 * @returns {boolean}   true si el CRC es valido, false si hay discrepancia.
 */
function validarCRC(msg) {
  if (msg.crc16 === undefined) {
    console.warn('[CRC-16] RX | Mensaje sin campo crc16 -- rechazado');
    return false;
  }
  const crc_rx  = msg.crc16;
  const sin_crc = Object.assign({}, msg);
  delete sin_crc.crc16;
  const base    = JSON.stringify(sin_crc);
  const recalc  = calcularCRC16(base);
  const ok      = (crc_rx === recalc);
  console.debug('[CRC-16] RX | base=' + base +
                ' | crc_rx=0x'  + crc_rx.toString(16).toUpperCase().padStart(4,'0') +
                ' | recalc=0x'  + recalc.toString(16).toUpperCase().padStart(4,'0') +
                ' | ' + (ok ? 'OK' : 'ERROR'));
  return ok;
}

// ═══════════════════════════════════════════════════════════════
//  ESTADO DE LA APLICACION
// ═══════════════════════════════════════════════════════════════

let ws                           = null;
let pingTimer                    = null;
let pongTimer                    = null;
let estadoActual                 = { encendido: false, color: null, r: 0, g: 0, b: 0 };
let esperandoRespuesta           = false;
let desconectandoVoluntariamente = false; // distingue cierre voluntario de abrupto
let ultimoCrcTxComando            = null;  // CRC-16 del ultimo comando enviado (no PING/PONG)

// ═══════════════════════════════════════════════════════════════
//  CONSTRUCCION DE LA GRILLA DE COLORES
//  El onclick de cada boton pasa el objeto completo {nombre, r, g, b}
//  a cmdColor() para incluir el triplete numerico en CMD_COLOR.
// ═══════════════════════════════════════════════════════════════

function construirGrid() {
  const grid = document.getElementById('color-grid');
  COLORES.forEach(c => {
    const btn = document.createElement('button');
    btn.className = 'color-btn';
    btn.id        = 'cbtn-' + c.nombre;
    btn.disabled  = true;
    btn.style.background = 'rgb(' + c.r + ',' + c.g + ',' + c.b + ')';
    btn.title     = c.nombre + '  RGB(' + c.r + ', ' + c.g + ', ' + c.b + ')';
    btn.innerHTML = '<span class="cn">' + c.nombre + '</span>';
    btn.onclick   = () => cmdColor(c); // pasar objeto completo con r, g, b
    grid.appendChild(btn);
  });
}
construirGrid();

// ═══════════════════════════════════════════════════════════════
//  WEBSOCKET -- CONEXION Y DESCONEXION
// ═══════════════════════════════════════════════════════════════

/**
 * Abre la conexion WebSocket con D2 (ws://192.168.4.1/ws).
 * Registra los cuatro handlers: onopen, onmessage, onclose, onerror.
 */
function conectar() {
  if (ws && ws.readyState === WebSocket.OPEN) return;
  desconectandoVoluntariamente = false;
  setBadge('conectando');
  mostrarBanner('info', 'Conectando a D2...');
  document.getElementById('btn-connect').disabled = true;

  console.log('[WS] Abriendo conexion -> ' + WS_URL);
  ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    console.log('[WS] Conexion establecida con D2');
    setBadge('conectado');
    ocultarBanner();
    habilitarControles(true);
    iniciarPing();
    mostrarBotonDesconectar(true);
  };

  ws.onmessage = (evt) => {
    // El log usa formatearJsonParaLog para mostrar "crc16" en hexadecimal,
    // directamente comparable con el monitor serial de D2. El JSON.parse()
    // sigue operando sobre evt.data original (formato estandar, sin tocar).
    console.log('[WS] RX crudo: ' + formatearJsonParaLog(evt.data));
    try {
      const msg = JSON.parse(evt.data);
      procesarMensaje(msg);
    } catch(e) {
      console.error('[WS] JSON invalido:', evt.data);
    }
  };

  ws.onclose = (evt) => {
    console.log('[WS] Conexion cerrada -- code=' + evt.code +
                ' reason="' + evt.reason +
                '" voluntario=' + desconectandoVoluntariamente);
    if (!desconectandoVoluntariamente) {
      manejarDesconexionAbrupta();
    } else {
      manejarDesconexionVoluntaria();
    }
  };

  ws.onerror = (err) => {
    console.error('[WS] Error de socket:', err);
    manejarDesconexionAbrupta();
  };
}

/**
 * Solicita desconexion voluntaria enviando CMD_DESCONECTAR a D2 (con CRC-16).
 *
 * Handshake de desconexion limpia (Graceful Shutdown):
 *  1. D3 envia CMD_DESCONECTAR con CRC-16 y activa desconectandoVoluntariamente.
 *  2. D2 valida el CRC, responde ACK_DESCONECTAR y llama a client->close(1000).
 *  3. El servidor inicia el cierre TCP; el navegador recibe el frame CLOSE.
 *  4. ws.onclose() se dispara con desconectandoVoluntariamente=true ->
 *     se ejecuta manejarDesconexionVoluntaria() (banner informativo, no de error).
 *  5. D2 dispara WS_EVT_DISCONNECT de forma deterministica y actualiza sus LEDs
 *     de estado de inmediato, sin esperar el timeout del heartbeat.
 *
 * Sin este handshake, ws.close() desde el navegador dejaba la sesion en
 * estado semiabierto en ESPAsyncWebServer durante varios segundos, retrasando
 * la actualizacion visual de los LEDs de D2.
 */
function desconectar() {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  desconectandoVoluntariamente = true;
  console.log('[WS] Iniciando Graceful Shutdown -> enviando CMD_DESCONECTAR');
  enviar({ tipo: 'CMD_DESCONECTAR' });
  // El cierre real lo inicia D2 via client->close(1000); no cerrar el socket aqui.
}

/** Gestiona una desconexion abrupta (perdida de red, timeout de PONG, o error de socket). */
function manejarDesconexionAbrupta() {
  console.warn('[WS] Desconexion abrupta detectada');
  detenerPing();
  setBadge('desconectado');
  habilitarControles(false);
  mostrarBotonDesconectar(false);
  mostrarBanner('error', 'Conexion perdida con D2. Presiona "Reconectar".');
  actualizarBotonConectar(true);
  esperandoRespuesta = false;
  ws = null;
}

/** Gestiona el cierre voluntario tras el handshake CMD_DESCONECTAR / ACK_DESCONECTAR. */
function manejarDesconexionVoluntaria() {
  console.log('[WS] Desconexion voluntaria completada (Graceful Shutdown OK)');
  detenerPing();
  setBadge('desconectado');
  habilitarControles(false);
  mostrarBotonDesconectar(false);
  mostrarBanner('info', 'Desconectado de D2. Presiona "Conectar" para reconectar.');
  actualizarBotonConectar(true);
  esperandoRespuesta           = false;
  desconectandoVoluntariamente = false;
  ws = null;
}

// ═══════════════════════════════════════════════════════════════
//  KEEPALIVE (PING / PONG a nivel aplicacion)
//
//  La API WebSocket del navegador no expone los frames de ping/pong
//  del protocolo RFC 6455. Se implementa un mecanismo equivalente a
//  nivel de aplicacion: D3 envia {"tipo":"PING","crc16":n} cada
//  PING_MS ms; D2 responde {"tipo":"PONG","crc16":n}. Si el PONG no
//  llega en PONG_WAIT ms, se considera la conexion muerta y se cierra.
// ═══════════════════════════════════════════════════════════════

function iniciarPing() {
  detenerPing();
  pingTimer = setInterval(() => {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    // Sin log de "Enviando PING": es trafico rutinario de heartbeat, no
    // un mensaje de comando. El warning de timeout SI se mantiene, porque
    // representa un fallo real de conexion (no trafico normal PING-PONG).
    enviar({ tipo: 'PING' });
    pongTimer = setTimeout(() => {
      console.warn('[PING] Timeout de PONG -- conexion muerta, cerrando socket');
      ws.close();
    }, PONG_WAIT);
  }, PING_MS);
}

function detenerPing() {
  clearInterval(pingTimer);
  clearTimeout(pongTimer);
}

// ═══════════════════════════════════════════════════════════════
//  PROCESAMIENTO DE MENSAJES ENTRANTES (D2 -> D3)
// ═══════════════════════════════════════════════════════════════

/**
 * Procesa un mensaje JSON recibido de D2.
 *
 * Flujo de validacion y despacho:
 *  1. Validar CRC-16 (validarCRC). Si falla -> log, setCrcStatus(false), retornar.
 *  2. Actualizar indicador visual de CRC (setCrcStatus) -- SOLO para mensajes
 *     de comando; PING/PONG nunca tocan la GUI ni la consola con un log de
 *     "mensaje procesado", para no saturar la interfaz con el heartbeat.
 *  3. Despachar por campo "tipo":
 *     - PONG            -> cancelar timer de timeout del keepalive (sin logs).
 *     - CRC_ERROR       -> D2 rechazo un mensaje de D3 (banner de aviso).
 *     - ACK_DESCONECTAR -> D2 acepto CMD_DESCONECTAR; D3 cierra el socket.
 *     - ESTADO_ACTUAL   -> actualizar UI con el estado al conectar.
 *     - RESULTADO_CMD   -> actualizar UI con el resultado del ciclo IR.
 *
 * Para todo mensaje de comando, el indicador #crc-status muestra el CRC-16
 * recibido de D2 (msg.crc16) junto con el CRC-16 que D3 envio en el comando
 * que disparo esa respuesta (ultimoCrcTxComando), permitiendo verificar a
 * simple vista que ambos extremos calculan el mismo valor sobre el mismo
 * payload. El log detallado de la trama JSON completa permanece solo en la
 * consola del navegador (F12); la GUI en si solo expone exito/fracaso + CRC.
 *
 * @param {Object} msg  Objeto JS parseado del mensaje WebSocket recibido.
 */
function procesarMensaje(msg) {
  const esKeepalive = (msg.tipo === 'PONG');

  if (!validarCRC(msg)) {
    if (!esKeepalive) {
      setCrcStatus(false, 'CRC-16 invalido en "' + (msg.tipo || '?') + '"');
    }
    console.error('[CRC-16] Mensaje descartado por CRC invalido:', msg);
    return;
  }

  // El indicador de la GUI y el log de "RX procesado" se omiten para PONG:
  // es trafico de heartbeat, no un mensaje de comando que el usuario deba ver.
  if (!esKeepalive) {
    console.log('[WS] RX procesado: tipo=' + msg.tipo, msg);
  }

  // Respuesta al keepalive: cancelar timeout de PONG (sin tocar la GUI)
  if (msg.tipo === 'PONG') {
    clearTimeout(pongTimer);
    return;
  }

  // D2 rechazo un mensaje nuestro por CRC-16 invalido
  if (msg.tipo === 'CRC_ERROR') {
    console.warn('[CRC-16] D2 reporto CRC_ERROR:', msg.info);
    setCrcStatus(false, 'CRC_ERROR · tx=' + crcHex(ultimoCrcTxComando) +
                        '  rx=' + crcHex(msg.crc16));
    mostrarBanner('warn', 'D2 rechazo un mensaje: ' + (msg.info || 'CRC invalido'));
    esperandoRespuesta = false;
    return;
  }

  // D2 confirmo CMD_DESCONECTAR; ahora D3 cierra el socket (codigo 1000 = normal)
  if (msg.tipo === 'ACK_DESCONECTAR') {
    console.log('[WS] ACK_DESCONECTAR recibido -- cerrando socket desde D3');
    setCrcStatus(true, 'ACK_DESCONECTAR · tx=' + crcHex(ultimoCrcTxComando) +
                       '  rx=' + crcHex(msg.crc16));
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.close(1000, 'Cierre voluntario iniciado por D3');
    }
    return;
  }

  // Estado inicial enviado por D2 al conectarse D3
  if (msg.tipo === 'ESTADO_ACTUAL') {
    console.log('[WS] ESTADO_ACTUAL -- encendido=' + msg.encendido +
                '  color=' + msg.color +
                '  RGB(' + msg.r + ',' + msg.g + ',' + msg.b + ')');
    estadoActual = { encendido: msg.encendido, color: msg.color,
                     r: msg.r, g: msg.g, b: msg.b };
    actualizarUI();
    esperandoRespuesta = false;
    // No hubo comando previo de D3 (es el push automatico al conectar),
    // por lo que solo se muestra el CRC-16 recibido de D2.
    setCrcStatus(true, 'ESTADO_ACTUAL · rx=' + crcHex(msg.crc16));
    return;
  }

  // Resultado del ciclo IR hacia D1
  if (msg.tipo === 'RESULTADO_CMD') {
    console.log('[WS] RESULTADO_CMD -- exito=' + msg.exito +
                '  encendido=' + msg.encendido +
                '  color=' + msg.color +
                '  RGB(' + msg.r + ',' + msg.g + ',' + msg.b + ')');
    estadoActual = { encendido: msg.encendido, color: msg.color,
                     r: msg.r, g: msg.g, b: msg.b };
    actualizarUI();
    esperandoRespuesta = false;

    // Indicador de CRC: muestra el par tx (comando que disparo esta
    // respuesta) / rx (esta misma respuesta), para verificar a simple
    // vista que ambos extremos calcularon el mismo CRC-16 sobre el
    // payload correspondiente.
    setCrcStatus(true, (msg.exito ? 'OK' : 'SIN ACK') +
                       ' · tx=' + crcHex(ultimoCrcTxComando) +
                       '  rx=' + crcHex(msg.crc16));

    if (msg.exito) {
      mostrarBanner('info', 'Comando aplicado -- ' + (msg.color || 'APAGADO') +
                    '  RGB(' + msg.r + ', ' + msg.g + ', ' + msg.b + ')');
      setTimeout(ocultarBanner, 2500);
    } else {
      mostrarBanner('error', 'D1 no confirmo el comando. Sin cambios.');
    }
    return;
  }

  console.warn('[WS] Tipo de mensaje desconocido:', msg.tipo);
}

/**
 * Formatea un CRC-16 numerico como string hexadecimal "0xXXXX" para mostrar
 * en la GUI. Devuelve "—" si el valor es null/undefined (por ejemplo, antes
 * de que D3 haya enviado su primer comando en la sesion).
 *
 * @param {number|null} crc  Valor de CRC-16 a formatear.
 * @returns {string}         Representacion hexadecimal de 4 digitos, o "—".
 */
function crcHex(crc) {
  if (crc === null || crc === undefined) return '—';
  return '0x' + crc.toString(16).toUpperCase().padStart(4, '0');
}

// ═══════════════════════════════════════════════════════════════
//  COMANDOS D3 -> D2 (con CRC-16)
// ═══════════════════════════════════════════════════════════════

/**
 * Serializa obj con CRC-16 y lo envia por WebSocket.
 *
 * Throttle: mientras esperandoRespuesta sea true, solo se permiten
 * PING y CMD_DESCONECTAR (que no inician un ciclo IR en D2).
 *
 * El CRC-16 calculado se guarda en ultimoCrcTxComando para los mensajes
 * de comando (no PING/PONG), de forma que la GUI pueda mostrarlo junto
 * al CRC-16 recibido en la respuesta y permitir la verificacion visual
 * de que ambos extremos calcularon el mismo valor.
 *
 * @param {Object} obj  Comando a enviar (sin crc16; se agrega aqui).
 */
function enviar(obj) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  if (esperandoRespuesta &&
      obj.tipo !== 'PING' &&
      obj.tipo !== 'CMD_DESCONECTAR') {
    console.warn('[WS] TX bloqueado (comando en vuelo): tipo=' + obj.tipo);
    return;
  }

  const esKeepalive = (obj.tipo === 'PING');
  const json = serializarConCRC(obj);   // obj.crc16 queda asignado in-place

  // El log crudo de TX se omite para PING: no es un mensaje de comando.
  // formatearJsonParaLog muestra "crc16" en hex, comparable con D2.
  if (!esKeepalive) {
    console.log('[WS] TX: ' + formatearJsonParaLog(json));
    ultimoCrcTxComando = obj.crc16;
  }

  ws.send(json);

  if (obj.tipo !== 'PING' && obj.tipo !== 'CMD_DESCONECTAR') {
    esperandoRespuesta = true;
    mostrarBanner('info', 'Enviando comando a D1...');
  }
}

/**
 * Envia CMD_COLOR con nombre simbolico y triplete RGB numerico.
 *
 * El objeto de comando incluye "r", "g", "b" explicitamente ademas de
 * "color" (nombre simbolico). Esto permite que el CRC-16 cubra los valores
 * numericos del color, garantizando integridad completa del triplete
 * transmitido y no solo del string del nombre.
 *
 * @param {Object} c  Objeto de color de COLORES[]: { nombre, r, g, b }
 */
function cmdColor(c) {
  console.log('[CMD] CMD_COLOR -- nombre=' + c.nombre +
              '  RGB(' + c.r + ', ' + c.g + ', ' + c.b + ')');
  enviar({ tipo: 'CMD_COLOR', color: c.nombre, r: c.r, g: c.g, b: c.b });
}

/** Envia CMD_ENCENDER para encender el LED con el ultimo color de D2. */
function cmdEncender() {
  console.log('[CMD] CMD_ENCENDER');
  enviar({ tipo: 'CMD_ENCENDER' });
}

/** Envia CMD_APAGAR para apagar el LED (D2 enviara RGB = 0,0,0 a D1). */
function cmdApagar() {
  console.log('[CMD] CMD_APAGAR');
  enviar({ tipo: 'CMD_APAGAR' });
}

// ═══════════════════════════════════════════════════════════════
//  ACTUALIZACION DE LA UI
// ═══════════════════════════════════════════════════════════════

/**
 * Sincroniza la interfaz visual con el estado actual recibido de D2.
 * Actualiza: preview del color, chips RGB numericos, boton de color activo.
 */
function actualizarUI() {
  const { encendido, color, r, g, b } = estadoActual;
  console.log('[UI] Actualizando -- encendido=' + encendido +
              '  color=' + color + '  RGB(' + r + ',' + g + ',' + b + ')');

  const preview = document.getElementById('preview');
  const span    = preview.querySelector('span');

  if (encendido) {
    preview.classList.remove('apagado');
    preview.style.background = 'rgb(' + r + ',' + g + ',' + b + ')';
    span.dataset.nombre = color || '';
  } else {
    preview.classList.add('apagado');
    preview.style.background = '';
    span.dataset.nombre = '';
  }

  // Chips numericos (muestran "—" cuando esta apagado)
  document.getElementById('val-r').textContent = encendido ? r : '—';
  document.getElementById('val-g').textContent = encendido ? g : '—';
  document.getElementById('val-b').textContent = encendido ? b : '—';

  // Resaltar el boton del color activo
  COLORES.forEach(c => {
    const btn = document.getElementById('cbtn-' + c.nombre);
    if (btn) btn.classList.toggle('activo', encendido && c.nombre === color);
  });
}

// ═══════════════════════════════════════════════════════════════
//  HELPERS DE UI
// ═══════════════════════════════════════════════════════════════

/** Actualiza el badge de estado de conexion en el encabezado. */
function setBadge(estado) {
  const b = document.getElementById('badge');
  b.className   = estado;
  b.textContent = {
    conectado:    'Conectado',
    desconectado: 'Desconectado',
    conectando:   'Conectando...'
  }[estado] || estado;
}

/** Habilita o deshabilita todos los controles de color y encendido. */
function habilitarControles(on) {
  ['btn-on','btn-off'].forEach(id => {
    document.getElementById(id).disabled = !on;
  });
  document.querySelectorAll('.color-btn').forEach(b => { b.disabled = !on; });
}

/**
 * Muestra u oculta el boton de desconexion voluntaria.
 * Cuando esta visible, el boton "Conectar" se deshabilita para evitar
 * abrir una segunda sesion WS simultanea.
 */
function mostrarBotonDesconectar(visible) {
  const disc    = document.getElementById('btn-disconnect');
  const connect = document.getElementById('btn-connect');
  disc.style.display = visible ? 'block' : 'none';
  connect.disabled   = visible;
}

/** Actualiza texto y estilo del boton de conexion entre "Conectar" y "Reconectar". */
function actualizarBotonConectar(reconectar) {
  const b = document.getElementById('btn-connect');
  b.disabled    = false;
  b.textContent = reconectar ? 'Reconectar a D2' : 'Conectar a D2';
  b.classList.toggle('reconectar', reconectar);
}

/** Muestra el banner de notificaciones con el tipo indicado ('info', 'error', 'warn'). */
function mostrarBanner(tipo, texto) {
  const b = document.getElementById('banner');
  b.className     = tipo;
  b.textContent   = texto;
  b.style.display = 'block';
}

/** Oculta el banner de notificaciones. */
function ocultarBanner() {
  const b = document.getElementById('banner');
  b.className     = '';
  b.style.display = 'none';
}

/**
 * Actualiza el indicador de integridad CRC visible en la UI.
 *
 * Desde la refactorizacion de logs, el texto recibido ya incluye el par
 * "tx=0xXXXX  rx=0xXXXX" (CRC-16 enviado por D3 en el comando / CRC-16
 * recibido en la respuesta de D2), permitiendo verificar visualmente que
 * ambos extremos calcularon el mismo valor. Nunca se llama para PING/PONG.
 *
 * @param {boolean} ok     true = CRC valido, false = error de integridad.
 * @param {string}  texto  Descripcion del estado (resultado + CRC tx/rx).
 */
function setCrcStatus(ok, texto) {
  const el = document.getElementById('crc-status');
  el.className   = ok ? 'ok' : 'err';
  el.textContent = ok ? ('✓ ' + texto) : ('✗ ' + texto);
}
</script>
</body>
</html>
)rawhtml";