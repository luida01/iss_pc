# BC 1D automático: la decodificación en CPU deja de necesitar variables de entorno

**Fecha:** 2026-10-19
**Alcance:** port nativo de inFAMOUS Second Son (CUSA00223, base 1.00 + 1.07), `port/gpu/**`.
**Equipo objetivo:** Windows 11, i5-13420H, 16 GB RAM, RTX 3050 Laptop **4 GB**, driver NVIDIA **617.42**.
**Estado:** parche aplicado en `port/gpu/**`; la librería se compiló con MSYS2 CLANG64 (ver §5). El juego **no** se ejecutó: la validación en vivo la hará el usuario con el run de §5.2.

> Este documento no declara el crash resuelto hasta que el run real lo reproduzca: describe la causa raíz medida, la detección añadida, la matriz de decisión y cómo verificarla.

---

## 1. Causa raíz

El formato BC 1D es opcional en Vulkan: cada driver decide si crea imágenes `VK_IMAGE_TYPE_1D` de formatos comprimidos. El driver **NVIDIA 617.42** los rechaza (`vkGetPhysicalDeviceImageFormatProperties` devuelve `VK_ERROR_FORMAT_NOT_SUPPORTED`), pero el código seguía creando la imagen porque un comentario antiguo asumía que «NVIDIA las reporta no soportadas pero las muestrea bien» (`image.cpp:280-284`). Con este driver la imagen no es utilizable y el juego termina leyendo memoria que nunca se escribió: el `Fault: read access to 0000000800da08c0 at tlg-probe.exe+0x361b0` tras `image format Bc1RgbaUnormBlock type 1D is not supported` (`evidence/20261019-104920-motion-test-fresh-180s`, `evidence/20261019-104439-motion-test-180s`).

Estado medido de los dos caminos existentes (los tres ya estaban implementados, pero solo por variable de entorno):

| Camino | Qué hace | Resultado medido |
|---|---|---|
| Nada (default NVIDIA hasta hoy) | Crea la imagen BC 1D aunque el driver la rechace | Crash al arrancar (dos evidencias citadas) |
| `TLG_BLOCK1D=1` | Promociona la textura a imagen **2D comprimida** de altura 1 | Evita el crash, pero el HUD/fuentes se ven repetidos en franjas (`evidence/20261019-104955-motion-test-block1d-180s` + captura del usuario) |
| `TLG_BC_TINY=1` | **Decodifica en CPU** BC1-BC5 1D (o <4 texels) a imágenes planas | Evita el crash y la UI sale limpia (`evidence/20261019-111234-bc-tiny-180s`: capturas 75/135/165 s normales, 180 s sin crash) |

La conclusión es que el camino seguro ya existía y estaba probado; solo faltaba activarlo automáticamente cuando la sonda del driver dice que BC 1D no está soportado.

## 2. Detección (una sola vez, antes de crear imágenes/shaders)

`port/gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp`:

- **`DriverSupportsBc1D()` (`:101-143`)** consulta `vkGetPhysicalDeviceImageFormatProperties` para los 12 formatos BC1-BC5 (unorm/sRGB/snorm) con `imageType = 1D` y la misma combinación que usará la creación real (`image.cpp:226-279`):
  - flags `MutableFormat | ExtendedUsage` siempre, y `BlockTexelViewCompatible` **si la sonda 2D de BC1 lo acepta** (la misma que decide `supports_block_texel_view` en `CollectImageFormatInfo`, `vk_instance.cpp:1047-1058`; así no se declara soportado algo que la creación real pedirá con ese flag).
  - usos `Sampled | TransferSrc | TransferDst` más `Storage` con el mismo fallback sin `Storage` que hace `image.cpp:269-279`.
  - Si **cualquier** formato falla ambas consultas, el driver se considera sin soporte BC 1D (conservador: un solo formato problemático basta).
- La sonda se llama en **`CollectDeviceParameters()` (`vk_instance.cpp:922-926`)**, que se ejecuta antes de `CreateDevice()` y de cualquier imagen o shader (`vk_instance.cpp:239-247`). Se imprime una línea de log informativa (`:931-938`).

## 3. Comportamiento nuevo (matriz de decisión)

Decidido en `CollectDeviceParameters()` (`vk_instance.cpp:922-969`). `TLG_BLOCK1D` y `TLG_BC_TINY` siguen existiendo con los mismos valores.

| Caso (sin variables) | `block1d_as_2d` | `bc_tiny_decode` | Efecto |
|---|---|---|---|
| El driver **soporta** BC 1D | off | off | **Sin cambios** (salvo driver AMD propietario, que ya decodificaba; ver abajo) |
| El driver rechaza BC 1D, **NVIDIA** | off (como hoy) | **ON (auto, nuevo)** | BC1-BC5 1D y <4 texels se decodifican en CPU; nunca se crea una imagen BC 1D. Fix del crash |
| El driver rechaza BC 1D, otros (AMD, Intel…) | on (default heredado) | **ON (auto, nuevo)** | Igual de seguro que AMD hoy: BC1-BC5 decodificados; la promoción 2D queda solo como fallback de formatos que el decoder no maneja (BC6/BC7) |
| `TLG_BLOCK1D=1` | on | off (auto anulado) | Promoción 2D comprimida, experimento puro; **puede repetir el HUD/fuentes** (documentado en el log) |
| `TLG_BC_TINY=1` | según `TLG_BLOCK1D` | on | Fuerza la decodificación en cualquier GPU |
| `TLG_BC_TINY=0` | según `TLG_BLOCK1D` | off | Vuelve al camino comprimido (solo para A/B) |
| `TLG_BC_TINY=auto` | según `TLG_BLOCK1D` | = decisión automática | Alias explícito de lo que el port haría solo |

Detalles de precedencia:

- `bc_tiny_auto_on = bc1d_refused && !block1d_forced` (`:957-958`): si el usuario pidió explícitamente la promoción (`TLG_BLOCK1D=1`), el default automático no decodifica, para que el experimento siga aislado (una variable).
- `TLG_BLOCK1D=0` **no** desactiva la decodificación: el driver sigue sin soportar BC 1D y la ruta segura debe mantenerse (`block1d_forced` es solo `=1`).
- GPUs que soportan BC 1D: `bc1d_refused = false` → `bc_tiny` automático = `false`; comportamiento intacto. (Único caso preexistente que no cambia: driver AMD propietario, que ya decodificaba por defecto.)
- Log nuevo, una sola vez (`:966-968`), solo cuando la decisión es automática y el driver no soporta BC 1D:

  ```
  GPU: BC 1D unsupported by driver; decoding BC1-BC5 1D and under-4-texel BC on the CPU (auto; TLG_BC_TINY=0 or TLG_BLOCK1D=1 override)
  ```

- El resto del pipeline no cambia: `image_info.cpp:170-195` ya convierte las texturas BC1-BC5 1D/<4 texels a planas (`props.is_bc_decoded`), `texture_cache.cpp:1527-1605` las rellena desde memoria emulada con el decoder de `texture_cache/tlg_bc_decode.h`, y `resource.h:329-341` (`IsBlock1D`) mantiene las imágenes decodificadas como 1D (nunca promocionadas). El flag entra en la clave de caché de pipelines (`vk_pipeline_cache.cpp:656-657`), así que no hay blobs obsoletos.

## 4. Archivos y líneas cambiadas

| Archivo | Cambio |
|---|---|
| `port/gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp` | `DriverSupportsBc1D()` en ns anónimo (`:101-143`); sonda + default automático + logs en `CollectDeviceParameters()` (`:922-938`, `:948-969`) |
| `port/gpu/shadps4/video_core/amdgpu/tlg_bc_tiny.h` | Comentario: el default ahora es automático en todo driver que rechace BC 1D |
| `port/gpu/shadps4/video_core/amdgpu/tlg_block1d.h` | Comentario: la promoción 2D ya no es el camino seguro por defecto; sigue siendo el experimento `TLG_BLOCK1D=1` |
| `port/gpu/shadps4/video_core/amdgpu/resource.h` | Comentario de `IsBlock1D()` actualizado (sin cambio funcional) |
| `docs/analysis/bc1d-auto-fix.md` | Este documento |

No se tocó `image_info.cpp`, `image.cpp` ni `texture_cache.cpp`: el camino de decodificación ya estaba completo y medido; el cambio es solo cuándo se activa.

## 5. Verificación

### 5.1 Compilación

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /c/Users/luisd/proyectos/infamous-port/port && bash build.sh'
```

Resultado (2026-10-19): build incremental OK; `vk_instance.cpp.obj` recompilado y `tlg-probe.exe`/`tlg-play.exe` enlazados y copiados a `port/out/`. Comprobación adicional con los flags exactos de CMake (`clang++ -fsyntax-only`): `SYNTAX_OK`. `bc-decode-test` (decoder CPU que usa este camino) compila y pasa (`bc-decode-test: ok`).

### 5.2 Run del usuario (sin variables de entorno)

Arranque normal (`AGENTS.md` §5) **sin** `TLG_BLOCK1D` ni `TLG_BC_TINY`. En el log (`port\out\logs\run-*.log`) deben aparecer, en orden:

```
GPU: block-compressed 1D textures kept 1D although the driver refuses them (TLG_BLOCK1D=1 makes them 2D, which can repeat HUD/fonts)
GPU: tiny block-compressed textures (BC1-BC5 1D, or under 4 texels) decoded on the CPU to plain images, 1D kept 1D (TLG_BC_TINY=0 keeps BC)
GPU: BC 1D unsupported by driver; decoding BC1-BC5 1D and under-4-texel BC on the CPU (auto; TLG_BC_TINY=0 or TLG_BLOCK1D=1 override)
```

Criterios de aceptación (mismos que el run BC_TINY ya medido):

1. No aparece `Fault:` ni `access to` al arrancar, durante ≥180 s.
2. La UI/HUD/fuentes se ven nítidos en capturas a 75/135/165 s (comparar con `evidence/20261019-111234-bc-tiny-180s`).
3. No se repite la línea `<Error> ... image format Bc1RgbaUnormBlock type 1D is not supported` en la fase de arranque (los BC1-5 1D ya no llegan al driver; si queda alguna, será de un formato fuera de BC1-5 o de una textura macro-tiled: ver §6).

A/B de control:

- `TLG_BLOCK1D=0 TLG_BC_TINY=1` debe producir **las mismas** decisiones que el arranque automático (mismos logs de sonda; el pipeline cache key es idéntico). Es la prueba de que el auto equivale al workaround probado.
- `TLG_BLOCK1D=1` debe volver a mostrar las franjas y **no** imprimir la línea `(auto)`; es la prueba de que el override sigue aislado.

### 5.3 Tests

- `ninja -C out/gpu bc-decode-test && out/gpu/bc-decode-test` (target `EXCLUDE_FROM_ALL`, `gpu/CMakeLists.txt:420-422`, fuente `tests/test_bc_decode.cpp`): valida el decoder CPU (BC1-BC5) que usa el camino automático. El diff de este parche no toca el decoder, pero confirma que la ruta que se activa sigue sana.
- La decisión en sí depende del dispositivo (sonda Vulkan) y no tiene test unitario: se valida con la línea de log del run real (§5.2) y con A/B.

## 6. Riesgos y límites

1. **Texturas macro-tiled:** el camino BC_TINY de `image_info.cpp:170-195` exige `!AmdGpu::IsMacroTiled(array_mode)`. Una BC 1D macro-tiled no se decodifica; en NVIDIA (sin promoción) seguiría creándose como BC 1D rechazada, igual que con `TLG_BC_TINY=1`, que fue lo medido (180 s sin crash). Si apareciera un fallo asociado, la palanca es `TLG_BLOCK1D=1` (con la contrapartida visual) o extender el decoder a macro-tiling.
2. **BP09 (`TLG_BP09=1`, `Image1DAs2D`):** promociona *toda* imagen 1D, antes de la decodificación (`resource.h:336-337`), así que con BP09 activo los BC 1D toman el camino 2D comprimido aunque el auto-decode esté on. Es una elección explícita del usuario; el default de BP09 sigue off.
3. **Coste de la decodificación:** solo texturas 1D o menores de un bloque (bytes despreciables: RGBA8 4×4 = 64 B frente a 8/16 B comprimidos). Ya era el default en AMD y el run BC_TINY de 180 s no mostró regresión.
4. **Drivers antiguos donde la promoción no hacía falta:** si un driver relativo a NVIDIA respondía «no soportado» pero muestreaba bien (el caso que documentaba el comentario viejo), ahora se decodifica en CPU. El resultado visual debe ser idéntico (la decodificación es la correcta de BC), con coste mínimo; `TLG_BC_TINY=0` lo revierte.
5. **Sonda conservadora:** si un solo formato BC1-BC5 1D falla, se decodifican todos los pequeños. Es la dirección segura (nunca crear lo rechazado) a cambio de algo de CPU.
6. **Textos del launcher desactualizados (actualizado):** `port/launcher/backports_table.py:213-218` describía los defaults viejos («excepto NVIDIA», «solo AMD»); ya dicen que la decodificación automática cubre a todo driver que rechace BC 1D. La lógica automática no depende de ello porque el preset «Automatic» no exporta las variables.
7. **No re-medido en AMD/Intel:** en AMD el default ya decodificaba (sin cambio funcional esperado); en Intel/otros que rechacen BC 1D la decodificación sustituye a la promoción como camino primario, con la promoción de respaldo activa para formatos no decodificables.
