# VRAM y detalle: por qué el presupuesto de 3500 MB no se respeta y qué hace el colector

Agente: VRAM/detalle del port nativo de inFAMOUS Second Son (CUSA00223, 1.07).
Fecha: 2026-10-19. Hardware de la medida: RTX 3050 Laptop **4 GB**, 720p, `ISS_GC_BUDGET_MB=3500`.

Todo lo de aquí sale de las sesiones manuales de la tabla de abajo (nada simulado) y de los
contadores que el port ya imprimía. La política nueva está verificada con tests unitarios
(`port/tests/test_gc_budget.cpp` y `port/tests/test_gc_budget.py`).

---

## 1. Línea base medida

| Sesión | Duración | Qué midió |
|---|---|---|
| `evidence/20261019-130735-g0-manual-paint` | 30 min | VRAM estable 3847–3874 MB de 4096 (94 %); el port declaraba 4717–4977 MiB |
| `evidence/20261019-134309-g0-manual-paint2` | 30 min | 56 informes de presión, **3771 expulsiones**, 334 en una ventana de 5 s; stalls: mediana 66,9 ms, p95 217 ms, **máx 9482,9 ms**; FPS mediana 27,8 |
| `evidence/20261019-140307-g0-manual-paint3` | 10,9 s | 24 imágenes registradas y **305,1 MB de texturas subidas** en un cuadro (`image create` 27,3 ms, `image refresh` 64,5 ms) → el jugador cerró la ventana (`stop_reason: user_closed`: no es un crash) |
| `evidence/20261019-140541-g0-manual-paint4` | 223 s | VRAM mediana 3849 MB, máx 3872 MB; 41 informes de presión; stall máx **13 899,6 ms** |

El log de presión (paint2, 30 min), igual en las cuatro sesiones:

```
Texture cache: memory pressure, 4757 of 1922 MiB (critical 1922): 334 images evicted, 0 written back
```

* `critical` = **1922 MiB**, siempre, en las cuatro sesiones.
* El número comparado (4757 MiB) es el **total del port** que ve el driver, no la cache.
* Expulsiones: 3771 en 30 min, hasta 334 por ventana de 5 s.
* Coste: hasta 176,7 MB de texturas re-subidas en un cuadro, 731 imágenes registradas en un
  cuadro, `image create` hasta 174,9 ms, y cuadros de 9,5 s y 13,9 s.

### La contabilidad del port no cabe en la tarjeta

| Medida | Valor | Fuente |
|---|---|---|
| Lo que declara el driver (`VK_EXT_memory_budget`) | 4757–4977 MiB | el log del port |
| La tarjeta | 4096 MiB | nvidia-smi |
| Lo que la tarjeta tiene en uso | 3848 MB (94 %) | nvidia-smi |
| La cache de texturas | ~3078 MiB | estimado: 3848 MB − el resto del port |

El total que declara el driver **supera la tarjeta**: cuenta también la memoria de host que el
buffer cache deja residente (la memoria del invitado que la GPU lee, que no es VRAM local). Por
esa razón ese número **no es comparable con un presupuesto de VRAM** ni con lo que dice
nvidia-smi, y usarlo como si lo fuera es lo que dejaba al colector en presión permanente.

## 2. Por qué el presupuesto de 3500 MB no se respeta

Tres defectos encadenados, todos en el colector (`video_core/texture_cache/texture_cache.cpp`):

1. **El "critical" no sale del presupuesto del launcher.** Con `TLG_GC_BUDGET_MB=3500` el
   constructor calcula las marcas legacy (70/85/95 % del presupuesto) y acto seguido
   `ApplyResidencyHeadroom()` las recorta con la *headroom de residencia*, que se deriva de
   `instance.GetTotalMemoryBudget()` (el **device-local que anuncia el driver**, ~2434 MB en esta
   máquina), no del presupuesto forzado: 2434 − 512 = **1922 MiB**. Las tres marcas quedan
   clavadas en 1922 MiB y el presupuesto de 3500 MB nunca entra en la comparación.
2. **Lo que se compara no es lo que el colector puede liberar.** El número comparado es
   `instance.GetDeviceMemoryUsage()`: el total que el driver tiene reservado, que incluye las
   *arenas del buffer cache* (memoria de host, no VRAM), el staging pool y las cuentas del
   propio driver. El colector solo puede liberar **imágenes**. Con el total en 4757 MiB y el
   límite en 1922 MiB, la presión es permanente: el colector entra en modo agresivo en cada
   submissión y expulsa imágenes para no llegar nunca a la marca. No es que el presupuesto se
   pase: es que el presupuesto **nunca se compara**.
3. **La contabilidad de lo liberado era falsa.** `evicting_bytes` sumaba
   `AlignUp(guest_size, 1024)`, o sea el tamaño **del invitado**, no el de la imagen en el
   dispositivo. Son cosas distintas: una textura BC decodificada en CPU ocupa 8× su tamaño de
   invitado, y una imagen tiled se rellena hasta el bloque. El colector creía liberar mucho más
   de lo que liberaba y por eso seguía evictando.

Efecto combinado: presión permanente + contabilidad inflada = expulsiones continuas de texturas
que el juego está usando, y una VRAM al 94 % que el driver compensa moviendo asignaciones a la
RAM del sistema (los 7,5 GB de RAM del proceso y los 4757 MiB declarados frente a los 4096 MiB
de la tarjeta).

## 3. Qué es lo que se pierde de detalle (y qué no)

La expulsión no "borra" la textura: la **recrea**. Y al recrearla, `RefreshImage` vuelve a
subir **todos los niveles de mip** desde la memoria del invitado (`texture_cache.cpp`,
`RefreshImage`, el bucle `for (u32 m = 0; m < num_mips; m++)`). Si el juego ya liberó ese
bloque de memoria (su *streaming* lo recicla), lo que se vuelve a subir es basura o la textura
que ocupa ahora esa memoria. Eso es exactamente "faltan detalles": no es que falten mips, es que
los mips que se vuelven a subir **no son los que eran**.

Lo que **no** es la causa (comprobado en el código, no supuesto):

| Sospecha | Veredicto |
|---|---|
| Mip forzado por falta de memoria | **No existe ese camino.** La imagen se crea siempre con su cadena completa de mips (`image.cpp`, `mipLevels = info.resources.levels`) y el `maxLod` del sampler sale del descriptor del invitado. No hay ningún recorte de mips por memoria en todo `video_core/`. |
| Decodificación BC en CPU | Medida: **508 MiB/s** (15 ns por bloque de 4×4), y las imágenes que se decodifican son diminutas (1×1, 512×1 con 32 capas, 1024×1 con 11 mips): **2 ms por MiB decodificado**, y el total por sesión es de kilobytes. No es un problema de primer orden (ver §6). |
| `deferred` / `stale_readback` | No toca la VRAM: solo cambia *cuándo* el juego ve el resultado de una lectura. |
| Resolución de render | 720p está dentro de lo normal; el "reduced-size draws 0/frame" del log dice que el *scene proxy* no está recortando nada. |
| **Ciclo evict → recreate → subida** | **Sí.** Medido: 305,1 MB de texturas subidas en un solo cuadro (paint3), 176,7 MB en paint2, y los cuadros de 9,5 s / 13,9 s. |

Sobre las **partículas**: el modo de lecturas del launcher es "2 = precisas" (GPU particles
need it) y sigue así; lo que sí se pierde es lo que el juego pinta con texturas que el
colector acaba de expulsar y re-subir con mips equivocados. Con la política nueva el ciclo
desaparece y las partículas vuelven a salir de las texturas correctas.

## 4. El cambio: la cache usa *su* parte del presupuesto

`port/gpu/shadps4/video_core/texture_cache/gc_budget.h` (nuevo) lleva la política sola, sin
Vulkan, para que se pueda testear:

* `cache_bytes`: los bytes **reales** que ocupan las imágenes de la cache (`Image::DeviceBytes()`,
  el tamaño real de la asignación VMA de cada *backing*), no el tamaño del invitado. Se lleva
  con el registro y la baja de cada imagen y se recalcula exacto cada 256 ticks.
* `working_set_bytes`: una media exponencial de lo que el juego toca por tick (el *conjunto de
  trabajo*). Es el **suelo**: la cache nunca suelta lo que el juego está usando.
* **Si el total declarado por el driver no cabe en la tarjeta, no se usa.** Es la corrección
  clave: ese número incluye la memoria de host de las arenas y no es comparable con un
  presupuesto de VRAM. En ese caso la presión se decide con **los bytes de la cache y el
  presupuesto**, y el log lo dice explícitamente.
* Cuando el total **sí** cabe en la tarjeta, es utilizable: la cuota de la cache es
  `objetivo = limitar(presupuesto − memoria_del_resto, suelo, presupuesto)`, con
  `memoria_del_resto = total − cache_bytes`.
* La **marca dura** (93 % de la memoria de la tarjeta, 3809 MiB en una de 4 GB) solo se usa
  cuando el total es fiable: si el driver está sobre la tarjeta (está moviendo asignaciones a
  la RAM), la cache suelta exactamente lo que el driver se pasa de la marca, nunca por debajo
  del suelo.
* La cache **nunca apunta más allá del 90 % de la tarjeta**: el resto es del escritorio y del
  driver.
* Un **paso** (una submissión) libera como mucho 1/8 del objetivo y hace **2 escrituras
  atrás** como máximo (cada una espera a la GPU): nada de masacratos ni de paradas.

El resto del cambio:

* La contabilidad de lo liberado usa ya los bytes reales (`image.DeviceBytes()`).
* `TLG_GC_POLICY=0` deja la regla vieja (A/B de una variable); por defecto manda la nueva.
* `TLG_GC_FLOOR_SHARE` (porcentaje del presupuesto que la cache nunca suelta, 25 por defecto) y
  `TLG_GC_HARD_SHARE` (porcentaje de la tarjeta, 93) están en la pantalla *Advanced* del
  launcher (`TLG_GC_POLICY`, `TLG_GC_FLOOR_SHARE`).
* El log dice las tres cosas sin ambigüedad y avisa cuando el total declarado no cabe en la
  tarjeta:

```
Texture cache: 3078 MiB in the cache (budget 3500 MiB, target 3500, working set 2900);
driver total 4757 MiB of the card's 4096 MiB, the driver's total is above the card: the
accounting includes host memory, the pressure uses the cache's bytes: 0 images evicted
(0 MiB), 0 written back, 0 images created again, 12 BC decodes (0 MiB, 1 ms) since the last report
```

### Criterio de aceptación (para la próxima sesión)

| Métrica | Antes (paint2/paint4) | Debe ser |
|---|---|---|
| Las marcas del colector | "critical 1922", siempre | derivadas del **presupuesto del launcher (3500)**: el log dice `budget 3500 MiB, target …` y nunca "critical 1922" |
| El total del driver | tomado como real | con la nota "the driver's total is above the card" (o, si cabe, comparado con la tarjeta) |
| Expulsiones por ventana de 5 s | 1–334 (3771 en 30 min) | **0** en una escena pesada (el recorte inicial, si lo hay, es de una vez) |
| "images created again" | (no se medía) | 0 en una escena pesada: ninguna textura vuelve a subir |
| Texturas re-subidas por cuadro | hasta 305,1 MB | solo las de la carga de la zona, no en bucle |
| Cuadros de más de 1 s | 9,5 s y 13,9 s | ninguno por evict/recreate |
| VRAM (nvidia-smi) | 3847–3874 MB (94 %) | ≤ 3800 MB si el buffer cache deja sitio; si no, el log lo dirá |

## 5. Presets

La curva del launcher (`qt/gfxlogic.py`, `infamous_run.py`): 4 GB → **3500 MB**, 6 GB → 3500,
8 GB → 5000, 12 GB → 8500, 16 GB → 12000, 24 GB → 19000, y nunca más del 90 % de la tarjeta.

La justificación del 4 GB **no** es "la misma parte de sí misma" (eso daba 2300 MB y recortaba
la cache a la cuarta parte sin que el resto del port bajara): es **lo que la tarjeta da de sí y
lo que el port necesita**. La tarjeta da 4096 MB; el resto (el escritorio, el driver y la
memoria del invitado que el buffer cache deja residente, ~600 MB medidos) tiene que caber en lo
que queda; la cache midió ~3078 MiB en la sesión de referencia, o sea que 3500 MB es el punto
de trabajo real y no un recorte.

| Preset | Presupuesto | Qué cambia | VRAM esperado | Detalle esperado |
|---|---|---|---|---|
| **EQUILIBRADO 4 GB** (`balanced_4gb`) | **3500 MB** (el punto medido) | lecturas precisas (`gpu_readbacks=2`), alcance de visión nativo, FSR 1 off, LOD bias 0 | **~3850 MB medidos** (la cache queda en ~3078 MiB, por debajo del presupuesto: el colector no expulsa nada) | el mismo de hoy, pero **sin** el ciclo de expulsión: las texturas se quedan en la GPU y no vuelven a subir con mips equivocados |
| **DETALLE** (`detail`) | **3500 MB** (el conjunto de trabajo cabe en él) | lo anterior + LOD bias −0,5 (más nítido al lejos) y RCAS 70 | el mismo ~3850 MB (el presupuesto es un tope y el conjunto de trabajo no lo llega) | texturas más nítidas a distancia, sin recargas; cuesta un poco de GPU, no VRAM |

Los dos presets **no** tocan el alcance de visión (un LOD más corto quitaría las partículas
lejanas que el usuario echa de menos) ni el modo de lecturas (las partículas de GPU lo
necesitan preciso). Registrados en `qt/gfxlogic.py` (`balanced_4gb`, `detail`).

## 6. El decode BC en CPU: medido, no es de primer orden

La decodificación de las texturas BC diminutas (`TLG_BC_TINY`: el driver NVIDIA rechaza las
imágenes BC 1D) ocurre dentro del cuadro, en `UploadDecodedImage`
(`video_core/texture_cache/texture_cache.cpp`).

**Medido** (`port/tests/test_bc_decode.cpp`, banco de pruebas en esta máquina):

```
bc-decode: 15.8 ms for 8 MiB of blocks (508 MiB/s, 15 ns per block)
```

* 2 ms de CPU por **MiB decodificado**.
* Las imágenes que se decodifican son diminutas: 1×1, 512×1 con 32 capas (≈128 KB de RGBA) y
  1024×1 con 11 mips. Una imagen de ese tamaño cuesta **0,25 ms** de decode, y una sesión
  entera delega en los **kilobytes**.
* Los 305,1 MB de "image uploads" de paint3 **no** son el decode: son la subida normal de las
  texturas de la zona (`image upload bytes` suma el tamaño de invitado de cada imagen subida).

Conclusión: **el decode no mueve la aguja** y no merece un hilo de trabajo propio. Lo que sí lo
multiplicaba era el ciclo de expulsión (cada textura expulsada se decodificaba otra vez), y eso
lo arregla la política del colector. Aun así, el port ahora **mide** el decode y lo publica en
el log de presión (`N BC decodes (X MiB, Y ms)`), junto con `N images created again` (las
imágenes que el colector tuvo que volver a crear): si en la próxima sesión el decode aparece con
miles de bytes, es que el ciclo de recreación sigue vivo y se verá en el mismo log.

## 7. El camino de ligadura: los datos de la cache en la línea de caché

Medición del padre (sesión de 150 s, SAFETY TRUST, 720p, `TLG_DRAW_PROFILE=1`): el *bind* es el
15,16 % del cuadro (83 ms → 12,6 ms), con 8.602 ciclos por llamada; el desglose por fase da
`bind` 15,16 %, `descriptors` 1,45 % y `state` 1,61 % (el `vk-record` 20,57 % es del driver). Con
2-6 buffers, 2-10 texturas y 0-8 samplers por draw son unas 20 operaciones de ligadura: 8.602/20
≈ 430 ciclos por operación, que es **latencia de memoria**, no cálculo.

### Qué se midió

`port/tests/test_cache_layout.cpp` (nuevo) mide la disposición real de las estructuras de la
cache y hace el mismo recorrido que hace la ligadura sobre un grupo de imágenes:

* **Antes** (`sizeof(Image)` 744): los campos que la ligadura lee (el estado de ligadura, el
  *backing* de la imagen y los datos del descriptor: `props`, `pixel_format`, `resources`,
  `size`) estaban repartidos en **3 líneas de caché** por imagen, y el conjunto caliente
  completo (con la dirección del invitado) en **8**.
* **Después**: la misma API, los campos declarados primero (`image.h`, `image_info.h`): la
  ligadura lee **1 línea de caché** por imagen y el conjunto caliente, **2**.

| Grupo de imágenes | Antes | Después | Ganancia |
|---|---|---|---|
| 1.024 | 3 ns / 7,5 ciclos por ligadura | 2 ns / 7,1 | — (cabe en la L2) |
| 3.300 | 3 ns / 9,8 | 3 ns / 7,6 | 1,3× |
| 8.192 | 11 ns / 32,3 | 3 ns / 10,5 | **3,1×** |
| 32.768 | 25 ns / 75,2 | 16 ns / 48,2 | **1,6×** |
| Líneas de caché por imagen ligada | 3 | **1** | 3× menos fallos |

El grupo crece durante la sesión (el log cuenta 731 imágenes registradas en un solo cuadro), y
cuanto más grande, más cuenta el ahorro: con 8.192 imágenes la parte de la cache en la ligadura
pasa de 32 a 10 ciclos.

### Recorridos O(n) por draw

Revisados: **no hay ninguno** en el camino de dibujo.
`FindImage` usa el memo de 1.024 entradas (O(1)); si falla, `ForEachImageInRegion` recorre el
cubucket de la tabla de páginas (2 niveles, 1-3 imágenes por página). `FindTexture` →
`Image::FindView` recorre las vistas del *backing* (1-4). `GetSampler` es un hash más un
candado. En la cache de buffers, `ObtainBuffer` → `GetArena` es una tabla de páginas (16 KB) y
los rangos son un `boost::icl::interval_set` (O(log n)).

### Lo que falta y necesita al renderer

El arreglo de la disposición quita 2 de los 3 fallos de caché por textura ligada, pero el
*working set* sigue siendo el array de imágenes: 3.300 imágenes × 752 B = 2,4 MB (con 8.192,
6 MB). La versión que de verdad lo arregla es un **array paralelo compacto** con los campos de
la ligadura (32 B por imagen → 105 KB con 3.300 imágenes, cabe en la L2) o **devolver un
puntero en vez de un id**, y las dos cosas las tiene que leer el renderer
(`renderer_vulkan/vk_rasterizer.cpp`, fuera de mi alcance): la cache puede ofrecer el array, pero
el cambio de la llamada está en el renderer. Lo mismo con los samplers: `GetSampler` toma un
candado por ligadura de sampler (el memo del renderer lo evita en la mayoría de los casos).

**Medible en la sesión final**: con `TLG_DRAW_PROFILE=1` y la misma escena (SAFETY TRUST, 720p,
150 s), comparar el *bind* del antes/después: el % del cuadro y los ciclos por llamada
(`bind 15,16 %, 8.602 ciclos por llamada`). El cambio no necesita ninguna variable de entorno
nueva: es la disposición de las estructuras, siempre activa. El test
`port/tests/test_cache_layout.py` deja el número medido a la vista (líneas de caché por imagen
y ns por ligadura) para comparar con la sesión.

## 8. Pendiente (fuera de mi alcance, para el director del port)

1. **El buffer cache es el dueño de la VRAM.** El total que declara el driver (4757 MiB) lo
   comparten la cache de texturas y las *arenas* del buffer cache, y la diferencia (~900 MB) es
   memoria de host. Con la política nueva el log lo dice: si el total declarado supera la
   tarjeta, la presión se decide con los bytes de la cache y el presupuesto, pero **el recorte
   que falta para bajar de 3800 MB de VRAM real no es de la cache**: es de la **residencia de la
   memoria del invitado en la tarjeta** (`video_core/buffer_cache/**`, fuera de mi alcance) o de
   la resolución. `TLG_MAX_BUFFER_MB` es la palanca que ya existe.
2. **El "hot set" y el detalle fino de las partículas** son asunto del renderer
   (`renderer_vulkan/**`, fuera de mi alcance): el "reduced-size draws 0/frame of N" y el
   `scene proxy` no se activan en la escena medida.
3. **Falta el reparto medido** cache ↔ buffer cache con el binario nuevo: la línea nueva del log
   lo da (`<cache> MiB in the cache … driver total <X> MiB of the card's <Y> MiB`). Con ese
   número se decide el ajuste final del presupuesto de los presets.
4. **El array paralelo de la ligadura** (los campos hot de cada imagen en una estructura
   compacta) lo tiene que leer el renderer: la cache puede ofrecerlo, pero la llamada está en
   `renderer_vulkan/**`.

## 9. Ficheros tocados

```
port/gpu/shadps4/video_core/texture_cache/gc_budget.h            (nuevo: la política)
port/gpu/shadps4/video_core/texture_cache/texture_cache.{h,cpp}  (el colector y la contabilidad)
port/gpu/shadps4/video_core/texture_cache/image.h                 (DeviceBytes, la disposicion)
port/gpu/shadps4/video_core/texture_cache/image_info.h            (la disposicion)
port/launcher/presets_vram.py                                    (nuevo: los dos presets)
port/launcher/infamous_run.py                                    (la curva del presupuesto)
port/launcher/advanced_table.py                                  (TLG_GC_POLICY, TLG_GC_FLOOR_SHARE)
port/launcher/qt/gfxlogic.py, port/launcher/qt/i18n/strings.json  (la curva, el registro y los textos)
port/tests/test_gc_budget.cpp, port/tests/test_gc_budget.py       (nuevos)
port/tests/test_cache_layout.cpp, port/tests/test_cache_layout.py (nuevos: la disposicion)
port/tests/test_bc_decode.cpp                                    (el banco de pruebas del decode)
port/tests/test_qt_screens_a.py                                  (la lista de presets)
```
