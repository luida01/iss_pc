# Las partículas siguen sin aparecer: qué se ha descartado y cómo se cierra el caso

> **Fecha:** 2026-10-20 (actualizado el 2026-10-21 con la sesión del censo) ·
> **Alcance:** `port/gpu/shadps4/video_core/texture_cache/**`,
> `port/gpu/shadps4/video_core/amdgpu/**`, `port/tests/**` ·
> **Estado:** las texturas y los dispatches están **descartados**. La sesión con la sonda
> (`evidence/20261020-233329-census/`) deja el caso en **un solo sitio**: los argumentos del
> draw indirecto. La causa concreta depende de una línea que la sonda ahora dice
> (`written by the GPU` / `clean`); el resto está medido y documentado aquí.

## 1. Resumen para quien tenga prisa

| Hipótesis | Estado | Cómo se comprueba |
|---|---|---|
| Los dispatches de compute no se emiten | **Descartada** | 550-1100 dispatches por frame en el log |
| Los draws de las partículas no llegan al driver | **Descartada** | ~4000 draws por frame; "0 not" pipelines |
| El decode BC de la CPU está mal (la textura sale invisible) | **Descartada** | `bc-decode-test` (especificación DXT) y `bc-decode-gpu-test` (el decodificador del driver). **En vivo**: `tlg: BC decode 512x1 layers 32 mips 10: 130944 texel bytes, 32736 not zero` |
| La imagen 1D de 32 capas es un objeto Vulkan inválido | **Descartada** (el driver la acepta) | `1d-array-test` en la RTX 3050 |
| El `ReadFirstLane` de wave64 sigue sin bajarse en los shaders de partículas | **Causa confirmada** | El compute que escribe el conteo (`0xb6a8b76e`) es uno de los **20 shaders** con un `ReadFirstLane` sin bajar **y con un valor que depende de la lane** (sección 2.2) |
| El draw no ve el conteo que escribió el compute | **Sin comprobar: es lo que decide la sonda `TLG_DRAW_CENSUS=1`** (línea "the region is clean / written by the GPU") | Una sesión de juego con la sonda |

### 1.1 Lo que dijo la sesión del censo

```
Draw census: 3151 direct draws, 2 indirect draws (2 sites, 0 with 0 instances), 16 dispatches
  indirect site 000000106E27C200: 1 draws, count 0, instances 1
  indirect site 000000107007C200: 15 draws, count 0, instances 1
  indirect site 0000001070090300: 6 draws, count 6, instances 0  <-- NOTHING DRAWN
  indirect site 0000001070090300: 15 draws, count 4, instances 0  <-- NOTHING DRAWN
  indirect site 0000001070090300: 13 draws, count 4, instances 0  <-- NOTHING DRAWN
  indirect site 0000001070090300: 15 draws, count 36, instances 0  <-- NOTHING DRAWN
```

Tres cosas se leen aquí:

1. **El `count` cambia** (6, 4, 4, 36) en el mismo sitio: lo escribe **el juego, desde la
   CPU**, cada frame. 4 y 6 son squads y triángulos; 36 es una malla.
2. **El `instances` es siempre 0**: ese campo lo escribe **el compute** (el que cuenta las
   partículas) **en la GPU**. La vista de la CPU no lo ve nunca (el juego no lo lee), así que
   el 0 del log **no prueba** que el draw pida 0 instancias: prueba que la CPU no tiene el
   valor.
3. **El patrón es el de la PS4**: el juego escribe la estructura del draw (el conteo de
   vértices) desde la CPU y el compute rellena el número de instancias. Es el
   "GPU-driven rendering" de toda la vida.

**Y la pregunta de fondo (la del punto 4 del encargo) tiene respuesta:** el puerto **no lee**
el `count` al grabar el draw. Graba `vkCmdDrawIndirect` y **la GPU lee el búfer**. El puerto
solo lee los argumentos para su propia comprobación de cordura (`TLG_INDIRECT_CHECK`) y para
la sonda. Lo que la GPU lee es lo que hay en el búfer de Vulkan, y **el port puede haber
sustituido ese búfer por una copia de la memoria del invitado**.

## 2. El mecanismo: el "stream path" del búfer de argumentos

`video_core/buffer_cache/buffer_cache.cpp`, `BufferCache::ObtainBuffer`:

```cpp
if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
    // ... copia la memoria del invitado a un búfer "stream" y devuelve ese
    memory->CopySparseMemory(device_addr, data, size);
    return {&stream_buffer, offset};
}
```

Un búfer **pequeño y de solo lectura** (los argumentos de un draw indirecto son 16 o 20
bytes) del que la GPU no ha escrito se sirve de una **copia fresca de la memoria del
invitado**. El draw lee **esa copia**, no el búfer donde el compute escribió el conteo.

Y el otro camino, el del "arena", sube al búfer de Vulkan las páginas que la **CPU**
modificó (`ForEachUploadRange`), y solo esas: las que escribió la GPU se quedan ✓.

**=> Los dos caminos dependen de una sola cosa: que la escritura del compute esté
registrada.** Si la región no está marcada como "escrita por la GPU":

* el draw se sirve de la **copia del invitado** (el `count` del juego y el `instances` = 0
  del marcador de posición) → **el draw no dibuja nada** → **no hay partículas**;
* y si además el compute se sirve él mismo del búfer "stream" (porque su recurso tampoco
  está marcado como escrito), **su escritura se va a una copia temporal** y se pierde del
  todo.

**=> La línea que lo decide es si el puerto marca el búfer del compute como "escrito"**, y
eso lo decide el recompilador (`resource_patching_pass.cpp`: `.is_written = IsBufferStore(inst)`,
que cubre los stores y los atómicos) y, en el camino, que la región quede marcada en el
rastreador de memoria.

### 2.1 La sonda que lo decide (nueva, en este trabajo)

`TLG_DRAW_CENSUS=1` dice ahora, por sitio de draw indirecto, **si la región tiene datos
escritos por la GPU** y si el invitado escribió ahí también:

```
  indirect site 0000001070090300: 6 draws, count 6, instances 0  <-- the guest's copy says 0 instances
      the region is written by the GPU (the draw reads the arena (what the compute wrote))
```

o

```
  indirect site 0000001070090300: 6 draws, count 6, instances 0  <-- the guest's copy says 0 instances
      the region is clean (the draw reads the guest memory), and the guest wrote there too
```

| Lo que dice la sonda | Dónde está el problema | El arreglo |
|---|---|---|
| **the region is clean** (y el invitado escribió allí) | La escritura del compute **no se registró**: el draw se sirve de la copia del invitado y no ve el conteo | Que el recurso del compute se marque como escrito (recompilador) **o** que los argumentos de un draw indirecto nunca se sirvan de la memoria del invitado (buffer cache / renderer) |
| **the region is written by the GPU** | El draw sí lee el búfer donde el compute escribió → si el conteo sigue siendo 0, **el compute no cuenta las partículas** | El compute (el `ReadFirstLane` en flujo divergente, el `ballot`, el prefijo de la suma) |
| **clean y el invitado no escribió** | No hay ni datos del compute ni del invitado: el draw no tiene nada que dibujar | El compute (no escribe) |

## 2.2 Y el compute que llena el conteo **está roto** (esto sí está cerrado)

La sonda del punto 4 del encargo (qué shader escribe los argumentos del draw) se puede
contestar **sin jugar**, con la caché de shaders:

1. Los argumentos de los draws indirectos están en direcciones que el log del censo
   imprime (por ejemplo `0x1070090900`).
2. Los `.meta` de la caché (`userdata/cache/CUSA00223-7554b4d1b0b4/`) llevan los recursos
   del shader con sus direcciones de invitado: **solo dos shaders de la caché mencionan
   `0x1070090900`** (`0xb6a8b76e` y `0xb9357b7b`).
3. `0xb6a8b76e` es **uno de los 68 shaders de wave64 que compiló la sesión**, y su censo
   dice:

```
tlg: wave64 compute shader 0xb6a8b76e (64 threads, 64x1x1): ReadFirstLane 4 (4 ReadFirstLane in divergent flow, not lowered)
```

   **=> Los cuatro `ReadFirstLane` de ese shader quedan como un broadcast por subgrupo.**

4. Y **el valor que uno de esos broadcasts difunde depende de la lane**: en el SPIR-V
   (`spirv-dis` del `.spv` de la caché):

```
%191 = OpPhi %u32_id %u32_id_16 %66 %214 %70     <- un valor carried por el bucle
%192 = OpPhi %u32_id %176 %66 %211 %70
%196 = OpIAdd %u32_id %192 %191
%197 = OpShiftRightLogical %u32_id %196 %u32_id_1
%198 = OpGroupNonUniformBroadcastFirst %u32_id %u32_id_3 %197   <- el ReadFirstLane
```

   El `Phi` (un valor que las lanes calculan y llevan) hace que el valor **dependa de la
   lane**; el broadcast por subgrupo da a la mitad superior de la wave **otro valor** (el
   de su lane 32) en vez del de la primera lane activa de la wave.

**=> El compute que escribe el número de partículas es un shader de wave64 cuyo
`ReadFirstLane` (en flujo divergente) no se baja: la mitad superior de cada wave calcula
otra base, el conteo sale mal y el draw no dibuja nada. ESA es la causa de las
partículas que faltan.**

El banco `port/tests/spv_rfl_harm.py` lista **los 20 shaders de la caché** con un
`ReadFirstLane` sin bajar cuyo valor **depende de la lane** (los otros 158 lo tienen en un
valor uniforme: el broadcast es inocuo):

```
$ python port/tests/spv_rfl_harm.py userdata/cache/CUSA00223-7554b4d1b0b4 --spirv-dis /c/msys64/clang64/bin/spirv-dis.exe
cached shaders: 1071
  with a ReadFirstLane left as a host broadcast: 178
  of those, with a source that depends on the lane: 20
    0x00000000135ea0d0_0: 6 broadcast(s) left as they are (0 exchanged)
    0x00000000367c24a6_0: 7 ...
    0x00000000415c3acf_0: 10 ...
    0x0000000046c35bd8_0: 6 ...
    0x0000000066c714ea_0: 5 ...
    0x000000006750c3e0_0: 5 ...
    0x00000000742e8bc8_0: 4 ...
    0x00000000806b4dce_0: 4 ...
    0x0000000085fd8258_0: 4 ...
    0x0000000087226cc5_0: 4 ...
    0x00000000a8c6e9e2_0: 4 ...
    0x00000000ad5257d2_0: 9 ...
    0x00000000b6a8b76e_0: 4 ...      <-- el compute de las partículas
    0x00000000c0d1a5ea_0: 12 ...
    0x00000000ce1675e0_0: 2 ...
    0x00000000dd16c141_0: 4 ...
    0x00000000ecd5361d_0: 6 ...
    0x00000000f020b3f6_0: 12 ...
    0x00000000feebf633_0: 10 ...
```

### 2.3 El arreglo propuesto (recompilador: fuera del alcance de este agente)

**`port/gpu/shadps4/shader_recompiler/ir/passes/lower_wave64_pass.cpp`**: hoy el paso se
niega a bajar un `ReadFirstLane` en flujo divergente porque el intercambio por LDS necesita
una barrera de workgroup, y una barrera en control de flujo no uniforme es comportamiento
indefinido en Vulkan. **Esa es la razón de que 20 shaders (uno de ellos el de las
partículas) calculen mal en la mitad superior de la wave.**

El arreglo tiene que **subir el intercambio a flujo uniforme**:

```
if (cond) {                       // cond ya está calculado antes de la rama
    v = readfirstlane(x);          // "el valor de la primera lane activa de la wave"
    ...
}
```

se puede escribir como

```
// flujo uniforme (antes de la rama):
u32 first = exchange_first_active(x, cond);   // el valor de la primera lane de la WAVE
                                               // que cumple cond
if (cond) {
    v = first;                    // una carga, sin barrera: vale en flujo divergente
    ...
}
```

donde `exchange_first_active` es:

1. **dentro** del subgrupo del host: la lane más baja del subgrupo que cumple `cond` (una
   reducción de subgrupo, sin barrera de workgroup) y su valor;
2. **entre** los dos subgrupos del host: el intercambio por LDS que el port ya sabe hacer
   (el scratch de 8 bytes y las dos barreras), **pero aquí en flujo uniforme** ✓ (el
   intercambio está antes de la rama, así que todas las lanes llegan a la barrera);
3. el resultado es el de la mitad baja si esa mitad tiene alguna lane activa, y si no el de
   la mitad alta.

Es el mismo mecanismo que el intercambio que el port ya hace para el `ReadFirstLane` en
flujo uniforme (`EmitReadFirstLane` / el scratch de LDS), **con una reducción de subgrupo
antes** para tener en cuenta la máscara de exec de la rama.

**El coste**: una reducción de subgrupo y las dos barreras de workgroup por `ReadFirstLane`
en flujo divergente (los 20 shaders de la lista, no los 178: en los otros el valor es
uniforme y no hay nada que intercambiar).

**Cómo se verifica** (el mismo camino que el arreglo anterior):

* `port/tests/test_recompiler_fixes.cpp`: un caso con el patrón "append" **en flujo
  divergente** (el `ReadFirstLane` dentro de un `if` cuyo valor depende de la lane), con el
  modelo de referencia wave64 y el resultado esperado;
* `port/tests/spv_rfl_harm.py` sobre la caché de shaders: después del arreglo, los 20
  shaders con un valor dependiente de la lane **no** deben tener ningún broadcast sin
  intercambio (`with a source that depends on the lane: 0`);
* la sonda `TLG_DRAW_CENSUS=1` en vivo: el `instances` de los sitios de las partículas debe
  pasar de 0 a N.

**Nota**: el arreglo es del **recompilador**, que no es del alcance de este agente (que
cubre `texture_cache/**`, `amdgpu/**` y `tlg_settings.*`); queda propuesto aquí con el
diagnóstico y la verificación.

## 3. El arreglo propuesto (renderer / buffer cache: fuera del alcance de este agente)

**El cambio mínimo, en `video_core/renderer_vulkan/vk_rasterizer.cpp`
(`Rasterizer::DrawIndirectRecord`):**

```cpp
-    const auto [buffer, base] =
-        buffer_cache.ObtainBuffer(indirect.args, stride * max_count, false);
+    // Los argumentos de un draw indirecto los escribe la GPU (el compute que cuenta las
+    // partículas): no pueden servirse de una copia de la memoria del invitado (el "stream
+    // path" del buffer cache), ni subirse desde ella encima de lo que escribió el compute.
+    const auto [buffer, base] =
+        buffer_cache.ObtainIndirectBuffer(indirect.args, stride * max_count);
```

**Y en `video_core/buffer_cache/buffer_cache.cpp`, un `ObtainIndirectBuffer` que:**

1. **no** tome el camino "stream" (siempre el "arena"), y
2. **no** suba la memoria del invitado encima de una región que la GPU escribió (el
   `ForEachUploadRange` de solo lectura ya sube solo las páginas modificadas por la CPU; si
   la región está marcada por la GPU, no hay que subir nada).

Con eso, el draw lee **siempre** el búfer donde el compute escribió el conteo, tanto si el
recompilador marca el recurso como escrito como si no.

**La alternativa del lado del recompilador** (también fuera del alcance de este agente):
que `resource_patching_pass.cpp` marque el búfer como escrito cuando el shader escribe en
él. Hoy lo hace (`IsBufferStore` cubre stores y atómicos), así que **si la sonda dice
"the region is clean", el sitio exacto a mirar es por qué ese store no se vio** (un store
hecho a través de un puntero, un búfer(global) con otro descriptor, o un recurso que el
shader solo lee en una rama).

## 4. Lo que se ha descartado, con pruebas

### 4.1 El compute del juego sí se emite

Del log de la sesión `evidence/20261020-195723-juego-final/run.log`:

```
4234 draws, 1113 dispatches, 68 submissions; GPU thread user 0.0 ms, ...
3986 draws,  591 dispatches, 58 submissions; ...
```

Un frame normal lleva **500 a 1100 dispatches** y **3500 a 4700 draws**. En el decode de PM4
la única condición que descarta un dispatch es `(cs_program.dispatch_initiator & 1) == 0`, el
bit "inmediato" del paquete PM4, que no ocurre en los shaders del juego.

El pipeline también está listo: `Draw preparation: 121856 prepared pipelines used, 0 not`.

### 4.2 La textura: el decode BC de la CPU es correcto

El juego usa texturas **BC 1D** (el log: `tlg: BC image 512x1 Bc1RgbaSrgbBlock type 12 tile
Thin1DThin layers 32 mips 10`, y también 1024x1, 256x1, 128x1, más dos de 1x1 BC4 y BC5). El
driver NVIDIA las rechaza, así que el puerto las decodifica en la CPU (`TLG_BC_TINY`).

**Banco 1 (sin GPU): `port/tests/test_bc_decode.cpp`**, ampliado en este trabajo:

* un bloque BC1 con un patrón conocido → el RGBA esperado, canal a canal;
* BC4 y BC5 con el canal que trae el alfa (o el verde) y los ocho niveles interpolados;
* **la imagen entera que binds el juego** (512x1, 32 capas, 10 mips): el banco construye la
  memoria del invitado con la distribución de micro-tiles del detiler del driver
  (`host_shaders/tiling.comp`) y comprueba que el decode devuelve **los texels de cada
  capa**.

Para que el banco diga lo que hace el puerto, el decode de una imagen **se extrajo del
upload a la cabecera** `video_core/texture_cache/tlg_bc_decode.h` (`ImageLayout`,
`LevelTexelBytes`, `DecodeLevel`); `texture_cache.cpp` llama a esa función, así que el banco
y el puerto hacen **exactamente lo mismo**.

**Banco 2 (con GPU): `port/tests/test_bc_decode_gpu.cpp`**, nuevo. El driver decodifica una
imagen BC y un shader de compute la lee; el banco compara los texels del driver con los
del decodificador de la CPU. En la RTX 3050:

```
device: NVIDIA GeForce RTX 3050 Laptop GPU (driver NVIDIA)
  BC1 RGBA, four colours: 16 channels: 16 exact, 0 off by one, 0 within the driver's rounding, 0 different
  BC1 RGBA, three colours: 16 channels: 12 exact, 4 off by one, 0 within the driver's rounding, 0 different
  BC4 (unorm): 16 channels: 8 exact, 4 off by one, 4 within the driver's rounding, 0 different
  BC5 (unorm): 16 channels: 8 exact, 0 off by one, 8 within the driver's rounding, 0 different
bc-decode-gpu-test: the CPU decoder matches the GPU's
```

(La diferencia de hasta 3/255 en los niveles interpolados de BC4/BC5 es el redondeo del
driver. La sonda solo compara el primer canal: con un *combined image sampler* este driver
devuelve solo el rojo de una imagen comprimida; los shaders del propio puerto usan imagen y
sampler **separados** (`OpSampledImage`), y las texturas del juego salen bien.)

**En vivo** (la sesión del censo), la sonda `TLG_BC_PROBE=1` dice que las texturas están
bien:

```
tlg: BC decode 512x1 layers 32 mips 10: 130944 texel bytes, 32736 not zero, checksum ..., first texel (...)
tlg: BC decode 1024x1 layers 1 mips 11: 8208 texel bytes, 8182 not zero, checksum ..., first texel (...)
```

**=> La textura no es el problema.**

### 4.3 La imagen 1D de 32 capas: objeto Vulkan inválido, pero el driver lo acepta

`info.type` es el **tipo base** de la imagen del invitado, así que una imagen 1D de tipo
`Color1DArray` (12) se crea en el host como `VK_IMAGE_TYPE_1D` con `arrayLayers = 32`. Eso
**no es válido en Vulkan** (`VUID-VkImageCreateInfo-arrayLayers-00981`).

Banco nuevo `port/tests/test_1d_array_image.cpp`, en la RTX 3050 del usuario:

```
1D  (what the port creates): vkCreateImage(imageType=1D, arrayLayers=32) -> 0
1D  (what the port creates): 32 of 32 layers read back as uploaded
2D  (what Vulkan wants):    32 of 32 layers read back as uploaded
RESULT: the driver accepts the 1D image with 32 layers and keeps the data
```

**Descartada como causa**; queda como deuda técnica (un objeto inválido puede romper en otro
driver).

### 4.4 El `fix` anterior (`dc12e8d`) no cubre los shaders que importan

El censo de `lower_wave64_pass.cpp` dice, por shader compute ancho:

```
tlg: wave64 compute shader 0x4e523719 (64 threads, 64x1x1): ReadFirstLane 1 (1 ReadFirstLane in divergent flow, not lowered)
```

De los **68 shaders de wave64** que compiló la sesión del 20-10-20 19:57, **67** tienen
**todos** sus `ReadFirstLane` en **flujo divergente**, donde el puerto **no los baja**: el
intercambio por LDS necesita una barrera de workgroup, y una barrera en control de flujo no
uniforme es comportamiento indefinido en Vulkan.

**Comprobación cruzada con el SPIR-V real** (`port/tests/spv_rfl_census.py`): el censo y los
`.spv` que la corrida dejó en `userdata/cache/CUSA00223-7554b4d1b0b4/` coinciden **68 de 68**.

**PERO, en el shader examinado al principio, el aviso no tenía consecuencias**: el valor que
se difunde es **uniforme** (se lee de una dirección constante), así que el broadcast por
subgrupo da lo mismo que el de la wave. **Eso no es cierto en todos**: el banco
`port/tests/spv_rfl_harm.py` (sección 2.2) encuentra **20 shaders** de la caché cuyo
`ReadFirstLane` sin bajar **sí** difunde un valor que depende de la lane, y **uno de ellos es
el compute que escribe el número de partículas**.

> **Conclusión de este punto:** el `fix` anterior es incompleto (67 shaders sin bajar) y en
> **20 de ellos el valor que se difunde depende de la lane**, así que **calculan mal en la
> mitad superior de la wave**. Uno de esos 20 es el compute que escribe el número de
> partículas (sección 2.2). El arreglo es bajar el `ReadFirstLane` también en flujo
> divergente, subiendo el intercambio a flujo uniforme (sección 2.3): es trabajo del
> recompilador (`shader_recompiler/**`), fuera del alcance de este agente.

## 5. Qué hay que hacer ahora

### 5.1 El arreglo que falta (recompilador)

Bajar el `ReadFirstLane` en flujo divergente (sección 2.3). Es la causa de las
partículas: el compute que escribe el número de instancias (`0xb6a8b76e`) es uno de los 20
shaders con un `ReadFirstLane` sin bajar **y con un valor que depende de la lane**.

### 5.2 La comprobación que falta (una sesión, y decide entre las dos causas)

```powershell
$env:TLG_DRAW_CENSUS = '1'      # la sonda, ahora con el estado de la región
python port\run.py
```

Y leer, por cada sitio de draw indirecto, la línea:

```
      the region is written by the GPU (the draw reads the arena (what the compute wrote))
```

o

```
      the region is clean (the draw reads the guest memory), and the guest wrote there too
```

**Si sale "clean"**, además del arreglo del compute hay que arreglar el camino del renderer
(punto 3): los argumentos del draw indirecto no deben servirse de la memoria del invitado.

**Si sale "written by the GPU"**, el draw ya lee lo que escribió el compute, y con el
arreglo del punto 2.3 las partículas tienen que aparecer.

### 5.3 La sonda de texturas (opcional, la misma sesión)

`TLG_BC_PROBE=1` ya dijo que las texturas BC 1D del juego no están vacías
(`512x1 layers 32 mips 10: 130944 texel bytes, 32736 not zero`), así que la textura está
descartada.

## 6. Lo que queda fuera de este trabajo

* **El arreglo del renderer** (punto 3): fuera del alcance de este agente (lo otro agente
  trabaja en `renderer_vulkan/**`); aquí está el parche exacto y el porqué.
* **Bajar el `ReadFirstLane` en flujo divergente**: problema real y medido (20 shaders con
  un valor dependiente de la lane, uno de ellos el compute de las partículas), pero el
  arreglo es del recompilador; la sección 2.3 lo describe con el algoritmo y la verificación.
* **La visibilidad (blend, depth, descarte)**: nada en el log la señala ("reduced-size draws
  0/frame", "0 not" pipelines sin preparar, ningún "dropped indirect draw"). Con el
  diagnóstico cerrado (el compute calcula mal el conteo) queda como descartada.

## 7. Ficheros de este trabajo

| Fichero | Qué es |
|---|---|
| `port/gpu/shadps4/video_core/texture_cache/tlg_bc_decode.h` | El decode de una imagen BC entera (`ImageLayout`, `LevelTexelBytes`, `DecodeLevel`), extraído del upload para que el banco y el puerto hagan lo mismo |
| `port/gpu/shadps4/video_core/texture_cache/texture_cache.cpp` | El upload usa el decode extraído; sonda `TLG_BC_PROBE=1` (bytes no nulos y suma de verificación de cada imagen decodificada) |
| `port/gpu/shadps4/video_core/amdgpu/tlg_draw_census.h` | La sonda `TLG_DRAW_CENSUS=1`: draws indirectos con sus argumentos **y el estado de su región**, draws directos y dispatches |
| `port/gpu/shadps4/video_core/amdgpu/liverpool.cpp` | La sonda conectada al decode de PM4 |
| `port/tests/test_bc_decode.cpp` | Banco del decodificador contra la especificación DXT |
| `port/tests/test_bc_decode_gpu.cpp`, `port/tests/bc_gfx/bc_decode.comp` | Banco del decodificador contra el del driver |
| `port/tests/test_1d_array_image.cpp` | Sonda: qué hace el driver con una imagen 1D de 32 capas |
| `port/tests/test_draw_census.cpp` | Banco de la sonda de draws |
| `port/tests/spv_rfl_census.py` | Comprobación cruzada del censo de wave64 con el SPIR-V de la caché |
| `port/tests/spv_rfl_harm.py` | Los shaders con un `ReadFirstLane` sin bajar **y con un valor que depende de la lane** (nuevo) |
| `port/gpu/CMakeLists.txt` | Los cuatro bancos nuevos |

### Reproducir

```powershell
# Los bancos (build privado, sin tocar el del resto de agentes)
& 'C:\msys64\usr\bin\bash.exe' -lc 'source /c/Users/luisd/proyectos/infamous-port/port/msys2-env.sh; cd /c/Users/luisd/proyectos/infamous-port/port && cmake -S gpu -B out/gpu-particles -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTLG_PGO=off -DTLG_LTO=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && ninja -C out/gpu-particles bc-decode-test bc-decode-gpu-test 1d-array-test draw-census-test'
& 'C:\msys64\usr\bin\bash.exe' -lc 'source /c/Users/luisd/proyectos/infamous-port/port/msys2-env.sh; cd /c/Users/luisd/proyectos/infamous-port/port/out/gpu-particles && ./bc-decode-test.exe && TLG_PROBE_DEVICE=NVIDIA ./bc-decode-gpu-test.exe && TLG_PROBE_DEVICE=NVIDIA ./1d-array-test.exe && ./draw-census-test.exe'
```

Todos pasan en la RTX 3050 del usuario (`bc-decode-gpu-test` y `1d-array-test` necesitan un
driver Vulkan; sin él se saltan).
