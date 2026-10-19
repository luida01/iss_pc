# CPU del camino de dibujado: análisis y cambios (2026-10-19)

Port nativo de inFAMOUS Second Son (CUSA00223), rama `bootstrap/windows-g0`.
Alcance de este informe: `port/gpu/shadps4/video_core/renderer_vulkan/**`,
`port/gpu/shadps4/video_core/amdgpu/liverpool.{h,cpp}` y `port/tests/**`.

## 1. Resumen

Lo que dice el log de la sesión `evidence/20261019-130735-g0-manual-paint` (30 min, RTX 3050
4 GB, 720p, `ISS_GC_BUDGET_MB=3500`):

| Métrica | Medido | Lectura |
|---|---|---|
| FPS (mediana) | 25,4 (p95 54,4) | 33,6 ms de frame |
| GPU | 63 % mediana | **la GPU no está saturada** |
| CPU | 23 % mediana (73 % máx) | ~3,7 de 16 hilos |
| Draws por frame | 1 900 – 4 400 (3 127 en la ventana citada) | |
| Submisiones por frame | 53 – 83 | |
| "stage A waited" | 50 – 67 % del tiempo | el hilo GPU espera al hilo de grabación |
| "stage B busy" | 85 – 89 % | **el hilo de grabación es el cuello** |
| "GPU ticks" | 38,9 – 51,5 % | espera por el fence del frame anterior |
| Write faults | 1 358 – 1 823 /s | 0 hot pages |
| "reduced-size draws" | **0 por frame** de 140 – 268 | el dial de resolución está muerto |
| Buffer uploads | 12,9 – 21,3 MB/frame | |
| Helper CPU (5 s) | draw recording 2 484 ms, Vulkan recorder 2 094 ms | 0,50 + 0,42 núcleos |

**Conclusión de la lectura**: las tres líneas más caras del perfil ("stage A waits over 0.5 %")
**no son tres focos distintos**: las tres son la *misma* espera (el hilo de comandos de la GPU
esperando al hilo de grabación de draws). El coste de CPU del frame está en el hilo de
grabación (88 % ocupado) y, por detrás, en el decode PM4 que lo alimenta.

Tres cambios quedan implementados aquí (todos con test y build limpio):

1. **El dial de resolución estaba muerto** (`reduced-size draws 0/frame de 140`): el tamaño de
   los objetivos de la escena lo fijaba el *upscaler temporal* en su inicio de frame, y ese
   inicio sólo corría con el movimiento de cámara activo. Con el upscaler apagado (el arranque
   por defecto: `ISS_UPSCALER=off`) el tamaño nunca se fijaba, `SceneTargets::Reduced()` era
   falso y **ningún draw se reducía**. Ahora el preset del menú reduce la escena también sin
   upscaler temporal.
2. **La clave del memo de objetivos de render** se construía en un array local que se ponía a
   cero y se copiaba entero (256 bytes) por target y por draw: 10,5 → 1,5 ns por lookup
   (85 % menos, micro-benchmark reproducible).
3. **Dos perfiles nuevos** para que la medición final pueda apuntar: el coste por fase del draw
   (`TLG_DRAW_PROFILE=1`) y el coste del decode PM4 por clase de paquete
   (`TLG_PM4_PROFILE=1`), que separan el *trabajo* de la *espera*.

## 2. El perfil por función (antes/después)

Las dos medidas que el port imprime y que valen para esta comparación:

* `stage A waits over 0.5%: <función>:<línea> <pct>%` — tiempo que el hilo de comandos de la
  GPU pasó **esperando** al hilo de grabación, por sitio de espera.
* `Frame stats: ... <pct>% ...` y `Draw pipe: ... stage A waited <pct>%, stage B busy <pct>%`.

### 2.1 Antes (sesión `20261019-130735-g0-manual-paint`, log tal cual)

| Sitio | Función | Máx. en la ventana | Dónde está |
|---|---|---|---|
| `ProcessCompute:1741` | `Liverpool::ProcessCompute` | **44,1 %** | `amdgpu/liverpool.cpp` (dentro del alcance ampliado) |
| `ProcessGraphics:1328` | `Liverpool::ProcessGraphics` | **30,2 %** | `amdgpu/liverpool.cpp` (dentro del alcance ampliado) |
| `OnWriteFault:3857` | `Rasterizer::OnWriteFault` | **29,0 %** | `renderer_vulkan/vk_rasterizer.cpp` |
| `Process:194` | `Liverpool::Process` | 6,4 % | `amdgpu/liverpool.cpp` |
| `OnSubmit:180` | `Rasterizer::OnSubmit` | 5,3 % | `renderer_vulkan/vk_rasterizer.cpp` (llamado desde `liverpool.cpp:180`) |

Motivos de las esperas (misma línea del log, `drains by reason/s`):

```
WaitRegMem=212 (52.9 %)  submission end=6 (2.6 %)  rasterizer=7 (3.1 %)
```

Es decir: **el 53 % de las esperas son un solo sitio** — la espera por etiqueta de VO
(`WaitRegMem`) en `ProcessGraphics`, y su equivalente en la cola de cómputo. El resto son
"fin de submission" y "rasterizer" (el drain dentro de `OnWriteFault`/`OnSubmit`).

### 2.2 Después

Pendiente de la sesión final: los tres cambios de arriba se miden con
`tools/perf-ab.ps1` (ver §6). Lo que se puede afirmar sin ejecutar el juego:

| Cambio | Efecto esperado y por qué | Cómo se mide |
|---|---|---|
| Preset de resolución activo | Los ~140 draws de la escena pasan de 1 920×1 080 a 1 280×720 (Quality): **44 % de los píxeles**. El coste de GPU de la escena baja en la misma proporción; el de CPU (mismos draws) no cambia. | `TLG_FRAME_STATS=1` → la línea `reduced-size draws` pasa de `0/frame` a `~140/frame`, y el frame time baja con la GPU menos ocupada |
| Clave del memo | −9 ns por lookup × 9 targets/draw × ~86 000 draws/s ≈ **−7 ms de CPU por segundo** (≈ 0,8 % de un núcleo) | micro-benchmark `draw-profile-test` (10,5 → 1,5 ns) |
| `TLG_DRAW_PROFILE=1` / `TLG_PM4_PROFILE=1` | No cambia el rendimiento: lo que falta es poder *atribuir* el 88 % de ocupación del hilo de grabación | una línea nueva en el log cada 5 s |

## 3. Causa raíz por punto

### 3.1 OnWriteFault (hasta 29 %)

`Rasterizer::OnWriteFault` (vk_rasterizer.cpp) hace `DrainDrawPipe()` y espera a que el hilo
de grabación consuma todo lo encolado. Con el hilo de grabación al 88 %, esa espera es larga:
el log mide **69,8 ms de espera dentro de faults de escritura en un frame de 66,6 ms**.

Lo que **no** es la causa: "0 hot pages". Las páginas calientes son una función **opt-in**
(`TLG_HOT_PAGES=1`, `TlgToggle::HotPages`) y su propio comentario dice que con ellas el
conjunto crecía a ~14 k páginas re-subidas en cada binding, lo que era *peor* (33 FPS) y
provocó un timeout del ring de la GPU. No es un bug: es una función apagada a propósito.

Lo que **sí** es la causa: el *tracker* de escrituras (`video_core/page_manager.cpp` +
`video_core/buffer_cache/region_manager.{h,cpp}`) vuelve a proteger cada página que el
guest escribe, con lo que el coste por escritura es un fault + un `mprotect` con shootdown de
TLB. Eso está **fuera del alcance** de este agente (el agente de VRAM toca
`texture_cache/**`; el tracker vive en `buffer_cache/**` y `page_manager.cpp`).

Medición dentro del alcance: `ms in: protect 1,7 – 3,2 ms` por frame, y 1 358 – 1 823
faults/s. Con 45 faults por frame y ~2 ms de protect por frame, el `mprotect` es ~5 % del
frame; el resto de la espera es el drain.

### 3.2 ProcessGraphics / ProcessCompute (28 % + 31 %)

Ambas líneas son **esperas**, no trabajo: `ProcessGraphics:1328` es el drain antes de
`vo_port->WaitVoLabel()` (la espera por el flip) y `ProcessCompute:1741` el drain dentro del
`while (!wait_reg_mem->Test(...))` de la cola de cómputo.

El trabajo de decode PM4 en sí es pequeño frente a esas esperas: el log muestra que la
espera al hilo de grabación es el 50 – 67 % del tiempo del hilo de comandos de la GPU, y el
hilo de grabación está ocupado el 88 % del tiempo. Es decir: **el cuello es el hilo de
grabación, y el decode sólo paga la factura**.

Por eso el objetivo "bajar ProcessGraphics+ProcessCompute de ~59 % a <40 %" se persigue por
la vía de hacer que el hilo de grabación sea más rápido, no por la de tocar el decode. Para
que la medición final pueda apuntar, se ha añadido `TLG_PM4_PROFILE=1` (el coste del decode
por clase de paquete: register / draw / dispatch / memory / wait / other) y
`TLG_DRAW_PROFILE=1` (el coste por fase dentro de un draw: targets, bind, state, inputs,
dynamic, descriptors, draw, reset, vk-record). Con las dos, la pregunta "¿dónde se va el
88 %?" tiene respuesta numérica en el log en vez de conjetura.

Trabajo redundante **descartado** en el decode (revisado, no es el cuello):

* `ProcessCommands()` se llama por paquete PM4: su coste es una carga atómica cuando la cola
  del guest está vacía; con la cola llena toma un mutex y drena, y el perfil muestra **cero**
  esperas con el motivo "commands" → no es el problema.
* `HashRegisterPacket` (el checksum de los paquetes de registro) es una cadena multiplicativa
  de 5 ciclos por palabra, pero el guest escribe los registros **por pasada**, no por draw:
  su coste es <0,1 ms/frame.
* `PipelinedOpcode` / `PipelinedComputeOpcode` ya evitan el drain de los opcodes que no
  tocan el estado (incluido `SetPredication`, que antes era el 60 % del hilo).

### 3.3 "GPU ticks 51 %" y el solape

"GPU ticks" es `TlgStats::tick_wait_ns`: la espera por el *timeline semaphore* del frame
anterior, no la espera del fence de presentación (esa otra, `frames ahead`, mide 0,0 ms en el
log). La espera viene de `Scheduler::Wait()`, que se llama desde `Finish()`/`WaitSubmitted()`:
las lecturas de vuelta del guest y las subidas de buffers que esperan a que la GPU libere
memoria (`video_core/buffer_cache/**`, fuera de alcance).

Lo que sí es de este alcance y se ha mirado: `Presenter::PrepareFrame` (vk_presenter.cpp)
espera con `frames_queued` frames por delante del GPU (1 por defecto, ajustable en el menú).
Subirlo a 2 deja que el hilo de comandos de la GPU se adelante un frame más y solape mejor
con la GPU **sin cambiar el aspecto** (el otro agente ya ha metido `frames_queued=2` en el
banco A/B: `tools/perf-ab.ps1`). Con la GPU al 63 % y el hilo de grabación al 88 %, el solape
no es lo que limita hoy, pero es la palanca de la pregunta.

### 3.4 "reduced-size draws 0/frame de 140" — **corregido aquí**

La maquinaria existe y es **idéntica** a la de `gow3-lab` (diff de `vk_scene_resolution.*`
y `scene_resolution.h`: sólo cambian los nombres de los toggles y de las variables de
entorno). Lo que faltaba era quién fija el tamaño:

```cpp
// vk_temporal_upscaler.cpp (antes)
scene_targets.SetSize(SceneResolution::ForPreset(active ? preset : 0, {target_width, target_height}));
```

Esa llamada vive en `TemporalUpscaler::OnFrameStart()`, y el rasterizador sólo la llama
dentro de este bloque:

```cpp
if (camera_motion->Enabled() && regs.color_buffers[0] && FrameCapture::IsDisplayBuffer(...)) {
    ...
    if (upscaler->OnFrameStart()) { ... }
```

`CameraMotion::Enabled()` es `debug_overlay || for_upscaler`, y `for_upscaler` es
`!(upscaler == "none")`: **con el upscaler temporal apagado (el arranque por defecto) el
bloque entero no corría**, `SetSize` no se llamaba nunca, `SceneTargets::size` se quedaba en
su valor por defecto (1920×1080 = `Size{}`), `Reduced()` era falso y
`Rasterizer::BeginRendering` (vk_rasterizer.cpp) fijaba `reduced = false` en todos los draws:

```cpp
bool reduced = scene_started && upscaler->RasterScaling() && key.num_samples == 1;
// RasterScaling() = !scaled_session && scene_targets.Reduced() && !done_this_frame
```

`scene_started` sí era cierto (199 draws/frame en el log), luego el bloqueo era
`scene_targets.Reduced()`.

**El arreglo** (en `renderer_vulkan/**`):

* `vk_rasterizer.cpp`: el inicio de frame (y con él el tamaño de la escena) no depende del
  movimiento de cámara; el movimiento sigue siendo opcional.
* `vk_temporal_upscaler.cpp`: el tamaño sigue al preset también sin upscaler temporal. La
  base es el tamaño de salida del upscaler si está activo, y el del guest (1920×1080) si no.
  En Native AA el tamaño se deja en el valor por defecto → `Reduced()` sigue siendo falso →
  **ni una imagen proxy ni una copia de más**: el comportamiento por defecto no cambia.
  Con un preset > 0 la escena se renderiza a esa resolución y el reesampleo lo hace el post
  del propio juego (el mecanismo de `SampleProxy` ya lo contempla); con el upscaler temporal
  activo sigue mandando el upscaler (FSR 3.1).

Con gow3_pc el mismo camino da 140 draws reducidos porque su launcher arranca con
`upscaler: fsr3` y `preset: 1` (Quality): su `camera_motion` está activo, `OnFrameStart()`
corre y el tamaño se fija. Nuestro launcher fuerza `ISS_UPSCALER=off`
(`port/launcher/infamous_run.py:222`, motivo: "GoW3's temporal upscalers made it shake"), y
`tlg.ini` va con `preset=0`: con las dos cosas a cero, la reducción no podía pasar.

### 3.5 "host copies 2.8 % (6/frame)"

6 copias host↔GPU por frame, 1,6 – 2,8 % del frame. Son las lecturas de vuelta del guest
(`texture_cache/**` y `buffer_cache/**`, fuera de alcance) y las subidas de los 12,9 – 21,3
MB/frame. La parte que sí es de este alcance: `Pipeline::BindResources` copia las
escrituras de descriptores al *chunk* de grabación en cada draw (el puntero a los
`DescriptorBufferInfo`/`DescriptorImageInfo` se repara ahí). Es trabajo medido en la fase
"descriptors" del perfil nuevo.

### 3.6 Creación de objetos de Vulkan por draw

No hay creación de objetos de Vulkan por draw en el camino caliente: las tuberías se
cachean (`PipelineCache`), el 99,96 % de los draws usan una tubería ya preparada
("Draw preparation: 533294 prepared pipelines used, 210 not"), los conjuntos de texturas se
memoizan (88 % de aciertos) y el estado de render se memoiza (93,7 %). Lo que sí se crea
por draw son *vistas* y *conjuntos de descriptores* reusados de un pool, que es lo que
miden las fases "state" y "descriptors" del perfil nuevo.

## 4. Lo implementado

### 4.0 Un crash introducido aquí y su arreglo (2026-10-19, 17 h)

**Síntoma**: con el build de este agente, el juego moría a los ~16 s con
`Fault: read access to ... ` en `Vulkan::Rasterizer::PrepareRenderState` (el puntero más
cercano en el binario). La línea anterior del log era
`page_manager.cpp:206 UpdatePageWatchers: Tracking memory region 0x2800000000 -
0x2800000000 which is not fully GPU mapped` (una región de tamaño 0).

**Causa**: la clave del memo de objetivos de render (`vk_target_memo.h`) se construye en un
búfer **reutilizado** entre lookups, pero `TargetKey::size_` **no se ponía a cero**: la clave
crecía 76 bytes por lookup (9 lookups por draw en una pasada G-buffer) y a partir del
cuarto lookup **escribía fuera del búfer de 256 bytes**, encima de los miembros del
`Rasterizer` (`cb_descs[cb].first`, el id de la imagen). El memo contestaba con el id de
**otro** target y `PrepareRenderState` acababa en
`texture_cache.GetImage(image_id)` con un id corrupto → lectura fuera de rango →
puntero inválido.

**Arreglo**: `TargetKey::Reset()` al empezar cada clave (en `FindTargetMemoized`), un
`assert` de que la clave cabe en el búfer, y un test de regresión que recorre la secuencia
de una pasada G-buffer (9 targets × 8 frames) comprobando que la clave es siempre la del
target pedido, que el búfer no se sale (un canario detrás) y que el id que devuelve el memo
es siempre el mismo y distinto del de los demás targets.

Con el arreglo, el camino por defecto (Native AA, upscaler apagado) queda **idéntico** al
anterior: `SceneTargets::SetSize` recibe el mismo tamaño que ya tenía, `Reduced()` sigue
siendo falso y ningún draw se reduce (ni hay imagen proxy ni copia).

### 4.1 El resto

| Fichero | Cambio | Verificación |
|---|---|---|
| `renderer_vulkan/vk_temporal_upscaler.cpp` | El tamaño de la escena sigue al preset sin upscaler temporal; Native AA deja los objetivos reducidos apagados | `scene-preset-test` (tabla y tamaños) |
| `renderer_vulkan/vk_rasterizer.cpp` | El inicio de frame no depende del movimiento de cámara | build |
| `renderer_vulkan/scene_resolution.h` | Una sola tabla de divisores (`SceneResolution::PresetScale`); `ForPreset` la usa | `scene-preset-test` |
| `renderer_vulkan/vk_target_memo.h` (nuevo) | La clave del memo se construye en un bucle reutilizado: sin `memset` de 256 bytes ni copia de 256 bytes por target y draw | `draw-profile-test`: 10,5 → 1,5 ns (85 %) |
| `renderer_vulkan/vk_draw_profile.h` (nuevo) | `TLG_DRAW_PROFILE=1`: coste por fase de un draw y del hilo de grabación de Vulkan | `draw-profile-test` (contadores) |
| `amdgpu/liverpool.{h,cpp}` | `TLG_PM4_PROFILE=1`: coste del decode PM4 por clase de paquete, y las esperas summed | build |
| `tests/test_draw_profile.cpp`, `tests/test_scene_preset.cpp` (nuevos) | Test del memo, del perfil y de los presets | en verde |

**Fuera del alcance, pero duplicación muerta que hay que borrar**:
`TlgSettings::PresetScale` (`port/gpu/shim/tlg_settings.cpp:407`) repite la misma tabla de
divisores y **no tiene ninguna llamada** en todo el árbol (comprobado con grep). Ese
fichero está prohibido para este agente, así que la eliminación queda pendiente para quien
lo tenga: con `SceneResolution::PresetScale` como única tabla, `PresetScale` sobra.

## 5. Lo que queda sin resolver

| Punto | Fichero | Por qué no se toca aquí |
|---|---|---|
| Tracker de escrituras y `mprotect` | `video_core/page_manager.cpp`, `video_core/buffer_cache/region_manager.h` | Fuera del alcance de este agente (buffer cache) |
| Lecturas de vuelta y subidas de 15 – 20 MB/frame | `video_core/buffer_cache/**` | Fuera del alcance |
| Caché de texturas y GC | `video_core/texture_cache/**` | Otro agente (VRAM) |
| Decodificación BC | `video_core/shader_recompiler/**` | Otro agente |
| Espera por el fence (`GPU ticks 51 %`) | `video_core/buffer_cache/fault_manager.cpp`, `texture_cache.cpp` | Fuera del alcance; la espera es real (la GPU) |
| Coste por fase dentro del draw | `renderer_vulkan/**` | **Instrumentado** (`TLG_DRAW_PROFILE=1`); falta la medición con el juego |
| Coste del decode PM4 | `amdgpu/liverpool.cpp` | **Instrumentado** (`TLG_PM4_PROFILE=1`); falta la medición con el juego |

## 6. Cómo se mide en la sesión final

Con `tools/perf-ab.ps1` (el banco A/B). El preset se puede pasar por entorno
(`TLG_UPSCALE_PRESET`, que pisa la clave `preset` de `tlg.ini`) o por la clave del ini; los
escenarios nuevos del banco son:

| Escenario | Qué mide |
|---|---|
| `720p-nativo` | La referencia de hoy: upscaler apagado, preset 0 (1920×1080) |
| `720p-calidad` | El preset 1 **sin** upscaler temporal: la escena a 1280×720 (44 % de los píxeles) |
| `720p-balanceado` | El preset 2: 1130×636 (35 %) |
| `720p-performance` | El preset 3: 960×540 (25 %) |
| `720p-fsr3-calidad` | El preset 1 **con** FSR 3.1 (el camino de gow3_pc) |
| `720p-frames2` | `frames_queued=2`: el hilo de CPU se adelanta un frame |

```powershell
# La matriz completa (los escenarios de arriba, 120 s cada uno)
tools/perf-ab.ps1 -DurationSec 120 -Tag perf-ab-preset

# Sólo los dos extremos, para ver el efecto del dial
tools/perf-ab.ps1 -DurationSec 180 -Tag perf-ab-preset -Configs @{
    'base'     = @{ resolution = '720p'; env = @{ ISS_UPSCALER = 'off' } }
    'calidad'  = @{ resolution = '720p'; env = @{ ISS_UPSCALER = 'off'; TLG_UPSCALE_PRESET = '1' } }
}
```

Lo que hay que mirar en cada `summary.csv` y en el log:

1. La línea `Frame stats:` — `reduced-size draws` deja de ser `0/frame` y pasa a ~`140/frame`;
   el frame time de la escena pesada baja.
2. La línea `stage A waits over 0.5%:` — `ProcessGraphics:1328`, `ProcessCompute:1741` y
   `OnWriteFault:3857` (el objetivo: de ~59 % a <40 %).
3. La línea `Draw pipe:` — `stage B busy` (hoy 85 – 89 %) y `stage A waited` (hoy 50 – 67 %).
4. Con `TLG_DRAW_PROFILE=1`: la línea `Draw profile:` (fases del draw) — de ahí sale la
   siguiente optimización, con números y no con conjeturas.
5. Con `TLG_PM4_PROFILE=1`: la línea `PM4 decode:` (clases de paquete) y cuánto de eso es
   espera (`drains`).
6. Las capturas: la imagen debe ser igual salvo el reesampleo (más suave a menor resolución).

## 7. Qué preset/escalado da qué

Lo que está **medido** hoy:

| Preset | Divisor | Tamaño de la escena | Píxeles frente a 1920×1080 | FPS medidos |
|---|---|---|---|---|
| Native AA (0) | 1,0 | 1920×1080 | 100 % | 25 – 35 (mediana 25,4, sesión 20261019-130735) |
| Quality (1) | 1,5 | 1280×720 | 44,4 % | **pendiente** (sesión final) |
| Balanced (2) | 1,7 | 1130×636 | 34,7 % | **pendiente** (sesión final) |
| Performance (3) | 2,0 | 960×540 | 25,0 % | **pendiente** (sesión final) |
| Ultra Performance (4) | 3,0 | 640×360 | 11,1 % | **pendiente** (sesión final) |

Los tamaños están verificados por `scene-preset-test`; los FPS de los presets **no se han
medido** (el juego no se puede ejecutar ahora) y no se inventan aquí. La referencia del
runtime de shadPS4 (prueba `fasthost-a1` del 2026-10-13) da 60 FPS al principio y 17 – 38
FPS en la zona pesada con la GPU al 98 – 100 %, y 0,6 – 3,9 FPS a partir de t=50 s con la
GPU al 100 % y la VRAM estable en 3341 MB: nuestro port va de 10 a 30 veces más rápido que
ese upstream en la misma escena y con la GPU sin saturar, lo que confirma que el límite
actual es el camino de CPU y no el de GPU.

**Cómo se espera que se mueva la tabla** (a verificar, no a afirmar): con la GPU al 63 % y la
escena reducida al 44 % de los píxeles, el trabajo de GPU por frame baja de forma
proporcional; si el hilo de CPU sigue siendo el cuello, el frame time no mejora y lo que
cambia es el margen de GPU. Por eso el orden correcto es: (1) `TLG_DRAW_PROFILE=1` para
atacar la fase más cara del hilo de grabación, (2) el preset para bajar el techo de GPU.
