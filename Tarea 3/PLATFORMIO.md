# PlatformIO + VS Code: Guía de Entorno para Desarrollo Embebido

> Documentación del entorno de desarrollo utilizado en el **TP3 Integrador — Comunicación de Datos**  
> Ingeniería en Computación | Bevilacqua, Francisco — Clement, Sebastián | Junio 2026

---

## Índice

1. [¿Qué es PlatformIO?](#1-qué-es-platformio)
2. [Características y ventajas clave](#2-características-y-ventajas-clave)
3. [Gestión de librerías y dependencias](#3-gestión-de-librerías-y-dependencias)
4. [El archivo `platformio.ini`](#4-el-archivo-platformioini)
5. [Requisitos previos](#5-requisitos-previos)
6. [Instalación de VS Code y PlatformIO](#6-instalación-de-vs-code-y-platformio)
7. [Abrir los proyectos de este repositorio](#7-abrir-los-proyectos-de-este-repositorio)
8. [Compilar el firmware](#8-compilar-el-firmware)
9. [Cargar el firmware en el ESP32](#9-cargar-el-firmware-en-el-esp32)
10. [Monitor Serial](#10-monitor-serial)
11. [Solución de problemas frecuentes](#11-solución-de-problemas-frecuentes)
12. [Referencia rápida de atajos](#12-referencia-rápida-de-atajos)

---

## 1. ¿Qué es PlatformIO?

**PlatformIO** es un ecosistema de desarrollo profesional para sistemas embebidos, distribuido como extensión de VS Code (y otros IDEs). A diferencia de entornos dedicados a una sola plataforma (como Arduino IDE, que nació orientado exclusivamente al ecosistema Arduino/AVR), PlatformIO es **agnóstico de plataforma**: soporta más de 1.500 placas de desarrollo distintas —Arduino, ESP32, STM32, Raspberry Pi Pico, Nordic nRF, RISC-V y muchas más— y múltiples frameworks (Arduino, ESP-IDF, Zephyr, CMSIS, etc.), todo desde un único entorno y flujo de trabajo unificado.

Su componente central es el **PlatformIO Core**, una herramienta de línea de comandos en Python que gestiona:

- La descarga e instalación de **toolchains** (compiladores, linkers, herramientas de flasheo) específicas para cada plataforma objetivo.
- La resolución del **árbol de dependencias** de librerías, con soporte de versionado semántico.
- La compilación del proyecto mediante **SCons** (sistema de build basado en Python, más expresivo que Make).
- La carga del firmware al microcontrolador mediante **esptool**, OpenOCD, avrdude u otras herramientas según la plataforma.
- El **Monitor Serial** integrado, con soporte de filtros de post-procesamiento.

Toda esta funcionalidad queda expuesta en VS Code a través de una interfaz gráfica (la extensión PlatformIO IDE) y a través de atajos de teclado, sin necesidad de abrir una terminal para operaciones cotidianas.

---

## 2. Características y ventajas clave

### Proyectos reproducibles

El estado completo de un proyecto PlatformIO —plataforma, board, framework, librerías con versiones exactas— queda declarado en un único archivo de texto: `platformio.ini`. Cualquier persona que clone el repositorio y abra el proyecto obtiene exactamente el mismo entorno de compilación sin instalar nada manualmente. Esto fue determinante para trabajar en pareja en este TP: ambos integrantes compilaban y flasheaban desde sus propias máquinas sin discrepancias de versiones de librerías.

### Gestión de dependencias aislada por proyecto

Cada proyecto descarga sus librerías en `.pio/libdeps/<env>/`, una carpeta local al proyecto. Esto significa que dos proyectos distintos pueden usar versiones diferentes de la misma librería sin conflicto, y que desinstalar una librería de un proyecto no afecta ningún otro. No existe un repositorio global de librerías que pueda corromperse al actualizar una dependencia.

### IntelliSense completo de C++

PlatformIO genera automáticamente la configuración de `c_cpp_properties.json` (necesaria para el motor IntelliSense de VS Code / clangd) incluyendo todos los paths de headers del SDK de la plataforma. El resultado es autocompletado, navegación a definiciones y detección de errores en tiempo real, comparables a los de un proyecto C++ nativo de escritorio.

### Compilación incremental

SCons rastrea dependencias a nivel de archivo objeto: si solo cambia `main.cpp`, solo recompila ese archivo. En proyectos con muchas librerías (como D2, que incluye ESPAsyncWebServer + AsyncTCP + ArduinoJson + IRremote), esto reduce el tiempo de compilación tras una edición de segundos a pocos segundos.

### Soporte de múltiples entornos en un mismo proyecto

Un `platformio.ini` puede definir múltiples entornos `[env:xxx]` con distintas placas, parámetros de compilación o flags. Esto permite, por ejemplo, tener un entorno `debug` (con logs activados) y uno `release` (sin logs) en el mismo proyecto, seleccionando cuál compilar/flashear desde la interfaz.

En este proyecto usamos esta capacidad para mantener `CORE_DEBUG_LEVEL=0` en producción y cambiarlo a `3` durante el desarrollo para ver los logs internos del stack Wi-Fi del ESP32:

```ini
[env:esp32dev]
build_flags =
    -DCORE_DEBUG_LEVEL=0   ; produccion: sin logs del core

; Para depuracion, cambiar a:
; -DCORE_DEBUG_LEVEL=3
```

### Decodificación automática de excepciones

PlatformIO incluye el filtro `esp32_exception_decoder` para el monitor serial. Cuando el ESP32 entra en pánico (Guru Meditation Error), el stack trace normalmente muestra solo direcciones de memoria sin contexto. El decodificador traduce esas direcciones a nombres de función y número de línea del código fuente, haciendo los crashes depurables en segundos.

---

## 3. Gestión de librerías y dependencias

### Declaración en `platformio.ini`

Las librerías se declaran en la clave `lib_deps` del entorno. PlatformIO soporta múltiples formatos de especificación:

```ini
lib_deps =
    ; Por nombre en el registro de PlatformIO (usando alias del autor):
    z3t0/IRremote @ ^4.7.1

    ; Por nombre en el registro con owner/repo:
    me-no-dev/AsyncTCP @ ^1.1.1

    ; Versión exacta (sin ^):
    bblanchon/ArduinoJson @ 7.2.0

    ; Desde un repositorio Git directamente:
    https://github.com/autor/libreria.git#v1.2.3
```

### Semántica de versiones (SemVer)

El operador `^` sigue el estándar SemVer: `^4.7.1` acepta cualquier versión `>= 4.7.1` y `< 5.0.0`. Esto permite recibir correcciones de bugs y features compatibles sin romper la API. En este proyecto fijamos el major para garantizar estabilidad, dado que las librerías de ESP32 frecuentemente tienen cambios de API entre versiones major.

### Instalación automática

La primera vez que se compila el proyecto, PlatformIO descarga todas las dependencias declaradas y sus dependencias transitivas. El resultado se almacena en `.pio/libdeps/`. Esta carpeta **no se versiona** (está en `.gitignore`); quien clone el repositorio las descargará automáticamente al compilar.

### Aislamiento del sistema

Las librerías instaladas por PlatformIO no tocan el sistema operativo ni otros proyectos. Si se actualiza o elimina una librería en un proyecto, los demás proyectos —que tienen sus propias copias en sus respectivos `.pio/`— no se ven afectados. Esto contrasta con Arduino IDE, donde las librerías se instalan globalmente en `~/Arduino/libraries/` y un cambio de versión afecta a todos los sketchs.

### Librerías incluidas en el ESP32 Core

Algunas librerías vienen incluidas en el ESP32 Arduino Core que PlatformIO instala como parte de la plataforma, y no necesitan declararse en `lib_deps`. En este proyecto, `Preferences` (para NVS) y `WiFi` son ejemplos de esto:

```cpp
// No requieren lib_deps; vienen con el ESP32 Core:
#include <Preferences.h>
#include <WiFi.h>
```

---

## 4. El archivo `platformio.ini`

`platformio.ini` es el único archivo de configuración del proyecto. Un ejemplo comentado de los campos más relevantes, tomado del D2 de este TP:

```ini
[env:esp32dev]
; --- Plataforma y board ---
platform  = espressif32      ; SDK del fabricante (Espressif)
board     = esp32dev         ; Perfil de hardware: ESP32 DevKit v1
framework = arduino          ; Framework de programacion (API Arduino sobre ESP-IDF)

; --- Puerto serie ---
monitor_speed = 115200       ; Velocidad del monitor serial (debe coincidir con Serial.begin())
upload_speed  = 921600       ; Velocidad de flasheo (mas rapido que el default de 115200)
upload_port   = COM5         ; Puerto especifico (omitir para deteccion automatica)
monitor_port  = COM5         ; Puerto del monitor serial

; --- Dependencias ---
lib_deps =
    z3t0/IRremote @ ^4.7.1
    me-no-dev/AsyncTCP @ ^1.1.1
    me-no-dev/ESPAsyncWebServer @ ^1.2.3
    bblanchon/ArduinoJson @ ^7.2.0

; --- Flags de compilacion ---
build_flags =
    -DCORE_DEBUG_LEVEL=0     ; 0=silencio, 3=verbose (logs del core ESP32)

; --- Filtros del monitor serial ---
monitor_filters = esp32_exception_decoder
```

Campos clave:

| Campo | Descripción |
|---|---|
| `platform` | SDK de la familia de microcontroladores. `espressif32` descarga el toolchain Xtensa LX6, esptool y los headers del ESP32. |
| `board` | Perfil específico de la placa. Define el tamaño de flash, RAM, frecuencia de CPU y el esquema de particiones por defecto. |
| `framework` | API de programación. `arduino` hace disponible la función `setup()`, `loop()`, y todas las abstracciones del framework Arduino sobre el ESP-IDF de Espressif. |
| `lib_deps` | Lista de dependencias con versiones. PlatformIO las resuelve y descarga automáticamente. |
| `build_flags` | Flags pasados al compilador C++. Permite definir macros de preprocesador o activar optimizaciones. |
| `monitor_filters` | Filtros de post-procesamiento del monitor serial. `esp32_exception_decoder` traduce stack traces de pánico a nombres de función. |

---

## 5. Requisitos previos

### Sistema operativo

Compatible con Windows 10/11, macOS 11+, y Linux (Ubuntu 20.04+, Fedora, Arch y derivados).

### Python 3.x

PlatformIO Core está escrito en Python y requiere Python 3 instalado y en el `PATH`. Verificar con:

```bash
python --version
# o si coexisten Python 2 y 3:
python3 --version
```

Si no está instalado:
- **Windows:** descargar desde https://www.python.org/downloads/ — marcar **"Add Python to PATH"** durante la instalación.
- **macOS:** `brew install python3` o descargar desde python.org.
- **Ubuntu/Debian:** `sudo apt install python3 python3-pip`

### Driver USB del ESP32

El ESP32 DevKit v1 usa un chip USB-Serial para la comunicación con la PC. Los dos chips más comunes son:

**CP2102 (Silicon Labs):**
- Windows 10/11: se instala automáticamente por Windows Update.
- macOS / Linux: generalmente incluido en el kernel. Descargar manualmente desde https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers si no es reconocido.

**CH340:**
- Windows: https://www.wch-ic.com/downloads/CH341SER_EXE.html
- macOS: https://github.com/adrianmihalko/ch340g-ch34g-ch34x-mac-os-x-driver
- Linux: incluido en el kernel (módulo `ch341`).

**Cómo identificar el chip:** revisar la serigrafía del integrado pequeño ubicado cerca del conector USB en la placa.

**Verificar reconocimiento:**
- Windows: Administrador de dispositivos → Puertos (COM y LPT) → `Silicon Labs CP210x` o `USB-SERIAL CH340 (COMx)`.
- macOS: `ls /dev/cu.*` → `/dev/cu.usbserial-XXXX` o `/dev/cu.SLAB_USBtoUART`.
- Linux: `ls /dev/ttyUSB*` → `/dev/ttyUSB0`.

---

## 6. Instalación de VS Code y PlatformIO

### VS Code

Descargar e instalar desde https://code.visualstudio.com/.

- **Windows:** marcar "Agregar al PATH" durante la instalación.
- **macOS:** mover a `/Applications/`.
- **Linux (Debian/Ubuntu):** `sudo dpkg -i code_*.deb` o usar el repositorio oficial de Microsoft.

### PlatformIO IDE (extensión de VS Code)

1. Abrir VS Code.
2. Presionar `Ctrl+Shift+X` para abrir el panel de extensiones.
3. Buscar `PlatformIO IDE` y seleccionar la extensión publicada por **PlatformIO** (logo de hormiga roja).
4. Presionar **Install**.
5. Esperar la descarga del PlatformIO Core y las toolchains iniciales (**5–15 minutos** según la conexión a Internet). El progreso es visible en la barra de estado inferior.
6. Cuando VS Code solicite reinicio, hacer clic en **Restart**.

Después del reinicio, el ícono de PlatformIO (hormiga) aparece en la barra lateral izquierda. Al hacer clic abre el **PlatformIO Home**.

---

## 7. Abrir los proyectos de este repositorio

El repositorio contiene dos proyectos PlatformIO independientes bajo `D1_Esclavo/` y `D2_Maestro/`. Cada uno tiene su propio `platformio.ini` y debe abrirse por separado.

> **Importante:** no abrir la carpeta raíz del repositorio como proyecto. PlatformIO identifica un proyecto por la presencia de `platformio.ini` en la carpeta raíz que se abre.

### Abrir un proyecto

```
File → Open Folder... → seleccionar D1_Esclavo/ (o D2_Maestro/)
```

VS Code detecta el `platformio.ini` automáticamente e inicializa el entorno. En el primer uso puede aparecer el mensaje _"Do you trust the authors of the files in this folder?"_; confirmar con **Yes**.

### Workspace multi-raíz (D1 + D2 simultáneamente)

Para editar ambos proyectos en la misma ventana:

1. Abrir `D1_Esclavo/` como se describió.
2. `File → Add Folder to Workspace...` → seleccionar `D2_Maestro/`.
3. `File → Save Workspace As...` → guardar como `TP3-CdD.code-workspace` en la raíz del repositorio.

En sesiones futuras: `File → Open Workspace from File...` → seleccionar el `.code-workspace`.

---

## 8. Compilar el firmware

### Configuración previa (solo D2)

Antes de compilar el Maestro, editar `D2_Maestro/include/config.h` y completar las credenciales del Access Point:

```cpp
#define AP_SSID     "NombreDeRed"          // SSID de la red Wi-Fi creada por D2
#define AP_PASSWORD "ContraseñaSegura!"    // WPA2, mínimo 12 caracteres
```

### Compilar

```
Ctrl+Alt+B
```

O desde la barra lateral de PlatformIO → **Project Tasks** → `esp32dev` → **Build**.

Salida esperada al compilar exitosamente:

```
Compiling .pio/build/esp32dev/src/main.cpp.o
Linking .pio/build/esp32dev/firmware.elf
RAM:   [=         ]   9.8% (used 32084 bytes from 327680 bytes)
Flash: [====      ]  36.0% (used 471845 bytes from 1310720 bytes)
=== [SUCCESS] Took 12.34 seconds ===
```

La primera compilación tarda más porque PlatformIO descarga las librerías declaradas en `lib_deps` y compila también sus fuentes. Las compilaciones incrementales posteriores solo recompilan los archivos modificados.

---

## 9. Cargar el firmware en el ESP32

### Paso 1 — Conectar el ESP32

Usar un cable **USB con pines de datos** (D+ y D−). Los cables de solo carga no establecen comunicación serial y el puerto no aparece en el sistema.

### Paso 2 — Verificar detección del puerto

- **Windows:** Administrador de dispositivos → Puertos (COM y LPT).
- **Linux:** `ls /dev/ttyUSB*`
- **macOS:** `ls /dev/cu.*`

**Permisos en Linux:** agregar el usuario al grupo `dialout` si aparece un error de acceso denegado:

```bash
sudo usermod -a -G dialout $USER
# Cerrar sesión y volver a iniciarla para que tome efecto
```

### Paso 3 — Flashear

```
Ctrl+Alt+U
```

O desde la barra lateral → **Project Tasks** → **Upload**.

Si PlatformIO no detecta el puerto automáticamente (puede ocurrir con múltiples dispositivos seriales conectados), especificarlo en `platformio.ini`:

```ini
upload_port = COM5         ; Windows
; upload_port = /dev/ttyUSB0   ; Linux
; upload_port = /dev/cu.usbserial-0001  ; macOS
```

### Modo de programación manual

Algunos módulos ESP32 —especialmente los de bajo costo— no implementan el circuito de auto-reset por DTR/RTS. En ese caso:

1. Mantener presionado el botón **BOOT** del módulo mientras la consola muestra `Connecting........`.
2. Soltar cuando aparece `Chip is ESP32...` o `Writing at 0x...`.

Salida esperada al flashear exitosamente:

```
esptool.py v4.x
Connecting...
Chip is ESP32-D0WDQ6 (revision v1.0)
Writing at 0x00010000... (100 %)
Wrote 471845 bytes in 5.4 seconds
Hash of data verified.
Hard resetting via RTS pin...
```

---

## 10. Monitor Serial

El monitor serial muestra los mensajes que el firmware imprime por `Serial.printf()`, lo que permite seguir el estado del sistema en tiempo real durante el desarrollo y las pruebas.

### Abrir

```
Ctrl+Alt+S
```

O desde la barra inferior → ícono del enchufe.

La velocidad es la configurada en `platformio.ini` (`monitor_speed = 115200`).

> El monitor y la carga del firmware no pueden ejecutarse simultáneamente en el mismo puerto. PlatformIO gestiona esto automáticamente: cierra el monitor al iniciar una carga y lo reabre al terminar.

### Decodificador de excepciones

Ambos proyectos incluyen en su `platformio.ini`:

```ini
monitor_filters = esp32_exception_decoder
```

Este filtro intercepta las líneas de backtrace que el ESP32 genera ante un Guru Meditation Error y las traduce a nombres de función y número de línea del código fuente. Sin el filtro, el stack trace solo muestra direcciones hexadecimales sin contexto útil.

### Ejemplo de log del D2 en operación normal

```
[D2] === Maestro IR + Servidor Wi-Fi - Iniciando ===
[NVS] Recuperado idx=2 encendido=SI
[Wi-Fi] AP activo. IP: 192.168.4.1
[D2] Servidor HTTP+WS iniciado. Esperando conexion de D3...

[WS] Cliente #1 conectado desde 192.168.4.2
[WS-TX] ESTADO_ACTUAL -> {"tipo":"ESTADO_ACTUAL","encendido":true,"color":"VERDE",...}

[WS-RX] #1: {"tipo":"CMD_COLOR","color":"AZUL"}
[IR-TX] Enviando trama: 0xAA0000FF
[IR-RX] Respuesta: 0xBB0000FF | cab=0xBB R=0 G=0 B=255
[IR] ACK valido recibido
[D2] Estado actualizado: idx=4 enc=SI R=0 G=0 B=255
[NVS] Guardado idx=4 encendido=SI
[WS-TX] RESULTADO_CMD -> {"tipo":"RESULTADO_CMD","exito":true,...}
```

---

## 11. Solución de problemas frecuentes

### El ESP32 no aparece como puerto serial

- Cable de solo carga → reemplazar por cable USB data.
- Driver no instalado → ver [Sección 5](#5-requisitos-previos).
- Puerto ocupado por otro programa → cerrar Arduino IDE u otra aplicación con el monitor abierto.

### `A fatal error occurred: Failed to connect to ESP32`

El chip no entró en modo de programación. Usar el procedimiento manual con el botón BOOT (ver [Sección 9](#9-cargar-el-firmware-en-el-esp32)).

### `IRremote.hpp: No such file or directory`

PlatformIO no descargó las librerías. Verificar conexión a Internet y luego: barra lateral → **Clean** → **Build**. Si persiste, eliminar la carpeta `.pio/` y compilar de nuevo.

### `AsyncWebSocket.h: No such file or directory`

`ESPAsyncWebServer` depende de `AsyncTCP`. Verificar que `platformio.ini` de D2 declara ambas en `lib_deps`.

### El ESP32 se reinicia en loop (Guru Meditation Error)

Abrir el monitor serial con `esp32_exception_decoder` activo. El filtro decodifica el backtrace e indica la función y línea exacta donde ocurrió el crash. Causas comunes: stack overflow por VLAs en callbacks de red, acceso a puntero nulo, o `delay()` dentro de un callback de ESPAsyncWebServer (bloqueando el Core 0).

### D3 no puede conectarse a la red de D2

1. Verificar que LED_AP (azul) en D2 está encendido.
2. Confirmar que el SSID en `config.h` coincide con el que aparece en la lista de redes del dispositivo.
3. Navegar a `http://192.168.4.1` (HTTP explícito, sin HTTPS).
4. En iOS Safari: desactivar temporalmente "Limit IP Address Tracking" en la configuración de esa red Wi-Fi.

### El LED de D1 no cambia pero D2 no reporta timeout

- Verificar línea de visión directa entre KY-005 y KY-022.
- Confirmar que los pines en `config.h` coinciden con el cableado físico.
- Abrir el monitor serial de D1 y verificar si aparece `[IR-RX] Trama:` al enviar un comando.

---

## 12. Referencia rápida de atajos

| Acción | Windows / Linux | macOS |
|---|---|---|
| Panel de extensiones | `Ctrl+Shift+X` | `Cmd+Shift+X` |
| Compilar (Build) | `Ctrl+Alt+B` | `Cmd+Alt+B` |
| Cargar firmware (Upload) | `Ctrl+Alt+U` | `Cmd+Alt+U` |
| Monitor Serial | `Ctrl+Alt+S` | `Cmd+Alt+S` |
| Paleta de comandos | `Ctrl+Shift+P` | `Cmd+Shift+P` |
| Terminal integrada | `` Ctrl+` `` | `` Cmd+` `` |
| Buscar en el proyecto | `Ctrl+Shift+F` | `Cmd+Shift+F` |

**Panel lateral de PlatformIO (ícono de hormiga):**

```
PlatformIO Home
  └── Project Tasks
        ├── Build               compilar sin cargar
        ├── Upload              compilar y flashear
        ├── Monitor             abrir monitor serial
        ├── Upload and Monitor  flashear y abrir monitor en un paso
        └── Clean               eliminar artefactos de compilación
```

---

*Documentación elaborada en el marco del TP3 Integrador — Comunicación de Datos, Ingeniería en Computación, UNRaf.*  
*Bevilacqua, Francisco — Clement, Sebastián — Junio 2026.*