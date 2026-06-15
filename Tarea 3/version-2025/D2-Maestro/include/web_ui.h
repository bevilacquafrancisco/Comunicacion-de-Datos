/**
 * @file    web_ui.h
 * @brief   Interfaz web del Dispositivo 3 — embebida en flash del ESP32.
 *
 * La página completa (HTML + CSS + JavaScript) se almacena como un string
 * literal en flash (PROGMEM implícito para const en ESP32) y es servida
 * por ESPAsyncWebServer en la ruta raíz ("/").
 *
 * @section arquitectura Arquitectura de la interfaz
 *
 *  - HTML semántico, una sola página (SPA mínima).
 *  - CSS con variables custom para theming; sin frameworks externos.
 *  - JavaScript vanilla con API WebSocket nativa del navegador.
 *  - Compatibilidad: cualquier navegador moderno (Chrome, Firefox,
 *    Safari, Edge) en Android, iOS, Windows, macOS, Linux.
 *  - Sin dependencias externas: funciona sin acceso a Internet,
 *    lo que es crítico dado que D2 opera como AP aislado.
 *
 * @section protocolo Protocolo WebSocket utilizado
 *
 *  D3 → D2 (comandos):
 *   {"tipo":"CMD_COLOR",    "color":"VERDE"}
 *   {"tipo":"CMD_ENCENDER"}
 *   {"tipo":"CMD_APAGAR"}
 *
 *  D2 → D3 (respuestas):
 *   {"tipo":"ESTADO_ACTUAL", "encendido":true, "color":"VERDE", "r":0,"g":255,"b":0}
 *   {"tipo":"RESULTADO_CMD", "exito":true,     "encendido":true, "color":"VERDE","r":0,"g":255,"b":0}
 *
 * @authors Bevilacqua, Francisco — Clement, Sebastián
 * @date    7 de junio 2026
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
    --bg:        #0d0d0f;
    --surface:   #18181c;
    --border:    #2a2a30;
    --text:      #e8e8ed;
    --muted:     #6b6b78;
    --accent:    #7c6af7;
    --conn:      #34d399;
    --disc:      #f87171;
    --warn:      #fbbf24;
    --radius:    14px;
    --shadow:    0 4px 32px rgba(0,0,0,.45);
    --font:      'Courier New', 'Lucida Console', monospace;
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
    width: 100%;
    max-width: 480px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    border-bottom: 1px solid var(--border);
    padding-bottom: 14px;
  }
  header h1 {
    font-size: 1rem;
    letter-spacing: .12em;
    text-transform: uppercase;
    color: var(--muted);
  }
  header h1 span { color: var(--accent); }

  /* ── Badge de conexión ──────────────────────────────────── */
  #badge {
    font-size: .72rem;
    letter-spacing: .08em;
    text-transform: uppercase;
    padding: 4px 12px;
    border-radius: 99px;
    border: 1px solid currentColor;
    transition: color .3s, border-color .3s;
  }
  #badge.conectado  { color: var(--conn); }
  #badge.desconectado { color: var(--disc); }
  #badge.conectando { color: var(--warn); }

  /* ── Tarjeta principal ──────────────────────────────────── */
  .card {
    width: 100%;
    max-width: 480px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 24px;
    box-shadow: var(--shadow);
  }

  /* ── Preview del color ──────────────────────────────────── */
  .color-preview {
    width: 100%;
    height: 110px;
    border-radius: 10px;
    border: 1px solid var(--border);
    transition: background .5s ease;
    display: flex;
    align-items: center;
    justify-content: center;
    margin-bottom: 18px;
    position: relative;
    overflow: hidden;
  }
  .color-preview span {
    font-size: .78rem;
    letter-spacing: .14em;
    text-transform: uppercase;
    font-weight: 700;
    text-shadow: 0 1px 4px rgba(0,0,0,.6);
    opacity: .85;
  }
  .color-preview.apagado { background: #111113 !important; }
  .color-preview.apagado span::before { content: "· APAGADO ·"; }
  .color-preview:not(.apagado) span::before { content: attr(data-nombre); }

  /* ── Info RGB ───────────────────────────────────────────── */
  .rgb-info {
    display: flex;
    gap: 10px;
    margin-bottom: 22px;
  }
  .rgb-info .chip {
    flex: 1;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 8px 6px;
    text-align: center;
    font-size: .75rem;
  }
  .rgb-info .chip .label { color: var(--muted); font-size: .65rem; letter-spacing: .1em; }
  .rgb-info .chip .val   { font-size: 1rem; margin-top: 2px; }
  #chip-r .val { color: #f87171; }
  #chip-g .val { color: #34d399; }
  #chip-b .val { color: #60a5fa; }

  /* ── Controles power ────────────────────────────────────── */
  .power-row {
    display: flex;
    gap: 10px;
    margin-bottom: 22px;
  }
  .btn {
    flex: 1;
    padding: 13px 0;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--bg);
    color: var(--text);
    font-family: var(--font);
    font-size: .78rem;
    letter-spacing: .1em;
    text-transform: uppercase;
    cursor: pointer;
    transition: background .2s, border-color .2s, opacity .2s, transform .1s;
  }
  .btn:active { transform: scale(.97); }
  .btn:disabled { opacity: .3; cursor: not-allowed; transform: none; }
  .btn.on  { border-color: var(--conn); color: var(--conn); }
  .btn.off { border-color: var(--disc); color: var(--disc); }
  .btn.on:hover  { background: rgba(52,211,153,.08); }
  .btn.off:hover { background: rgba(248,113,113,.08); }

  /* ── Grid de colores ────────────────────────────────────── */
  .color-section-title {
    font-size: .68rem;
    letter-spacing: .14em;
    text-transform: uppercase;
    color: var(--muted);
    margin-bottom: 12px;
  }
  .color-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 8px;
  }
  .color-btn {
    aspect-ratio: 1;
    border-radius: 10px;
    border: 2px solid transparent;
    cursor: pointer;
    transition: transform .15s, border-color .2s, box-shadow .2s;
    position: relative;
    overflow: hidden;
  }
  .color-btn:hover  { transform: scale(1.08); }
  .color-btn:active { transform: scale(.95); }
  .color-btn:disabled { opacity: .3; cursor: not-allowed; transform: none; }
  .color-btn.activo {
    border-color: white;
    box-shadow: 0 0 14px rgba(255,255,255,.35);
  }
  .color-btn .cn {
    position: absolute;
    bottom: 4px;
    left: 0; right: 0;
    text-align: center;
    font-size: .58rem;
    letter-spacing: .06em;
    text-transform: uppercase;
    text-shadow: 0 1px 3px rgba(0,0,0,.8);
    color: #fff;
  }

  /* ── Banner de alerta ───────────────────────────────────── */
  #banner {
    width: 100%;
    max-width: 480px;
    border-radius: 10px;
    padding: 14px 18px;
    font-size: .8rem;
    letter-spacing: .06em;
    display: none;
    border: 1px solid;
  }
  #banner.error {
    display: block;
    background: rgba(248,113,113,.08);
    border-color: var(--disc);
    color: var(--disc);
  }
  #banner.info {
    display: block;
    background: rgba(124,106,247,.08);
    border-color: var(--accent);
    color: var(--accent);
  }

  /* ── Botón de conexión ──────────────────────────────────── */
  #btn-connect {
    width: 100%;
    max-width: 480px;
  }
  #btn-connect.reconectar {
    border-color: var(--accent);
    color: var(--accent);
  }
  #btn-connect.reconectar:hover { background: rgba(124,106,247,.1); }

  /* ── Footer ─────────────────────────────────────────────── */
  footer {
    font-size: .65rem;
    color: var(--muted);
    letter-spacing: .08em;
    text-align: center;
    margin-top: auto;
  }
</style>
</head>
<body>

<header>
  <h1>TP3 · <span>RGB</span> Control</h1>
  <div id="badge" class="desconectado">Desconectado</div>
</header>

<!-- Tarjeta de estado y control -->
<div class="card">

  <!-- Preview del color actual -->
  <div class="color-preview apagado" id="preview">
    <span data-nombre=""></span>
  </div>

  <!-- Chips RGB numéricos -->
  <div class="rgb-info">
    <div class="chip" id="chip-r">
      <div class="label">R</div>
      <div class="val" id="val-r">—</div>
    </div>
    <div class="chip" id="chip-g">
      <div class="label">G</div>
      <div class="val" id="val-g">—</div>
    </div>
    <div class="chip" id="chip-b">
      <div class="label">B</div>
      <div class="val" id="val-b">—</div>
    </div>
  </div>

  <!-- Botones encender / apagar -->
  <div class="power-row">
    <button class="btn on"  id="btn-on"  disabled onclick="cmdEncender()">▶ Encender</button>
    <button class="btn off" id="btn-off" disabled onclick="cmdApagar()">■ Apagar</button>
  </div>

  <!-- Grid de selección de color -->
  <div class="color-section-title">Seleccionar color</div>
  <div class="color-grid" id="color-grid">
    <!-- Generado por JS -->
  </div>
</div>

<!-- Banner de notificaciones -->
<div id="banner"></div>

<!-- Botón conectar / reconectar -->
<button class="btn" id="btn-connect" onclick="conectar()">Conectar a D2</button>

<footer>CdD · TP3 Integrador · Bevilacqua &amp; Clement · Junio 2026</footer>

<script>
// ═══════════════════════════════════════════════════════════════
//  CONSTANTES
// ═══════════════════════════════════════════════════════════════

const COLORES = [
  { nombre:"ROJO",     r:255, g:0,   b:0   },
  { nombre:"AMARILLO", r:255, g:200, b:0   },
  { nombre:"VERDE",    r:0,   g:255, b:0   },
  { nombre:"CELESTE",  r:0,   g:255, b:255 },
  { nombre:"AZUL",     r:0,   g:0,   b:255 },
  { nombre:"LILA",     r:180, g:0,   b:255 },
  { nombre:"BLANCO",   r:255, g:255, b:255 },
  { nombre:"ROSA",     r:255, g:105, b:180 },
];

const WS_URL     = `ws://${location.hostname}/ws`;
const PING_MS    = 5000;   // intervalo de keepalive
const PONG_WAIT  = 3000;   // timeout para considerar conexión muerta

// ═══════════════════════════════════════════════════════════════
//  ESTADO DE LA APLICACIÓN
// ═══════════════════════════════════════════════════════════════

let ws           = null;
let pingTimer    = null;
let pongTimer    = null;
let estadoActual = { encendido: false, color: null, r: 0, g: 0, b: 0 };
let esperandoRespuesta = false;   // bloqueo mientras hay un comando en vuelo

// ═══════════════════════════════════════════════════════════════
//  CONSTRUCCIÓN DE LA UI
// ═══════════════════════════════════════════════════════════════

function construirGrid() {
  const grid = document.getElementById('color-grid');
  COLORES.forEach(c => {
    const btn = document.createElement('button');
    btn.className  = 'color-btn';
    btn.id         = `cbtn-${c.nombre}`;
    btn.disabled   = true;
    btn.style.background = `rgb(${c.r},${c.g},${c.b})`;
    btn.title = c.nombre;
    btn.innerHTML  = `<span class="cn">${c.nombre}</span>`;
    btn.onclick    = () => cmdColor(c.nombre);
    grid.appendChild(btn);
  });
}
construirGrid();

// ═══════════════════════════════════════════════════════════════
//  WEBSOCKET
// ═══════════════════════════════════════════════════════════════

function conectar() {
  if (ws && ws.readyState === WebSocket.OPEN) return;
  setBadge('conectando');
  mostrarBanner('info', 'Conectando a D2...');
  document.getElementById('btn-connect').disabled = true;

  ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    console.log('[WS] Conexión establecida');
    setBadge('conectado');
    ocultarBanner();
    habilitarControles(true);
    iniciarPing();
    actualizarBotonConectar(false);
  };

  ws.onmessage = (evt) => {
    try {
      const msg = JSON.parse(evt.data);
      // Respuesta al ping de keepalive: no procesar como mensaje normal
      if (msg.tipo === 'PONG') { clearTimeout(pongTimer); return; }
      procesarMensaje(msg);
    } catch(e) {
      console.error('[WS] JSON inválido:', evt.data);
    }
  };

  ws.onclose = (evt) => {
    console.warn('[WS] Conexión cerrada:', evt.code, evt.reason);
    manejarDesconexion();
  };

  ws.onerror = (err) => {
    console.error('[WS] Error:', err);
    manejarDesconexion();
  };
}

function manejarDesconexion() {
  detenerPing();
  setBadge('desconectado');
  habilitarControles(false);
  mostrarBanner('error', '⚠ Conexión perdida con D2. Presioná "Reconectar" para intentar de nuevo.');
  actualizarBotonConectar(true);
  esperandoRespuesta = false;
  ws = null;
}

// ═══════════════════════════════════════════════════════════════
//  KEEPALIVE (ping / pong a nivel aplicación)
// ═══════════════════════════════════════════════════════════════

function iniciarPing() {
  detenerPing();
  pingTimer = setInterval(() => {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({ tipo: 'PING' }));
    // Si no llega respuesta en PONG_WAIT ms, consideramos la conexión muerta
    pongTimer = setTimeout(() => {
      console.warn('[WS] Timeout de pong — cerrando');
      ws.close();
    }, PONG_WAIT);
  }, PING_MS);
}

function detenerPing() {
  clearInterval(pingTimer);
  clearTimeout(pongTimer);
}

// ═══════════════════════════════════════════════════════════════
//  PROCESAMIENTO DE MENSAJES ENTRANTES
// ═══════════════════════════════════════════════════════════════

function procesarMensaje(msg) {
  console.log('[WS] Recibido:', msg);

  if (msg.tipo === 'ESTADO_ACTUAL' || msg.tipo === 'RESULTADO_CMD') {
    estadoActual = {
      encendido: msg.encendido,
      color:     msg.color,
      r:         msg.r,
      g:         msg.g,
      b:         msg.b
    };
    actualizarUI();
    esperandoRespuesta = false;

    if (msg.tipo === 'RESULTADO_CMD') {
      if (msg.exito) {
        mostrarBanner('info', `✓ Comando aplicado — ${msg.color || 'APAGADO'}`);
        setTimeout(ocultarBanner, 2500);
      } else {
        mostrarBanner('error', '✗ D1 no confirmó el comando. Sin cambios.');
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  COMANDOS ENVIADOS A D2
// ═══════════════════════════════════════════════════════════════

function enviar(obj) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  if (esperandoRespuesta) return;   // throttle: un comando a la vez
  esperandoRespuesta = true;
  ws.send(JSON.stringify(obj));
  mostrarBanner('info', '⟳ Enviando comando a D1...');
}

function cmdColor(nombre)  { enviar({ tipo: 'CMD_COLOR', color: nombre }); }
function cmdEncender()     { enviar({ tipo: 'CMD_ENCENDER' }); }
function cmdApagar()       { enviar({ tipo: 'CMD_APAGAR'   }); }

// ═══════════════════════════════════════════════════════════════
//  ACTUALIZACIÓN DE LA UI
// ═══════════════════════════════════════════════════════════════

function actualizarUI() {
  const { encendido, color, r, g, b } = estadoActual;
  const preview = document.getElementById('preview');
  const span    = preview.querySelector('span');

  // Preview del color
  if (encendido) {
    preview.classList.remove('apagado');
    preview.style.background = `rgb(${r},${g},${b})`;
    span.dataset.nombre = color || '';
  } else {
    preview.classList.add('apagado');
    preview.style.background = '';
    span.dataset.nombre = '';
  }

  // Chips numéricos
  document.getElementById('val-r').textContent = encendido ? r : '—';
  document.getElementById('val-g').textContent = encendido ? g : '—';
  document.getElementById('val-b').textContent = encendido ? b : '—';

  // Resaltar botón de color activo
  COLORES.forEach(c => {
    const btn = document.getElementById(`cbtn-${c.nombre}`);
    if (btn) btn.classList.toggle('activo', encendido && c.nombre === color);
  });
}

// ═══════════════════════════════════════════════════════════════
//  HELPERS DE UI
// ═══════════════════════════════════════════════════════════════

function setBadge(estado) {
  const b = document.getElementById('badge');
  b.className = estado;
  b.textContent = {
    conectado:    'Conectado',
    desconectado: 'Desconectado',
    conectando:   'Conectando...'
  }[estado] || estado;
}

function habilitarControles(habilitar) {
  ['btn-on','btn-off'].forEach(id => {
    document.getElementById(id).disabled = !habilitar;
  });
  document.querySelectorAll('.color-btn').forEach(b => {
    b.disabled = !habilitar;
  });
}

function actualizarBotonConectar(mostrar) {
  const b = document.getElementById('btn-connect');
  b.disabled  = false;
  if (mostrar) {
    b.textContent = 'Reconectar a D2';
    b.classList.add('reconectar');
  } else {
    b.textContent = 'Conectado';
    b.disabled    = true;
    b.classList.remove('reconectar');
  }
}

function mostrarBanner(tipo, texto) {
  const b = document.getElementById('banner');
  b.className   = tipo;
  b.textContent = texto;
}

function ocultarBanner() {
  const b = document.getElementById('banner');
  b.className   = '';
  b.style.display = 'none';
}
</script>
</body>
</html>
)rawhtml";
