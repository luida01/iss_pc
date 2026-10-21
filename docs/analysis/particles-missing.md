# Las partículas siguen sin aparecer: qué se ha descartado y cómo se cierra el caso

> **Fecha:** 2026-10-20 · **Alcance:** `port/gpu/shadps4/video_core/texture_cache/**`,
> `port/gpu/shadps4/video_core/amdgpu/**`, `port/tests/**` ·
> **Estado:** el *fix* anterior (`dc12e8d`, `ReadFirstLane` de wave64) **no explica** las
> partículas que faltan: se ha medido que sigue sin aplicarse a los shaders que importan.
> La causa concreta **no está cerrada todavía**: este documento deja fuera de discusión las
> cuatro hipótesis que se podían comprobar sin jugar y deja una sonda que la cierra en una
> sola sesión.

## 1. Resumen para quien tenga prisa

| Hipótesis | Estado | Cómo se comprueba |
|---|---|---|
| Los dispatches de compute no se emiten | **Descartada** | El log de la sesión: 550 dispatches por frame; el puerto los cuenta (`TlgStats::dispatches`) y los emite |
| Los draws de las partículas no llegan al driver | **Descartada** (el puerto los envía) | 4000 draws por frame; el conteo del pipeline: "0 not" |
| El decode BC de la CPU está mal (la textura sale invisible) | **Descartada** | `bc-decode-test` (contra la especificación DXT) y `bc-decode-gpu-test` (contra el decodificador del driver) |
| La imagen 1D de 32 capas es un objeto Vulkan inválido | **Descartada** (el driver la acepta) | `1d-array-test` en la RTX 3050 |
| El `ReadFirstLane` de wave64 sigue sin bajarse en los shaders de partículas | **Medido, y sigue siendo un agujero** | El censo de la corrida + el SPIR-V de la caché (68 de 68 coinciden) |
| **El draw de las partículas pide 0 instancias** (el compute no llenó la lista) | **Sin comprobar: es lo que decide la sonda `TLG_DRAW_CENSUS=1`** | Una sesión de juego con la sonda |

La sonda nueva (`TLG_DRAW_CENSUS=1`, en `video_core/amdgpu/tlg_draw_census.h`) imprime, cada
5 s, **los argumentos de cada draw indirecto** (el número de instancias que el juego
calculó en la GPU) y **el tamaño de cada dispatch**. Con eso, una sola sesión separa las
tres posibilidades que quedan:

* un draw indirecto con **0 instancias** → el compute que llena la lista de partículas no
  produce nada (el problema está en el compute, no en el draw);
* un draw indirecto con **N instancias** → el draw está y la lista está: el problema está
  en la visibilidad (blend, depth, textura) o en el estado del pipeline;
* **ningún dispatch** con la forma de la actualización de partículas → el compute ni
  siquiera se lanza.

## 2. Lo que se ha medido, con pruebas

### 2.1 El compute del juego sí se emite

Del log de la sesión `evidence/20261020-195723-juego-final/run.log`:

```
4234 draws, 1113 dispatches, 68 submissions; GPU thread user 0.0 ms, ...
3986 draws,  591 dispatches, 58 submissions; ...
```

Un frame normal del juego lleva **500 a 1100 dispatches** y **3500 a 4700 draws**. El
decode de PM4 por clase lo confirma: `register 1,6 %, draw 11,4 %, dispatch 4,1 %`. Los
dispatches no se saltan: en `video_core/amdgpu/liverpool.cpp` la única condición que
descarta un dispatch es `(cs_program.dispatch_initiator & 1) == 0`, que es el bit "inmediato"
del paquete PM4 y no ocurre en los shaders del juego (si ocurriera, el propio log del
censo lo contaría).

El pipeline también está listo: `Draw preparation: 121856 prepared pipelines used, 0 not`.

### 2.2 La textura: el decode BC de la CPU es correcto

El juego usa texturas **BC 1D** (el log: `tlg: BC image 512x1 Bc1RgbaSrgbBlock type 12
tile Thin1DThin layers 32 mips 10`, y también 1024x1, 256x1, 128x1, más dos de 1x1 BC4 y
BC5). El driver NVIDIA las rechaza, así que el puerto las decodifica en la CPU
(`TLG_BC_TINY`, `video_core/amdgpu/tlg_bc_tiny.h`) y sube texels planos.

**Banco 1 (sin GPU): `port/tests/test_bc_decode.cpp`**, ampliado en este trabajo:

* un bloque BC1 con un patrón conocido (los cuatro índices en las cuatro columnas) → el
  RGBA esperado, canal a canal;
* BC4 y BC5 con el canal que trae el alfa (o el verde) y los ocho niveles interpolados;
* **la imagen entera que binds el juego** (512x1, 32 capas, 10 mips): el banco construye la
  memoria del invitado con la distribución de micro-tiles del detiler del driver
  (`host_shaders/tiling.comp`) y comprueba que el decode devuelve **los texels de cada
  capa** (una capa que leyera los bloques de otra es un atlas de sprites que muestra el
  sprite equivocado, o ninguno).

Para que el banco diga lo que hace el puerto, el decode de una imagen **se extrajo del
upload a la cabecera** `video_core/texture_cache/tlg_bc_decode.h` (`ImageLayout`,
`LevelTexelBytes`, `DecodeLevel`); `texture_cache.cpp` llama a esa función, así que el
banco y el puerto hacen **exactamente lo mismo** (antes el bucle estaba dentro de
`UploadDecodedImage` y el banco no podía reaches).

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

(La paleta, los índices y los niveles coinciden; la diferencia de hasta 3/255 en los
niveles interpolados de BC4/BC5 es el redondeo del driver, no del decodificador. La sonda
solo puede comparar el **primer canal**: con un *combined image sampler* este driver
devuelve solo el rojo de una imagen comprimida; los shaders del propio puerto usan
imagen y sampler **separados** (`OpSampledImage`), y las texturas del juego salen
bien —lo confirma la captura de la sesión—, así que eso es un artefacto de la sonda.)

**En vivo, para cerrar el caso:** `TLG_BC_PROBE=1` imprime, por cada imagen BC decodificada,
cuántos bytes no son cero y una suma de verificación:

```
tlg: BC decode 512x1 layers 32 mips 10: 130944 texel bytes, 123456 not zero, checksum ..., first texel (r, g, b, a)
```

Si la línea dice "0 not zero", la textura está vacía y el problema es el decode; si dice
"casi todos", la textura está bien y el problema es el draw.

### 2.3 La imagen 1D de 32 capas: objeto Vulkan inválido, pero el driver lo acepta

`info.type` es el **tipo base** de la imagen del invitado, así que una imagen 1D de tipo
`Color1DArray` (12) se crea en el host como `VK_IMAGE_TYPE_1D` con `arrayLayers = 32`.
Eso **no es válido en Vulkan** (`VUID-VkImageCreateInfo-arrayLayers-00981`: una imagen 1D
tiene una sola capa; un array 1D en Vulkan es una imagen 2D de altura 1).

Banco nuevo `port/tests/test_1d_array_image.cpp`, en la RTX 3050 del usuario:

```
1D  (what the port creates): vkCreateImage(imageType=1D, arrayLayers=32) -> 0
1D  (what the port creates): 32 of 32 layers read back as uploaded
1D  (what the port creates): vkCreateImageView(viewType=1D_ARRAY) -> 0
2D  (what Vulkan wants):    vkCreateImage(imageType=2D, arrayLayers=32) -> 0
2D  (what Vulkan wants):    32 of 32 layers read back as uploaded
RESULT: the driver accepts the 1D image with 32 layers and keeps the data
```

**Descartada como causa**: el driver acepta la imagen y guarda las 32 capas donde toca. Queda
como deuda técnica (un objeto inválido puede romper en otro driver), pero no es lo que
falta hoy.

### 2.4 El `fix` anterior (`dc12e8d`) no cubre los shaders que importan

El censo de `lower_wave64_pass.cpp` dice, por shader compute ancho:

```
tlg: wave64 compute shader 0x4e523719 (64 threads, 64x1x1): ReadFirstLane 1 (1 ReadFirstLane in divergent flow, not lowered)
```

De los **68 shaders de wave64** que compiló la sesión del 20-10-20 19:57, **67** tienen
**todos** sus `ReadFirstLane` en **flujo divergente**, donde el puerto **no los baja**: el
intercambio por LDS necesita una barrera de workgroup, y una barrera en control de flujo
no uniforme es comportamiento indefinido en Vulkan.

Eso significa que el arreglo anterior (bajar el `ReadFirstLane` en todo shader ancho)
**no tocó estos 67 shaders**: el documento `docs/analysis/wave64-particles.md` dice "331 de
346 shaders bajados", pero ese número venía del censo **anterior** al cambio, que no
contaba los divergentes (el aviso se añadió en el mismo commit).

**Comprobación cruzada con el SPIR-V real** (nuevo: `port/tests/spv_rfl_census.py`): el
censo y los `.spv` que la corrida dejó en `userdata/cache/CUSA00223-7554b4d1b0b4/`
coinciden **68 de 68**:

```
$ python port/tests/spv_rfl_census.py userdata/cache/CUSA00223-7554b4d1b0b4 \
      evidence/20261020-195723-juego-final/run.log --spirv-dis /c/msys64/clang64/bin/spirv-dis.exe
cached shaders: 1071
  with OpGroupNonUniformBroadcastFirst (a wave64 ReadFirstLane): 178

against the run's census:
  0x4e523719: census 1/1 not lowered; 1 broadcast(s) in the SPIR-V -> 1 unlowered (agrees)
  ...
  68 agree, 0 disagree, 0 not in the cache
```

**PERO, en el shader examinado, el aviso no tiene consecuencias**: el valor que se difunde
es **uniforme**, así que el broadcast por subgrupo da lo mismo que el de la wave. En
`0x4e523719` (SPIR-V, `spirv-dis`):

```
%2000 = OpIAdd %u32_id %u32_id_0 %buf10_dword_off      <- una dirección constante
%2001 = OpAccessChain %_ptr_StorageBuffer_u32_id %ssbo_11 %u32_id_0 %2000
%2002 = OpLoad %u32_id %2001                          <- el mismo valor en todas las lanes
%2004 = OpConvertFToU %u32_id %2003
%2005 = OpGroupNonUniformBroadcastFirst %u32_id %u32_id_3 %2004
```

El `ReadFirstLane` difunde un valor que **ya es igual en todas las lanes**, así que da igual
que lo difunda el subgrupo o la wave. **El aviso del censo es, en este shader, ruido.**

> **Conclusión de este punto:** el `fix` anterior es incompleto (67 shaders sin bajar) pero
> **no es la causa de las partículas que faltan**. Lo que hace falta es saber, para esos
> shaders, si el valor que difunden **depende de la lane**; con eso se decide si hay que
> bajar el `ReadFirstLane` en flujo divergente (intercambio por LDS en flujo uniforme, o
> un intercambio por memoria) o si el aviso se puede silenciar. Eso es trabajo del
> recompilador de shaders (`shader_recompiler/**`), fuera del alcance de este agente.

## 3. La sonda que cierra el caso: `TLG_DRAW_CENSUS=1`

Nueva en `video_core/amdgpu/tlg_draw_census.h` (con su banco,
`port/tests/test_draw_census.cpp`), conectada al decode de PM4
(`video_core/amdgpu/liverpool.cpp`).

Por cada ventana de 5 s imprime:

```
Draw census: 3 direct draws, 3 indirect draws (2 sites, 1 with 0 instances), 3 dispatches
  direct draws by instance count: 1:2 4:1
  indirect site 0000000000001000: 2 draws, count 6, instances 120
  indirect site 0000000000002000: 1 draws, count 6, instances 0  <-- NOTHING DRAWN
  dispatches by size: (64,1,1)x2 (256,1,1)x1
```

* **los draws indirectos**, por sitio (la dirección de sus argumentos): el `count` (vértices
  o índices por instancia) y las **`instances`** (el número de partículas que el juego
  calculó en la GPU). Los argumentos se leen una vez por sitio y ventana, así que el coste
  es de unas decenas de lecturas por 5 s, no de miles por frame.
* **los draws directos**, por número de instancias.
* **los dispatches**, por su tamaño (x, y, z): el que actualiza la lista de partículas
  tiene una forma propia.

La línea `instances 0  <-- NOTHING DRAWN` es la firma de "el compute no llenó la lista".

> **Cómo se leen los números:** son la vista de la CPU sobre la memoria del invitado. Una
> ranura que la GPU escribió y que el juego nunca leyó de vuelta está ahí desfasada (a
> menudo a cero), así que los números se leen **en grupo**: si los otros sitios tienen
> instancias y uno no, ese no dibuja nada de verdad; si **todos** los sitios están a cero,
> la CPU no ve los valores de la GPU y hay que preguntar al renderer (fuera del alcance de
> este agente).

### Cómo usarla

```powershell
$env:TLG_DRAW_CENSUS = '1'
$env:TLG_BC_PROBE = '1'      # la sonda de la textura (opcional, misma sesión)
python port\run.py
```

Con las dos sondas, una sola sesión dice:

| Lo que dice el log | Dónde está el problema |
|---|---|
| `instances 0` en el sitio de las partículas | El compute que llena la lista: el `ReadFirstLane` en flujo divergente, el `ballot` o el prefijo de la suma (recompilador) |
| `instances N` (N > 0) | El draw está: la visibilidad (blend, depth) o el pipeline (renderer) |
| Ningún dispatch con la forma de la actualización | El dispatch se salta antes de llegar al driver |
| `tlg: BC decode ... 0 not zero` | La textura: el decode de la CPU |

## 4. Lo que queda fuera de este trabajo

* **Bajar el `ReadFirstLane` en flujo divergente.** Es un problema real y medido (67
  shaders), pero el arreglo es del recompilador (`shader_recompiler/ir/passes/`), no del
  alcance de este agente, y necesita un intercambio entre los dos subgrupos del host que
  no puede ser una barrera de workgroup en flujo no uniforme.
* **La visibilidad (blend, depth, descarte).** Nada en el log la señala: "reduced-size
  draws 0/frame", "0 not" pipelines sin preparar, ningún "dropped indirect draw" (la
  comprobación de `TLG_INDIRECT_CHECK` que descarta un draw con argumentos absurdos no
  aparece en el log). Queda por confirmar con la sonda, que separa el draw del compute.
* **El motor del juego (SotC) y el "shader cache"**: sin cambios en este trabajo.

## 5. Ficheros de este trabajo

| Fichero | Qué es |
|---|---|
| `port/gpu/shadps4/video_core/texture_cache/tlg_bc_decode.h` | El decode de una imagen BC entera (`ImageLayout`, `LevelTexelBytes`, `DecodeLevel`), extraído del upload para que el banco y el puerto hagan lo mismo |
| `port/gpu/shadps4/video_core/texture_cache/texture_cache.cpp` | El upload usa el decode extraído; sonda `TLG_BC_PROBE=1` (bytes no nulos y suma de verificación de cada imagen decodificada) |
| `port/gpu/shadps4/video_core/amdgpu/tlg_draw_census.h` | La sonda `TLG_DRAW_CENSUS=1` (nuevo) |
| `port/gpu/shadps4/video_core/amdgpu/liverpool.cpp` | La sonda conectada al decode de PM4: draws indirectos con sus argumentos, draws directos y dispatches |
| `port/tests/test_bc_decode.cpp` | Banco del decodificador contra la especificación DXT (patrón conocido, BC4/BC5, la imagen de 32 capas) |
| `port/tests/test_bc_decode_gpu.cpp`, `port/tests/bc_gfx/bc_decode.comp` | Banco del decodificador contra el del driver (nuevo) |
| `port/tests/test_1d_array_image.cpp` | Sonda: qué hace el driver con una imagen 1D de 32 capas (nuevo) |
| `port/tests/test_draw_census.cpp` | Banco de la sonda de draws (nuevo) |
| `port/tests/spv_rfl_census.py` | Comprobación cruzada del censo de wave64 con el SPIR-V de la caché (nuevo) |
| `port/gpu/CMakeLists.txt` | Los cuatro bancos nuevos |

### Reproducir

```powershell
# Los bancos (build privado, sin tocar el del resto de agentes)
& 'C:\msys64\usr\bin\bash.exe' -lc 'source /c/Users/luisd/proyectos/infamous-port/port/msys2-env.sh; cd /c/Users/luisd/proyectos/infamous-port/port && cmake -S gpu -B out/gpu-particles -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTLG_PGO=off -DTLG_LTO=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && ninja -C out/gpu-particles bc-decode-test bc-decode-gpu-test 1d-array-test draw-census-test'
& 'C:\msys64\usr\bin\bash.exe' -lc 'source /c/Users/luisd/proyectos/infamous-port/port/msys2-env.sh; cd /c/Users/luisd/proyectos/infamous-port/port/out/gpu-particles && ./bc-decode-test.exe && TLG_PROBE_DEVICE=NVIDIA ./bc-decode-gpu-test.exe && TLG_PROBE_DEVICE=NVIDIA ./1d-array-test.exe && ./draw-census-test.exe'
```

Todos pasan en la RTX 3050 del usuario (`bc-decode-gpu-test` y `1d-array-test` necesitan un
driver Vulkan; sin él se saltan).
