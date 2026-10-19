# Controles — inFAMOUS Second Son (port nativo)

Este documento describe los controles del port en Windows. El juego conserva los prompts de
PlayStation 4; el port traduce los botones del mando y del teclado a esos botones. No cambia el
contenido del juego.

## 1. Mando Xbox 360 / Xbox (recomendado)

Conecta el mando antes de abrir el juego. El port usa SDL3 y reconoce mandos XInput / Xbox 360 de
forma nativa. La asignación por defecto es:

| Xbox 360 | Botón PS4 | Uso habitual |
|---|---|---|
| A | Cross (✕) | Saltar, confirmar, avanzar |
| B | Circle (○) | Cancelar, ataque cuerpo a cuerpo |
| X | Square (□) | Acción contextual |
| Y | Triangle (△) | Acción contextual |
| LB | L1 | Modificador / ataque secundario |
| RB | R1 | Ataque / habilidad |
| LT | L2 | Apuntar |
| RT | R2 | Disparar / usar poder |
| LS | L3 | Correr / pulsar stick izquierdo |
| RS | R3 | Pulsar stick derecho |
| Start | Options | Pausa / menú del juego |
| Back / View | Clic derecho del touchpad | Pausa / foto |
| D-pad | Cruceta | Navegación de menús |
| Stick izquierdo | Stick izquierdo | Mover al personaje |
| Stick derecho | Stick derecho | Cámara |

Notas para Xbox 360:

- El mando Xbox 360 **no tiene touchpad, Share ni botones MISC1**. Para el clic izquierdo del
  touchpad, mantén **LB + Back/View**; para el gesto de deslizar (por ejemplo, abrir una puerta),
  mantén **Back/View** y empuja el stick derecho hacia el lado indicado. Un toque rápido sin mover
  el stick sigue siendo un clic.
- El modo sensor del grafiti se activa con **LS+RS** (o con el botón Share/Capture en mandos que lo
  tengan). Mantén el mando de lado, apunta con el stick izquierdo, **A** agita el spray y **LT**
  (L2) pinta.
- Si el menú del port muestra nombres de botones de PlayStation, arranca con
  `TLG_GLYPHS=xbox` para que use letras A/B/X/Y en las ayudas.

## 2. Teclado y ratón

| Tecla / ratón | Botón PS4 | Uso |
|---|---|---|
| W A S D | Stick izquierdo | Mover |
| Flechas | Stick derecho | Cámara |
| Espacio | Cross | Saltar / confirmar |
| Shift izquierdo | Circle | Cancelar / ataque |
| E | Square | Acción contextual |
| Q | Triangle | Acción contextual |
| 1 / 3 | L1 / R1 | Modificadores |
| R / F | L2 / R2 | Apuntar / disparar |
| Z / C | L3 / R3 | Pulsar sticks |
| Enter | Options | Pausa |
| Tab | Touchpad izquierdo | Foto / gesto |
| Retroceso | Touchpad derecho | Pausa |
| I / K / J / L | Cruceta ↑ / ↓ / ← / → | Navegación |
| Ratón | Stick derecho | Cámara (si `mouse_enabled` está activo) |
| Clic izquierdo / derecho / central | R2 / L2 / R3 | Acciones |

Ratón:

- El juego captura el ratón mientras la ventana tiene el foco. **Esc** lo libera; un clic en la
  ventana lo vuelve a capturar.
- **Modo touchpad**: mantén **Ctrl izquierdo** o **Mouse X1**. El botón izquierdo toca y arrastra
  (clic / swipe), el central toca sin clic y el derecho simula dos dedos.
- Sensibilidad, inversión, suavizado, aceleración y anti-zona muerta se ajustan en
  **Insert → Controls → Mouse**.

Grafiti sin giroscopio (teclado):

- El prompt del juego pide **L2**: pinta con **R** o con el **botón derecho del ratón**. `F` es R2
  y no completa el grafiti.
- Mantén **T** o **G** para inclinar el mando a un lado.
- Con T/G mantenido, las **flechas** o el **ratón** apuntan el cursor del grafiti.
- **Y** agita el spray.
- **H** (o **Z**+**C**) enciende y apaga el **modo sensor**: el mando se pone de lado como una lata
  y los sticks y el cross se ocultan al juego (ni caminar, ni saltar, ni girar la cámara).
- **Ctrl+H** arma el **asistente de una tecla**: con el modo sensor encendido, él solo barre la
  mira, agita a ~2 Hz y pinta en pulsos (L2 y R2), así que el tutorial se completa sin tocar nada.
  Vuelve a pulsar **Ctrl+H** para pararlo; si no, se para solo a los 150 s
  (`TLG_MOTION_ASSIST_SECS`). Solo actúa dentro del modo sensor. Detalle y medidas:
  `docs/analysis/motion-assist.md`.

## 3. Menú y HUD del port

| Acción | Tecla / mando |
|---|---|
| Mostrar/ocultar FPS, frametime, GPU y VRAM | **F10** |
| Abrir/cerrar el menú del port | **Insert** o **R3 + L2** (Xbox: **RS + LT**) |
| Cambiar de pestaña en el menú | **Q / E** o **L1 / R1** (Xbox: **LB / RB**) |
| Cerrar el menú | **Esc** o **Insert** |
| Captura de frame Vulkan (si `TLG_FRAME_CAPTURE_KEY=1`) | **F11** |

El HUD aparece por defecto en la esquina superior izquierda al arrancar. Muestra:

```
FPS   frametime   GPU %   VRAM usada / presupuesto del driver
```

Se puede cambiar a otra esquina, tamaño, opacidad, disposición y elementos en
**Insert → Overlay**. Los valores se guardan en `tlg.ini` (`show_fps`, `overlay_items`,
`overlay_corner`, `overlay_scale`, `overlay_opacity`, `overlay_layout`). `F10` solo lo muestra u
oculta durante la sesión; para que arranque oculto, pon `show_fps=0` en `tlg.ini` o usa
`TLG_SHOW_FPS=0`.

## 4. Remapeo

El port permite cambiar botones sin recompilar:

```powershell
# Mando: Q pasa a ser Cross y Espacio Triangle; el resto igual
$env:TLG_PAD_MAP = 'cross=q,triangle=space'

# Teclado/ratón: Cross en Q, Circle en clic derecho del ratón
$env:TLG_KEY_MAP = 'cross=q,circle=Mouse Right'
```

- Formato: `boton_ps4=nombre`, separados por comas. Varios orígenes para un botón con `|`
  (`cross=q|Mouse Left`). `none` deja el botón sin fuente.
- Nombres de botón PS4: `cross`, `circle`, `square`, `triangle`, `l1`, `r1`, `l2`, `r2`, `l3`,
  `r3`, `options`, `touchpad`/`touchpad_left`, `touchpad_right`, `up`, `down`, `left`, `right`.
- Nombres de mando: los de SDL (`a`, `b`, `x`, `y`, `leftshoulder`, `righttrigger`, `back`,
  `start`, `guide`, `misc1`, etc.). Nombres de teclado: los de SDL (`q`, `space`, `Left Shift`,
  `Return`, ...). Ratón: `Mouse Left`, `Mouse Middle`, `Mouse Right`, `Mouse X1`, `Mouse X2`,
  `Wheel Up`, `Wheel Down`, `Wheel Left`, `Wheel Right`.
- La configuración del teclado también se puede guardar en `tlg.ini` con `key_map=...`.

## 5. Comprobación rápida

1. Abre `Insert` y comprueba que la pestaña **Controls** muestra el mapeo de teclado y ratón.
2. Pulsa **F10**: el HUD de FPS debe aparecer/desaparecer.
3. En el menú, **Q/E** o **LB/RB** cambian de pestaña.
4. Con el mando, **A** debe confirmar y **B** cancelar en los menús del port.
5. Si el mando no responde, revisa `port/out/logs/run-*.log`: debe aparecer
   `Runtime: gamepad connected: ...`.
