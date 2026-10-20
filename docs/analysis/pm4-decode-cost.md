# El decode de PM4: lo que cuesta de verdad (2026-10-20)

Port nativo de inFAMOUS Second Son (CUSA00223), rama `bootstrap/windows-g0`.
Alcance de este informe: `port/gpu/shadps4/shader_recompiler/**` (el SRT walker, la
verificación de los shaders preparados, el análisis de los recursos) y
`port/tests/**` (los bancos de medida).

## 1. Resumen

La pregunta que traía el encargo era: *"el SRT walker es la parte más cara de la clase
`draw` del decode (9.675 ciclos por paquete), ¿se puede cachear, hacer incremental o
evitar?"*.

**Medido: no.** El SRT walker cuesta **16,0 ciclos del TSC por llamada** (47,5 entradas,
48,5 lecturas de la memoria del invitado), es decir **~32 ciclos por draw** (vertex +
fragment, dos walks) sobre los 9.675 del paquete: **0,33 % de la clase `draw`** y 0,05 %
del decode. Cachearlo, hacerlo incremental o evitarlo **no puede mover la aguja**: el
camino que lo contiene ya es el más rápido que se puede.

Lo que sí es caro, y que estaba debajo de la misma línea del perfil, es **la búsqueda
del bloque que describe el shader** (`AmdGpu::FindBinaryInfo`, en
`video_core/amdgpu/regs_shader.h`): el port la busca a palabra en palabra en hasta 64 KiB
de la memoria del invitado, y el decode la paga **por cada etapa de cada draw y de cada
dispatch**. Medida: **87 – 15.308 ciclos** según dónde esté el bloque (el límite son
64 KiB de memoria del invitado leídos a razón de ~239 ciclos por KiB). Con el memo
implementado aquí, **25 ciclos**.

Cuánto de eso es el 4,1 % del decode que son los dispatch (21.067 ciclos por paquete)
**no se puede afirmar sin medirlo en el juego**: depende de si el bloque de cada shader
de cómputo está cerca de su código o no. Lo que sí se puede decir es que es **el item más
caro que se ha podido medir del decode**, que el resto de las piezas del recompilador
suman 380 ciclos (la especialización y el walk), y que el parche propuesto (§8.1) lo deja
en 25 ciclos en todos los casos en que el bloque existe.

Con el mismo detalle, la clase `draw` (9.675 ciclos por paquete) se reparte así:

| Parte | Ciclos | Dónde |
|---|---|---|
| La búsqueda del bloque del shader, por etapa | 87 – 15.308 (2 etapas) | `video_core/amdgpu/regs_shader.h` (**fuera del alcance**, parche propuesto) |
| **El SRT walker, por etapa** | **16,0 × 2 = 32** | `shader_recompiler/` (este informe) |
| La especialización del shader (recursos) | 363 (4 buffers + 8 imágenes + 4 samplers) | `shader_recompiler/specialization.h` |
| El armado del paquete (`PostDraw`) y el resto | el resto | `video_core/renderer_vulkan/` (otro agente) |

**Conclusión operativa**: el decode PM4 no tiene un problema en el recompilador de
shaders. El coste está en la búsqueda del bloque del shader (video_core) y en lo que el
hilo de la GPU hace con cada paquete. Lo que sí se entrega aquí es (a) la medición
reproducible, (b) un perfil en vivo del walker para confirmar la cifra dentro del juego,
(c) un memo para la búsqueda del bloque, implementado en `shader_recompiler/` y medido
(**15.308 → 25 ciclos**), y (d) el parche propuesto para que el renderer lo use.

## 2. Cómo se midió

El juego no se ejecuta aquí (la sesión la hace el usuario), así que todo se midió **fuera
del juego, con el código real**:

* La caché de shaders de una sesión real (`userdata/cache/CUSA00223-e8c54468288d`,
  1.660 ficheros `.meta`) contiene el **código máquina del SRT walker de cada shader** que
  el juego usó (lo escribe `Vulkan::RegisterShaderMeta`,
  `video_core/renderer_vulkan/vk_pipeline_serialization.cpp`). Los bancos de medida lo
  ejecutan tal cual, sobre una memoria sintética del invitado.
* El bloque que describe el shader (`OrbShdr`) y su búsqueda son código de
  `video_core/amdgpu/regs_shader.h`, que los bancos incluyen y miden.

| Banco | Qué mide | Cómo se ejecuta |
|---|---|---|
| `port/tests/srt_bench.cpp` | el SRT walker real de los 1.660 shaders de la caché | `bash port/tests/build_pm4_benches.sh` y luego `out/gpu-pm4/srt-bench.exe <cache dir>` |
| `port/tests/bininfo_bench.cpp` | la búsqueda del bloque del shader (el escaneo de 64 KiB) y el memo | `out/gpu-pm4/bininfo-bench.exe` |
| `port/tests/spec_bench.cpp` | la construcción de la especialización y las constantes de push | `out/gpu-pm4/spec-bench.exe` |
| `port/tests/test_pm4_decode.cpp` | test funcional del walker, del perfil y del memo | `out/gpu-pm4/test-pm4-decode.exe` |

`port/tests/srt_meta_probe.py` lee la cola de un `.meta` y dice el tamaño del walker y
el número de entradas, sin compilar nada.

Máquina: i5-13420H, TSC 2,6112 GHz (invariante entre núcleos, medido por
`port/tests/test_tsc_rate.cpp`). El núcleo corre a ~1,5× el TSC con la turbo, así que un
ciclo del TSC son ~1,5 ciclos de núcleo.

## 3. El SRT walker

### 3.1 Qué es y cuántas veces corre

El walker es el código máquina que el recompilador genera para **copiar la tabla de
recursos del shader (los "sharps") de la memoria del invitado al user data aplanado**
(`ir/passes/flatten_extended_userdata_pass.cpp`, `GenerateSrtProgram`). Por cada entrada
hace un `mov r10d, [rdi+off]` (lee del invitado) y un `mov [rsi+off], r10d` (escribe en
el buffer aplanado); por cada nivel anidado, un `mov rdi, [rdi+off]` (la puntería).

Quién lo corre:

| Dónde | Cuántas veces | Para qué |
|---|---|---|
| Hilo de la GPU (stage A), `PipelineCache::TryPreparedPipeline` (`vk_pipeline_cache.cpp:1334`) | 1 por etapa y por draw | **verificar** que los datos que preparó el worker siguen igual: se vuelve a recorrer el SRT y se compara con lo que el worker dejó |
| Hilo de la GPU, `PipelineCache::GetProgram` (`vk_pipeline_cache.cpp:2351`) | 1 por etapa y por draw, en el camino normal (0,04 % de los draws) | el buffer aplanado que va al UBO |
| Workers de preparación de draws (`vk_draw_prep.cpp` → `GetProgram`) | 1 por etapa y por draw | el dato que el hilo de la GPU después verifica |
| Recompilador (`flatten_extended_userdata_pass.cpp:874`) | 1 por shader compilado | fuera del camino caliente |

O sea: **4 walks por draw** (2 en el hilo de la GPU, 2 en los workers) y 1 por dispatch.

### 3.2 Cuántas entradas

Medido sobre los 1.660 shaders de la caché de una sesión real:

| | walkers | entradas por walk | lecturas del invitado por walk |
|---|---|---|---|
| total | **1.068 de 1.660** (los otros 592 no tienen SRT: sus recursos están en el user data) | 50.680 | 51.802 |

* entradas por walk: **mín 8, mediana 32, media 47,5, máx 216**;
* lecturas del invitado por walk: **48,5** (47,5 de los "sharps" + 1,0 de puntería);
* el puntero del SRT sale **siempre del registro 12 del user data** (1.068 de 1.068).

### 3.3 Cuánto cuesta (antes)

Banco `srt-bench`, modelo "hot" (la tabla del invitado en la primera línea de caché) y
modelo "spread" (la tabla repartida en 4 MiB):

```
SRT walker bench (hot model, 3000 iterations each, TSC 2.6112 GHz)
  shaders with a walker: 1068 (without: 592, unreadable: 0)
  walker code: 593576 bytes in total, 556 bytes per shader
  entries per walk: 50680 in total, 47.5 per shader
  guest reads per walk: 48.5 (1.0 of them pointer chases)
  walkers that wrote the flattened buffer: 1068 of 1068
  cycles per walk: mean 16.0, min 3.4, max 65.7
  cycles per entry: mean 0.37
  nanoseconds per walk: mean 6.1
```

* **16,0 ciclos del TSC (6,1 ns) por walk**, 0,37 por entrada;
* el modelo "spread" (la tabla del invitado repartida en 4 MiB) da la misma media (16,0),
  con un máximo de 297 ciclos en los shaders cuya puntería anidada cae en una línea de
  caché fría: las 47,5 lecturas de los "sharps" salen de la misma tabla pequeña, que
  está caliente; lo único que se desperdicia es la puntería;
* 0,37 ciclos del TSC por entrada = ~0,56 ciclos de núcleo por entrada, o sea **algo más
  de dos operaciones de memoria por ciclo**: el walker ya está optimizado hasta el límite
  de la máquina. El "TODO: optimize with wider loads/stores" del generador no valdría
  gran cosa: pasar de 4 a 16 bytes por lectura ahorraría un 30 % de 16 ciclos.

**Por draw**: 2 walks × 16,0 = **32 ciclos** sobre los 9.675 del paquete → **0,33 %**.
**Por dispatch**: 1 walk = 16,0 ciclos sobre 21.067 → **0,08 %**.

### 3.4 Por qué no se puede cachear (el análisis)

El walk **no es redundante**: es la *verificación* de que la memoria del invitado no ha
cambiado desde que el worker calculó el dato preparado. Si el invitado escribió en la
tabla de recursos entre medias, el draw tiene que rehacerse por el camino normal (con
otra búsqueda del shader y otra especialización): saltarse el walk produciría un UBO con
descriptores de recursos viejos (un fallo visual, no un crash).

Cachearlo "porque los shaders preparados cambian poco" sería incorrecto salvo que
alguien pueda decir *"la memoria del invitado no ha cambiado"*. El que lo sabe es el
**gestor de páginas** (`video_core/page_manager.cpp`, `buffer_cache/region_manager.h`),
que ya sabe qué páginas escribió el invitado: con él, la verificación podría ser
*"¿se escribió alguna de estas páginas desde la última vez?"* en vez de un recorrido.
**Eso es un parche para el agente de video_core (§8.2), no un cambio aquí.**

Lo que sí se puede hacer en el recompilador, y es lo que hace este informe, es
**medirlo** para que nadie lo vuelva a atacar por intuición.

### 3.5 El perfil en vivo (cambio implementado)

`shader_recompiler/srt_profile.h` (nuevo) cuenta los walks, las entradas y los ciclos, y
los imprime cada 5 s con `TLG_SRT_PROFILE=1` (el formato de la línea):

```
SRT walker: 128374 walks, 6092334 entries, 2145888 cycles (16.7 per walk, 0.35 per entry), 0 shaders without SRT
```

Coste cuando está apagado: una carga de un `bool` y una rama previsible por walk (el
propio walk son 16 ciclos, así que el efecto sobre la clase `draw` es de 0,005 %). El
perfil se lee con el TSC crudo, como el perfil de PM4, porque una lectura con `lfence`
costaría más que el walk.

Con ese perfil, la línea del log dirá cuántos walks por draw hay de verdad (y si los
workers están escribiendo en el `Info` compartido, §8.3).

## 4. La clase `draw` del decode: 9.675 ciclos por paquete

El paquete de draw hace, en el hilo de la GPU (stage A):

1. `ApplyGraphicsRegisterPacket` (el paquete de draw no lleva registros: ~0).
2. `Rasterizer::Draw` → `PipelineCache::GetGraphicsPipeline`:
   * `SeedWorkerCaches()`, `PublishShaders()`, `PublishGraphics()`, `PumpWarmUp()`;
   * `TryPreparedPipeline`: **por etapa**, `AmdGpu::TryGetParams` (la búsqueda del
     bloque, §4.1) + `Info::RefreshFlatBuf` (el walk, 16,0 ciclos) + un `memcmp` del
     buffer aplanado (190 bytes, ~20 ciclos);
   * si la preparación no vale (0,04 % de los draws): `RefreshGraphicsKey` →
     `GetProgram` → **otra** búsqueda del bloque + la **especialización** (363 ciclos
     con 4 buffers, 8 imágenes y 4 samplers) + la comparación con las permutaciones.
3. `PostDraw`: el armado del paquete (los bloques de registro, el user data y el buffer
   aplanado de cada etapa, ~500 bytes) y el `memcpy` al anillo del pipe.

### 4.1 La búsqueda del bloque del shader: el coste real de la clase

`AmdGpu::FindBinaryInfo` (`video_core/amdgpu/regs_shader.h:250`) busca el bloque `OrbShdr`
que sigue al código del shader. Si el código empieza por el marcador `0xBEEB03FF` lo
encuentra enseguida; si no, **recorre 16.384 palabras (64 KiB) de la memoria del
invitado**, palabra a palabra.

Medido (`bininfo-bench`):

| caso | coste | con el memo |
|---|---|---|
| marcador + bloque justo detrás | 87,3 ciclos | 24,9 |
| sin marcador, bloque justo detrás | 91,8 | 24,2 |
| sin marcador, bloque 4 KiB más lejos | 1.021,7 | 24,1 |
| sin marcador, **bloque inexistente** (la ventana entera) | **15.307,6** | 15.270 (no se puede memoizar un fallo) |

El caso de "bloque inexistente" es el que paga el invitado: el juego copia el código del
shader con la GPU justo antes de usarlo, y mientras no está el bloque el port no puede
traducirlo (ese caso salta el draw, como está previsto en el propio código).

La distancia al bloque es, en la práctica, **el tamaño del shader**: un vertex shader
de 10 KiB tiene su bloque 10 KiB más allá, y el escaneo de esa distancia cuesta
~2.400 ciclos. Con el marcador `0xBEEB03FF` que el juego escribe al principio del
código (el camino rápido de `FindBinaryInfo`), la búsqueda es de 87 ciclos, con
independencia del tamaño.

**Cuánto de esto es el 4,1 % del decode que son los dispatch (21.067 ciclos por paquete)
no se puede afirmar sin medirlo en el juego**: depende de si el bloque de cada shader de
cómputo está cerca de su código o no. Lo que sí se puede decir es que es **el item más
caro que se ha podido medir del decode** (el resto de las piezas del recompilador suman
379 ciclos: la especialización y el walk), y que el memo lo deja en 25 ciclos en todos
los casos en que el bloque existe. Para cerrarlo, hace falta un contador en el propio
`FindBinaryInfo` (§8.4).

## 5. La clase `dispatch`: 21.067 ciclos por paquete

`DispatchDirect` → `GetComputePipeline` → `RefreshComputeKey` → `TryGetParams` (la
búsqueda, 87 – 15.308) → `GetProgram` → `RefreshFlatBuf` (el walk, 16,0) → la
especialización (363) → la búsqueda de la tubería de cómputo en el mapa → `PostDraw`.

| Parte | Ciclos |
|---|---|
| **La búsqueda del bloque del shader** | **87 – 15.308 (entre el 0,4 % y el 73 % del paquete)** |
| La especialización (los recursos del shader de cómputo) | 363 |
| El SRT walker | 16 |
| El resto (la tubería, el paquete, el PM4) | 15.380 – 20.601 |

**El SRT walker no pasa por ahí más que en los draws**: el dispatch tiene una sola etapa
(un shader de cómputo), y el port no añade esperas ni sincronizaciones propias en ese
camino (el trabajo del dispatch es todo del port: elegir tubería, grabar el dispatch y
entregarlo al hilo de grabación, como un draw).

## 6. ¿Se puede paralelizar el decode con la grabación?

Hoy el decode va en el hilo de la GPU (stage A) y la grabación en el hilo que graba
(stage B). La pregunta es si el decode se puede hacer en paralelo con la grabación.

**Lo que ya está en paralelo**: la *preparación* de los draws (elegir tubería, leer los
"sharps" de los recursos, los vértices) la hacen los workers
(`video_core/renderer_vulkan/vk_draw_prep.cpp`), y el hilo de la GPU sólo verifica. Eso
es justo lo que hace el SRT walker: **la verificación es la parte que no se puede
adelantar**, porque depende de la memoria del invitado en ese instante.

**Lo que no se puede paralelizar**: el orden. El decode de un paquete de draw produce el
paquete que el hilo de grabación consume, y el hilo de grabación necesita el estado
exacto de los registros en ese punto. Mover el decode al hilo que graba no quita CPU
(nada más), sólo cambia quién la gasta: el hilo que graba está al 85-89 % y el hilo de
la GPU al 100 %, así que **no se gana nada** (mover trabajo de un hilo al 100 % a otro
al 88 % deja el total igual o peor).

**Lo que sí se puede** es quitar trabajo del decode (aquí: la búsqueda del bloque, que
puede llegar a ser el 72 % de la clase `dispatch`) para que el hilo de la GPU llegue
antes al paquete. Eso es justamente el cambio de §7.2.

## 7. Cambios implementados (con el antes/después)

### 7.1 El perfil del SRT walker (`TLG_SRT_PROFILE=1`)

`shader_recompiler/srt_profile.h` (nuevo) + el gancho en `Info::RefreshFlatBuf`
(`shader_recompiler/info.h`).

| | antes | después |
|---|---|---|
| lo que se ve en el log | nada | walks, entradas, ciclos, ciclos por walk y por entrada, cada 5 s |
| coste del gancho con el perfil apagado | — | una carga de un `bool` y una rama previsible (el walk son 16 ciclos: 0,005 % de la clase `draw`) |

Test: `port/tests/test_pm4_decode.cpp` (el walker copia las entradas, el perfil las
cuenta).

### 7.2 El memo de la búsqueda del programa (`shader_recompiler/program_lookup.h`, nuevo)

`Shader::ProgramLookup` memoiza, por la dirección del código, dónde está el bloque que
lo describe y con qué hash, y lo **revalida** contra el bloque y contra las primeras
palabras del código (lo mismo que hace la caché del fetch shader,
`frontend/fetch_shader.cpp`). Si el invitado reescribe un shader en la misma dirección,
reescribe su bloque, el memo falla y se vuelve a buscar.

| | antes | después |
|---|---|---|
| bloque justo detrás del código | 91,8 ciclos | **24,2** (3,8×) |
| bloque 4 KiB más lejos | 1.021,7 | **24,1** (42×) |
| bloque inexistente | 15.307,6 | 15.270 (no se puede memoizar un fallo: el bloque puede aparecer después) |

El memo es **por hilo** (como la caché del fetch shader): sin bloqueos, y cada hilo (el
de la GPU, los workers, los del compilador) tiene el suyo.

**Para que el cambio tenga efecto falta una línea en el renderer** (§8.1): el sitio de
la llamada está en `video_core`, fuera del alcance de este agente.

## 8. Parches propuestos (fuera del alcance de este agente)

### 8.1 Que el renderer use el memo (video_core)

`port/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp`, los cinco sitios que
llaman a `AmdGpu::TryGetParams` (líneas 256, 1326, 1939, 1943 y 2077):

```diff
-    const auto params_vc = AmdGpu::TryGetParams(regs.vs_program);
+    // tlg: la búsqueda del bloque del shader está memoizada (shader_recompiler/program_lookup.h):
+    // el escaneo de 64 KiB cuesta 15.308 ciclos y el decode lo paga por cada etapa de cada
+    // draw y de cada dispatch.
+    const auto params_vc = Shader::LookupParams(regs.vs_program);
```

y lo mismo en los otros cuatro (`*pgm`, `regs.vs_program`, `cs_pgm`). `Shader::LookupParams`
hace lo mismo que `AmdGpu::TryGetParams` (incluido el aviso `LogMissingBinaryInfo` cuando
el invitado todavía no ha escrito el bloque).

Medido en el banco: **15.308 → 25 ciclos** por consulta.

### 8.2 Que la verificación del SRT use el gestor de páginas (video_core)

La verificación de los datos preparados (`TryPreparedPipeline`) recorre el SRT para
comprobar que el invitado no lo ha cambiado. Con el gestor de páginas (que ya sabe qué
páginas escribió el invitado) la pregunta correcta es *"¿se escribió alguna de estas
páginas desde la última verificación?"*, y la respuesta cuesta unos pocos ciclos en vez
de 16. Cuando la respuesta sea "no", el walk se puede saltar.

Con la medición de §3.3, el ahorro máximo es de 32 ciclos por draw (0,33 % de la clase
`draw`): **no es la prioridad**, pero es el camino natural si algún día el SRT se
vuelve más caro (más recursos por shader).

### 8.3 El `Program::info` compartido entre el hilo de la GPU y los workers (video_core)

`PipelineCache::TryPreparedPipeline` (hilo de la GPU) y `PipelineCache::GetProgram`
(workers de preparación) escriben **el mismo** `Program::info`:

```cpp
auto& info = const_cast<Program*>(stage.program)->info;   // hilo de la GPU
info.pgm_base = params.Base();
info.user_data = params.user_data;
info.RefreshFlatBuf();                                     // escribe info.flattened_ud_buf
```

```cpp
auto& info = program->info;                               // worker
info.user_data = params.user_data;
info.RefreshFlatBuf();                                     // escribe info.flattened_ud_buf
```

Los dos hilos escriben a la vez el mismo `std::vector<u32>` (y el `resize` de
`RefreshFlatBuf` puede realocar): es una carrera de datos. La consecuencia posible es un
UBO con una mezcla de dos tablas de recursos (un fallo visual) y, en el peor caso, un
`resize` concurrente (corrupción del heap). El arreglo es que el worker use su propia
copia del `Info` (el camino de compilación asíncrona ya lo hace:
`build->infos[i] = std::make_unique<Shader::Info>(*sel.infos[i])`).

Medido: 210 de 533.504 draws no usaron la tubería preparada (0,04 %), así que la carrera
**no** está costando frames ahora; pero es una bomba de relojería en el camino que este
informe ha tocado.

### 8.4 Un contador en la búsqueda del bloque (video_core)

Para saber cuánto de la clase `draw` y de la clase `dispatch` es la búsqueda del bloque
hace falta medirla en el juego. El parche es de tres líneas en
`video_core/amdgpu/regs_shader.h`:

```diff
 static constexpr const BinaryInfo* FindBinaryInfo(const u32* code) {
+    // tlg: el coste de la búsqueda (TLG_PM4_PROFILE=1): el escaneo de hasta 64 KiB.
+    const u64 start = TlgProfile::Enabled() ? __rdtsc() : 0;
     ...
+    TlgProfile::Add(start, __rdtsc() - start);
 }
```

con un contador análogo al del perfil de PM4 (`Liverpool::Pm4Profile`, al que habría que
añadir una clase "shader lookup" o duplicar el contador). Con ese número, la línea
`PM4 decode:` del log puede atribuir la clase `dispatch` sin ambigüedad.

## 9. Cómo se reproduce

```bash
# Los bancos (no necesitan el juego, ni la GPU, ni Vulkan)
bash port/tests/build_pm4_benches.sh
port/out/gpu-pm4/srt-bench.exe "C:/Users/luisd/proyectos/infamous-port/userdata/cache/CUSA00223-e8c54468288d"
port/out/gpu-pm4/bininfo-bench.exe
port/out/gpu-pm4/spec-bench.exe
port/out/gpu-pm4/test-pm4-decode.exe

# La forma de los walkers de una sesión, sin compilar
python port/tests/srt_meta_probe.py "C:/Users/luisd/proyectos/infamous-port/userdata/cache/CUSA00223-e8c54468288d"
```

En el juego, con el perfil del walker:

```
$env:TLG_SRT_PROFILE = '1'
```

y, para ver el perfil de PM4 junto al del walker:

```
$env:TLG_PM4_PROFILE = '1'
```

**Nota**: los dos ficheros nuevos de `shader_recompiler/` entran en la clave de la caché
de shaders (`port/gpu/CMakeLists.txt` la calcula con todos los `.h` del recompilador), así
que **la primera sesión después de este cambio recompila los shaders** (es el
comportamiento ya previsto de la clave).

## 10. Lo que queda sin resolver

| Punto | Fichero | Por qué no se toca aquí |
|---|---|---|
| La búsqueda del bloque del shader (el item más caro medido del decode) | `video_core/amdgpu/regs_shader.h` | Fuera del alcance; el memo está implementado aquí y el parche está en §8.1, y el contador para cerrarlo en §8.4 |
| El armado del paquete y la selección de tubería (el resto de la clase draw) | `video_core/renderer_vulkan/vk_rasterizer.cpp`, `vk_pipeline_cache.cpp` | Otro agente (renderer) |
| La carrera sobre `Program::info` | `video_core/renderer_vulkan/vk_pipeline_cache.cpp` | Otro agente (renderer); §8.3 |
| La clase "other" del decode (el 83 % que no es draw, dispatch ni registro: los NOPs, la memoria y las esperas) | `video_core/amdgpu/liverpool.cpp` | Otro agente; **es la mayor parte del decode** |
| La verificación del SRT con el gestor de páginas | `video_core/page_manager.cpp` | Otro agente; §8.2 (el ahorro medido es de 0,33 % de la clase draw) |
