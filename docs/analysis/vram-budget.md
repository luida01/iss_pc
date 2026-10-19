# Presupuesto de VRAM en 4 GB — mapa, causas y matriz A/B

**Fecha:** 2026-10-19  
**Alcance:** análisis de solo lectura del port nativo de inFAMOUS Second Son (CUSA00223, base 1.00 + 1.07).  
**Base del repositorio:** `fb7e452` (clon superficial de un solo commit; no hay historial local).  
**Equipo objetivo:** Windows 11, i5-13420H, 16 GB RAM, RTX 3050 Laptop **4 GB**.  
**Estado:** no se ejecutó el juego, no se compiló y no se modificó código de producción. Todas las referencias son al árbol de trabajo actual; las cifras de VRAM del port nativo están pendientes de medir.

> Este documento no declara que el juego sea jugable en 4 GB. Ordena el problema, separa lo que el código demuestra de lo que solo es hipótesis, y propone experimentos de una sola variable. La referencia externa de caché host se incluye como evidencia estructural, no como cambio validado para este port.

---

## Resumen ejecutivo

1. **Con el launcher, el presupuesto por defecto es 3500 MB en cualquier GPU.** El launcher lo escribe siempre en `ISS_GC_BUDGET_MB` (`port/launcher/infamous_run.py:221`, `:239`, `:689`). En una tarjeta de 4 GB, tras la reserva de sistema de `vk_instance.cpp`, el presupuesto de dispositivo queda en **~3.1-3.5 GiB** según el `heapBudget` que reporte el driver. El propio launcher sugiere unos 2300 MB para 4 GB (`gfxlogic.py:107-134`), pero no cambia el valor solo. **Atención:** ejecutar `python port\run.py` directamente no pasa por `infamous_run.py`; en ese camino no hay presupuesto forzado salvo que se exporte `ISS_GC_BUDGET_MB`, y el GC usa los umbrales automáticos (con `trigger=0`).
2. **El sospechoso principal no es la caché de texturas: es la residencia de buffers sparse.** Cualquier binding de más de 16 KB, o escrito por la GPU, va a arenas sparse; `EnsureResident` hace residente el rango completo en gránulos de 16 MB (`buffer_cache.cpp:677-764`). Un solo draw de carga de partida en inFAMOUS pide unos **2.4 GB** (`buffer_cache.h:61-64`, `buffer_cache.cpp:81-82`). No hay LRU ni expulsión de buffers: la memoria queda residente hasta destruir la arena.
3. **El GC de texturas mide el heap total, no solo texturas.** Cuando `CanReportMemoryUsage()` está disponible, `total_used_memory = instance.GetDeviceMemoryUsage()` (`texture_cache.cpp:1823-1824`) incluye buffers y driver. Pero el GC solo libera imágenes. Si el heap está lleno por buffers, el GC puede estar expulsando texturas sin resolver nada.
4. **En 4 GB los umbrales se comprimen.** Con presupuesto forzado de 3500 MB, el margen de 1 GiB para buffers hace que `critical` y `pressure` colapsen en torno a **2.1-2.5 GiB de uso total**; el `trigger` queda algo por debajo (`texture_cache.cpp:1831-1852`, `texture_cache.h:50-57`). La banda entre “presión” y “crítico” desaparece y el GC pasa de pasivo a agresivo de golpe.
5. **El GC corre una vez por submit y tiene límites duros:** solo imágenes con edad ≥16/80/160 ticks, como máximo 10/20/40 por pasada y 256 candidatas visitadas (`texture_cache.cpp:1895-1899`, `:1907`, `:1920-1927`). Una carga de guardado o un streaming de geometría puede pedir gigabytes entre dos submits.
6. **Las imágenes tiled escritas por la GPU quedan pinneadas.** No se pueden descargar (`SafeToDownload`) y el GC las salta (`texture_cache.cpp:1920-1927`, `:2067-2071`). El comentario de `:1901-1904` documenta que, antes del cambio de cola, el VRAM crecía con cada zona visitada. Es una hipótesis de código, no una medición del port nativo.
7. **Los fallbacks por encima del presupuesto esconden la causa.** Imagen y buffer reintentan sin `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT` (`image.cpp:182-213`, `buffer.cpp:114-149`); el juego sigue, pero el driver puede mover memoria a RAM del sistema. El síntoma pasa de “OOM” a “stutter y memoria del sistema”, sin arreglar la residencia.
8. **Readbacks y staging son sobre todo RAM host, no VRAM.** La cola de readback de imágenes es 64 copias/64 MB (`readback_queue.h:16-20`), el staging retiene hasta 512 MB host (`vk_staging_buffer_pool.cpp:24-56`) y la ventana de descarga de buffers es 512 KB (`buffer_cache.cpp:257-261`). Afectan latencia y sincronización; no son el principal consumidor de VRAM, salvo el stream buffer de 128 MB si se elige `device`.
9. **La caché de pipeline es disco, no VRAM.** El blob del driver se guarda en `<cache>/driver-<serial>/*.vkc` con tope de 1 GB (`vk_pipeline_cache.cpp:371-400`, `:491-509`). Puede haber copias internas del driver no medidas, pero no hay evidencia de que sea el factor de 4 GB. `ISS_PIPELINE_BINARIES=1` sigue crasheando en NVIDIA 595 (`infamous_run.py:220`, `backports_table.py:240-243`).
10. **La referencia externa demuestra que la residencia sparse es atacable, pero no es una solución lista.** En el fork externo de shadPS4 (base `94e2177`, mismo portátil), el baseline falla una asignación sparse de **566.689.792 bytes** tras ~2.44 GiB sparse residentes; la caché host canónica con límite de 3200 MiB sobrevivió **1322.5 s** sin OOM/DeviceLost, con pico GPU de **1651 MiB** y RAM privada de **8033.86 MiB**. No está validada en el port nativo, no prueba 30 FPS y tiene riesgos de coherencia.

**Orden recomendado:** instrumentar primero (sin cambiar comportamiento), después A/B de presupuesto (2300/2000/0), luego cadencia de GC/readbacks, y solo con evidencia diseñar residencia de buffers o una caché host. No portar la caché host antes de tener contadores por caché y pruebas de coherencia.

---

## 1. Mapa de flujo de memoria/VRAM

### 1.1 De la configuración al presupuesto

```
settings.json (vram_budget_mb, por defecto 3500)
        │
        ▼
infamous_run.py:689  →  ISS_GC_BUDGET_MB
        │
        ▼
probe.c:829-865  alias ISS_* → TLG_*  (solo si TLG_* no existe)
        │
        ▼
texture_cache.cpp:1831-1852  TLG_GC_BUDGET_MB (presupuesto forzado)
        │
        ├─ trigger  = 70 %
        ├─ pressure = 85 %
        ├─ critical = 95 %
        └─ ApplyResidencyHeadroom()  (tarjetas ≤6 GB, no integradas)
```

- `port/launcher/infamous_run.py:221` define `ISS_GC_BUDGET_MB=3500` como valor por defecto del entorno; `:239` define `vram_budget_mb=3500` en `DEFAULT_SETTINGS`; `:689` **siempre** escribe `ISS_GC_BUDGET_MB` desde `settings['vram_budget_mb']`, salvo que el usuario ya tenga `ISS_GC_BUDGET_MB` en el entorno.
- **`port/run.py` no aplica ese valor.** Si se lanza directamente (el flujo de `AGENTS.md`), `ISS_GC_BUDGET_MB` no se define y `TLG_GC_BUDGET_MB` tampoco: la rama forzada no se ejecuta. Para que un A/B de presupuesto sea comparable, hay que fijar `ISS_GC_BUDGET_MB` explícitamente en ambos caminos y comprobar en el log si aparece la línea `Texture cache: ... budget`.
- `port/src/probe.c:829-865` copia cada `ISS_X` a `TLG_X` al arrancar, sin sobrescribir un `TLG_X` ya presente. El núcleo de vídeo sigue leyendo `TLG_*`.
- `port/gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1831-1852` lee `TLG_GC_BUDGET_MB` una sola vez (static) y recalcula los tres umbrales. Si el valor es 0, no entra en la rama forzada y usa los umbrales automáticos.
- `port/launcher/qt/gfxlogic.py:107-134` contiene la curva de sugerencia (`BUDGET_POINTS`) y `budget_is_too_high()`. Para 4 GB sugiere ~2300 MB; el aviso solo salta si `budget > 0.9 * VRAM` (`:132-134`), así que 3500 MB sobre 4096 MB **no genera aviso**.

### 1.2 Presupuesto del dispositivo y uso real

- `port/gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:953-994` suma `heapBudget` de los heaps válidos. En una GPU discreta solo considera heaps device-local y resta una reserva de sistema: `min(total/8, 1 GB)`. En 4 GB eso deja ~3.1-3.5 GiB como presupuesto del port, según lo que el driver reporte como `heapBudget`.
- `vk_instance.cpp:1053-1079` implementa `GetDeviceMemoryUsage()` y `GetDeviceMemoryBudgetNow()`: suman `heapUsage` y `heapBudget` de los mismos heaps válidos. Es la fuente del GC y del overlay.
- En una GPU integrada se incluyen todos los heaps y se reservan 8 GB para el sistema (`vk_instance.cpp:981-994`); por eso el mismo presupuesto forzado de 3500 MB puede quedar por encima del presupuesto real del driver. El código ya usa el mínimo entre el forzado y el actual en integradas (`texture_cache.cpp:1836-1844`).

### 1.3 Caché de texturas: presupuesto, contabilidad y GC

**Umbrales**

- `texture_cache.cpp:53-93` calcula los umbrales normales. `TARGET_GC_THRESHOLD=8 GB`, `DEFAULT_PRESSURE_GC_MEMORY=1.5 GB`, `DEFAULT_CRITICAL_GC_MEMORY=3 GB` (`texture_cache.h:50-57`).
- En tarjetas no integradas de ≤6 GB, `headroom_gc_memory = max(device_local_memory - 1 GB, device_local_memory / 2)` y `ApplyResidencyHeadroom()` recorta `critical`, luego `pressure`, luego `trigger` (`texture_cache.cpp:81-101`, `texture_cache.h:54-57`). El log imprime `Texture cache: ... MB budget, collector critical at ... MB at most (... MB left for buffers)`.
- Ejemplo con `device_local_memory = 3584 MB` (si el driver reporta el heap completo): `headroom = 2560 MB`; forzado 3500 → `critical = min(3325, 2560) = 2560`, `pressure = min(2975, 2560) = 2560`, `trigger = min(2450, 2560) = 2450`. **Pressure y critical coinciden.** Si el `heapBudget` real es menor (por ejemplo ~3.1 GiB), `headroom` baja de 2450 y **los tres umbrales colapsan**: el GC no arranca hasta ~2.1 GiB y lo hace ya en modo agresivo (edad 160, 40 borrados). La cifra exacta se lee del log.
- Sin presupuesto forzado (`TLG_GC_BUDGET_MB=0`): `trigger = (device - min(device, 8 GB)) / 2 = 0`, `pressure ≈ 1.5 GB`, `critical ≈ 2.5 GB` tras el recorte. Es decir, el GC se ejecuta en **cada submit**, no es “sin GC”.

**Contabilidad**

- `texture_cache.cpp:1653` suma `AlignUp(image.info.guest_size, 1024)` al registrar una imagen; `:1696` lo resta al liberarla. Es tamaño **guest**, no el tamaño real de la asignación Vulkan.
- El tamaño real sí se guarda en `UniqueImage::size_bytes = alloc_info.size` (`image.cpp:199-212`), pero no se usa para la contabilidad del GC ni para el overlay.
- En `GarbageCollectImages`, `total_used_memory = instance.GetDeviceMemoryUsage()` (`texture_cache.cpp:1823-1824`) **sobrescribe** la suma de tamaños guest con el uso total del heap cuando `CanReportMemoryUsage()` es verdadero. Es decir, en ese caso los umbrales se comparan contra texturas + buffers + driver + staging device-local, no contra texturas solamente. Si la extensión no está disponible, se usan los umbrales por defecto y la suma guest.

**Ejecución del GC**

- `Rasterizer::OnSubmit()` llama `texture_cache.RunGarbageCollector()` una vez por submit (`vk_rasterizer.cpp:1998-2005`), después de `ProcessDownloadImages()`.
- `configure()` elige edad y número de borrados: `16/80/160` ticks y `10/20/40` imágenes (`texture_cache.cpp:1895-1899`).
- Solo recorre imágenes por debajo de `gc_tick - ticks_to_destroy`; una imagen recién usada no es candidata.
- Una pasada se detiene tras visitar 256 imágenes (`texture_cache.cpp:1907-1914`).
- Las imágenes tiled con `SafeToDownload()` se saltan porque no hay descarga no lineal (`texture_cache.cpp:1920-1927`). `SafeToDownload()` es `GpuModified && !Dirty` (`image.h:117-119`).
- `ReleaseMemoryForAllocation()` (`texture_cache.cpp:2056-2086`) solo libera imágenes con edad ≥16 ticks y también salta tiled+download. No toca buffers.

### 1.4 Caché de buffers: arenas sparse y residencia

```
draw con buffer >16 KB o escrito por GPU
        │
        ▼
BufferCache::ObtainBuffer()  (buffer_cache.cpp:496-556)
        │
        ├─ lectura ≤16 KB y no GPU-modified → stream buffer (128 MB host, por defecto)
        └─ resto → arena sparse
                │
                ▼
        EnsureResident()  (buffer_cache.cpp:677-764)
                │
                ├─ gránulos de 16 MB
                ├─ chunks de 256 MB
                ├─ rangos >64 MB → vkAllocateMemory dedicada
                └─ si falla: libera texturas y reintenta UNA vez; si falla, ASSERT
```

- `STREAM_THRESHOLD = 16 KB`: solo las lecturas pequeñas y no modificadas por GPU se copian al stream buffer (`buffer_cache.cpp:512-540`, `buffer_cache.h:57-60`).
- Las arenas sparse se crean con páginas de 4 GiB (o 2 GiB si el driver limita el buffer) y bloques de 16 KB (`buffer_cache.h:49-64`).
- `EnsureResident` alinea el rango a gránulos de 16 MB y hace residente el rango completo, no solo los bloques que el draw va a leer (`buffer_cache.cpp:38-39`, `:683-690`).
- `AllocateResidency` usa chunks de 256 MB; un rango mayor que 64 MB recibe una asignación dedicada (`buffer_cache.cpp:766-810`). El comentario de `buffer_cache.cpp:81-82` documenta el caso de ~2.4 GiB que cruzaba tres páginas de 1 GiB.
- No hay LRU ni expulsión de buffers: `resident_ranges` solo se libera al destruir la arena. `ReleaseMemoryForResidency` existe, pero solo llama a `texture_cache.ReleaseMemoryForAllocation()`; no libera buffers. Si la asignación falla, se reintenta una vez y, si vuelve a fallar, `ASSERT_MSG` (`buffer_cache.cpp:712-726`, `:822-846`).
- La carga se hace en partes de 256 MB (`MAX_UPLOAD_PART_BYTES`, `buffer_cache.h:61-64`; `buffer_cache.cpp:1477-1539`), pero **la residencia no se hace en partes**. Ese es el desacople clave: la subida es gradual, la VRAM se compromete de golpe.

### 1.5 Readbacks y staging

- Las lecturas de imágenes se difieren por defecto: `TLG_DEFERRED_READBACK` (`texture_cache.h:525-529`). La cola tiene 64 entradas y 64 MB por defecto (`readback_queue.h:16-20`), y `texture_cache.cpp:44-46` lo imprime al arrancar.
- `TLG_STALE_READBACK=1` permite que la CPU lea la última versión publicada por la GPU (un frame tarde) en lugar de esperar (`texture_cache.h:535-541`). Necesita readbacks diferidos. **Ojo con el default efectivo:** `graphics_options.DEFAULTS` pone `stale_readback='1'` (`graphics_options.py:20`) y `run.py:125` lo vuelca a `TLG_STALE_READBACK`, aunque `backports_table.py:230` documente “off”. El log de arranque imprime `; CPU reads use the previous frame's copy` cuando está activo; verificarlo antes de atribuir diferencias de rendimiento o de lógica.
- Las descargas de imágenes usan staging `HostCached` (`texture_cache.cpp:394-397`, `:425-428`); las de buffers usan una ventana de 512 KB (`TLG_DOWNLOAD_WINDOW_KB`, `buffer_cache.cpp:257-261`).
- El staging de subida retiene hasta 512 MB de RAM host (`TLG_STAGING_KEEP_MB`, `vk_staging_buffer_pool.cpp:24-56`), con 8 bloques de 16 MB precalentados (128 MB) y bloques que viven 3000 frames idle (`:24-30`, `:162-204`).
- En GPU discreta, `GetDeviceMemoryUsage()` solo suma heaps device-local; el staging HostCached/HostUncached **no entra** en el presupuesto de VRAM. En integrada sí puede entrar porque se incluyen todos los heaps.
- El stream buffer de 128 MB es host-cached por defecto (`TLG_STREAM_MEMORY=cached`); si se elige `device`, esos 128 MB sí van a VRAM (`advanced_table.py:107-113`).

### 1.6 Caché de pipeline

- BP08 guarda el blob del driver en disco: `<cache>/driver-<serial>/vk_pipeline_cache_<vendor>_<device>_<uuid>.vkc` (`vk_pipeline_cache.cpp:371-400`).
- El tope es 1 GB (`TLG_DRIVER_CACHE_MAX_MB`, `:491-509`); si se supera, no se escribe. Vulkan no permite expulsar entradas de una pipeline cache; borrar el archivo la reinicia.
- Es un archivo, no una asignación de VRAM del port. El driver puede tener copias internas, pero no hay contador que las separe. El coste observable principal es **compilación/estutter**, no pico de VRAM.
- `ISS_PIPELINE_BINARIES=1` sigue crasheando en NVIDIA 595 (`infamous_run.py:220`; `backports_table.py:240-243`).

### 1.7 Calentamiento y trabajo en vuelo

- `TLG_HEAP_WARM` está encendido por defecto (`advanced_table.py:119-121`). Los buffers de ≥256 MB que el juego bound enteros se suben 16 MB por submit desde el inicio, y se recuerdan hasta 16 rangos (`vk_rasterizer.cpp:1931-1964`, `:2006-2020`). Esto reduce el freeze del primer bind, pero **aumenta la residencia temprana** y puede chocar con el presupuesto antes de que el juego termine de cargar.
- El stream buffer y el staging no cambian el pico de VRAM por sí solos, pero el número de frames en vuelo y las copias pendientes sí cambian *cuándo* se libera memoria.

### 1.8 Visibilidad actual

- `tlg_perf_stats.cpp:115` muestrea `GetDeviceMemoryUsage()` cada 500 ms (`:123`); el overlay muestra “VRAM (used / available)” (`tlg_overlay.cpp:634`).
- No hay desglose por caché, ni bytes residentes de buffers, ni tamaño real agregado de imágenes, ni contador de expulsiones/fallos.
- El visor de memoria de devtools está desactivado en este port: `GpuMemoryViewer::IsEnabled()` devuelve `false` (`gpu_memory.h:5-12`), y `StagingBufferPool::PublishStats()` sale antes de publicar (`vk_staging_buffer_pool.cpp:196-204`).

### 1.9 Diagrama de flujo completo

```
┌─────────────────────────────┐
│ settings.json / tlg.ini     │
│ vram_budget_mb = 3500       │
│ render_resolution = native  │
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐        ┌──────────────────────────────┐
│ launcher / run.py           │        │ probe.c: ISS_* → TLG_*       │
│ ISS_GC_BUDGET_MB            │───────▶│ (no sobrescribe TLG_*)       │
└──────────────┬──────────────┘        └──────────────┬───────────────┘
               │                                      │
               ▼                                      ▼
┌─────────────────────────────┐        ┌──────────────────────────────┐
│ TextureCache                │        │ BufferCache                  │
│ · imágenes VMA              │        │ · arenas sparse 4/2 GiB      │
│ · contabilidad guest        │        │ · gránulos 16 MB             │
│ · GC en OnSubmit            │        │ · chunks 256 MB              │
│ · solo imágenes             │        │ · sin LRU ni expulsión       │
└──────────────┬──────────────┘        └──────────────┬───────────────┘
               │                                      │
               └──────────────┬───────────────────────┘
                              ▼
               ┌─────────────────────────────────────┐
               │ Heaps device-local (≈4 GB)          │
               │ uso total = GetDeviceMemoryUsage()  │
               └──────────────┬──────────────────────┘
                              │
              ┌───────────────┼────────────────┐
              ▼               ▼                ▼
        GC de imágenes   fallback VMA     driver/WDDM
        (edad/límites)   sin budget       demotion a RAM
                              │
                              ▼
                    OOM / DeviceLost /
                    stutter y RAM del sistema

Readbacks: GPU → staging HostCached (RAM host, no VRAM)
Pipeline cache: disco (1 GB), no asignación del port
```

---

## 2. Top 10 de causas probables

Ordenado por **impacto esperado en 4 GB / esfuerzo de mitigación**. “Confianza” es sobre el mecanismo de código; la magnitud en el port nativo sigue sin medirse.

| # | Causa | Impacto | Esfuerzo | Confianza | Evidencia principal |
|---|---|---|---|---|---|
| 1 | Rangos sparse gigantes residentes de una vez, sin caché host ni LRU de buffers | 5/5 | 4/5 | Alta | `buffer_cache.cpp:677-810`; `buffer_cache.h:61-64`; fork externo |
| 2 | Presupuesto 3500 MB + GC sobre heap total + colapso de umbrales | 5/5 | 1/5 | Alta | `infamous_run.py:221/239/689`; `texture_cache.cpp:1823-1852` |
| 3 | Cadencia y elegibilidad del GC insuficientes para ráfagas | 4/5 | 3/5 | Alta (código), media (magnitud) | `texture_cache.cpp:1895-1927`; `vk_rasterizer.cpp:1998-2005` |
| 4 | Imágenes tiled GPU-written pinneadas | 4/5 | 4/5 | Media | `texture_cache.cpp:1901-1927`, `:2067-2071`; `image.h:117-119` |
| 5 | Asignación dedicada >64 MB y reintento único; sin reclaim de buffers | 4/5 | 4/5 | Alta | `buffer_cache.cpp:766-810`, `:822-846` |
| 6 | Fallback over-budget que oculta la presión y demota a RAM | 3/5 | 1/5 | Alta | `image.cpp:182-213`; `buffer.cpp:114-149` |
| 7 | Readbacks y stalls de sincronización | 3/5 | 2/5 | Media | `texture_cache.cpp:394-428`; `buffer_cache.cpp:257-261`; `readback_queue.h:16-20` |
| 8 | MSAA / render targets / resolución interna | 3/5 | 1/5 | Media | `image.cpp:325-333`; `backports_table.py:219-221`; `graphics_options.py:91-101` |
| 9 | Caché de pipeline y compilación | 1-2/5 (VRAM) | 1/5 | Alta (es disco) | `vk_pipeline_cache.cpp:371-400`, `:491-509` |
| 10 | Heap warm, staging y frames en vuelo | 2/5 | 1-2/5 | Media | `vk_rasterizer.cpp:1931-1964`, `:2006-2020`; `vk_staging_buffer_pool.cpp:24-56` |

### 2.1 Rangos sparse gigantes residentes, sin caché host ni LRU de buffers

**Mecanismo.** Un descriptor de lectura de ~2.4 GB hace que `EnsureResident` comprometa todo el rango en gránulos de 16 MB. Los chunks de 256 MB reducen el número de `vkAllocateMemory`, pero no el total. El rango queda residente hasta destruir la arena; no hay una política que lo devuelva a RAM ni que lo recorte a lo que el shader realmente lee.

**Evidencia.** `buffer_cache.h:61-64` documenta el draw de 2.4 GB al cargar una partida; `buffer_cache.cpp:81-82` documenta el cruce de páginas de 1 GiB. En el fork externo, el diagnóstico aisló una petición de **566.689.792 bytes** tras **2.624.782.336 bytes** sparse residentes, con uso Vulkan de **3.702.108.160** frente a presupuesto de **3.531.289.396** (`EXPERIMENTO-MEMORIA.md`; `RESULTADOS.md`).

**Por qué pega en 4 GB.** El presupuesto total es ~3.5 GiB; un solo rango de 2.4 GiB deja menos de 1.1 GiB para texturas, render targets, staging device-local y driver. El GC de texturas no puede liberar buffers, así que la presión no baja.

**Confianza.** Alta en el mecanismo; alta en que el caso existe en inFAMOUS (el comentario cita el juego y el fork lo reprodujo); media en que sea el único caso grande del port nativo.

**Esfuerzo.** Alto: requiere una política de residencia por subrango, caché host canónica o ambas, con coherencia de escrituras CPU/GPU y aliases.

**Cómo confirmar.** Loguear cada `EnsureResident`: tamaño solicitado, rango, bytes ya residentes, uso/budget antes y después, y si hubo reintento. Sin eso, el debate “texturas vs buffers” es especulación.

### 2.2 Presupuesto 3500 MB, GC sobre heap total y colapso de umbrales

**Mecanismo.** El launcher pone 3500 MB aunque la GPU tenga 4096 MB. `vk_instance` resta la reserva de sistema y deja ~3.5 GiB. El GC compara el uso **total** del heap contra umbrales calculados con el presupuesto forzado, pero solo puede liberar imágenes. Además, el margen de 1 GiB para buffers recorta `critical` y `pressure` al mismo valor (~2.5 GiB), eliminando la banda intermedia.

**Evidencia.** `infamous_run.py:221`, `:239`, `:689`; `texture_cache.cpp:1823-1852`; `texture_cache.h:50-57`; `vk_instance.cpp:953-994`.

**Por qué pega en 4 GB.** En 6 GB el presupuesto 3500 deja más margen y el comentario del launcher dice que 5600 agota 6 GB. En 4 GB, 3500 es el 85% de la VRAM nominal y deja menos de 600 MB para el resto; el presupuesto efectivo tras la reserva es aún menor.

**Confianza.** Alta. Es aritmética de código, no una hipótesis.

**Esfuerzo.** Bajo: cambiar `vram_budget_mb` a 2300/2000 o poner `ISS_GC_BUDGET_MB=0` para comparar. Ojo: 0 no significa “sin GC”; significa umbrales automáticos con `trigger=0`.

**Cómo confirmar.** El log `Texture cache: ... critical at ... MB at most (... MB left for buffers)` da las cifras exactas. Registrar también el `heapBudget` del driver al inicio (`vk_instance.cpp:953-994`).

### 2.3 Cadencia y elegibilidad del GC

**Mecanismo.** El GC corre una vez por submit. En el mejor caso libera 10 imágenes de edad ≥16 ticks; en presión, 20 de edad ≥80; en crítico, 40 de edad ≥160. Una pasada visita como máximo 256 imágenes. Las imágenes recién creadas/usadas no son elegibles.

**Evidencia.** `texture_cache.cpp:1895-1899`, `:1907-1914`, `:1920-1927`; `vk_rasterizer.cpp:1998-2005`.

**Por qué pega en 4 GB.** Cargar una zona o un guardado puede crear decenas de imágenes y hacer residentes cientos de MB en pocos frames. Para cuando el GC ve el pico, el driver ya está por encima del presupuesto; el fallback over-budget o el OOM ocurren antes de que la edad de 16 ticks se cumpla.

**Confianza.** Alta en el mecanismo; la magnitud depende de la ráfaga real.

**Esfuerzo.** Medio: bajar la edad mínima, aumentar el número de candidatos, o ejecutar GC también antes de asignaciones grandes (no solo en `OnSubmit`), con cuidado de no expulsar imágenes en uso.

**Cómo confirmar.** Contadores por pasada: candidatas, saltadas por edad, saltadas por tiled, bytes liberados, si se alcanzó el límite de 256, y uso antes/después.

### 2.4 Imágenes tiled GPU-written pinneadas

**Mecanismo.** `SafeToDownload()` es verdadero para imágenes `GpuModified && !Dirty`; si además son tiled, el GC no las puede descargar (no hay soporte de download no lineal) y las deja en la cola. `ReleaseMemoryForAllocation` hace lo mismo (`texture_cache.cpp:1920-1927`, `:2067-2071`).

**Evidencia.** El comentario de `texture_cache.cpp:1901-1904` dice que, antes de mover las no elegibles al final de la cola, “nothing was ever freed again and VRAM grew with every area visited” porque las imágenes más viejas eran tiled GPU-written. El cambio mitiga el bloqueo de la cola, pero no libera esas imágenes.

**Por qué pega en 4 GB.** Suelen ser render targets de efectos/partículas; en una sesión larga se acumulan. No se pueden descargar sin implementar downloads no lineales, así que el GC no las toca ni en crítico.

**Confianza.** Media: el mecanismo está claro; falta medir cuántas hay y cuánto ocupan en el port nativo.

**Esfuerzo.** Alto: implementar descarga de tiled o una política de reutilización/aliasing segura; no basta con “borrarlas”.

**Cómo confirmar.** Contar imágenes tiled, tiled+GpuModified, bytes guest y bytes reales; y cuántas se saltan por esta condición en cada pasada.

### 2.5 Asignación dedicada >64 MB y reintento único; sin reclaim de buffers

**Mecanismo.** `AllocateResidency` da una asignación dedicada a rangos >64 MB (`buffer_cache.cpp:787-790`). Si falla, `EnsureResident` deshace lo asignado, llama `ReleaseMemoryForResidency` (solo texturas), hace `scheduler.Finish()` y reintenta **una vez**. Si vuelve a fallar, `ASSERT_MSG` (`buffer_cache.cpp:712-726`, `:822-846`).

**Por qué pega en 4 GB.** En el momento del fallo, el heap puede estar lleno de buffers que no se pueden liberar. Liberar texturas es necesario pero no suficiente; el segundo intento repite la misma cuenta. El resultado es un cierre determinista en la carga, no una degradación gradual.

**Confianza.** Alta en el mecanismo; el fork externo reprodujo el fallo exacto en `allocate sparse buffer memory`.

**Esfuerzo.** Medio-alto: residencia incremental, recorte a los rangos realmente accedidos, o host cache. Un simple “reintentar más veces” no arregla la falta de espacio.

**Cómo confirmar.** Log de cada intento con `bytes`, `resident_blocks`, uso/budget y resultado; y un contador de “falló tras liberar texturas”.

### 2.6 Fallback over-budget

**Mecanismo.** Si `vmaCreateImage`/`vmaCreateBuffer` devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY`, se reintenta sin `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT` (`image.cpp:182-213`; `buffer.cpp:114-149`). El comentario dice que el driver mueve memoria a RAM del sistema en lugar de fallar.

**Por qué pega en 4 GB.** El juego “sigue”, pero el pico de VRAM reportado puede dejar de ser el pico real y el coste se traslada a RAM/PCIe. Además, oculta que la asignación original ya estaba por encima del presupuesto: sin un contador de fallbacks, el análisis ve estabilidad donde hay demotion.

**Confianza.** Alta en el mecanismo; media en cuánto contribuye al stutter.

**Esfuerzo.** Bajo para instrumentar; alto para eliminar el fallback sin romper compatibilidad.

**Cómo confirmar.** Contar fallbacks por tipo, tamaño y heap; medir RAM privada y page faults junto al pico de VRAM. Un A/B con el fallback instrumentado (no eliminado) es suficiente para el diagnóstico.

### 2.7 Readbacks y stalls de sincronización

**Mecanismo.** Las descargas de imágenes usan staging host y, según el modo, esperan a la GPU. La cola por defecto es 64 copias/64 MB; `TLG_GC_BATCH_DOWNLOAD` agrupa las descargas del GC en un solo wait, con hasta 256 MB de staging. La ventana de descarga de buffers es 512 KB. `TLG_STALE_READBACK` evita la espera a cambio de leer un frame tarde.

**Evidencia.** `texture_cache.cpp:394-428`; `readback_queue.h:16-20`; `buffer_cache.cpp:257-261`; `texture_cache.cpp:1868-1888` (batch download).

**Por qué importa.** No suele ser el principal consumidor de VRAM, pero una espera de GPU en medio de una carga impide que el juego avance y hace que el GC llegue tarde. En el fork externo, los readbacks precisos costaron ~35-43% de FPS frente a desactivados en la misma escena; el modo relajado redujo el coste pero con coherencia más débil.

**Confianza.** Media. Falta medir el tiempo de espera y el volumen en el port nativo.

**Esfuerzo.** Bajo-medio: flags y cola; alto si se toca la coherencia.

**Cómo confirmar.** `TLG_READBACK_STATS=1` y contadores de cola: profundidad, bytes, esperas, stale hits, tiempo por descarga.

### 2.8 MSAA / render targets / resolución interna

**Mecanismo.** `NumSamples(info.num_samples, supported_samples)` decide el número de muestras de la imagen (`image.cpp:325-333`). `ISS_NO_MSAA` desactiva multisampling (`backports_table.py:219-221`). La resolución interna se cambia con el patch de `tlg.ini` (`render_resolution`: `native|480p|720p|1440p|1800p|4K`, `graphics_options.py:91-101`), no con una tecla del launcher.

**Por qué pega en 4 GB.** Los render targets MSAA y los buffers de postproceso crecen con la resolución. Bajar a 720p reduce el pico, pero el fork externo no vio una mejora consistente de FPS porque el juego estaba limitado por CPU; es una mitigación de VRAM, no un arreglo de la residencia de buffers.

**Confianza.** Media. El efecto existe; la magnitud en este port no está medida.

**Esfuerzo.** Bajo (flags/patch), pero hay que separar resolución de MSAA y verificar visualmente.

**Cómo confirmar.** A/B de 720p y de `ISS_NO_MSAA` por separado, con el mismo save y la misma ruta; medir pico de VRAM y p95, no solo FPS medio.

### 2.9 Caché de pipeline y compilación

**Mecanismo.** BP08 guarda el blob del driver en disco (`vk_pipeline_cache.cpp:371-400`) con tope de 1 GB (`:491-509`). El shader cache se guarda aparte (`cache_storage.cpp:103-110`). No es una asignación de VRAM del port.

**Por qué no es el problema de 4 GB.** El archivo puede ser grande, pero no compite directamente con las texturas en el heap device-local. El coste es compilación y stutter; borrar la caché empeora el arranque, no libera VRAM de forma significativa.

**Confianza.** Alta en que es disco; baja en el impacto interno del driver (no medido).

**Esfuerzo.** Bajo: comparar caché fría/caliente y tamaño del `.vkc`; no borrar en el baseline.

**Cómo confirmar.** Tiempo de compilación, número de pipelines, tamaño del archivo y pico de VRAM en ambos arranques. No usar `ISS_PIPELINE_BINARIES=1` en NVIDIA 595.

### 2.10 Heap warm, staging y frames en vuelo

**Mecanismo.** `TLG_HEAP_WARM=1` sube heaps ≥256 MB en slices de 16 MB desde el primer submit (`vk_rasterizer.cpp:1931-1964`, `:2006-2020`). El staging retiene hasta 512 MB host (`vk_staging_buffer_pool.cpp:24-56`). El stream buffer de 128 MB es host por defecto, pero `TLG_STREAM_MEMORY=device` lo pone en VRAM (`advanced_table.py:107-113`).

**Por qué importa.** El warm-up adelanta residencia y puede disparar el pico durante la carga; el staging no es VRAM en discreta, pero compite por ancho de banda y RAM. Los frames en vuelo determinan cuándo se recicla memoria.

**Confianza.** Media. Es un factor de forma del pico, no la causa estructural.

**Esfuerzo.** Bajo: A/B de `ISS_HEAP_WARM=0` y `TLG_STREAM_MEMORY=cached` vs `device`.

**Cómo confirmar.** Tiempo hasta el primer frame, pico de VRAM en los primeros 60 s y freeze del primer bind.

---

## 3. Matriz A/B

### 3.1 Protocolo común

- **Una variable por fila.** Nada de cambiar presupuesto y resolución a la vez.
- **Misma escena y mismo save.** Ruta fija: menú → intro → tutorial → primer exterior; y una carga separada del save “huida del sheriff” porque el OOM del fork ocurre al cargar.
- **Tres repeticiones por celda** (mínimo), alternando A/B/A para reducir deriva térmica y de caché. Si no se pueden hacer tres, registrar la limitación.
- **Duración:** 15 min de juego real + 3 min de carga; el Gate G0 pide 15-30 min.
- **Métricas:** pico de VRAM (nvidia-smi cada 250 ms y muestreo interno de 500 ms), VRAM libre mínima, RAM privada, page faults, FPS medio, p50/p95/p99 de frametime, tiempo hasta el primer frame, OOM/DeviceLost, capturas en puntos fijos y artefactos visuales.
- **Criterio de éxito:** 30 min sin OOM/DeviceLost, pico estable por debajo del presupuesto efectivo, sin corrupción visual, y p95 no peor que el baseline en más de un margen acordado (p. ej. 10%).
- **Registro:** log completo, `result.json`, CSV, flags exactos, hash del ejecutable, versión de driver, estado frío/caliente de caché y resolución.

### 3.2 Matriz

| ID | Hipótesis | Variable | Valor | Resto fijo | Escena | Métricas clave | Criterio |
|---|---|---|---|---|---|---|---|
| E0 | Baseline reproducible | `vram_budget_mb` | 3500 (default) | nativo, MSAA on, flags default | Ruta completa + carga | pico, p95, OOM, artefactos | Referencia |
| E1 | El presupuesto es el limitante | `vram_budget_mb` | 2300 | igual que E0 | Ruta completa + carga | pico, p95, OOM, texturas | Sin OOM y p95 ≤ E0+10% |
| E2 | Aún más margen ayuda | `vram_budget_mb` | 2000 | igual que E0 | Ruta completa + carga | pico, p95, OOM | Sin OOM; comparar con E1 |
| E3 | El forzado 3500 es el problema | `ISS_GC_BUDGET_MB` | 0 (auto) | igual que E0 | Ruta completa + carga | umbrales del log, pico, p95 | Sin OOM; explicar trigger=0 |
| E4 | Control negativo | `vram_budget_mb` | 5600 | igual que E0 | Solo carga, 3 min | OOM esperado | Reproduce el fallo |
| E5 | La resolución interna pesa | `render_resolution` | 720p | presupuesto de E1 | Ruta completa | pico, p95, calidad | Baja el pico sin empeorar p95 |
| E6 | MSAA pesa | `ISS_NO_MSAA` | 1 | presupuesto de E1 | Ruta completa | pico, p95, calidad | Baja el pico sin artefactos |
| E7 | Las descargas del GC cuestan | `ISS_GC_BATCH_DOWNLOAD` | 1 | presupuesto de E1 | Carga + transición de zona | esperas, p95, RAM host | Menos esperas sin corrupción |
| E8 | Los readbacks precisos cuestan | `stale_readback` (tlg.ini) | 0 | presupuesto de E1 | Ruta completa | esperas, p95, artefactos | Medir coste de coherencia |
| E9 | La cola de readback es grande | `ISS_READBACK_QUEUE_MB/ENTRIES` | 32/32 | presupuesto de E1 | Carga + transición | esperas, RAM host, p95 | Menos RAM host sin más stalls |
| E10 | El warm-up adelanta el pico | `ISS_HEAP_WARM` | 0 | presupuesto de E1 | Carga + primeros 60 s | tiempo al primer frame, pico | Menos pico sin freeze excesivo |
| E11 | La caché de pipeline no es VRAM | `.vkc` frío vs caliente | mover/renombrar | presupuesto de E1 | Arranque + 5 min | compilaciones, p95, pico | Sin cambio de pico |
| E12 | La caché host externa es aplicable | prototipo externo | solo tras E0-E4 | — | Carga + 5 min | pico, RAM, DeviceLost | No se ejecuta en el port sin instrumentación |

### 3.3 Notas de precedencia (importante para que el A/B sea válido)

- **`ISS_GC_BUDGET_MB` sí llega al núcleo** porque `run.py` no define `TLG_GC_BUDGET_MB`; el probe lo copia. El launcher lo escribe desde `vram_budget_mb`, pero un `ISS_GC_BUDGET_MB` en el entorno del proceso tiene prioridad (`infamous_run.py:689`). Si se lanza `run.py` directo, hay que exportarlo; si no, no hay presupuesto forzado y el GC usa `trigger=0`.
- **`ISS_STALE_READBACK`, `ISS_DEFERRED_READBACK` y `ISS_ASYNC_SHADERS` pueden ser sobrescritos por `tlg.ini`.** `run.py:121-125` llama `graphics_options.prepare()` y luego `env.update(graphics_options.environment(graphics))`, que define `TLG_DEFERRED_READBACK`, `TLG_STALE_READBACK` y `TLG_ASYNC_SHADERS` (`graphics_options.py:20`, `:109-112`). Como el probe no sobrescribe un `TLG_*` existente, el valor de `tlg.ini` gana. Para estos tres, el experimento debe editar `tlg.ini` (o usar el menú del juego), no solo exportar `ISS_*`. **El default de `tlg.ini` para `stale_readback` es `1`**, así que el baseline puede estar leyendo un frame tarde aunque la tabla del launcher diga “off”; el log lo confirma con `; CPU reads use the previous frame's copy`.
- **`ISS_NO_MSAA`, `ISS_GC_BATCH_DOWNLOAD`, `ISS_HEAP_WARM` y `ISS_READBACK_QUEUE_*` no están en `graphics_options.environment()`**, así que llegan por el alias del probe. Aun así, verificar en el log de arranque que el valor efectivo es el esperado.
- **La resolución no es una variable de entorno del launcher.** `render_resolution` vive en `tlg.ini` y se aplica como patch del juego (`graphics_options.py:91-101`; `run.py:186-193`). `TLG_RENDER_RES` es otra ruta de compatibilidad, no la del menú.
- **El presupuesto del launcher no cambia solo por detectar 4 GB.** Hay que fijarlo en `settings.json` o en el entorno. El aviso de `gfxlogic.py` no se dispara con 3500/4096.

### 3.4 Qué NO mezclar en la misma serie

- No comparar una sesión con caché de shaders fría contra otra caliente y atribuir la diferencia a VRAM.
- No cambiar `ISS_HEAP_WARM` y el presupuesto a la vez: ambos mueven el pico temprano.
- No usar `ISS_PIPELINE_BINARIES=1` en NVIDIA 595.
- No usar 720p como “prueba de que el problema es VRAM”: si el p95 no mejora, el límite puede ser CPU.
- No ejecutar la caché host externa en el port nativo antes de tener contadores por caché y una prueba de coherencia.

---

## 4. Riesgos por cambio y qué no tocar

### 4.1 Riesgos

| Cambio | Riesgo | Mitigación |
|---|---|---|
| Bajar presupuesto a 2300/2000 | Más expulsiones, texturas faltantes o corruptas, thrash de GC, p95 peor | Capturas en puntos fijos; comparar artefactos; no bajar más de lo necesario |
| `ISS_GC_BUDGET_MB=0` | `trigger=0` hace GC en cada submit; puede aumentar CPU y expulsar de más | Medir p95 y evictions; no usarlo como “sin límite” |
| `ISS_GC_BATCH_DOWNLOAD=1` | Hasta 256 MB de staging host por lote; escritura diferida sobre páginas que el juego puede reutilizar | Verificar coherencia y texturas; probar en carga y transición de zona |
| `ISS_STALE_READBACK=0/1` | Lecturas de CPU un frame tarde; lógica del juego puede leer datos viejos | Usar 0 para el baseline de corrección; 1 solo como experimento de rendimiento |
| `ISS_NO_MSAA=1` | Cambia calidad y posiblemente el comportamiento de shaders/RT | A/B visual; no mezclar con resolución |
| `render_resolution=720p` | No arregla la residencia de buffers; puede no mejorar FPS si el límite es CPU | Medir p95 y VRAM por separado; no declarar “fix” |
| `ISS_HEAP_WARM=0` | El primer bind puede congelarse segundos | Medir tiempo al primer frame y freeze; no usar en el baseline de fluidez |
| Cola de readback más pequeña | Más esperas y stalls | Medir esperas y p95; la cola no reduce VRAM |
| Caché host / residencia por subrango | Coherencia, aliases, páginas CPU sucias, DMA, DeviceLost | Diseño con tests; flag apagado por defecto; no merge directo |
| Quitar el fallback over-budget | El juego crashea antes y se pierde la reproducibilidad | Solo como diagnóstico, nunca en release |
| Borrar la caché de pipeline | Más stutter de compilación; no libera VRAM de forma relevante | Mantener caliente para medir VRAM; comparar frío/caliente aparte |

### 4.2 Qué no tocar

- **No desactivar el GC ni subir el presupuesto** para “ver si entra”: en 4 GB el fallo es el comportamiento esperado, no un bug de configuración.
- **No expulsar imágenes tiled GPU-written sin implementar su descarga.** Se pierden datos que el juego aún necesita; el código ya las protege a propósito (`texture_cache.cpp:1920-1927`).
- **No cambiar los buffers sparse a memoria host “porque hay RAM”.** En el fork externo, la máscara del probe era `memory_type_bits=0x2`: solo el tipo device-local era compatible; los tipos de RAM no. El fallback host no era aplicable a esos buffers.
- **No reducir los gránulos de 16 MB o los chunks de 256 MB a ciegas.** El comentario de `buffer_cache.cpp:766-810` explica que los chunks evitan miles de asignaciones y el límite de allocations de Windows; un cambio ingenuo puede empeorar el arranque o el driver.
- **No tocar la tabla de páginas DMA mientras se experimenta con caché host.** El fork documenta que la caché host es incompatible con la ruta DMA; mezclarlas produce corrupción/DeviceLost.
- **No confundir “residente” con “leído”.** El rango declarado por un descriptor no prueba que el shader lea todos los bytes; la solución debe basarse en accesos reales, no solo en el tamaño del descriptor.
- **No copiar el código del fork externo sin atribución GPL-2.0 y sin validación.** Es otro proyecto, otra base y otra ruta de memoria. Cualquier reutilización debe mantener `LICENSE` y atribuir a HayatoG, shadPS4, bloodborne_pc, gow3_pc y los terceros correspondientes.

---

## 5. Instrumentación faltante

La instrumentación actual no permite responder “¿quién llenó los 4 GB?”. Falta:

| Señal | Estado actual | Qué añadir | Dónde |
|---|---|---|---|
| Uso por caché | Solo `GetDeviceMemoryUsage()` total cada 500 ms (`tlg_perf_stats.cpp:115`) | Texturas (guest y real), buffers residentes, staging device-local, pipeline/driver, “otros” | `texture_cache`, `buffer_cache`, `vk_instance` |
| Imágenes | Tamaño guest para GC; `size_bytes` real existe pero no se agrega (`image.cpp:199-212`) | Suma de `alloc_info.size`, por formato/mips/tiled/GpuModified; pico y actual | `texture_cache` |
| Buffers | Sin contador de bytes residentes | `resident_bytes`, `resident_ranges`, allocations, chunk waste, dedicadas, fallos y reintentos | `buffer_cache.cpp:677-846` |
| GC | Solo aviso cada 5 s con evictions/downloads (`texture_cache.cpp:1982-2000`) | Por pasada: candidatas, saltadas por edad/tiled, límite 256, bytes liberados guest vs real, tiempo | `texture_cache.cpp:1895-2000` |
| Fallbacks | Warning con tamaño solo en buffer; imagen sin tamaño (`image.cpp:199-208`) | Tipo, tamaño, heap, uso/budget antes, resultado | `image.cpp`, `buffer.cpp` |
| OOM/DeviceLost | ASSERT genérico | Capturar la asignación exacta, el caller y el estado de las cachés | `buffer_cache.cpp:712-726` |
| Readbacks | `TLG_READBACK_STATS` opcional, apagado | Cola, bytes, esperas, stale hits, fuente, tiempo por descarga | `texture_cache`, `buffer_cache` |
| Presupuesto | Log inicial escueto | `heapBudget/heapUsage` por heap, reserva, forzado, umbrales finales, `headroom` | `vk_instance.cpp:953-994`; `texture_cache.cpp:81-101` |
| Config efectiva | Dispersa entre launcher, `tlg.ini` y entorno | Una línea al inicio: budget, resolución, MSAA, readbacks, heap warm, stream, driver | `run.py` / `infamous_run.py` |
| Pico | Muestreo 500 ms que puede perder el pico | High-water mark interno y muestreo de 250 ms durante la carga | `tlg_perf_stats.cpp` |
| Staging | `PublishStats()` no hace nada si el viewer está apagado (`vk_staging_buffer_pool.cpp:196-204`) | Contadores siempre activos, no solo con devtools | `vk_staging_buffer_pool.cpp` |

**Prioridad de instrumentación:** (1) bytes reales de imágenes y buffers residentes; (2) presupuesto/umbrales efectivos al inicio; (3) contadores de GC y fallbacks; (4) readbacks. Sin (1) y (2), cualquier A/B de presupuesto es difícil de interpretar.

---

## 6. Referencia externa: la caché host del fork de shadPS4

Esta sección compara conceptualmente con un proyecto **externo** al repositorio, no con el port nativo. Ruta local: `C:\Users\luisd\Documents\Codex\2026-09-29\trabajemos-en-esto-quiero-crear-una\outputs\Emulador-SecondSon` (base `94e2177`, rama local). No se copió código ni se modificó nada.

### 6.1 Qué se midió

| Condición | Resultado |
|---|---|
| Baseline oficial, mismo portátil | Falla una asignación sparse de **566.689.792 bytes** tras **2.624.782.336 bytes** sparse residentes; uso Vulkan **3.702.108.160** vs presupuesto **3.531.289.396** |
| Primeras cachés host (v10-v12) | Bajan el uso de GPU a ~1.0-2.2 GB, pero aparecen aserciones por aliases y `DeviceLost` |
| v24, crecimiento canónico + `SHADPS4_HOST_CACHE_LIMIT_MIB=3200` | **1322.5 s** de proceso, código 0, sin OOM/DeviceLost; pico GPU **1651 MiB**; RAM privada **8033.86 MiB**; crecimiento canónico real observado |
| v24, segundo arranque | 892 pipelines precargados desde la caché persistente; ~14 FPS en una cámara fija del exterior; no prueba jugabilidad completa |

Fuentes: `second-son/EXPERIMENTO-MEMORIA.md`, `second-son/RESULTADOS.md`, `second-son/SECOND_SON.md`, `src/video_core/buffer_cache/host_cache_plan.h`.

### 6.2 Qué demuestra y qué no

**Demuestra:**

- El fallo del baseline está en la **residencia sparse de buffers grandes**, no solo en la caché de texturas. La petición grande es un descriptor de lectura de ~2.4 GB; el acumulado sparse ya era ~2.44 GiB y la asignación que falla es de 566.689.792 bytes.
- Es posible mantener el uso de GPU muy por debajo de 2 GB si los rangos grandes de solo lectura se respaldan en RAM host y se migran con cuidado.
- La diferencia entre “seguir” y “no seguir” no es solo el presupuesto de texturas: la ruta de buffers puede tumbar el juego aunque las texturas estén controladas.

**No demuestra:**

- Que funcione en el port nativo: es otro código, otra base (`94e2177` vs `fb7e452`) y otro runtime.
- Que sea correcto en general: hubo `DeviceLost`, aserciones por aliases y problemas de páginas CPU sucias; la v24 solo sobrevivió tras un crecimiento canónico y un límite explícito.
- Que dé 30 FPS: las cifras del fork son 12-16 FPS en escenas fijas, con RAM privada de ~8 GB.
- Que sea compatible con DMA: el propio fork documenta que la caché host no convive con la tabla de páginas DMA.
- Que se pueda mergear sin más: el `host_cache_plan.h` es una pieza de planificación (crecimiento, prioridad CPU/host/sparse, copias), no un parche completo.

### 6.3 Lectura para el port nativo

La lección aplicable es de diseño, no de código:

1. **La residencia debe ser por subrango o por página, no por descriptor completo.** El tamaño declarado no es el tamaño leído.
2. **La fuente de verdad debe ser explícita:** páginas CPU sucias > contenido host canónico > sombra sparse. El plan externo ya modela esa prioridad (`PlanCopies` en `host_cache_plan.h`).
3. **El crecimiento debe ser canónico y con límite.** Reemplazar hosts solapados por un buffer único, con tope de RAM y pico temporal contabilizado, evita el alias roto.
4. **La coherencia es el coste real.** Sin pruebas de escrituras CPU/GPU, aliases y DMA, la caché host es una fuente de corrupción, no una optimización.
5. **No es la primera palanca.** Antes de portar nada, el A/B de presupuesto y la instrumentación deben decir si el port nativo se cae por texturas o por buffers. El fork ya demostró que, en su base, era por buffers.

---

## 7. Conclusiones y secuencia recomendada

1. **No hay evidencia para declarar 4 GB viables hoy.** El presupuesto por defecto del launcher (3500 MB) está por encima de lo que él mismo recomienda para 4 GB (2300 MB), y el camino de buffers puede pedir ~2.4 GB en una sola operación.
2. **La hipótesis más fuerte es la residencia sparse de buffers.** El GC de texturas no puede liberarla, el fallback over-budget la esconde y el fork externo la reprodujo con números concretos.
3. **La segunda es el presupuesto y el colapso de umbrales.** En 4 GB, `pressure` y `critical` se juntan; el GC no tiene una banda gradual para reaccionar.
4. **La tercera es la cadencia del GC.** Una pasada por submit, con edad mínima y límites de 10/20/40, no alcanza para una carga de guardado o un streaming de geometría.
5. **La caché host es una referencia, no un plan de ejecución.** Antes de portarla: contadores por caché, prueba de coherencia, límite de RAM y A/B de presupuesto. Si el A/B muestra que 2300 MB elimina el OOM, el problema es texturas; si el OOM persiste, es buffers y la caché host pasa a ser una línea de investigación justificada.

**Secuencia propuesta para el Gate G0:**

- **Paso 0 (sin cambios de comportamiento):** instrumentar presupuesto efectivo, bytes reales por caché, GC, fallbacks y readbacks. Repetir el baseline 3 veces.
- **Paso 1 (bajo riesgo):** E1/E2/E3 (2300, 2000, 0). Si 2300 elimina el OOM y mantiene la calidad, ya hay una configuración mínima defendible para 4 GB.
- **Paso 2 (medio):** E7/E8/E9 (batch download, stale readback, cola). Miden el coste de sincronización, no el pico.
- **Paso 3 (medio):** E5/E6/E10 (720p, no MSAA, heap warm). Separan render targets de residencia de buffers.
- **Paso 4 (alto):** solo si E1-E3 no bastan, diseñar residencia por subrango o caché host con flag apagado por defecto, pruebas de coherencia y límite de RAM.

**Criterio de decisión:** si con 2300 MB y sin MSAA el juego completa 30 min sin OOM/DeviceLost y sin corrupción, el port es viable en 4 GB con calidad reducida. Si el OOM aparece en la carga de guardado incluso con 2000 MB, el problema es la ruta de buffers y ninguna configuración de texturas lo va a resolver.

---

## Apéndice A. Variables y archivos citados

| Variable | Efecto | Dónde se lee |
|---|---|---|
| `vram_budget_mb` | Presupuesto del launcher; se escribe en `ISS_GC_BUDGET_MB` | `infamous_run.py:239`, `:689` |
| `ISS_GC_BUDGET_MB` / `TLG_GC_BUDGET_MB` | Presupuesto forzado del GC; 0 = automático | `texture_cache.cpp:1831-1852` |
| `ISS_NO_MSAA` / `TLG_NO_MSAA` | Desactiva multisampling | `backports_table.py:219-221` |
| `ISS_GC_BATCH_DOWNLOAD` | Agrupa descargas del GC en un wait | `texture_cache.h:531-534` |
| `ISS_STALE_READBACK` / `TLG_STALE_READBACK` | Lecturas CPU un frame tarde | `texture_cache.h:535-541` |
| `ISS_DEFERRED_READBACK` / `TLG_DEFERRED_READBACK` | Readbacks diferidos | `texture_cache.h:525-529` |
| `ISS_READBACK_QUEUE_MB` / `_ENTRIES` | Tamaño de la cola de readback | `readback_queue.h:16-20` |
| `ISS_HEAP_WARM` / `TLG_HEAP_WARM` | Subida temprana de heaps ≥256 MB | `vk_rasterizer.cpp:1941-1944` |
| `TLG_STREAM_MEMORY` | `cached` (host) o `device` (VRAM) para 128 MB | `advanced_table.py:107-113` |
| `TLG_STAGING_KEEP_MB` | Bloques de staging retenidos (512 MB por defecto) | `vk_staging_buffer_pool.cpp:24-56` |
| `TLG_DRIVER_CACHE_MAX_MB` | Tope del blob de pipeline (1 GB) | `vk_pipeline_cache.cpp:491-509` |
| `render_resolution` (tlg.ini) | `native/480p/720p/1440p/1800p/4K` | `graphics_options.py:91-101` |

## Apéndice B. Referencias clave

- `port/launcher/infamous_run.py:221`, `:239`, `:689`
- `port/src/probe.c:829-865`
- `port/gpu/shadps4/video_core/texture_cache/texture_cache.cpp:53-93`, `:1653`, `:1696`, `:1823-1852`, `:1895-1927`, `:2056-2086`
- `port/gpu/shadps4/video_core/texture_cache/texture_cache.h:50-57`, `:525-541`
- `port/gpu/shadps4/video_core/texture_cache/image.cpp:182-213`, `:325-333`
- `port/gpu/shadps4/video_core/texture_cache/readback_queue.h:16-20`
- `port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:38-39`, `:61-123`, `:512-556`, `:677-846`, `:1477-1539`
- `port/gpu/shadps4/video_core/buffer_cache/buffer_cache.h:49-64`
- `port/gpu/shadps4/video_core/buffer_cache/buffer.cpp:114-149`
- `port/gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:953-994`, `:1053-1079`
- `port/gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:24-56`, `:162-204`
- `port/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:371-400`, `:491-509`, `:674-692`
- `port/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1931-1964`, `:1998-2020`
- `port/gpu/shim/tlg_perf_stats.cpp:110-123`
- `port/gpu/shim/tlg_overlay.cpp:634`
- `port/gpu/shim/core/devtools/widget/gpu_memory.h:5-12`
- `port/launcher/qt/gfxlogic.py:107-134`
- `port/launcher/backports_table.py:219-243`
- `port/launcher/advanced_table.py:107-121`
- `port/scripts/graphics_options.py:20`, `:91-112`
- `port/run.py:121-125`, `:186-193`
- `docs/ROADMAP.md:49`
- `docs/EXPERIMENTAL_FIXES.md:74`
- Fork externo: `.../Emulador-SecondSon/second-son/EXPERIMENTO-MEMORIA.md`, `RESULTADOS.md`, `SECOND_SON.md`; `.../src/video_core/buffer_cache/host_cache_plan.h`

---

**Limitaciones de este análisis:** no se ejecutó el port nativo ni se capturaron métricas propias; las líneas citadas son del árbol actual y pueden cambiar con el desarrollo; las cifras del fork externo corresponden a otra base y a otro runtime; la atribución de magnitud (cuánto de los 4 GB es cada caché) requiere la instrumentación del §5.
