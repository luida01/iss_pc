# Hoja de ruta de rendimiento — llegar a 60 FPS jugables en 4 GB

> **Fecha:** 2026-10-19 · **Objetivo:** que inFAMOUS Second Son sea jugable en la RTX 3050 Laptop de
> 4 GB: 60 FPS estables en zonas normales, ≥30 FPS en las pesadas, sin congelaciones y sin perder detalle.
> **Regla del proyecto:** un cambio cuenta como mejora solo con 3 repeticiones de la misma escena
> (AGENTS.md §5). Nada de esto se da por bueno sin medición.

## 1. Línea base (sesión manual del 2026-10-19, 30 min, 720p, `ISS_GC_BUDGET_MB=3500`)

Evidencia: `evidence/20261019-130735-g0-manual-paint/` (29 capturas, `samples.csv`, `run.log`).

### 1.1 Comparación con el runtime de referencia (shadPS4 upstream + parches, 2026-10-13)

Prueba `fasthost-a1` (misma máquina, mismo juego): `work/graphics-tests/fasthost-a1-v58b2-20261013/`

| Momento | Referencia (upstream) | Nuestro port |
|---|---|---|
| Primeros 12 s | 60,0 FPS, **GPU 98-100 %**, VRAM 3039-3356 MB | 57-60 FPS |
| Zona pesada (t>14 s) | 17-38 FPS, GPU 100 % | **25-35 FPS, GPU 63 %** |
| t>50 s | **colapso a 0,6-3,9 FPS** con la GPU al 100 % | 25-35 FPS (sin colapso) |
| VRAM | estable en 3341 MB | 3847-3874 MB (94 %) |

**Lectura**: nuestro port va de 10 a 30 veces más rápido que el upstream en la zona pesada y la GPU
**no** está saturada (63 %). El techo actual es nuestro camino de CPU, no la tarjeta. Si se despeja,
el límite pasa a ser el trabajo de GPU (~40 FPS a 720p nativo en esa escena) y de ahí a 60 FPS
bajando la resolución interna (FSR 3.1 / live resolution, como hace `gow3_pc` por defecto).

### 1.2 Números de la sesión

| Métrica | Valor | Lectura |
|---|---|---|
| FPS | mediana **25,4**, p95 54,4 | Zonas ligeras 57-60 FPS; escenas pesadas 25-28 |
| Frame time | 33,6 ms mediana, 67,8 ms p99 | Picos de **2,5-9,5 s** (congelaciones) |
| VRAM | 3847-3874 MB de 4096 | **94 %**: el presupuesto de 3500 no se respeta |
| RAM | 7,6 GB working set, 9,5 GB privados | Cabe en 16 GB, pero apretado |
| GPU | 63 % mediana, 76 °C, 37,8 W | **No satura**: el cuello es la CPU del port |
| Perfil del port | ProcessCompute 31 %, ProcessGraphics 28 %, OnWriteFault 29 %, Process 11 % | Tres focos claros |
| Draws | 860-1900 por frame | La escena pesada hace muchos draws |
| `reduced-size draws` | **0 de 140** | El juego pide 140 draws a media resolución y no los aprovechamos |
| Write faults | 900-1200/s, "0 hot pages" | El tracker protege y faulta sin cachear las calientes (y la caché "caliente" ya se midió como **peor**: más subiduras que faults) |
| Texture cache | "4757 of 1922 MiB (critical 1922): 334 images evicted, 0 written back" | **Ciclo de expulsión y recarga** |
| Decode BC | El 13-oct: **0 decodes**; hoy: BC 1D decodificado en CPU (el driver 617.42 rechaza las imágenes BC 1D) | Coste de CPU nuevo en el camino de frame, repetido por el ciclo de expulsión |

## 2. Lo que hace GOW3 (referencia: 60+ FPS estables en la misma máquina)

RE del clon `C:\Users\luisd\proyectos\gow3-lab` (`main` @ `41e9473`) frente a nuestro árbol:

| Técnica de GOW3 | Cómo la:apply |
|---|---|
| `upscaler: fsr3` + **`live_resolution: auto`** por defecto en su launcher | Renderiza por debajo de la nativa y sube la imagen; la resolución dinámica ajusta sola. Nosotros: AA nativo 720p, `live_resolution=0` |
| `55ab9ee`: parche del **global de FPS del motor en memoria** (`engine_fps_global`) | Permite 60/120 FPS reales sin XML de parche; nosotros solo tenemos la opción (inerte sin parche) |
| `970fdc2`: el **buffer de media resolución** del juego se escala con el parche de resolución | Origen de los "reduced-size draws": si el juego sabe el tamaño a media resolución, puede renderizar efectos cheaper |
| `85a670c`/`90aaf36`: fix del **black screen de arranque** (interbloqueo GPU↔flips) | Técnica aplicable a nuestros parches de arranque |
| `b646683` (ya en el nuestro): vistas de altura parcial para poderes y efectos de pantalla | Ya está: `PartialHeightViews`, `needs_native` |
| Estructura del core | **Es el nuestro**: somos copia + sync. No hay un renderer "mejor" que copiar: el 60+ FPS de GOW3 viene de renderizar menos píxeles, no de un pipeline superior |

**Conclusión de la RE**: no hay un truco de motor que nos falte; lo que falta es (a) no estrangular la
memoria de texturas, (b) quitar el trabajo inútil por frame y (c) decidir con datos qué resolución
(escalado) usar en 4 GB.

## 3. Palancas (con dueño y criterio de aceptación)

| # | Palanca | Dueño | Estado | Criterio de aceptación |
|---|---|---|---|---|
| P1 | **Política del colector de texturas**: que compare los bytes propios de la caché contra el presupuesto del launcher (3500) y no el total del port contra el `critical` derivado del device-local del driver (1922) | agente VRAM | en curso | 0 expulsiones sostenidas en escena pesada; VRAM ≤ 3800 MB; el "critical" sale de 3500, no de 1922 |
| P2 | **Coste del write-fault** (29 % del frame): 1358-1823 faults/s; "0 hot pages" es correcto (las páginas calientes son opt-in y se midieron **peor**: 33 FPS + timeout del ring) | agente renderer | caracterizado | faults/s por debajo de 200 con el mismo aspecto, o explicación de por qué no se puede |
| P3 | **ProcessGraphics / ProcessCompute** (30 % / 44 %): **no son el decode PM4**: las tres líneas caras del perfil son la **misma espera** (el hilo GPU esperando al hilo de grabación; `WaitRegMem` = 52,9 % de las esperas). El cuello real es **stage B al 85-89 % ocupado** | agente renderer | caracterizado + perfiles nuevos | `TLG_DRAW_PROFILE=1` (fases del draw) y `TLG_PM4_PROFILE=1` (decode por clase de paquete, separado de la espera) para atribuir el 88 % |
| P4 | **`reduced-size draws` = 0 de 140-268**: el tamaño de la escena lo fijaba el upscaler temporal, y el rasterizador solo lo llamaba si había cámara virtual activa → con el upscaler apagado (nuestro arranque) nunca se llamaba | agente renderer | **hecho** (`1318134`) | con el preset activo la línea pasa de 0 a ~140 y el frame time baja con la GPU menos ocupada; Native AA deja los objetivos reducidos apagados (comportamiento idéntico) |
| P5 | **Shaders wave64 y partículas** (`LowerWave64BallotPass ... ReadFirstLane`) | agente shaders | **hecho** (`dc12e8d`) | ver P13 |
| P6 | **Resolución y escalado**: qué predefinimos para 4 GB (nativo / FSR 3.1 / FSR 4.1.1 / preset Quality) | yo + agente VRAM | pendiente | tabla con FPS, frame time y VRAM de cada opción (`tools/perf-ab.ps1`) |
| P7 | **Presupuesto de VRAM coherente con el driver**: el device-local real es ~2,4 GB, no 4096, y la contabilidad del port declara 4757 MiB (más que la tarjeta) | agente VRAM | en curso | el presupuesto por defecto no supera lo que la tarjeta da en local, y el log avisa cuando el total declarado es mayor que la tarjeta |
| P8 | **Global de FPS del motor** (técnica `55ab9ee` de GOW3) para inFAMOUS | yo (RE) | descartado de momento | el motor ya corre a 60 (`applied_engine_fps`); subirlo no mejora los FPS, solo la latencia |
| P9 | **Copias host→GPU y syncs del recorder** (2,8 % / 127-300 por frame; 12,9-21,3 MB/frame) | agente renderer | medido | pendientes de atribuir con el perfil nuevo |
| P10 | **Presets del launcher** con las palancas anteriores, explicados al usuario | yo (Qt launcher) | pendiente | el usuario elige "Detalle / Equilibrado / 4 GB" y sabe qué gana y qué pierde |
| P11 | **Decode BC fuera del camino crítico**: el driver 617.42 rechaza las imágenes BC 1D y las decodificamos en CPU durante la subida; con el ciclo de expulsión (P1) se repite | agente VRAM | pendiente | el decode no aparece en el frame que lo pide (hilo aparte) y las texturas decodificadas no se vuelven a decodificar al recrearlas |
| P12 | **Emulación ISA de `EXTRQ`**: el juego la ejecuta ~67 millones de veces en 30 min (5 sitios) y la emulamos nosotros | yo | medido | coste medido por instrucción y, si es alto, una versión rápida en línea |
| P13 | **Partículas: `ReadFirstLane` en wave64** — el port solo lo bajaba en los shaders que usaban ballot o `v_mbcnt`: 331 de 346 shaders compute (949 de 1005 instrucciones) quedaban sin bajar, así que en un host de 32 lanes la mitad superior de cada wave leía su propia lane 32 en vez de la primera. En el patrón "append" (una lane reserva el hueco con un atómico y difunde la base) **la mitad de la lista se perdía**: esas son las partículas que faltaban | agente shaders | **hecho** (`dc12e8d`) | el broadcast por subgrupo coincide con la referencia wave64 (sonda en la RTX 3050) y el test cubre uniforme, divergente y dos waves por workgroup; queda medir el coste de las barreras añadidas (A/B `TLG_RFL_ALL=0` vs `1`) |

## 3.1 Auditoría de cableado (qué existe pero no hace nada)

Barrido automático de los ajustes y de los toggles del port:

| Hallazgo | Detalle | Acción |
|---|---|---|
| **El preset no escala la escena** | `SceneResolution::ForPreset` (scene_resolution.h:13) y `TlgSettings::PresetScale` (tlg_settings.cpp:407) están definidos y **nadie los llama**: elegir "Quality"/"Balanced"/"Performance" no reduce la resolución interna | cablear el preset al tamaño de escena (agente renderer) |
| `live_resolution` sin uso | 0 usos en todo el port: es una capacidad del driver (exige ≥8 GB y GPU suitable); en nuestra 4 GB no aplica | documentarlo, no tocarlo |
| `dlss_supported` / `dlss_problem` sin uso | Campos muertos del puente DLSS | limpiar cuando se toque ese puente |
| 52 de 53 toggles conectados | Solo `RegionCache` no se usa en ningún sitio | dejar constancia; el resto está bien |
| **A/B en caliente disponible** | `TLG_TOGGLE_FILE` se relee cada 250 ms: es la máscara de optimizaciones **desactivadas** | `tools/toggle.ps1` (listar / desactivar / reactivar por nombre) para medir en vivo sin reiniciar |

## 3.2 Reglas para la medición final

- **Una variable por pasada**: con `tools/perf-ab.ps1` (resolución + claves de `tlg.ini` + entorno) para
  cambios de configuración, y con `tools/toggle.ps1` para A/B en caliente de una optimización.
- Tres repeticiones por cambio; se comparan mediana, p95, frame time p99, VRAM máxima y las capturas.
- Ningún cambio se acepta sin que el perfil muestre la mejora **y** las capturas no pierdan detalle.

## 4. Definición de "jugable" (la we'll medir en la sesión final)

- 60 FPS (frame time ≤ 16,7 ms) en zonas normales.
- ≥ 30 FPS en la escena más pesada conocida (la de los tableros, ~1900 draws).
- **Ningún frame por encima de 500 ms** (hoy hay de 9,5 s).
- VRAM estable ≤ 3800 MB y sin "evicted" en el log.
- 15-30 min de juego real sin reinicios ni congelaciones (Gate G0).

## 5. Protocolo de medición

1. `tools\perf-ab.ps1` encadena configuraciones y produce `summary.csv` + `summary.md`.
2. Cada pasada: 120-180 s, la misma zona, capturas cada 30 s.
3. Se comparan mediana, p95, VRAM máxima, RAM máxima y el aspecto (capturas).
4. 3 repeticiones por cambio; el que gana se queda.

## 6. Fase final: checklist (cuando lleguen los tres agentes)

1. **Integrar**: commits de los agentes (renderer, VRAM/GC, shaders) + el fix de partículas (`dc12e8d`)
   + el enlace `onecore` del test de shaders.
2. **Build limpio**: `bash build.sh` (rama MSYS: compila la librería GPU y los tres ejecutables) y
   `ninja` de los targets de test afectados. Sin errores ni warnings nuevos.
3. **Tests**: `motion-assist-test`, `pad-map-test`, `perf-overlay-test`, `bc-decode-test`,
   `orb-multiplier-test`, `recompiler-fixes-test`, `test_gc_budget`, `test_draw_profile` y la suite
   Python (`python -m unittest discover -s port/tests -t port/tests`; 7 fallos ambientales
   preexistentes: WSL, PySide6, bloqueos de fichero en Windows, cp1252).
4. **Primera medición (A/B)**: `tools\perf-ab.ps1 -DurationSec 120` con la matriz por defecto
   (720p nativo, fsr3, fsr3+jitter, fsr411, fsr3+preset Quality, 480p, la variante vieja del
   fix de partículas y `frames_queued=2`). Salida: `evidence/perf-ab-<ts>/summary.csv` y `summary.md`.
   **Antes, una pasada de calentamiento** de 60 s: el fix del recompilador (`dc12e8d`) invalida la
   caché de shaders una vez, y sin calentar la primera configuración mediría la compilación.
5. **Elegir configuración**: la que de más FPS con el detalle intacto en las capturas; si hace
   falta, un A/B en caliente con `tools\toggle.ps1` sobre la opción ganadora.
6. **Sesión manual larga** (Gate G0): 15-30 min con capturas cada 30 s, HUD visible, y el
   grafiti probado con el modo de pintado. Sin frames > 500 ms, sin expulsiones de textura
   sostenidas y sin congelaciones.
7. **Cerrar**: documentar los números finales en este documento y en `docs/analysis/gate-g0-baseline.md`.

## 7. Lo que NO se va a hacer

- Copiar código de GOW3: se replica el **comportamiento** (atribución en `port/UPSTREAM.md`).
- Bajar la resolución a ciegas: primero se mide (P6) y se informa del coste en detalle.
- Tocar la red ni descargar nada del juego: todo el trabajo es sobre nuestro dump y nuestro código.
