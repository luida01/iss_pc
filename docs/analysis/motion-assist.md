# Asistente de motion (graffiti) — patrón, cobertura y límites

> **Fecha:** 2026-10-19 · **Alcance:** `port/src/runtime_pad.c` (asistente de una tecla),
> `port/tests/test_motion_assist.c` (test), `tools/motion_coverage.py` (medición).
> **Estado:** implementado y medido; **sin validar en vivo** (el juego no se ejecutó: falta la
> sesión manual del usuario con el tutorial del graffiti).

## 1. Qué hace

El grafiti es la única escena que pide *motion*: el juego dice «Hold Wireless Controller Sideways»
y hay que agitar la lata y pintar. Sin giroscopio (teclado/ratón o mando sin sensores) el prompt
no se puede cumplir. El asistente lo resuelve solo:

| Pieza | Comportamiento |
|---|---|
| **Modo sensor** (`H`, o `Z`+`C`, o los sticks + stick izquierdo) | El mando se pone de lado (roll 90°) como una lata, los sticks y el cross quedan ocultos al juego (ni caminar, ni saltar, ni girar la cámara) y el stick izquierdo apunta la mira del grafiti |
| **Asistente** (`Ctrl+H`, o `TLG_MOTION_ASSIST=1` al arrancar) | Con el modo sensor activo, mantiene la lata de lado, barre la mira, sacude a ~2 Hz y dispinta en pulsos: el tutorial se completa sin tocar nada |
| **Teclado** | `T`/`G` giran la lata, flechas apuntan, `Y` sacude, `R` = **L2** (pinta; es lo que pide el juego) y `F` = R2, botón derecho = L2, botón izquierdo = R2 |
| **Presupuesto** | `TLG_MOTION_ASSIST_SECS` (150 s por defecto, 0 = nunca) limita el tiempo en modo sensor; al agotarlo, el asistente se apaga y devuelve el control |

El asistente **solo actúa dentro del modo sensor**: en juego normal no toca nada.

## 2. El patrón (y por qué cambió)

`motion_assist_at(t)` es una función pura (sin estado ni reloj): la misma `t` da siempre lo mismo,
así que se puede testear sin el juego.

```
yaw       = 0.75 * sin(2*pi*0.37*t)
aim_pitch = 0.60 * sin(2*pi*0.35*t)
sacudida  = (t mod 0.5) < 0.2      (los primeros 0.2 s de cada medio segundo)
spray     = (t mod 0.5) >= 0.2     (el resto: L2 y R2 a la vez)
```

Dos frecuencias próximas pero distintas (0.37 y 0.35 Hz) hacen que la figura de lissajous **rellene** la
elipse del stencil en vez de dibujar anillos. La sacudida y el spray nunca se piden a la vez: el
juego lee una u otra.

El patrón anterior era una espiral (`r*sin(2*pi*0.30*t)`, `r` entre 0.2 y 0.7 con respiración de
10 s). **Medición** (`tools/motion_coverage.py`): la espiral dejaba **74,6 %** del stencil sin
pintar, porque los anillos quedaban más separados que el pincel (0.1 rad). El lissajous actual
cubre **99,7 % en 30 s** y **100 % en 60 s**; sobre una elipse más amplia (0.90×0.75) llega al
94,5 %, frente al 58 % de la espiral.

| Patrón | vel. trazo | stencil 0.65×0.55 (30 s / 60 s) | amplia 0.90×0.75 (30 s) |
|---|---|---|---|
| espiral 0.30 Hz (anterior) | 76 °/s | 74,6 % / 74,6 % | 58,2 % |
| espiral 0.45 Hz, respiro 8 s | 113 °/s | 97,3 % / 97,5 % | 78,9 % |
| **lissajous 0.37/0.35 (elegido)** | 125 °/s | **99,6 % / 100 %** | **94,3 %** |

Se eligió el lissajous porque llena antes y aguanta un stencil mayor; la velocidad del trazo se
midió para que no fuese un barrido imposible de imitar (los ~70 °/s de un jugador que repasa a
mano, contra los 125 °/s del asistente, que va más rápido pero con trazo discontinuo: el spray
está activo 0.3 s de cada 0.5 s).

## 3. Modo «pintado asistido» (el que faltaba para el usuario)

En la sesión manual del 2026-10-19 (218 s, 60 FPS, sin crash) el log **no tiene ni una línea
`Runtime: sensor mode on`**: el jugador nunca entró en modo sensor, así que la escena del grafiti
no recibía ni la pose de lado ni la sacudida y la pintura no avanzaba.

Por eso el asistente tiene ahora un segundo disparador (`TLG_MOTION_ASSIST_TRIGGER`):

| Valor | Qué hace |
|---|---|
| `sensor` (por defecto) | El asistente acts mientras el modo sensor está encendido (comportamiento anterior) |
| `paint` | El asistente acts **mientras el jugador mantiene el spray** (`R` o botón derecho): el port pone la lata de lado, la sacude sin parar y barre la mira; el jugador pinta. No hace falta modo sensor y los sticks y el cross siguen con el juego, así que se puede seguir jugando |

En modo `paint` el asistente **no pulsa los gatillos** (`TLG_MOTION_ASSIST_SPRAY` sin definir = 0):
el spray lo pone el jugador. Con `TLG_MOTION_ASSIST_SPRAY=1` vuelve a pulsar L2 y R2 él mismo.
`TLG_MOTION_ASSIST_AIM=0` deja la mira en manos del jugador (flechas o ratón).

```
TLG_MOTION_ASSIST=1 TLG_MOTION_ASSIST_TRIGGER=paint TLG_MOTION_ASSIST_LOG=1
```

El log del port escribe `Runtime: motion assist running (paint trigger: you paint, the port holds
and shakes the can; 150 s budget)` y, con `TLG_MOTION_ASSIST_LOG=1`, una línea por 0.6 s con la
mira y el estado de sacudida y spray.

## 4. Test

`port/tests/test_motion_assist.c` (target CMake `motion-assist-test`) incluye `runtime_pad.c` y
comprueba, sin juego ni GPU:

- la mira se mantiene dentro de los límites que el runtime impone (yaw ±1.4, aim pitch ±1.2);
- la sacudida y el spray nunca coinciden, y el spray ocupa ~60 % de cada ciclo;
- la mira no se teletransporta entre muestras de 1/60 s (salto < 0.05 rad);
- **la cobertura del stencil**: ≥ 95 % en 30 s, creciente a 60 s y ≥ 80 % a los 15 s.

```
ninja -C port/out/gpu motion-assist-test && port/out/gpu/motion-assist-test.exe
motion-assist-test: ok (stencil covered 99.7% in 30 s, 100.0% in 60 s)
```

## 5. Avisos y textos corregidos en el mismo pase

- El aviso en pantalla decía «R2 paints / R2 pinta», pero el prompt del juego es **[L2]**; ahora
  dice «L2 sprays / L2 pinta» (y «cross shakes» / «A sacode»).
- Los comentarios de `runtime_pad.c` describen ahora el mapeo real: `R`/botón derecho = L2 (lo
  que se pide) y `F`/botón izquierdo = R2 (no completa el grafiti).
- `port/tests/test_pad_map.c` lacked los stubs de `tlggpu_overlay_notice`,
  `tlggpu_graffiti_guide` y `tlggpu_sensor_aim_scale` que el asistente añadió al runtime: el
  target `pad-map-test` no enlazaba. Corregido.

## 6. Qué falta (para la sesión manual)

1. Jugar el tutorial del grafiti con el asistente armado (`Ctrl+H`): confirmar que la escena pide
   L2 y que el stencil se completa (si el juego pide el spray *continuo* y no por pulsos, hay que
   pasar a sakude y pintar a la vez).
2. Comprobar en el log las líneas `Runtime: motion assist ...` con `TLG_MOTION_ASSIST_LOG=1`.
3. Verificar que al terminar el modo sensor se apaga solo y el juego vuelve a aceptar el mando.

## 7. Nota de método

La medición original del agente quedó a medias: su script de simulación (JavaScript en la
herramienta `execute`) se quedó girando en el servidor y la sesión se colgó 52 minutos; el agente
se canceló y la medición se rehízo aquí en Python (`tools/motion_coverage.py`), que da los mismos
números que el test en C.
