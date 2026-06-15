# Detección de Errores por CRC
## Fundamentos Teóricos, Matemáticos e Implementación en el Proyecto TP3

> **TP3 Integrador — Comunicación de Datos**  
> Ingeniería en Computación | Bevilacqua, Francisco — Clement, Sebastián | Junio 2026

---

## Índice

1. [Introducción: el problema de la integridad en comunicaciones](#1-introducción-el-problema-de-la-integridad-en-comunicaciones)
2. [Fundamentos matemáticos del CRC](#2-fundamentos-matemáticos-del-crc)
3. [El algoritmo CRC: funcionamiento bit a bit](#3-el-algoritmo-crc-funcionamiento-bit-a-bit)
4. [Parámetros de un estándar CRC](#4-parámetros-de-un-estándar-crc)
5. [Capacidad de detección de errores y Distancia Hamming](#5-capacidad-de-detección-de-errores-y-distancia-hamming)
6. [Panorama de estándares CRC y criterios de selección](#6-panorama-de-estándares-crc-y-criterios-de-selección)
7. [CRC-8/MAXIM en el enlace IR (D1 ↔ D2)](#7-crc-8maxim-en-el-enlace-ir-d1--d2)
8. [CRC-16/IBM en el enlace WebSocket (D2 ↔ D3)](#8-crc-16ibm-en-el-enlace-websocket-d2--d3)
9. [Problemas detectados y soluciones implementadas](#9-problemas-detectados-y-soluciones-implementadas)
10. [Comparativa de ambos CRC en el sistema](#10-comparativa-de-ambos-crc-en-el-sistema)

---

## 1. Introducción: el problema de la integridad en comunicaciones

Todo canal de comunicación introduce la posibilidad de que los datos lleguen alterados respecto a como fueron enviados. Las causas son variadas: ruido térmico en el canal eléctrico, interferencia electromagnética, reflexiones ópticas en el canal IR, congestión en redes IP, colisiones, o simplemente la cuantización inherente a los sistemas digitales.

El modelo de capa de enlace del modelo OSI define como responsabilidad de esta capa garantizar que los bloques de datos (tramas) que se entregan a la capa de red sean íntegros. El mecanismo más universal para esto es agregar un campo de **checksum** calculado matemáticamente sobre el contenido de la trama, que el receptor puede recalcular independientemente para detectar discrepancias.

En este sistema conviven dos canales de naturaleza radicalmente distinta:

- **Canal IR (D1 ↔ D2):** óptico, half-duplex, sin protocolo de control de flujo a nivel físico. Propenso a ecos, ruido ambiental y distorsión por desalineación del haz. El payload es corto y fijo (3 bytes RGB).

- **Canal WebSocket (D2 ↔ D3):** TCP/IP sobre Wi-Fi 802.11, con control de flujo y retransmisión a nivel de transporte. El payload es variable (strings JSON de hasta ~300 bytes). Los errores son menos frecuentes pero su consecuencia (ejecutar un comando de color erróneo) es inaceptable.

La elección de **CRC-8** para el canal IR y **CRC-16** para el canal WebSocket no es arbitraria. Responde a un análisis de la longitud del mensaje, la frecuencia esperada de errores y la potencia de detección requerida en cada capa.

---

## 2. Fundamentos matemáticos del CRC

### 2.1 Aritmética módulo 2 y el cuerpo GF(2)

El CRC opera sobre el **cuerpo de Galois GF(2)**, el conjunto {0, 1} con las operaciones:

- **Suma módulo 2:** equivalente al XOR lógico. `0+0=0`, `0+1=1`, `1+0=1`, `1+1=0`.
- **Multiplicación módulo 2:** equivalente al AND lógico. `0×0=0`, `0×1=0`, `1×0=0`, `1×1=1`.

La propiedad fundamental que hace al CRC eficiente es que en GF(2) **la resta es idéntica a la suma** (ambas son XOR). Esto elimina el acarreo y hace que toda la aritmética sea implementable con operaciones de desplazamiento y XOR, sin requerir unidades aritméticas complejas.

### 2.2 Polinomios sobre GF(2)

Un bloque de datos de n bits se representa como un **polinomio** cuyos coeficientes son 0 o 1. Por ejemplo, la secuencia de bits `10110011` corresponde al polinomio:

```
x^7 + x^5 + x^4 + x^1 + x^0   →   representación hexadecimal: 0xB3
```

El exponente indica la posición del bit (MSB primero en la notación canónica). Esta representación permite aplicar toda la teoría de divisibilidad polinomial al problema de detección de errores.

### 2.3 Principio de la división polinomial

Sea M(x) el polinomio del mensaje de k bits y G(x) el **polinomio generador** de grado r (con r+1 bits, siendo el bit más significativo siempre 1).

El procedimiento del CRC es:

**Emisor:**
1. Multiplicar M(x) por x^r (equivalente a desplazar r bits a la izquierda, agregando r ceros al final).
2. Dividir M(x) · x^r entre G(x) en aritmética módulo 2.
3. El **residuo** R(x) de esta división, de exactamente r bits, es el CRC.
4. Transmitir T(x) = M(x) · x^r + R(x) (el mensaje con los r bits de CRC al final).

**Receptor:**
1. Recibir T'(x) (que puede diferir de T(x) si hubo errores).
2. Dividir T'(x) entre G(x).
3. Si el residuo es 0x00 (o el valor XorOut esperado), la trama es íntegra.
4. Si el residuo es distinto de cero, hay al menos un error detectado.

La razón por la que esto funciona: si no hubo errores, T'(x) = T(x) = M(x) · x^r + R(x). Por construcción, T(x) es divisible por G(x) (es múltiplo de G), entonces T(x) mod G(x) = 0. Cualquier patrón de error E(x) que no sea múltiplo de G(x) producirá un residuo no nulo, detectando el error.

### 2.4 Implementación equivalente: registro de desplazamiento con realimentación

En lugar de realizar la división polinomial completa sobre el mensaje entero (costosa en tiempo y memoria), el CRC se calcula de forma iterativa usando un **registro de desplazamiento con realimentación XOR (LFSR — Linear Feedback Shift Register)**.

El algoritmo bit a bit procesa un bit a la vez del mensaje de izquierda a derecha:

```
Para cada bit b del mensaje:
    b_salida = MSB del registro CRC
    Desplazar registro una posición a la izquierda
    Ingresar b en el bit 0 del registro (con XOR)
    Si b_salida == 1: XOR del registro con el polinomio generador (sin el bit MSB implícito)
```

La variante **byte a byte LSB-first** (usada en CRC-8/MAXIM y CRC-16/IBM) invierte el orden de procesamiento de los bits dentro de cada byte (RefIn=true), lo que en hardware corresponde a reflejar la entrada. La implementación en software es equivalente:

```
Para cada byte d del mensaje:
    registro XOR= d
    Repetir 8 veces:
        Si LSB del registro == 1:
            registro = (registro >> 1) XOR polinomio_reflejado
        Sino:
            registro = registro >> 1
```

Esta es exactamente la implementación presente en el firmware de este proyecto para ambos CRC.

---

## 3. El algoritmo CRC: funcionamiento bit a bit

Para construir la intuición, se muestra la traza completa de un ejemplo manual con un mensaje corto.

### Ejemplo: CRC-8 sobre el byte 0xAB con polinomio simplificado 0x07 (CRC-8/SMBUS)

Mensaje: `0xAB = 10101011b`  
Polinomio: `x^8 + x^2 + x^1 + 1 = 100000111b`, parte divisora (sin el bit 8): `00000111b = 0x07`  
Init: `0x00`

```
Paso inicial: crc = 0x00
Byte 0xAB: crc XOR 0xAB = 0xAB = 10101011b

Iteración bit 1: LSB=1 → crc = (10101011 >> 1) XOR 00000111
                        = 01010101 XOR 00000111 = 01010010 = 0x52
Iteración bit 2: LSB=0 → crc = 01010010 >> 1 = 00101001 = 0x29
Iteración bit 3: LSB=1 → crc = (00101001 >> 1) XOR 00000111
                        = 00010100 XOR 00000111 = 00010011 = 0x13
Iteración bit 4: LSB=1 → crc = (00010011 >> 1) XOR 00000111
                        = 00001001 XOR 00000111 = 00001110 = 0x0E
Iteración bit 5: LSB=0 → crc = 00001110 >> 1 = 00000111 = 0x07
Iteración bit 6: LSB=1 → crc = (00000111 >> 1) XOR 00000111
                        = 00000011 XOR 00000111 = 00000100 = 0x04
Iteración bit 7: LSB=0 → crc = 00000100 >> 1 = 00000010 = 0x02
Iteración bit 8: LSB=0 → crc = 00000010 >> 1 = 00000001 = 0x01

Resultado CRC-8/SMBUS(0xAB) = 0x01
```

Este proceso es exactamente el que ejecuta el firmware en el bucle interno de `calcularCRC8()` y `calcularCRC16()`, extendiéndolo a múltiples bytes de forma concatenada.

---

## 4. Parámetros de un estándar CRC

Cualquier estándar CRC queda completamente definido por seis parámetros (modelo de Williams, 1993):

| Parámetro | Descripción | Impacto |
|-----------|-------------|---------|
| **Width** | Número de bits del CRC (r) | Determina el tamaño del campo de checksum |
| **Poly** | Polinomio generador (sin el bit r implícito) | Define qué patrones de error detecta |
| **Init** | Valor inicial del registro CRC | Afecta la detección de ceros iniciales en el mensaje |
| **RefIn** | Reflexión de bits de entrada (LSB-first vs MSB-first) | Compatibilidad con hardware serie |
| **RefOut** | Reflexión del resultado final | Simetría con RefIn |
| **XorOut** | XOR aplicado al resultado antes de entregarlo | Permite invertir el CRC final |

Dos implementaciones con el mismo **Poly** pero distinto **Init** producirán CRCs completamente distintos para el mismo mensaje y serán incompatibles entre sí. Esta es la razón por la que el `Init=0xFF` elegido en este proyecto (contra el `Init=0x00` de otras variantes CRC-8) es un parámetro crítico que debe coincidir exactamente entre D1, D2 y la implementación JavaScript de D3.

---

## 5. Capacidad de detección de errores y Distancia Hamming

### 5.1 Distancia Hamming

La **Distancia Hamming** (HD) entre dos palabras de código es el número de posiciones de bit en que difieren. Para un código de detección de errores, la distancia mínima entre cualquier par de palabras válidas determina cuántos errores puede detectar:

- **HD = d** → detecta todos los errores de hasta **d-1** bits.
- **HD = d** → corrige todos los errores de hasta **⌊(d-1)/2⌋** bits.

El CRC no es un código corrector; es un detector. Su fortaleza es la alta HD para mensajes largos con registros de CRC relativamente cortos.

### 5.2 Propiedades de detección del CRC en general

Para cualquier CRC de r bits con polinomio generador bien elegido:

- **Todos los errores de 1 bit:** detectados con probabilidad 1 (100%), siempre que el polinomio tenga al menos dos términos no nulos.
- **Todos los errores de 2 bits:** detectados si el polinomio no divide a x^k + 1 para ningún k menor que la longitud del mensaje.
- **Todos los errores de número impar de bits:** detectados si el polinomio tiene el factor (x+1).
- **Ráfagas de error de longitud ≤ r:** detectadas con probabilidad 1.
- **Ráfagas de longitud r+1:** detectadas con probabilidad 1 - 1/2^(r-1).
- **Errores aleatorios de cualquier longitud:** no detectados con probabilidad 1/2^r.

### 5.3 Distancia Hamming en función de la longitud del mensaje

La HD de un CRC **no es constante**: depende del polinomio y de la longitud del mensaje. Los estándares publicados especifican la HD garantizada para ciertos rangos de longitud:

| CRC | Poly | HD=4 para mensajes de | HD=3 hasta | HD=2 hasta |
|-----|------|-----------------------|------------|------------|
| CRC-8/MAXIM (0x31) | 0x31 | ≤ 119 bits (≤ 14 bytes) | 119–127 bits | Ilimitado |
| CRC-16/IBM (0x8005) | 0x8005 | ≤ 32.767 bits (≤ 4 KB) | 32.767–65.535 bits | Ilimitado |
| CRC-32/ISO | 0x04C11DB7 | ≤ 91.607 bits (≤ 11 KB) | Hasta ~4 GB | Ilimitado |

Para la carga útil de este proyecto (3 bytes IR = 24 bits; JSON WebSocket ≤ 2.400 bits), ambos CRC operan en su rango óptimo de HD=4.

---

## 6. Panorama de estándares CRC y criterios de selección

### 6.1 Familia CRC-8

| Variante | Poly | Init | Aplicaciones típicas |
|----------|------|------|----------------------|
| CRC-8/SMBUS | 0x07 | 0x00 | Protocolo SMBus (I2C con integridad) |
| **CRC-8/MAXIM (Dallas)** | **0x31** | **0xFF** | **1-Wire (temperatura, sensores)** |
| CRC-8/DVB-S2 | 0xD5 | 0x00 | Broadcast digital satelital |
| CRC-8/SAE-J1850 | 0x1D | 0xFF | CAN bus automotriz |
| CRC-8/ITU | 0x07 | 0x00 | ATM, telefonía |

El polinomio **0x31** (CRC-8/MAXIM) fue elegido para el enlace IR por:
- Origen y uso en el protocolo Dallas/1-Wire, un protocolo también de propósito embebido con características similares al bus IR de este sistema.
- `Init=0xFF`: propiedad adicional que invalida la trama nula, resolviendo el problema de eco descubierto experimentalmente.
- HD=4 para 3 bytes: capacidad de detección óptima para el payload de 24 bits de este sistema.

### 6.2 Familia CRC-16

| Variante | Poly | Init | Aplicaciones típicas |
|----------|------|------|----------------------|
| **CRC-16/IBM (ARC)** | **0x8005** | **0x0000** | **ARC, Bluetooth, USB** |
| CRC-16/CCITT | 0x1021 | 0xFFFF | XMODEM, HDLC, Bluetooth LE |
| CRC-16/MODBUS | 0x8005 | 0xFFFF | Protocolo MODBUS RTU |
| CRC-16/DNP | 0x3D65 | 0x0000 | SCADA, redes de control industrial |
| CRC-16/USB | 0x8005 | 0xFFFF | Control de flujo USB |

El polinomio **0x8005** (CRC-16/IBM) fue elegido para el WebSocket por:
- Estándar de facto para longitudes de mensaje de kilobytes, con HD=4 garantizado hasta 32.767 bits.
- Mayor cobertura que CRC-8 para los payloads JSON variables de este sistema (hasta ~2.400 bits).
- Compatible con la implementación en JavaScript nativo del cliente (navegador), sin librerías externas.

### 6.3 ¿Por qué no CRC-32?

CRC-32 (HD=4 hasta ~11 KB, detecta el 99.9999998% de errores en ráfagas) sería excesivo para un sistema embebido con payloads de bytes a kilobytes. El campo CRC-32 ocupa 4 bytes en una trama IR de 4 bytes totales (100% overhead de integridad), lo que es estructuralmente inviable. En el WebSocket, el overhead de 4 bytes sobre JSON de ~100 bytes es manejable pero innecesario dado que CRC-16 cubre ampliamente el rango de longitud real.

---

## 7. CRC-8/MAXIM en el enlace IR (D1 ↔ D2)

### 7.1 Contexto del canal

El enlace infrarrojo opera con:
- **Modulación:** portadora de 38 kHz (NEC Raw), generada por el periférico RMT hardware del ESP32.
- **Duplexidad:** half-duplex. Solo un nodo transmite a la vez; el otro escucha.
- **Trama:** 32 bits fijos, sin cabecera ni campo de dirección explícito.
- **Riesgos:** ecos del propio emisor capturados por el receptor, ruido IR ambiental (luz solar, lámparas LED parpadeantes), reflexiones en superficies.

### 7.2 Estructura de la trama IR de 32 bits

```
 Bit 31        Bit 24   Bit 23        Bit 16   Bit 15         Bit 8   Bit 7          Bit 0
┌─────────────────────┬─────────────────────┬─────────────────────┬─────────────────────┐
│   Componente R      │   Componente G      │   Componente B      │   CRC-8 (o ~CRC-8)  │
│      8 bits         │      8 bits         │      8 bits         │      8 bits         │
│      0 – 255        │      0 – 255        │      0 – 255        │   checksum de R,G,B │
└─────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┘

Trama CMD  (D2 → D1):  [R][G][B][ CRC-8(R,G,B) ]   ← firma directa
Trama ACK  (D1 → D2):  [R][G][B][~CRC-8(R,G,B)]    ← firma invertida (NOT bit a bit)
```

La ausencia de cabecera fija (a diferencia del TP2, que usaba `0xAA`/`0xBB`) transfiere la responsabilidad de autenticación al CRC. La asimetría entre CMD y ACK (`CRC` vs `~CRC`) actúa como el campo de dirección implícito ausente.

### 7.3 Parámetros del CRC-8/MAXIM implementado

```
Width   : 8 bits
Poly    : 0x31   (x^8 + x^5 + x^4 + 1)
Init    : 0xFF   ← modificado respecto al estándar por guarda contra eco nulo
RefIn   : true   (LSB-first)
RefOut  : true
XorOut  : 0x00
```

### 7.4 Implementación C++ en D1 y D2

```cpp
uint8_t calcularCRC8(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t crc = 0xFF;              // Init=0xFF: guarda contra trama nula 0x00000000
    uint8_t datos[3] = { r, g, b };

    for (uint8_t i = 0; i < 3; i++) {
        crc ^= datos[i];             // XOR del byte de datos con el registro

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x01) {        // LSB=1: desplazar y XOR con polinomio (RefIn=true)
                crc = (crc >> 1) ^ 0x31;
            } else {
                crc >>= 1;           // LSB=0: solo desplazar
            }
        }
    }
    return crc;
}
```

**Esta misma función es idéntica en D1 y D2.** Ambos nodos usan el mismo polinomio, el mismo Init y el mismo orden de procesamiento. La diferencia está en cómo se usa el resultado:

- D2 construye la trama CMD con `CRC = calcularCRC8(r, g, b)` y espera un ACK con `~calcularCRC8(r, g, b)`.
- D1 valida la trama CMD verificando `crc_rx == calcularCRC8(r, g, b)` y envía el ACK con `~calcularCRC8(r, g, b)`.

### 7.5 Traza matemática completa: CMD para VERDE (R=0, G=255, B=0)

```
Init: crc = 0xFF = 11111111b

── Byte 0: R = 0x00 ──────────────────────────────────────
  crc ^= 0x00 → crc = 0xFF = 11111111b

  bit 1: LSB=1 → crc = (11111111 >> 1) ^ 00110001 = 01111111 ^ 00110001 = 01001110 = 0x4E
  bit 2: LSB=0 → crc = 01001110 >> 1 = 00100111 = 0x27
  bit 3: LSB=1 → crc = (00100111 >> 1) ^ 00110001 = 00010011 ^ 00110001 = 00100010 = 0x22 (*)
  bit 4: LSB=0 → crc = 00100010 >> 1 = 00010001 = 0x11
  bit 5: LSB=1 → crc = (00010001 >> 1) ^ 00110001 = 00001000 ^ 00110001 = 00111001 = 0x39
  bit 6: LSB=1 → crc = (00111001 >> 1) ^ 00110001 = 00011100 ^ 00110001 = 00101101 = 0x2D
  bit 7: LSB=1 → crc = (00101101 >> 1) ^ 00110001 = 00010110 ^ 00110001 = 00100111 = 0x27
  bit 8: LSB=1 → crc = (00100111 >> 1) ^ 00110001 = 00010011 ^ 00110001 = 00100010 = 0x22 (*)
  → tras byte R=0: crc = 0x22

── Byte 1: G = 0xFF ──────────────────────────────────────
  crc ^= 0xFF → crc = 0x22 ^ 0xFF = 0xDD = 11011101b

  bit 1: LSB=1 → crc = (11011101 >> 1) ^ 00110001 = 01101110 ^ 00110001 = 01011111 = 0x5F
  bit 2: LSB=1 → crc = (01011111 >> 1) ^ 00110001 = 00101111 ^ 00110001 = 00011110 = 0x1E
  bit 3: LSB=0 → crc = 00011110 >> 1 = 00001111 = 0x0F
  bit 4: LSB=1 → crc = (00001111 >> 1) ^ 00110001 = 00000111 ^ 00110001 = 00110110 = 0x36
  bit 5: LSB=0 → crc = 00110110 >> 1 = 00011011 = 0x1B
  bit 6: LSB=1 → crc = (00011011 >> 1) ^ 00110001 = 00001101 ^ 00110001 = 00111100 = 0x3C
  bit 7: LSB=0 → crc = 00111100 >> 1 = 00011110 = 0x1E
  bit 8: LSB=0 → crc = 00011110 >> 1 = 00001111 = 0x0F
  → tras byte G=255: crc = 0x0F

── Byte 2: B = 0x00 ──────────────────────────────────────
  crc ^= 0x00 → crc = 0x0F (sin cambio)

  (proceso de 8 bits sobre 0x0F = 00001111b)
  bit 1: LSB=1 → (00001111>>1) ^ 0x31 = 00000111 ^ 00110001 = 00110110 = 0x36
  bit 2: LSB=0 → 0x36 >> 1 = 0x1B
  bit 3: LSB=1 → (00011011>>1) ^ 0x31 = 00001101 ^ 00110001 = 00111100 = 0x3C
  bit 4: LSB=0 → 0x3C >> 1 = 0x1E
  bit 5: LSB=0 → 0x1E >> 1 = 0x0F
  bit 6: LSB=1 → (00001111>>1) ^ 0x31 = 00000111 ^ 00110001 = 00110110 = 0x36
  bit 7: LSB=0 → 0x36 >> 1 = 0x1B
  bit 8: LSB=1 → (00011011>>1) ^ 0x31 = 00001101 ^ 00110001 = 00111100 = 0x3C
  → tras byte B=0: crc = 0x3C

Resultado: CRC-8({0, 255, 0}) = 0x3C

Trama CMD:  [0x00][0xFF][0x00][0x3C] → uint32: 0x00FF003C
Trama ACK:  [0x00][0xFF][0x00][0xC3] → uint32: 0x00FF00C3  (~0x3C = 0xC3)
```

El log del sistema durante esta operación muestra exactamente estos valores:

```
[IR-TX] Trama COMANDO: 0x00FF003C
        R=0  G=255  B=0
        CRC_tx=0x3C  (init=0xFF, poly=0x31, firma normal)
        ACK esperado: R=0 G=255 B=0 CRC=0xC3  (~CRC_tx)
```

### 7.6 Validación del ACK en D2: discriminación eco vs ACK

Al recibir cualquier trama durante la espera del ACK, D2 ejecuta la siguiente lógica de tres vías:

```cpp
uint8_t crc_base     = calcularCRC8(ack_r, ack_g, ack_b);  // firma esperada de eco/CMD
uint8_t crc_esperado = ~crc_base;                           // firma esperada de ACK legítimo

if (ack_crc == crc_base) {
    // Eco del propio CMD: descartado de forma determinística
    continue;
}
if (ack_crc != crc_esperado) {
    // Trama corrupta o de otra fuente: descartada
    continue;
}
// ack_crc == crc_esperado: ACK legítimo de D1
if (ack_r == r && ack_g == g && ack_b == b) {
    return true;  // éxito total: CRC válido y datos RGB coinciden
}
```

Esta lógica tiene tres caminos de rechazo distintos y un único camino de aceptación, maximizando la robustez ante condiciones adversas del canal IR.

### 7.7 Construcción de la trama en C++

```cpp
// CMD (D2 → D1): firma directa
uint32_t trama = ((uint32_t)r      << 24) |
                 ((uint32_t)g      << 16) |
                 ((uint32_t)b      <<  8) |
                  (uint32_t)crc_tx;

// ACK (D1 → D2): firma invertida
uint8_t crc_ack = ~calcularCRC8(r, g, b);
uint32_t trama_ack = ((uint32_t)r       << 24) |
                     ((uint32_t)g       << 16) |
                     ((uint32_t)b       <<  8) |
                      (uint32_t)crc_ack;
```

Los casts explícitos a `uint32_t` antes del desplazamiento son obligatorios en C++: desplazar un `uint8_t` más de 7 posiciones es comportamiento indefinido (UB). El cast promueve el operando a 32 bits antes del shift, garantizando portabilidad y corrección.

---

## 8. CRC-16/IBM en el enlace WebSocket (D2 ↔ D3)

### 8.1 Contexto del canal

El enlace WebSocket opera sobre:
- **Transporte:** TCP/IP, con control de errores propio (checksum TCP de 16 bits). TCP detecta errores pero no los corrige a nivel de aplicación: simplemente retransmite el segmento.
- **Payload:** strings JSON de longitud variable, entre 30 y ~300 bytes según el tipo de mensaje.
- **Dirección:** full-duplex. Ambos nodos pueden enviar en cualquier momento.
- **Riesgo principal:** un error no detectado en el string JSON haría que D2 procesara un comando con valores RGB incorrectos, o que D3 mostrara un estado del sistema incorrecto.

### 8.2 ¿Por qué CRC-16 y no CRC-8?

La capacidad de detección de CRC-8 cae a HD=2 para mensajes mayores de 119 bits. El JSON más corto del sistema (`{"tipo":"PING"}`) tiene ya 15 bytes = 120 bits, lo que supera el límite de HD=4 del CRC-8/MAXIM. Con CRC-16/IBM, HD=4 se mantiene hasta 32.767 bits, cubriendo con margen los ~2.400 bits máximos de este sistema.

| Payload de ejemplo | Bytes | Bits | HD con CRC-8/MAXIM | HD con CRC-16/IBM |
|--------------------|-------|------|---------------------|-------------------|
| `{"tipo":"PING"}` | 15 | 120 | HD=3 | HD=4 |
| `{"tipo":"CMD_COLOR","color":"TURQUESA","r":0,"g":210,"b":140}` | 58 | 464 | HD=2 | HD=4 |
| Mensaje RESULTADO_CMD completo | ~150 | 1200 | HD=2 | HD=4 |

### 8.3 Parámetros del CRC-16/IBM implementado

```
Width   : 16 bits
Poly    : 0x8005   (x^16 + x^15 + x^2 + 1)
Init    : 0x0000
RefIn   : true     (LSB-first)
RefOut  : true
XorOut  : 0x0000
```

### 8.4 Implementación C++ en D2

```cpp
uint16_t calcularCRC16(const char *payload, size_t len) {
    uint16_t crc = 0x0000;            // Init = 0x0000

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint8_t)payload[i];   // XOR con byte actual

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {        // LSB=1 (RefIn=true)
                crc = (crc >> 1) ^ 0x8005;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
```

### 8.5 Implementación JavaScript en D3

```javascript
function calcularCRC16(str) {
    const POLY = 0x8005;
    let crc = 0x0000;

    for (let i = 0; i < str.length; i++) {
        crc ^= str.charCodeAt(i);
        for (let bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc = ((crc >>> 1) ^ POLY) & 0xFFFF;  // >>> lógico, no aritmético
            } else {
                crc = (crc >>> 1) & 0xFFFF;
            }
        }
    }
    return crc;
}
```

**Nota crítica sobre `>>>` en JavaScript:** JavaScript no tiene enteros de 16 bits nativos; usa `Number` (IEEE 754 de 64 bits con parte entera de 53 bits). El operador `>>` en JavaScript es un right shift **aritmético**: extiende el bit de signo, produciendo valores negativos cuando el MSB es 1. El operador `>>>` es **lógico**: inserta ceros desde el MSB. Para simular un registro de 16 bits sin signo, se usa `>>>` combinado con `& 0xFFFF` para descartar los bits superiores. Sin esta distinción, el CRC calculado en JavaScript sería diferente al del C++, rompiendo la interoperabilidad.

### 8.6 Protocolo de serialización y verificación

El CRC-16 no puede incluirse en el dato sobre el que se calcula (dependencia circular). La solución es el **patrón de serialización en dos pasos**:

**Envío (D2 o D3):**

```
1. Construir el objeto de datos sin el campo "crc16":
   doc = { "tipo": "CMD_COLOR", "color": "VERDE", "r": 0, "g": 255, "b": 0 }

2. Serializar a string (buf_base):
   '{"tipo":"CMD_COLOR","color":"VERDE","r":0,"g":255,"b":0}'

3. Calcular CRC-16 sobre buf_base:
   crc = calcularCRC16(buf_base, len(buf_base))

4. Agregar el campo al documento:
   doc["crc16"] = crc

5. Serializar el documento completo (buf_final):
   '{"tipo":"CMD_COLOR","color":"VERDE","r":0,"g":255,"b":0,"crc16":XXXX}'

6. Transmitir buf_final.
```

**Recepción y verificación (D2 o D3):**

```
1. Parsear el JSON recibido → doc
2. Extraer crc_rx = doc["crc16"]
3. Eliminar el campo: doc.remove("crc16")  (o delete en JS)
4. Serializar doc sin el campo → buf_check
5. Recalcular: crc_calc = calcularCRC16(buf_check, len)
6. Si crc_rx == crc_calc → mensaje íntegro → procesar
   Si crc_rx != crc_calc → error → descartar y notificar CRC_ERROR
```

### 8.7 La invarianza del orden de campos: contrato de protocolo

El CRC se calcula sobre un **string JSON específico**. Si el orden de los campos en la serialización varía entre emisor y receptor, el string será distinto y el CRC calculado no coincidirá, aunque los datos sean semánticamente idénticos.

Ejemplo: `{"a":1,"b":2}` tiene CRC distinto de `{"b":2,"a":1}`, aunque ambos representan el mismo objeto.

Esto se resuelve estandarizando el orden de inserción en ambas implementaciones:

- **D2 (ArduinoJson):** los campos se agregan al `JsonDocument` en un orden fijo y determinista en cada función. ArduinoJson preserva el orden de inserción.
- **D3 (JavaScript):** `JSON.stringify()` preserva el orden de propiedades de inserción en V8, SpiderMonkey y JavaScriptCore (los tres motores de navegadores modernos). El campo `"crc16"` siempre se agrega al final.
- **Verificación en D2:** `doc.remove("crc16")` elimina el campo preservando el orden de los restantes. `serializeJson(doc)` reproduce exactamente el `buf_base` original.

Este contrato de orden es parte implícita del protocolo y debe respetarse si se agregan campos nuevos en futuras versiones.

### 8.8 Ejemplo numérico concreto: CMD_COLOR VERDE con RGB

El nuevo protocolo incluye los campos `r`, `g`, `b` además del nombre simbólico, para que el CRC-16 cubra también los valores numéricos del triplete:

```
Objeto D3 antes de agregar CRC:
  { tipo: "CMD_COLOR", color: "VERDE", r: 0, g: 255, b: 0 }

Serialización base (JSON.stringify en D3):
  '{"tipo":"CMD_COLOR","color":"VERDE","r":0,"g":255,"b":0}'
  Longitud: 56 bytes

CRC-16/IBM sobre esos 56 bytes (Init=0x0000, Poly=0x8005, LSB-first):
  → Resultado: 0xXXXX (calculado en runtime)

JSON final enviado a D2:
  '{"tipo":"CMD_COLOR","color":"VERDE","r":0,"g":255,"b":0,"crc16":XXXX}'
```

D2 recibe el mensaje, extrae `crc16`, elimina el campo, serializa y recalcula sobre exactamente los mismos 56 bytes. Si el CRC coincide, procesa el comando usando los valores `r=0, g=255, b=0` directamente desde el JSON (en lugar de buscarlos en la tabla por nombre), garantizando que los valores numéricos transmitidos son exactamente los que D2 usará para el ciclo IR.

### 8.9 Capacidad de detección del CRC-16/IBM para este sistema

| Tipo de error | Probabilidad de detección |
|---------------|--------------------------|
| Error de 1 bit en cualquier posición | 100% |
| Error de 2 bits en cualquier posición | 100% |
| Error de 3 bits | 100% |
| Ráfaga de error de ≤ 16 bits contiguos | 100% |
| Ráfaga de 17 bits | 99.9985% (1 − 1/2^15) |
| Ráfaga de 18+ bits | 99.9969% (1 − 1/2^16) |
| Error aleatorio de cualquier longitud | 99.9969% en promedio |

Para el peor payload del sistema (~300 bytes = 2.400 bits), estas garantías se mantienen en su valor máximo porque 2.400 bits << 32.767 bits (límite de HD=4).

---

## 9. Problemas detectados y soluciones implementadas

### 9.1 Problema: eco nulo del receptor KY-022

**Descripción técnica:** el canal IR es half-duplex. Tras una transmisión, el receptor KY-022 se reactiva mediante `IrReceiver.start()`. En ese instante de transición, el circuito analógico interno del TSOP (amplificador + filtro de banda + demodulador) puede generar un pulso espurio en la salida digital, que IRremote interpreta como una trama recibida. Ese pulso, al no tener contenido real, produce la trama nula `0x00000000`.

**Por qué es peligroso con Init=0x00:** con `Init=0x00`, el CRC8 sobre el payload `{R=0, G=0, B=0}` es exactamente `0x00`, porque:

```
crc = 0x00
crc ^= 0x00   → 0x00  (R)
[8 iteraciones sobre 0x00: ningún XOR con polinomio, siempre LSB=0, resultado 0x00]
crc ^= 0x00   → 0x00  (G)
[idem]
crc ^= 0x00   → 0x00  (B)
[idem]
Resultado: CRC8_init0({0,0,0}) = 0x00
```

La trama nula `0x00000000` tiene CRC `0x00`, que coincide con el CRC calculado localmente sobre `{0,0,0}`: **pasa la validación de forma espuria**.

**Solución: Init=0xFF:** con `Init=0xFF`, el proceso sobre `{0,0,0}` produce un resultado no nulo (verificable ejecutando `calcularCRC8(0,0,0)` con `Init=0xFF`). La trama nula tiene el campo CRC igual a `0x00`, pero el CRC calculado localmente es distinto de `0x00`, por lo que la validación falla y la trama espuria es rechazada.

Este cambio es matemáticamente elegante: no agrega lógica condicional ni overhead computacional. Solo cambia el valor semilla del registro, lo que altera el punto de partida de toda la secuencia de XOR y elimina la colisión.

### 9.2 Problema: Self-ACK (Falso Positivo por Eco)

**Descripción técnica:** la simetría total del protocolo original (CMD y ACK con estructura `[R][G][B][CRC8]` idéntica) permite que D2 acepte su propio eco como ACK válido. El canal IR óptico no tiene término de la propagación directamente controlable; cualquier superficie reflectante (pared, mesa, componentes electrónicos) puede devolver el haz emitido de vuelta hacia el receptor de D2.

**Por qué es un problema real:** se verificó experimentalmente que al desalinear o tapar el KY-022 de D1 (impidiendo que D1 reciba el CMD), D2 igualmente actualizaba su estado y el LED cambiaba de color. El eco del propio CMD tenía el mismo CRC que un ACK legítimo de D1, siendo indistinguible matemáticamente.

**Solución: asimetría de firma (CRC vs ~CRC):**

```
CMD  (D2 → D1): [R][G][B][ CRC8]    firma directa
ACK  (D1 → D2): [R][G][B][~CRC8]    firma invertida (complemento a uno)
```

La imposibilidad matemática de que `x == ~x` para cualquier `x` de 8 bits (ya que implicaría que todos los bits son simultáneamente 0 y 1) garantiza que un eco del CMD, que lleva `CRC8`, nunca pasará la validación que espera `~CRC8`.

La lógica de D2 al recibir una trama distingue tres casos:

```
crc_recibido == CRC8(datos)   → eco del propio CMD      → descartar
crc_recibido == ~CRC8(datos)  → ACK legítimo de D1      → aceptar (si RGB coincide)
otro valor                    → trama corrupta o externa → descartar
```

Esta solución no requiere hardware adicional, no aumenta la latencia y es robusta contra cualquier nivel de eco ambiental.

### 9.3 Problema: latencia en la actualización de LEDs al desconectar

**Descripción técnica:** en la versión original, la desconexión se iniciaba desde D3 con `ws.close()` en el navegador. Los navegadores modernos (Chrome, Firefox, Safari) implementan el cierre de WebSocket de forma **asimétrica**: el cliente envía el frame `CLOSE` del protocolo WebSocket (RFC 6455 §5.5.1), pero el socket TCP subyacente no se cierra inmediatamente. El cliente espera el frame `CLOSE` de confirmación del servidor antes de cerrar el TCP.

ESPAsyncWebServer procesa el frame `CLOSE` entrante en el Core 0 (tarea lwIP), pero la generación del evento `WS_EVT_DISCONNECT` (que actualiza los LEDs) puede retrasarse hasta que el TCP cierra limpiamente, lo que en algunos navegadores tarda varios segundos.

**Solución: Graceful Shutdown iniciado desde el servidor:**

```
D3                              D2
 │                               │
 │── CMD_DESCONECTAR + CRC-16 ──►│
 │                               │  D2 responde y cierra desde el servidor
 │◄─ ACK_DESCONECTAR + CRC-16 ──│
 │                               │  client->close(1000, "Cierre solicitado por D3")
 │   ws.close() automático       │  ← el cierre desde servidor es determinístico
 │                               │  WS_EVT_DISCONNECT disparado inmediatamente
 │                               │  LEDs actualizados sin latencia
```

Cuando el servidor llama a `client->close(1000, ...)`, ESPAsyncWebServer genera `WS_EVT_DISCONNECT` de forma síncrona e inmediata antes de que el TCP termine de cerrarse, actualizando los LEDs en tiempo real.

En D3, al recibir `ACK_DESCONECTAR`, la interfaz actualiza visualmente su estado (badge, banner, botones) sin esperar al evento `onclose` del socket, que puede llegar con retardo. El resultado percibido por el usuario es la actualización instantánea de los tres indicadores: LEDs en D2 y badge en D3.

---

## 10. Comparativa de ambos CRC en el sistema

| Característica | CRC-8/MAXIM (D1 ↔ D2) | CRC-16/IBM (D2 ↔ D3) |
|----------------|------------------------|----------------------|
| Poly | `0x31` | `0x8005` |
| Init | `0xFF` (guarda eco nulo) | `0x0000` |
| Bits del checksum | 8 | 16 |
| Overhead de integridad | 8/32 bits = 25% de la trama | 2/~100+ bytes ≈ 2% del JSON |
| Longitud de mensaje | 3 bytes fijos (24 bits) | Variable (30–300 bytes) |
| HD=4 garantizado | ≤ 119 bits (suficiente) | ≤ 32.767 bits (suficiente) |
| Ráfaga de error 100% detectada | ≤ 8 bits | ≤ 16 bits |
| Probabilidad fallo de detección | 1/256 = 0.39% | 1/65536 = 0.0015% |
| Canal | IR óptico half-duplex | TCP/IP WebSocket full-duplex |
| Riesgo dominante | Eco, ruido ambiental IR | JSON malformado, corrupción en buffer |
| Asimetría adicional | ~CRC para distinguir CMD/ACK | N/A (full-duplex, no hay eco) |
| Implementación | C++ en D1 y D2 (idéntica) | C++ en D2 + JavaScript en D3 |
| Compatibilidad cruzada | Mismo código, mismo Init | Mismo Poly, misma lógica bit |

### Resumen de la estrategia de integridad del sistema completo

El sistema aplica defensa en profundidad con dos capas de CRC independientes:

```
D3 ──[CRC-16/IBM]── D2 ──[CRC-8/MAXIM + ~CRC]── D1
     JSON WebSocket      NEC Raw IR 32 bits
     cobertura ≤32767b   cobertura ≤119b
```

Un mensaje que atraviesa el sistema completo pasa por dos verificaciones de integridad con polinomios, longitudes de registro e incluso capas físicas completamente distintas. La probabilidad de que un error no sea detectado en ninguna de las dos capas es el producto de las probabilidades individuales: 0.39% × 0.0015% = 5.85 × 10⁻⁶ (aproximadamente 6 errores no detectados por cada millón de transacciones completas).

---

*Documentación elaborada en el marco del TP3 Integrador — Comunicación de Datos, Ingeniería en Computación.*  
*Bevilacqua, Francisco — Clement, Sebastián — Junio 2026.*