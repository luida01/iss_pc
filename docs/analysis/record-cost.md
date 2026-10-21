# Lo que cuesta grabar los draws (2026-10-20, revisado el 2026-10-21)

Port nativo de inFAMOUS Second Son (CUSA00223), rama `bootstrap/windows-g0`.
Alcance: `renderer_vulkan/vk_scheduler.{h,cpp}`, `renderer_vulkan/vk_rasterizer.cpp` (el
camino de grabación de draws), `renderer_vulkan/vk_draw_pipe.h`, `shim/tlg_copy.{h,cpp}` y los
bancos de `port/tests/**`.

Hardware: RTX 3050 Laptop 4 GB (driver NVIDIA 595), i5-13420H (16 hilos), MSYS2 CLANG64
(clang 22.1.7), TSC a 2,611 GHz.

## 1. Resumen

**El diagnóstico de la primera tanda (el 20,6 % de `vk-record` era el driver grabando
comandos) era Wrong: la sesión del 2026-10-21 (`evidence/20261021-005214-verify-particles`,
`TLG_DRAW_PROFILE=1`) dice que `vk-record` es el 1,83 % de la ventana y que el coste de la
grabación es, sobre todo, ESPERA.**

| Fase (sesión del 21-10) | De la ventana | Qué es |
|---|---|---|
| las fases del draw (`targets` … `submit`) | 24,74 % | el trabajo: el `bind` solo son 15,16 % |
| `vk-record` | **1,83 %** | el driver grabando comandos (128.385 ciclos por chunk) |
| `task` | 48,52 % de la grabación | 6.722 llamadas, 116.144 ciclos (46 µs) cada una |
| `host-copies` | 12,15 % de la grabación | 286 llamadas, 683.567 ciclos (262 µs) cada una |
| `signals` | 11,64 % de la grabación | 601 llamadas, 311.613 ciclos (119 µs) cada una |
| `packet` | 2,84 % de la grabación | 1.470 llamadas, 31.056 ciclos cada una |

Las cuatro preguntas del encargo, contestadas con números:

1. **La fase `task` (la mitad de la grabación)**: son las tareas *en orden* con los draws
   (las escrituras del invitado, los fences, los readbacks). **No son un trabajo: son
   esperas**, y las esperas que contienen son las de `host-copies` y `signals` (§4.1).
   Hay ~1.200 `RELEASE_MEM` de compute por frame (el comentario del propio código lo dice),
   o sea **2,2 tareas por draw**: cada una espera a lo que tiene debajo.
2. **`host-copies` (262 µs por espera)**: la espera **no es por copiar**: es por *llegar* a
   la copia. Las copias se grababan en el chunk de comandos y el hilo que las necesita (el
   hilo de grabación de draws, dentro de una tarea) tenía que esperar a que el hilo de
   grabación de Vulkan **alcanzara la copia en el chunk** (§4.2).
3. **`signals` (119 µs por espera)**: lo mismo con un matiz: la señal (el fence que el
   invitado ve) se **registraba** en el chunk, así que el hilo que la esperaba depended
   otra vez del hilo de grabación de Vulkan (§4.3).
4. **Las copias (30-40 MB por ventana)**: el trabajo de copiar es pequeño; lo caro era la
   **latencia** de la espera, no la copia.

**El cambio (implementado y medido)**: las copias y las señales ya no dependen del chunk de
comandos. Las copias van a una cola propia (`HostCopyQueue`) que **el hilo que espera
ejecuta**, y las señales se registran en el pool de copias en el momento en que se piden.
En el banco (`port/tests/test_host_copies.cpp`), la espera por una copia que está detrás de
40 comandos grabados pasa de **2.084.500 ns a 44.000 ns (-47x)**.

**El techo**: la fase de grabación son 12 % de la ventana (10,6 ms por frame de 90 ms) y
~80 % de eso son esperas. Las dos esperas que este agente puede quitar (las copias y las
señales) son el **24 % de la grabación** (2,6 ms por frame, el 3 % del frame). Lo que queda
(la espera de la `task` por los readbacks, el `bind`, el decode de PM4) es de otros agentes.

## 2. La fase `vk-record` (el driver grabando comandos)

El banco `port/tests/vkbench/` mide el coste de grabación de cada comando que el port
emite por draw, en la GPU discreta, en el estado en que el port los graba (un pass de
render abierto, la tubería ligada):

```
cmake -S port/tests/vkbench -B port/out/gpu-rec/vkbench -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C port/out/gpu-rec/vkbench
port/out/gpu-rec/vkbench/vk-record-cost.exe
```

| | ns |
|---|---|
| el estado dinámico (20 comandos, 5-20 ns cada uno) | 2 - 20 |
| `setVertexInputEXT` / `bindVertexBuffers` / `bindIndexBuffer` | 7 - 9 |
| `pushConstants` (128 B) | 8 |
| `drawIndexed` | 15 |
| `pushDescriptorSetKHR` (1 / 4 / 8 / 16 / 32 escrituras) | 11 / 38 / 62 / 121 / 235 |
| `bindPipeline` | **92** |
| `beginRendering` + `endRendering` | **355** |
| `pipelineBarrier2` (0 / 4 / 16 / 32 transiciones de imagen) | 121 / 321 / 984 / **1.838** |
| **la mezcla completa de un draw** | **1.001** |

**Lo que dice**: la fase `vk-record` (1,83 % de la ventana) es barata *por comando*; lo
caro son dos comandos (la tubería y el pass) y, sobre todo, **las transiciones de imagen de
las barreras** (el código que las emite está en `vk_runtime.cpp` y las transiciones las
pone la caché de texturas: fuera del alcance de este agente).

## 3. El recorrido del chunk (el propio)

El perfil mide la fase `vk-record` como el tiempo de `chunk->Execute()`: el recorrido del
chunk **más** las llamadas al driver. El banco `port/tests/test_chunk_cost.cpp` mide el
recorrido (dos hilos, como el port: el hilo de draws escribe el chunk y el de Vulkan lo
lee):

| Variante | ns/chunk | ns/draw | ns/comando |
|---|---|---|---|
| Lista enlazada (antes) | 2.885 - 3.049 | 192 - 203 | 15 - 16 |
| **Empaquetada (ahora)** | **1.075 - 1.187** | **72 - 79** | **6** |

El chunk era una lista enlazada y el hilo de grabación de Vulkan la recorría con una carga
dependiente por comando (cada una, un fallo de caché: el chunk lo escribe el hilo de draws
y lo lee otro núcleo). Ahora los comandos van uno detrás de otro con sus desplazamientos
en un array: el lector los recorre sin perseguir punteros y prefetcha el siguiente.

## 4. Las esperas (el diagnóstico nuevo)

### 4.1 Qué es una `task` y qué espera

Las tareas son los paquetes que el hilo de comandos de la GPU entrega al pipe de draws
(`Rasterizer::RunInOrder`) para que se ejecuten **en orden con los draws**:
`RunWriteData` (las escrituras del invitado), `RunDmaData` (las copias DMA),
`RunEventWriteEop/Eos` (los fences), `RunComputeReleaseMem` (el fence de la cola de
compute) y `SignalFlip`.

Cada una hace, en este orden:

1. `WaitHostCopies()` → la fase `host-copies` (12,15 %).
2. `WaitDeferredSignals()` → la fase `signals` (11,64 %).
3. `ProcessDownloadImages()` y `EagerCopyAtFence()` → **lo que no espera nada**: los
   readbacks (el texture cache) y la copiaurne de la caché de buffers. Aquí está la espera
   por la GPU (la parte de la `task` que no es `host-copies` ni `signals`: 24,7 % de la
   grabación). Es de la caché de texturas y de la de buffers (fuera de alcance).

**Por qué 2,2 tareas por draw**: el juego emite ~1.200 `RELEASE_MEM` de compute por frame
(el comentario de `TextureCache::ProcessDownloadImages` lo dice: *"this runs at every fence
(~1200 compute RELEASE_MEMs a frame)"*), y cada una es una tarea. Con 3.127 draws por frame
son 2,2 tareas por draw. **No es un coste del draw: es el fence de la cola de compute**, y
cada fence espera a lo que tiene debajo.

### 4.2 `host-copies`: 262 µs por espera

Las copias de la memoria del invitado (las subidas de los búferes y de las texturas) se
grababan **en el chunk de comandos** (`Scheduler::RecordHostCopy` → `Record(...)`), y el
hilo de grabación de Vulkan las ejecutaba al recorrer el chunk. El hilo que las necesita
(el de las tareas) esperaba a que **el hilo de grabación de Vulkan alcanzara la copia en el
chunk**: la copia está donde la dejó el draw que la pidió, y el chunk tiene 32 KiB de
comandos por delante.

La espera no era por copiar (las copias son de unos pocos KiB): era por **llegar**.

### 4.3 `signals`: 119 µs por espera

El fence que el invitado ve (`RunEventWriteEop` → `SignalAfterHostCopies`) se
**registraba** en el chunk: el hilo de grabación de Vulkan lo registraba al llegar a él, y
el hilo que esperaba la señal (la tarea siguiente) dependía otra vez del hilo de
grabación de Vulkan.

### 4.4 Lo implementado

| | Antes | Ahora |
|---|---|---|
| `RecordHostCopy` | la copia es un comando del chunk (la ejecuta el hilo de grabación de Vulkan) | la copia va a una cola propia (`HostCopyQueue`, `vk_scheduler.h`) |
| `WaitHostCopies` | espera a que el hilo de Vulkan alcance la copia | **el hilo que espera ejecuta las copias** (`RunHostCopies`) |
| `SignalAfterHostCopies` | la señal se registra en el chunk (al llegar a él) | la señal se registra en el **pool de copias** en el momento en que se pide (el pool la corre cuando las copias anteriores están hechas) |
| `WaitDeferredSignals` | espera a que el hilo de Vulkan registre la señal | espera a que el pool la corra (y la ayuda) |

El orden se conserva:
- La copia se hace **antes** de que el invitado pueda reescribir su memoria (la tarea de la
  escritura del invitado espera a las copias ✓) y **antes** de la submissión que la lee
  (`SubmitExecution` → `WaitHostCopies` ✓).
- La señal (el fence) se hace **después** de las copias que la anteceden: ahora que las
  copias están en el pool, el mecanismo de épocas de `TlgCopy::AfterCopies` las cubre de
  verdad (antes las copias pequeñas no estaban en el pool y la señal podía adelantarse a
  ellas).
- El semáforo de la señal y el de las copias son **contadores**, no números de secuencia
  (las copias ya no terminan en orden).

El banco (`port/tests/test_host_copies.cpp`) comprueba las tres cosas:

1. Las copias se ejecutan **en el orden** en que se encolaron, y los contadores cuadran.
2. Varios hilos (el de los draws y el del ayudante de texturas) encolan copias: todas se
   ejecutan y los contadores cuadran.
3. **La espera**: con 40 comandos grabados antes de la copia, la espera por la copia pasa
   de **2.084.500 ns** (la copia en el chunk de otro hilo) a **44.000 ns** (la copia en la
   cola del que espera).

```
the wait for a copy (40 commands before it in the chunk):
  in the chunk of another thread (before):   2084500 ns
  in the queue of the waiter (now):          44000 ns
```

## 5. Los draws indirectos y los argumentos que escribe la GPU

El agente de partículas dejó escrito (sin aplicar) que los argumentos de un draw
indirecto no pueden servirse de la **copia de la memoria del invitado** (el "stream path"
de `BufferCache::ObtainBuffer`, que devuelve una copia fresca de un búfer pequeño y de solo
lectura del que la GPU no ha escrito): el compute que cuenta las partículas escribe ahí el
número de instancias y el draw leería la copia del invitado, sin ese número
(`docs/analysis/particles-missing.md` §2.4).

**Aplicado aquí (lo que cabe en este alcance)**: el sitio de la llamada
(`Rasterizer::DrawIndirectRecord`, `vk_rasterizer.cpp`) pide ahora el búfer como
"escrito por la GPU" (`ObtainBuffer(..., true)`), lo que **omite el stream path**: el draw
lee el búfer donde el compute escribió, no la copia del invitado.

**Lo que no cabe aquí (y queda para quien tenga la caché de búferes)**: el arreglo completo
pide un `BufferCache::ObtainIndirectBuffer` que además **no suba la memoria del invitado
encima de una región que la GPU escribió** (el `ForEachUploadRange` de solo lectura ya
sube solo las páginas modificadas por la CPU, pero con `is_written = true` la región se
marca como escrita por la GPU, y eso tiene efectos colaterales: la copia "eager" marca la
página para volver a copiarla en el fence). El método nuevo es de la caché de búferes, que
no es de este agente.

## 6. Lo que NO se toca, y por qué

| Palanca | Cuánto valdría | Por qué no |
|---|---|---|
| La espera de la `task` por los readbacks (el 24,7 % de la grabación que no son `host-copies` ni `signals`) | ~2,5 ms por frame | Es la caché de texturas y la de búferes (`ProcessDownloadImages`, `EagerCopyAtFence`): otros agentes. |
| El giro del grabador | 67 % del hilo de grabación está girando | El giro adaptativo ya se probó y fue una **regresión** (26.378 → 826 draws/s). No se toca. |
| El `bind` (15,16 % de la ventana) | ~13 ms por frame | La caché de texturas (el VRAM). |
| Menos `bindPipeline` / `setVertexInputEXT` redundantes | 92 ns y 9 ns por bind | Los pases del port (`host_passes/pp_pass.cpp`, `vk_scene_resolution.cpp`) también ligan tuberías de gráficos en el mismo command buffer: para saltarlas con seguridad hay que invalidar el estado en cada comando que pueda ligar una, y esos ficheros no son de este alcance. Con el estado mal invalidado, el port ligaría la tubería equivocada. |
| El coste del driver por comando | 1,83 % de la ventana | Los dos comandos caros (la barrera con sus transiciones de imagen y el pass) están en `vk_runtime.cpp` y en la caché de texturas. |
| El número de tareas por draw (2,2) | ~1.200 fences de compute por frame | Son del invitado: el port no puede dejar de encolarlos. Lo que sí puede es que no esperen (§4.4). |

## 7. Cómo se cierra la pregunta en la próxima sesión

Con `TLG_DRAW_PROFILE=1` (la línea de la sesión del 21-10):

```
Draw profile: ... ms window, ... draws
  ...
  vk-record      1.83 % of the window, ...
```

y el reparto de la grabación (`packet`, `task`, `host-copies`, `signals`). **Lo que hay que
mirar ahora**:

1. **`host-copies` y `signals` deberían caer a casi cero** (el hilo que espera ejecuta las
   copias; la señal la corre el pool). Si no caen, la copia está en otro sitio (el pool de
   copias, que sí tiene su propia espera: `copy_threads_wait_ns`).
2. **La `task` que queda** es la espera por los readbacks (la GPU): ahí la baptism es
   `texture_cache` (los readbacks) y `buffer_cache` (la copia eager). Es el siguiente
   trabajo, y es de otros agentes.
3. Para atribuir el resto de la fase `vk-record` por tipo de comando, el perfil cuenta
   *cuántos* comandos de cada tipo se graban pero no *cuánto* cuestan. Con el tipo de
   comando en el cierre (el `Record()` de `vk_scheduler.h` podría llevarlo) y un `__rdtsc`
   por comando en `RecordChunk::Execute()`, la fase se reparte comando a comando sin
   conjeturas. Es un cambio pequeño en `vk_scheduler.h` + `vk_draw_profile.h` (el fichero
   del perfil, de otro agente).

## 8. Reproducir las medidas

```bash
# El banco del driver (necesita una GPU discreta).
cmake -S port/tests/vkbench -B port/out/gpu-rec/vkbench -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C port/out/gpu-rec/vkbench
port/out/gpu-rec/vkbench/vk-record-cost.exe

# El recorrido del chunk y la espera por las copias.
cmake -S port/gpu -B port/out/gpu-rec/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTLG_LTO=OFF
ninja -C port/out/gpu-rec/gpu chunk-cost-test host-copies-test draw-pipe-test
port/out/gpu-rec/gpu/chunk-cost-test.exe
port/out/gpu-rec/gpu/host-copies-test.exe
port/out/gpu-rec/gpu/draw-pipe-test.exe
```

Dos avisos que costaron tiempo y que el banco deja escritos (`vk_probe.c` es la sonda
mínima que los encontró):

* **Un dispositivo sin colas hace que el driver NVIDIA se caiga al reservar memoria.**
  El `vkCreateDevice` con `queueCreateInfoCount = 0` es legal según la especificación,
  pero el driver 595 peta en el `vkAllocateMemory` siguiente. El banco crea la cola.
* **La primera GPU de la lista no es la que se usa.** La máquina tiene la Intel
  integrada y la NVIDIA: el banco elige la **discreta**, que es la que corre el juego.
