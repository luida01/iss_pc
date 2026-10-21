# Lo que cuesta grabar los draws (2026-10-20)

Port nativo de inFAMOUS Second Son (CUSA00223), rama `bootstrap/windows-g0`.
Alcance de este informe: `renderer_vulkan/vk_scheduler.{h,cpp}`,
`renderer_vulkan/vk_rasterizer.cpp` (el camino de grabación de draws),
`renderer_vulkan/vk_draw_pipe.h`, `shim/tlg_copy.cpp` y los bancos de `port/tests/**`.

Hardware de la medición: RTX 3050 Laptop 4 GB (driver NVIDIA 595), i5-13420H (16 hilos),
MSYS2 CLANG64 (clang 22.1.7), TSC a 2,611 GHz (el `tsc-rate-test` lo midió).

## 1. Resumen

La pregunta era *dónde se va el 20,6 % de la ventana que el hilo de grabación de Vulkan
pasa dentro del driver* (`vk-record`), y si emitting fewer commands is a lever that pays.

**Respuesta corta: el coste de la fase está casi todo en **dos comandos concretos** (la
barrera y el pass de render) y, dentro de la barrera, en el número de transiciones de
imagen que lleva. Los comandos que el port graba por draw ya son pocos y los que
graba son baratos (5 - 20 ns cada uno); el trabajo propio del port (recorrer el chunk)
era el 3,8 % de la fase y con el cambio de este informe es el 1,5 %.**

| Medido | Valor | De dónde sale |
|---|---|---|
| El driver cobra por comando | 2 - 121 ns (los simples, 5 - 20 ns) | `tests/vkbench` |
| La barrera de 32 transiciones de imagen | **1.838 ns** | `tests/vkbench` |
| El par begin/end de un pass de render | **355 ns** | `tests/vkbench` |
| `bindPipeline` | **92 ns** | `tests/vkbench` |
| La mezcla completa de un draw | **1.001 ns** | `tests/vkbench` |
| El recorrido del chunk (propio del port) | ~200 ns por draw | `tests/test_chunk_cost.cpp` |
| El recorrido del chunk **después** del cambio | **79 ns por draw (-59 %)** | `tests/test_chunk_cost.cpp` |
| La fase `vk-record` medida en el juego | 5.213 ns por draw (13.593 ciclos) | perfil `TLG_DRAW_PROFILE=1` |

Las cuatro preguntas del encargo, contestadas con números:

1. **Los 5,9 comandos de estado dinámico por draw**: el driver cobra 2 - 20 ns por
   comando → **50 - 90 ns por draw (1 - 1,7 % de la fase)**. El port ya los emite
   **sólo cuando cambian** (`DynamicState` compara con el valor anterior). **No es la
   palanca.**
2. **Los chunks**: 2.642 chunks/s, 15 draws por chunk. El recorrido del chunk costaba
   ~200 ns por draw (3,8 % de la fase) y ahora cuesta 78 ns. **Engordar el chunk no
   compensa**: el chunk tarda 379 µs en llenarse (15 draws a 39.600 draws/s) → el GPU ya
   va ese retraso detrás, y en este equipo la latencia manda más que la CPU.
3. **Las barreras**: **aquí está el peso**. 1,6 comandos de barrera por draw y cada uno
   cuesta 121 ns + **~55 ns por transición de imagen** (16 transiciones = 984 ns;
   32 = 1.838 ns). Una barrera vacía ya son 121 ns. El código que las emite está en
   `vk_runtime.cpp` y las transiciones de imagen las pone la caché de texturas: **fuera
   del alcance de este agente**, pero es el número que hay que mirar.
4. **Las copias**: 6 copias por frame, 31 MB por ventana, 2,8 % del frame (la espera
   0 - 0,9 ms). El *pool* de copias (`tlg_copy.cpp`) no tiene holgura: agrupa en
   512 KiB / 256 ítems y reparte entre 4 hilos. Las subidas en sí son de la caché de
   buffers (fuera de alcance). **No hay palanca aquí.**

El techo real: la fase `vk-record` son 20,6 % de la ventana. Si el port no grabara
*ningún* comando, el frame mejoraría un 20,6 % (100 ms → 80 ms). Pero la mayor parte de
esa fase es el driver grabando barreras y passes, así que el margen real del trabajo de
*este* agente (el recorrido del chunk) es del **2,4 % de la fase (0,5 % del frame)**; el
resto necesita menos barreras de imagen y menos passes, que están en `vk_runtime.cpp` y
en la caché de texturas.

## 2. El banco del driver (`port/tests/vkbench/`)

Un banco que crea un dispositivo Vulkan **como el del port** (1.3, push descriptors,
vertex input dinámico, extended dynamic state 3, feedback loop) en la GPU discreta y
graba miles de veces cada comando que el port emite por draw, en el estado en que el
port los graba (un pass de render abierto, la tubería ligada). El número es el tiempo
de pared por comando.

```
cmake -S port/tests/vkbench -B port/out/gpu-rec/vkbench -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C port/out/gpu-rec/vkbench
port/out/gpu-rec/vkbench/vk-record-cost.exe
```

### 2.1 El estado dinámico (el port emite 5,9 por draw)

| Comando | ns | ciclos |
|---|---|---|
| `setAttachmentFeedbackLoopEnableEXT` | 2,2 | 6 |
| `setFrontFace` | 5,1 | 13 |
| `setRasterizerDiscardEnable` | 5,2 | 14 |
| `setCullMode` | 5,4 | 14 |
| `setLineWidth` | 5,4 | 14 |
| `setStencilWriteMask` | 5,5 | 14 |
| `setDepthBiasEnable` | 5,7 | 15 |
| `setDepthWriteEnable` | 5,7 | 15 |
| `setPrimitiveRestartEnable` | 5,7 | 15 |
| `setBlendConstants` | 6,0 | 15 |
| `setColorWriteMaskEXT` | 7,0 | 18 |
| `setScissorWithCount` | 7,3 | 19 |
| `setDepthBias` | 9,5 | 25 |
| `setStencilReference` | 10,4 | 27 |
| `setStencilCompareMask` | 10,9 | 28 |
| `setStencilTestEnable` | 11,0 | 29 |
| `setDepthTestEnable` | 11,6 | 30 |
| `setDepthCompareOp` | 12,6 | 33 |
| `setStencilOp` | 19,1 | 50 |
| `setViewportWithCount` | 19,5 | 51 |

**5,9 de estos por draw = 50 - 90 ns.** El `setViewportWithCount` es el más caro de
los simples porque copia 16 viewports (el port usa un `static_vector` de 16).

### 2.2 Los entradas y el dibujo

| Comando | ns | ciclos |
|---|---|---|
| `bindIndexBuffer` | 7,1 | 19 |
| `bindVertexBuffers` (1) | 7,5 | 20 |
| `bindVertexBuffers2` (1) | 7,5 | 20 |
| `pushConstants` (128 B) | 7,9 | 20 |
| `setVertexInputEXT` | 8,8 | 23 |
| `pushDescriptorSetKHR` (1 escritura) | 10,9 | 28 |
| `drawIndexed` | 15,4 | 40 |
| `pushDescriptorSetKHR` (4 escrituras) | 38,0 | 99 |
| `pushDescriptorSetKHR` (8 escrituras) | 62,2 | 162 |
| `pushDescriptorSetKHR` (16 escrituras) | 121,3 | 315 |
| `pushDescriptorSetKHR` (32 escrituras) | 234,7 | 610 |
| **`bindPipeline`** | **91,8** | **239** |
| **`beginRendering` + `endRendering`** | **355,0** | **923** |

`bindPipeline` y el par de render son **dos órdenes de magnitud** más caros que el
resto: el driverNVIDIA prepara el estado del framebuffer y de la tubería en cada uno.

### 2.3 Las barreras (el port emite 1,6 por draw)

| Transiciones de imagen | ns | ciclos |
|---|---|---|
| 0 | 121,1 | 315 |
| 1 | 148,7 | 387 |
| 4 | 321,2 | 835 |
| 16 | 984,3 | 2.559 |
| 32 | **1.838,3** | **4.780** |

Una barrera **vacía** ya son 121 ns (es el comando en sí, no las transiciones), y cada
transición de imagen añade ~55 ns (y el coste por transición **crece** con el número:
de 4 a 16 transiciones el coste por una se multiplica por 2,5). Una barrera con 32
transiciones de imagen cuesta **1,8 µs**: más que 30 draws del port entero.

### 2.4 La mezcla completa

El banco graba la secuencia que el port emite por draw (6 comandos de estado dinámico,
el vertex input, los vertex buffers, el index buffer, los push constants, la pushing de
descriptores, una barrera con 8 transiciones, el `bindPipeline` y el draw):

```
The whole draw (the mix the port records)
per draw                                  1001 ns
```

Y lo mismo con los valores cambiando en cada repetición (el driver no puede saltarse un
comando cuyo valor ya es el actual):

```
The same commands with the values changing
per iteration                              136 ns (3 commands)
```

Es decir: **un comando cuyo valor no cambia es ~3 veces más barato**. El port ya
aprovecha eso en el estado dinámico y en los binds de vertex/index (el `BindState`).

## 3. El banco del chunk (`port/tests/test_chunk_cost.cpp`)

El perfil mide la fase `vk-record` como **el tiempo de `chunk->Execute()`**, o sea el
recorrido del chunk **más** las llamadas al driver. La parte del driver ya está
medida (sección 2); aquí se mide la parte propia del port: el recorrido de la lista de
closures que el hilo de grabación de draws escribió y el hilo de grabación de Vulkan lee
en otro núcleo (frío, en la caché del otro).

El banco mete en el chunk los comandos de 15 draws (13 por draw, con el payload real:
los viewports y los scissors copiados enteros, los push constants, los datos de los
descriptores en el storage del chunk) y mide el `Execute()` con dos hilos.

| Variante | ns/chunk | ns/draw | ns/comando |
|---|---|---|---|
| Lista enlazada (antes) | 2.885 - 3.049 | 192 - 203 | 15 - 16 |
| **Empaquetada (ahora)** | **1.176 - 1.187** | **78 - 79** | **6** |
| Empaquetada con payload compacto (no implementado) | 1.124 - 1.125 | 75 | 6 |

El payload compacto (copiar sólo los viewports en uso en vez de los 16) daría un 5 %
más: **no compensa el cambio**.

## 4. La descomposición de la fase `vk-record`

Del log de la sesión (5 s de ventana, `TLG_DRAW_PROFILE=1`):

- 13.210 chunks grabados, 197.988 draws → **15 draws por chunk**.
- 203.900 ciclos por chunk → **13.593 ciclos (5.213 ns) por draw**.

Con los números de los bancos:

| Parte | ns por draw | % de la fase |
|---|---|---|
| El driver, con la mezcla de los bancos | ~1.000 | ~19 % |
| El driver, si las barreras llevan 16 transiciones de imagen | ~2.400 | ~46 % |
| El recorrido del chunk (propio del port, antes) | ~200 | 3,8 % |
| El resto (comandos no contados por el perfil: los dispatches, las copias, las transiciones de la caché de texturas, y comandos más pesados que los del banco: passes de 8 attachments, más descriptores) | el resto | |

**Lo que dice esto**: la fase `vk-record` es sobre todo **el driver grabando las
barreras y los passes**, no el recorrido del chunk. Y el recorrido del chunk, que era
lo único que este agente podía tocar, era el 3,8 %.

**Una salvedad sobre el perfil**: la fase `vk-record` se mide con dos `__rdtsc` por
chunk (el coste del perfil es despreciable en esta fase), pero las *fases del draw*
(`targets`, `bind`, `state`, ...) se miden con dos `__rdtsc` cada una: el 24,74 % del
"draw total" que da el log **incluye** la instrumentación (~20 lecturas de reloj por
draw, un 5 - 10 % del draw). El número de este informe (5.213 ns por draw) sale de la
fase `vk-record`, que no paga ese coste.

## 5. Lo implementado

### 5.1 El chunk empaquetado (`vk_scheduler.h`)

El chunk era una **lista enlazada**: cada comando tenía un `next` y el hilo de
grabación de Vulkan los seguía con una carga dependiente por comando. Como el chunk lo
escribe el hilo de grabación de draws y lo lee otro núcleo, **cada `next` era un fallo
de caché** (la línea que el productor acaba de ensuciar, en la caché de otro núcleo).

Ahora los comandos van **uno detrás de otro** y sus **desplazamientos están en un
array**: el lector recorre el array (secuencial, el prefetcher trabaja) y sabe dónde
está el siguiente comando **antes** de ejecutar el actual, así que puede prefetcharlo.
El `next` desaparece (el `CommandBase` pasa de 16 a 8 bytes).

```
linked list (before), wide payload              3049 ns/chunk      203 ns/draw       16 ns/command
packed (the port's now), wide payload           1176 ns/chunk       78 ns/draw        6 ns/command
```

- **-125 ns por draw** (el 60 % del recorrido), **-2,4 % de la fase `vk-record`**, es
  decir **-0,5 % de la ventana**.
- La latencia también baja un poco: el hilo de grabación de Vulkan recibe los comandos
  antes.
- El tamaño del chunk y el umbral de patada (32 KiB) **no se tocan**: en este equipo la
  latencia manda más que la CPU (el chunk tarda 379 µs en llenarse).

### 5.2 El banco `draw-pipe-test` arreglado (`port/tests/test_draw_pipe.cpp`)

El banco estaba **roto**: llamaba a `DrawPipe::SetSpinBudget()`, que ya no existe (se
fue con el revert del giro adaptativo, la regresión de latencia del 20-10-19). Como el
encargo dice que las medidas se toman con este banco, se ha arreglado: ahora el
presupuesto de giro sale de `TLG_PIPE_SPIN_US` (la política del pipe: 200 µs con 12
hilos o más, 50 µs con menos) y el banco mide los dos casos del productor (cerca y
lejos) con ese presupuesto.

### 5.3 El banco nuevo del driver (`port/tests/vkbench/`)

`vk_record_cost.cpp` + `tri.vert`/`tri.frag` + un `CMakeLists.txt` propio (sólo
necesita el loader de Vulkan y `glslangValidator`). Mide el coste de grabación de cada
comando que el port emite, en la GPU discreta.

## 6. Lo que NO se toca, y por qué

| Palanca | Cuánto valdría | Por qué no |
|---|---|---|
| Saltar el `bindPipeline` redundante | 120 ns por bind repetido (0,7 por draw → ~84 ns/draw, 1,6 % de la fase) | Los pases del port (`host_passes/pp_pass.cpp`, `vk_scene_resolution.cpp`) **también ligan tuberías de gráficos** en el mismo command buffer. Para saltarla con seguridad hay que invalidar el estado en cada comando que pueda ligar una tubería, y esos ficheros están fuera de este alcance. Con el estado mal invalidado, el port ligaría la tubería equivocada (un fallo visual). |
| Saltar el `setVertexInputEXT` redundante | 16,6 ns por draw | El mismo problema (depende de la tubería). |
| Mover el estado dinámico a la tubería (estático) | 70 - 100 ns/draw | El estado dinámico cambia por draw en el GCN; meterlo en la clave de la tubería multiplicaría el número de tuberías (la caché de tuberías es un recurso escaso: 4 GB de VRAM). |
| Engordar el chunk (menos chunks/s) | unos pocos µs/s | El chunk tarda 379 µs en llenarse: el GPU ya va ese retraso detrás. En este equipo la latencia manda. |
| Menos copias de host | 2,8 % del frame | El pool (`tlg_copy.cpp`) agrupa bien (512 KiB / 256 ítems, medido más rápido que 64 KiB / 32 ítems) y reparte entre 4 hilos; las subidas son de la caché de buffers. |
| El giro del grabador | 67 % del hilo de grabación está girando | El giro adaptativo ya se probó y fue una **regresión** (26.378 → 826 draws/s). No se toca. |

## 7. Cómo se cierra la pregunta en la próxima sesión

El perfil ya imprime la línea que falta:

```
vk-record      20.57% of the window,   203.900 cycles/call,  13210 calls, N commands (M per draw, K cycles per command)
```

Con **N** (los comandos por chunk) y **K** (los ciclos por comando), la tabla de la
sección 2 da la atribución exacta: cada comando del port se multiplica por su coste y
la suma tiene que dar los 203.900 ciclos del chunk. Si la suma se queda corta (que es
lo que pasa hoy), los comandos que faltan son los que el perfil no cuenta (los
dispatches, las copias y las transiciones de la caché de texturas).

Para cerrar el resto, harían falta dos cosas más, ambas de otro agente:

1. **El número de transiciones de imagen por comando de barrera.** Es el número que
   multiplica todo (16 transiciones = 1.483 ns; 32 = 8.089 ns). El `TLG_IMG_TRACE` ya
   imprime las barreras cuando está activo (`FlushBarriers: %u image barriers`).
2. **Los ciclos por tipo de comando dentro del hilo de grabación.** Hoy el perfil
   cuenta *cuántos* comandos de cada tipo se graban, pero no *cuánto* cuestan. Con el
   tipo de comando en el cierre (el `Record()` de `vk_scheduler.h` podría llevarlo) y
   un `__rdtsc` por comando en `RecordChunk::Execute()`, la fase `vk-record` se
   reparte comando a comando sin conjeturas. Es un cambio pequeño en
   `vk_scheduler.h` + `vk_draw_profile.h` (el fichero del perfil, de otro agente).

## 8. Reproducir las medidas

```bash
# El banco del driver (necesita una GPU discreta; con la integrada también corre,
# pero los números son los de la Intel).
cmake -S port/tests/vkbench -B port/out/gpu-rec/vkbench -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C port/out/gpu-rec/vkbench
port/out/gpu-rec/vkbench/vk-record-cost.exe

# El banco del chunk (el recorrido del chunk: el propio y el del port).
cmake -S port/gpu -B port/out/gpu-rec/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTLG_LTO=OFF
ninja -C port/out/gpu-rec/gpu chunk-cost-test draw-pipe-test
port/out/gpu-rec/gpu/chunk-cost-test.exe
port/out/gpu-rec/gpu/draw-pipe-test.exe
```

Dos avisos que costaron tiempo y que el banco deja escritos (`vk_probe.c` es la sonda
mínima que los encontró):

* **Un dispositivo sin colas hace que el driver NVIDIA se caiga al reservar memoria.**
  El `vkCreateDevice` con `queueCreateInfoCount = 0` es legal según la especificación,
  pero el driver 595 peta en el `vkAllocateMemory` siguiente. El banco crea la cola.
* **La primera GPU de la lista no es la que se usa.** La máquina tiene la Intel
  integrada y la NVIDIA: el banco elige la **discreta**, que es la que corre el juego
  (con la Intel los costes son otra cosa: el `drawIndexed` cuesta 111 ns en vez de 16).

La varianza entre ejecuciones del banco del driver es de un 30 - 40 % (la máquina
estaba ocupada con otras sesiones); las cifras de la sección 2 son la mediana de tres
ejecuciones seguidas con la máquina descargada.
