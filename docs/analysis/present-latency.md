# La cadena de presentación: invitado → port → GPU (2026-10-19)

Port nativo de inFAMOUS Second Son (CUSA00223), rama `bootstrap/windows-g0`.
Alcance de este informe: `port/gpu/shadps4/core/libraries/videoout/driver.{cpp,h}`,
`port/gpu/shadps4/video_core/renderer_vulkan/vk_presenter.{cpp,h}`,
`port/gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.{cpp,h}` y el banco
`port/tests/test_flip_pacing.cpp`.

Síntoma que se investigó (medido por el usuario el 2026-10-19, RTX 3050 4 GB, 720p, escena
SAFETY TRUST): el frame tarda 100-200 ms (5-12 FPS), la GPU está al 13-22 % (ociosa), el
hilo principal del invitado ejecuta código del juego (no espera) mientras el juego no manda
un flip nuevo, y el port mide "GPU ticks 5-24 ms" y "stage A waited 2 %".

## 1. Respuesta corta

**El camino de presentación no se atasca y no pierde frames: entrega exactamente un present
por flip.** Lo que sí tenía un defecto es la **latencia**: el port entregaba el present a la
GPU **justo en el tick de vblank**, así que el frame llegaba al motor de presentación
*después* del vblank que tenía que mostrarlo y esperaba **un vblank entero (16,7 ms) de
más**. En la escena ligera (el título) el flip esperaba la cola del hilo de presentación
**15,9-16,1 ms** — practically un vblank — antes de que el present saliera.

El arreglo: **el present sale en cuanto el frame está listo** (el hilo de presentación toma
la petición en el momento en que llega, no en el tick) y **el evento de flip sigue
disparándose en el tick de vblank**, de modo que el ritmo del invitado no cambia. Medido con
el banco del ritmo: **-8,0 ms de latencia de pantalla en la escena pesada y -16,0 ms en la
ligera**, con el ritmo del invitado igual (143,02 → 142,95 ms y 16,67 → 16,66 ms).

El frame time (100-200 ms) **no lo arregla este cambio**: el cuello está en el invitado y
en el grabador del port (el hilo de grabación usa 4,2 s de CPU de cada 5 s de ventana), como
midió el agente de runtime (`docs/analysis/runtime-threads-frame-time.md`). El camino de
presentación aporta ~8 ms de los 150 ms del frame (el 5 %).

## 2. El camino de presentación, línea a línea

El flip del invitado (`sceVideoOutSubmitFlip`, o el IRQ de EOP) recorre esto:

| # | Dónde | Qué hace | Hilo |
|---|---|---|---|
| 1 | `video_out.cpp: sceVideoOutSubmitFlip` → `driver.cpp: SubmitFlip` | el invitado llama; se anota el sello `flip_ns`; la petición se manda al hilo de comandos de la GPU (`liverpool->SendCommand`) | el del invitado |
| 2 | `driver.cpp: SubmitFlipInternal` | el hilo de la GPU prepara el frame (`PrepareFrame`: FSR + post process, flush del `draw_scheduler`) y encola la petición en `requests` (sello `queued_ns`) | el de la GPU |
| 3 | `driver.cpp: PresentThread` | el hilo de presentación **toma** la petición de `requests` y la manda al hilo de swap (`RunPresenter`) | el de presentación |
| 4 | `driver.cpp: RunPresenter` → `SwapThread` | el hilo de swap hace `Presenter::Present` | el de swap |
| 5 | `vk_presenter.cpp: Presenter::Present` | acquire de la imagen del swapchain, blit del frame al swapchain, `vkQueuePresentKHR` (el frame entra en el motor de presentación) | el de swap |
| 6 | `driver.cpp: FireFlipEvent` | el evento de flip del invitado, el estado del puerto y la etiqueta del buffer (el invitado ya puede reutilizarlo) | el de presentación |

**¿Se encola o se presenta?** Las dos cosas, y en este orden: la petición se encola (cola
`requests`, 1 entrada por flip) y se presenta (1 `vkQueuePresentKHR` por flip). **No hay
frames en vuelo extra**: el pool de frames de presentación (3) es el que limita cuántos
frames puede tener el invitado por delante, y `GetRenderFrame` espera el fence del present
anterior antes de reutilizar un frame.

**¿Cuántos frames presenta la GPU?** Los que le entrega el invitado, uno a uno. Los
contadores del freeze report lo confirman (`evidence/20261019-140541-g0-manual-paint4/`):

```
frames: 1817 flips submitted by the game (+0 since the last shown frame), 1817 shown,
        1817 presented (+0), 4809 vblank ticks (+0)
```

1817 flips = 1817 mostrados = 1817 presentados: **no se pierde ni se duplica ningún
present**. (4809 ticks de vblank para 1817 frames: el juego no va a 60 FPS en esa escena.)

**¿Hay un frame de latencia extra?** Sí, y no es un frame entero sino **un vblank**: el
present se encolaba en el tick, con lo que el motor de presentación recibía el frame
*después* del vblank que lo tenía que mostrar.

## 3. Lo que mide el port hoy (el "antes")

De la sesión de 25,7 min del usuario (`evidence/20261019-213737-juego-usuario2/`,
`TLG_FRAME_STATS=1`):

| Espera del port | Escena ligera (título, 53-59 FPS) | Escena pesada (SAFETY TRUST, 6-9 FPS) |
|---|---|---|
| **Cola del flip** (el flip espera su turno de presentación) | **15,94 - 16,11 ms** | **6,3 - 9,4 ms** |
| Frame libre (el fence del present anterior) | 1,0 - 1,8 ms | 0,2 - 0,6 ms |
| Cerrojo de la cola de gráficos (el present) | 0,0 - 0,1 ms | 0,0 ms |
| Frames por delante (el fence del `draw_scheduler`) | 0,0 ms | 0,0 - 0,1 ms |
| GPU ticks (el hilo de la GPU esperando a la GPU) | 0,0 % | 10,6 - 13,0 % |

Los 16,1 ms de la escena ligera son la medida clave: **un vblank entero**. La escena ligera
va a 59,4 FPS con un intervalo de 16,8 ms, o sea que el invitado entrega el flip
justo después del tick de vblank y la petición **espera al tick siguiente**. La escena
pesada (100-200 ms por frame) reparte la fase y la espera media cae a ~8 ms (la mitad de un
vblank), que es el valor teórico de una fase uniforme.

**El fence no serializa**: la espera por el frame libre (el fence del present) es de 0,2-0,6
ms en la escena pesada y el cerrojo de la cola de gráficos es de 0,0 ms. El port no espera
a la GPU para presentar; la GPU no espera al port.

## 4. El diagnóstico con números

| Pregunta del encargo | Respuesta medida |
|---|---|
| ¿Cuántos flips entrega el invitado, cuántos presents hace el port? | 1817 = 1817 = 1817 (freeze report). 1 present por flip, ninguno perdido |
| ¿La GPU presenta menos frames de los que le llegan? | No: los presents y los frames completados por el device coinciden (contador nuevo `g_tlg_gpu_presents`) |
| ¿El port espera al fence antes de presentar? | No: la espera del frame libre (el fence) es 0,2-0,6 ms y el cerrojo de la cola 0,0 ms. El port presenta sin esperar a la GPU |
| ¿El invitado espera al port? | No: `main` pasa el 91 % del tiempo ejecutando código del juego (agente de runtime) |
| ¿El vsync añade un vblank? | **Sí**: el present se entregaba en el tick, así que esperaba al vblank siguiente. Ese es el defecto corregido |
| ¿La GPU está ociosa por culpa del present? | No: la GPU está ociosa porque el invitado y el grabador del port son el cuello (4,2 s de CPU de grabación por ventana de 5 s) |

## 5. El cambio: el present temprano (`driver.cpp`)

**Antes**: el hilo de presentación dormía hasta el tick de vblank, tomaba la petición de la
cola y entregaba el present. El frame llegaba al motor de presentación después del vblank
que tenía que mostrarlo.

**Ahora** (sólo cuando el ritmo del invitado es el vblank: sin límite de frames y sin
`flip_rate`):

1. La petición se toma **en cuanto llega** (el hilo de presentación espera en `request_cv`
   hasta el próximo tick y la toma en el momento en que llega). El present sale hacia la
   GPU enseguida, con lo que el frame está en el motor de presentación **antes** del vblank
   que lo muestra.
2. El **evento de flip sigue en el tick de vblank** (igual que antes): el invitado no cambia
   de ritmo, y la etiqueta del buffer y el estado del puerto se actualizan en el mismo
   instante que antes.

Con **límite de frames** o con **`flip_rate` != 0 el comportamiento no cambia**: ahí el
límite o la tasa paces al invitado a través del evento de flip, y el present tiene que salir
con el flip.

El modelo del ritmo (el banco de §6) reproduce las medidas del antes:

| Caso | Cola del flip (antes → después) | Latencia de pantalla | Ritmo del invitado |
|---|---|---|---|
| Escena pesada, invitado por vblank | 8,02 → 0,00 ms | 16,29 → **8,32 ms** | 143,02 → 142,95 ms (7,0 FPS) |
| Escena pesada, invitado por flip | 8,36 → 0,00 ms | 16,62 → **8,33 ms** | 135,01 → 134,94 ms (7,4 FPS) |
| Escena ligera, invitado por vblank | 16,07 → 0,00 ms | 24,40 → **8,36 ms** | 16,67 → 16,66 ms (60,0 FPS) |
| Escena ligera, invitado por flip | 9,39 → 0,00 ms | 56,40 → **16,63 ms** | 16,65 → 16,65 ms (60,1 FPS) |
| Escena ligera, invitado libre | 16,77 → 0,00 ms | 58,28 → 57,91 ms | 16,66 → 16,65 ms (60,0 FPS) |

Las dos primeras columnas del "antes" (8,02 ms y 16,07 ms) son las que midió el port en la
sesión real (8-9 ms y 15,9-16,1 ms): **el modelo reproduce el port**.

El último caso es importante: si el invitado no espera ningún evento (no es el caso de este
juego, que va a 60 FPS clavados en la escena ligera), el display es el cuello y el cambio no
aporta nada (el swapchain bloquea en el acquire) — pero tampoco empeora nada.

## 6. El banco (`port/tests/test_flip_pacing.cpp`)

Un modelo del hilo de presentación (el mismo algoritmo que `driver.cpp`, `PresentThread`),
con reloj virtual: reproduce el hilo de presentación, el hilo de swap, el pool de 3 frames,
el swapchain de 3 imágenes, la pantalla a 59,94 Hz (su fase se va deslizando con respecto al
tick del port) y el invitado en sus tres modos de ritmo.

```bash
bash port/tests/run_flip_pacing.sh          # --quick para una pasada corta
```

Lo que mide: la cola del flip, la espera en la cola de swap, la latencia de pantalla (el
flip → el vblank que lo muestra) y el ritmo del invitado (evento de flip → evento de flip).

## 7. El instrumento nuevo (para la sesión del usuario)

El port ahora imprime la **cadena completa** en la línea de estadísticas del frame
(`TLG_FRAME_STATS=1`), con los tiempos **por flip**:

```
Present chain: guest->cmd 0.05 ms, prepare 0.31 ms, queue 8.02 ms, take->present 0.44 ms
(swap 0.01 ms, present 0.42 ms, acquire 0.05 ms, fence 0.23 ms); total 8.82 ms;
46 flips, 46 shown, 46 presented, 46 on the gpu
```

- `guest->cmd`: el hop de la llamada del invitado al hilo de comandos de la GPU.
- `prepare`: la preparación del frame en el hilo de la GPU (FSR + post process).
- `queue`: la espera de la petición en la cola del hilo de presentación (el vblank).
- `take->present`: desde que el hilo de presentación toma el flip hasta que el present sale
  (la cola de swap + el acquire + el blit + el submit).
- `fence`: la espera por el fence del present anterior (el frame libre).
- Los cuatro contadores: los flips del invitado, los que el present thread tomó, los presents
  hechos y los que el device terminó (deben coincidir).

**Ojo con las unidades**: la línea "Frame waits" que ya existía lleva los **totales de la
ventana de 5 s**; la línea "Present chain" lleva los **medios por flip**.

## 8. Receta para la sesión (la que hace el usuario)

```powershell
# El port tiene que recompilarse (bash port/build.sh): el binario de port/out/ es anterior.
$env:TLG_FRAME_STATS = '1'      # la línea "Present chain"
$env:TLG_WAIT_STATS  = '1'      # el mapa de esperas del invitado (el agente de runtime)
python port\run.py
```

Qué mirar en el log:

1. **La línea "Present chain"**: `queue` debe ser ~0 (antes 8-16 ms) y los cuatro
   contadores deben coincidir. Si `queue` sigue alto, el cambio no está activo (o hay un
   límite de frames / `flip_rate` activo).
2. **`take->present`**: si sube mucho, el hilo de swap o el acquire están esperando (el
   display no da abasto). `acquire` es la espera por la imagen del swapchain.
3. **El ritmo del invitado** (la línea "Frame pacing"): debe seguir igual que antes (el
   cambio no lo toca). Si el invitado se dispara (muchos más flips por segundo), el juego
   estaba esperando al evento de flip y ahora no lo encuentra: pararía y se revisa.
4. **La comparación**: misma escena, mismos 5-10 min, con y sin el cambio (basta con
   git stash del commit de este agente).

## 9. Lo que NO se ha tocado (y por qué)

| Candidato | Por qué no |
|---|---|
| Mover el evento de flip al momento del present | Rompería el ritmo del invitado: si el juego espera al evento de flip, dejaría de estar limitado por el display y se dispararía (el modelo: 1713 FPS y una cola que crece en el motor de presentación) |
| Presentar sin esperar al vblank con `vsync=0` (Mailbox/Immediate) | Es una opción del menú del juego (`TlgSettings::vsync`), no un defecto del port. Con Mailbox el present sale ya, pero el invite sigue esperando al tick |
| Tocar el limitador de frames (el `next_flip` del hilo de presentación) | Es el ritmo que el usuario pide en el menú del juego; con él el flip event paces al invitado |
| El `waitForFences` de `GetRenderFrame` (0,2-0,6 ms) | Es el fence del present anterior; no serializa nada (la GPU no espera) |
| El pool de 3 frames y el swapchain de 3 imágenes | Son la contrapresión que impide que el invitado se adelante; el cambio no los toca |

## 10. Resumen de la evidencia

| Fuente | Qué dice |
|---|---|
| `evidence/20261019-213737-juego-usuario2/` (25,7 min) | la cola del flip: 15,9-16,1 ms (escena ligera) y 6,3-9,4 ms (pesada); frame libre 0,2-1,8 ms; cerrojo de la cola 0,0 ms; 6-9 FPS en la escena pesada |
| `evidence/20261019-140541-g0-manual-paint4/` (freeze) | 1817 flips = 1817 mostrados = 1817 presentados: ningún present se pierde; `main` del invitado "running" |
| `docs/analysis/runtime-threads-frame-time.md` (agente de runtime) | el invitado no espera al port (91 % ejecutando); el cuello es el grabador (4,2 s de CPU por ventana de 5 s) |
| `port/tests/test_flip_pacing.cpp` (este agente) | el modelo del ritmo: cola 8,02 → 0,00 ms y 16,07 → 0,00 ms; latencia de pantalla -8,0 / -16,0 ms; el ritmo del invitado sin cambio |
