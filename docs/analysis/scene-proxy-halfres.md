# La escena a media resolución (el *scene proxy*): diagnóstico, números y qué se ha cambiado

Fecha: 2026-10-21. Alcance: `renderer_vulkan/vk_scene_resolution.{h,cpp}` y
`port/tests/**` (el rasterizador y el resto del renderer quedan fuera de alcance: se
proponen los parches, no se aplican).

## 1. Resumen (lo que hay que saber antes de tocar el preset)

1. **El "reduced-size draws 0/frame de 140" no es un fallo del *proxy*: es que el *proxy*
   está apagado.** El tamaño de la escena lo fija el preset de resolución; con Native AA
   (preset 0, el valor por defecto de `tlg.ini`) ese tamaño es el propio 1920×1080 del
   invitado, `SceneTargets::Reduced()` es falso y **ningún** draw se reduce (ni hay imagen
   proxy ni copia). Con `preset=1` (Quality) el *proxy* sí se aplica: el log pasa a
   `reduced-size draws 65-73/frame de 175-197`.
2. **Con el *proxy* aplicado la imagen se rompe.** Las capturas de la escena SAFETY TRUST
   (las de `evidence/20261019-213303-g3-preset-calidad/automation-80s.png` frente a
   `evidence/20261019-212619-g3-nativo/automation-80s.png`) muestran el mismo encuadre con
   la imagen **corrupta**: ruido disperso, rectángulos blancos y bandas horizontales. La
   imagen nativa está limpia.
3. **El *proxy* además empeora el frame**: en esa misma escena, 7,0 FPS con el modo nativo
   frente a 6,3 FPS con `preset=1` (−10 %), y el hilo de la GPU pasa de 2,6-3,4 % a
   6,5-7,1 % de tiempo bloqueado (el HUD: 16 % → 31 % de GPU). Se renderizan un 44 %
   menos de píxeles y aun así la GPU trabaja el doble: lo que cuesta son las copias extra
   (los *fills* y los *resolves*, con el resample del depth por shader).
4. **Por tanto, el orden correcto es: arreglar la imagen primero, y después (y solo si) usar
   el *proxy* para bajar el trabajo de la GPU.** Este agente deja el diagnóstico, los
   contadores que lo miden y una parte de las correcciones; los parches que faltan están
   en el renderer (§5).

## 2. Por qué el log dice "0/frame de 140": la cadena exacta

```
TemporalUpscaler::OnFrameStart()            vk_temporal_upscaler.cpp:329
    wanted = (active || preset > 0) ? ForPreset(preset, base) : Size{};
    scene_targets.SetSize(wanted);          vk_temporal_upscaler.cpp:331
        SceneTargets::SetSize()              vk_scene_resolution.cpp
            if (next == size) return false;  // el tamaño por defecto es {1920, 1080}
    SceneTargets::Reduced()                  size != Size{1920, 1080}
Rasterizer::BeginRenderingFull()             vk_rasterizer.cpp:3526
    bool reduced = scene_started && upscaler->RasterScaling() && key.num_samples == 1;
        TemporalUpscaler::RasterScaling()    vk_temporal_upscaler.cpp:264
            !scaled_session && scene_targets.Reduced() && !done_this_frame
```

En la sesión medida (`evidence/20261020-225008-clk`): `tlg.ini` sin `preset` (o sea,
Native AA) y `ISS_UPSCALER=off` (lo fuerza el launcher), luego

* `active` = falso (no hay upscaler temporal),
* `preset` = 0 → `wanted` = `Size{1920,1080}` = el valor por defecto → `SetSize` no cambia
  nada → `Reduced()` = **falso** → `RasterScaling()` = **falso** → `reduced` = **falso** en
  todos los draws.

`scene_draws` (los 140-197 del log) cuenta los draws *después* de que empieza la escena, no
los reducidos: por eso el "0 de 140".

**Cómo se enciende:** con `preset=1` en el `tlg.ini` (o `-IniExtra @{preset='1'}` en
`tools/run-baseline.ps1`). No hace falta el upscaler temporal: el tamaño de la escena sigue
al preset desde `1318134` ("el dial de resolución reduce la escena").

## 3. Qué pasa cuando el *proxy* sí se aplica (preset 1)

Medido en `evidence/20261019-213303-g3-preset-calidad` (Quality, 720p, upscaler apagado):

```
Scene resolution: raster 1280x720, half-resolution targets 640x360, UI 1920x1080 (live)
Frame stats: ... reduced-size draws 65/frame of 175 in the scene
Frame stats: ... reduced-size draws 69/frame of 197 in the scene
scene proxies sampled directly: 1909 bindings
```

Es decir: **de ~180 draws de la escena se reducen ~70**. El resto lo excluye el rasterizador
(§5, parche B), no el *proxy*.

Coste en la escena SAFETY TRUST (las mismas dos sesiones, la misma escena a los 80 s):

| | FPS (mediana de la escena) | hilo GPU bloqueado | HUD |
|---|---|---|---|
| nativo (preset 0) | **7,0** | 2,6-3,4 % | GPU 16 % |
| Quality (preset 1) | **6,3** | 6,5-7,1 % | GPU 31 % |

Y la imagen: `automation-80s.png` de las dos sesiones, lado a lado. La del preset 1 tiene
ruido disperso, rectángulos blancos (los restos de debris, con el *bloom*GUI) y bandas
horizontales; la nativa está limpia.

Causas candidatas (en orden de sospecha, todas medibles con lo que este agente deja):

1. **Los mip levels del *proxy*.** La imagen proxy tiene **un solo nivel** (`ci.mipLevels = 1`
   en `vk_scene_resolution.cpp`), pero el invitado la muestrea con LOD implícito: el
   hardware recorta el LOD a 0 y el shader se queda con el nivel más nítido en vez del
   nivel difuminado que pedía. Un *bloom* sampled así sale con los bordes duros (los
   rectángulos blancos) y el resto de la escena con el aliasing de la resolución.
2. **La copia nativa↔proxy es del vecino más próximo.** `Copy()` usa `vk::Filter::eNearest`
   ("nearest preserves depth/stencil"), así que la copia nativa→proxy (el *fill*) y la
   proxy→nativa (el *resolve*) son nearest: al escalar, la imagen nativa que leen los
   shaders que hacen `texelFetch` (los que el puerto marca `needs_native`) sale escalonada.
   **Este agente lo ha cambiado a filtrado bilineal para el color** (§4.3).
3. **El *LOD bias* de la escena solo se aplica con el upscaler temporal.**
   `TemporalUpscaler::SceneMipBias()` devuelve 0 si `!Active()`, y con el upscaler apagado
   (nuestro caso) las texturas de material del G-buffer se muestrean con el LOD de 1920×1080
   sobre una escena de 1280×720: aliasing garantizado (§5, parche A).
4. **Los tamaños que no son 1920×1080 ni 960x540 nunca eran elegibles.** Si el juego tiene
   un buffer de post-proceso de otro tamaño (1024×576, 1280×720...), el *proxy* lo dejaba
   **nativo** mientras el resto de la escena se reducía: la cadena de post mezcla una
   imagen reducida y otra nativa y la relación de tamaños no cuadra. **Este agente lo ha
   arreglado** (§4.2): ahora cualquier objetivo que sea una vista del frame del invitado
   (la misma relación de aspecto) escala con la escena.

## 4. Lo implementado en este ámbito

### 4.1 Los contadores: por qué no se reduce un draw (`vk_scene_resolution.{h,cpp}`)

`SceneTargets` lleva ahora un bloque de contadores (`SceneTargets::Stats`) que se imprimen
cada 2 s en la línea `Scene proxy:` del log (con `TLG_FRAME_STATS=1`, que el harness pone
siempre):

```
Scene proxy: eligible 412, size 40, format 12, mips 8; size changes 6; proxies 6,
fills 4, resolves 12, samples 210, drawn 96
```

* `eligible`: los chequeos de elegibilidad que pasaron (uno por adjunto y por frame).
* `size`/`format`/`mips`/`samples`/`layers`/`volume`: los que **no** pasaron, con el motivo.
* `size changes`: el tamaño del proxy cambió entre dos chequeos (una pasada que mezcla
  tamaños no se puede reducir: es la regla `same_size` del rasterizador).
* `proxies`/`fills`/`resolves`/`samples`/`drawn`: la vida de los proxies (creados, copias
  nativa→proxy, proxy→nativa, muestreados, dibujados).

Con esto, la siguiente sesión dice **exactamente** qué impide que un draw se reduzca, sin
tocar el rasterizador.

### 4.2 La elegibilidad: cualquier objetivo que sea una vista del frame (`vk_scene_resolution.cpp`)

Antes solo eran elegibles 1920×1080 (`div` 1) y 960×540 (`div` 2); el resto de tamaños
nunca. Ahora:

```cpp
bool SceneTargets::ScalesWithTheScene(const VideoCore::ImageInfo& i) {
    if (i.size.width == 0 || i.size.height == 0) return false;
    // La misma relación de aspecto que el frame del invitado (1920x1080).
    return 1920ull * i.size.height == 1080ull * i.size.width;
}
SceneResolution::Size SceneTargets::ProxySizeFor(const VideoCore::ImageInfo& i,
                                                 SceneResolution::Size scene, u32 level) {
    ...
    // El proxy del objetivo con el factor de la escena, redondeado a un tamaño par.
    proxy = {native_w * scene.width / 1920, native_h * scene.height / 1080} >> level;
}
```

Todos los objetivos del juego que son vistas del frame (1920×1080, 960×540, 480×270,
1024×576, 1280×720...) escalan ahora con la escena, y el factor es el mismo para todos (es
lo que hace que la conversión de FragCoord del shader siga siendo correcta: 1920/scene
vale para todos). Los *shadow maps* y las texturas (otra relación de aspecto) siguen sin
proxy ✓.

El conmutador `SceneHalfRes` (bit 50) sigue gating los objetivos **más pequeños** que el
frame (la cadena de post a media resolución); el 1920×1080 no depende de él, como antes.

### 4.3 Las copias con filtro (`vk_scene_resolution.cpp`)

El `fill` (nativa→proxy) y el `resolve` (proxy→nativa) del **color** usan ahora
`vk::Filter::eLinear` cuando el formato lo soporta (`eSampledImageFilterLinear`); el
depth/stencil y los formatos enteros siguen con el vecino más próximo (no se pueden
filtrar). Antes, el fill de una escala usaba el vecino más próximo: la imagen que el
invitado dibuja encima salía aliasada.

### 4.4 El test (`port/tests/test_scene_proxy.cpp`, target `scene-proxy-test`)

* Sin dispositivo: qué tamaños escalan con la escena y de qué tamaño es su proxy
  (1920×1080 → la escena; 960×540 → la mitad; 480×270 → la cuarta; 1024×576 → 682×384;
  1280×720 → 852×480; un 1024×1024 o un 512×512 no escalan) y los niveles de mip.
* Con dispositivo (`TLG_SCENE_PROXY_GPU=1`): dibujar en el proxy, resolverlo de vuelta y
  comprobar que la imagen es la misma, y que una escritura nativa invalida el proxy (la
  coherencia). **En este host (Windows) la parte con dispositivo no se puede ejecutar**: la
  instancia de Vulkan del port no se puede crear en un binario de test (el
  `VkLayerSettingsCreateInfoEXT` está siempre en la cadena `pNext` de la instancia pero la
  extensión que lo define solo se activa si una capa la trae: `vk_platform.cpp`, §5 parche
  C). El CI (Linux) sí la ejecuta; el test está compilado y verificado en verde en la parte
  sin dispositivo.

## 5. Los parches que faltan (en el renderer; no aplicados)

### A. El *LOD bias* de la escena, también sin upscaler temporal (`vk_temporal_upscaler.cpp`)

Hoy el sesgo de LOD de las texturas de material solo se aplica con el upscaler temporal
(`SceneMipBias()` devuelve 0 si `!Active()`), pero es justo cuando la escena se reduce sin
upscaler (nuestro caso) cuando hace falta: sin él, el G-buffer de 1280×720 muestrea las
texturas con el LOD de 1920×1080 y la escena sale con aliasing.

```cpp
// vk_temporal_upscaler.cpp
float TemporalUpscaler::SceneMipBias() const {
    if (TlgSettings::Get().upscaler == TlgSettings::UpscalerTaa) return 0.0f;
    const float render = float(TlgSettings::Get().active_render_width.load());
    const float output = float(Scaled() ? target_width : 1920u);
    if (!Active()) {
        // Sin upscaler temporal el reescalado lo hace el post del propio juego: el sesgo
        // tiene que ser el de la escena reducida (muestrear más difuminado).
        return render > 0.0f && render < output ? std::log2(output / render) : 0.0f;
    }
    return render > 0.0f && render < output ? std::log2(render / output) : 0.0f;
}
```

(Ojo con el signo: con el upscaler temporal (FSR) el sesgo es negativo (el upscaler
reconstruye el detalle); con el post del juego es positivo (hay que difuminar más). Hay que
medirlo con capturas: el objetivo es que el preset 1 no se vea ni peor ni mejor que el
nativo.)

### B. Contar los draws de la escena que **no** se reducen, con el motivo (`vk_rasterizer.cpp`)

El log ya dice cuántos se reducen (`reduced-size draws X/frame of Y`), pero no por qué se
saltan los otros. En `BeginRenderingFull` (vk_rasterizer.cpp:3550-3571) ya se calcula
todo: falta contarlo.

```cpp
// vk_rasterizer.cpp, en BeginRenderingFull
const char* not_reduced = nullptr;   // el motivo, para las estadísticas
for (u32 cb = 0; cb < num_attachments; ++cb) {
    ...
    if (!scene_targets->Eligible(image)) not_reduced = "not eligible";
    else if (image.binding.is_bound) not_reduced = "sampled and target";
    else if (image.binding.needs_rebind) not_reduced = "needs rebind";
    else if (desc.view_info.range.base.layer) not_reduced = "layer";
    else if (!same_size(image, desc.view_info.range.base.level)) not_reduced = "mixed sizes";
}
if (db_desc.first && (!scene_targets->Eligible(texture_cache.GetImage(db_desc.first)) ||
                      !same_size(texture_cache.GetImage(db_desc.first), 0))) {
    not_reduced = "depth";
}
if (scene_started && !reduced && not_reduced) {
    TlgStats::scene_not_reduced[reason_index(not_reduced)]++;
}
```

### C. La instancia de Vulkan sin capa (`vk_platform.cpp`)

`CreateInstance` mete siempre el `VkLayerSettingsCreateInfoEXT` en la cadena `pNext`, pero
la extensión `VK_EXT_layer_settings` solo se activa si una capa la trae
(`GetLayerExtensions`). Sin la extensión, el driver rechaza la cadena y la instancia no se
crea: eso es lo que impide ejecutar los tests de Vulkan en Windows.

```cpp
// vk_platform.cpp: solo añadir el struct a la cadena si la extensión está activa
const bool layer_settings = std::find(extensions.begin(), extensions.end(),
                                      VK_EXT_LAYER_SETTINGS_EXTENSION_NAME) != extensions.end();
vk::StructureChain<vk::InstanceCreateInfo, vk::LayerSettingsCreateInfoEXT> chain{
    vk::InstanceCreateInfo{...},
    vk::LayerSettingsCreateInfoEXT{...},
};
auto [instance_result, instance] = layer_settings
    ? vk::createInstanceUnique(chain.get())
    : vk::createInstanceUnique(vk::InstanceCreateInfo{...});
```

### D. Los mip levels del *proxy* (`vk_scene_resolution.cpp`, el fichero de este agente)

La imagen proxy tiene un solo nivel (`ci.mipLevels = 1`), así que el LOD implícito del
invitado se recorta a 0 y las texturas con mips salen demasiado nítidas (el *bloom* con los
bordes duros). El arreglo sería dar a la proxy los mismos niveles que la nativa (el proxy
del nivel L = `scene/d >> L`) y que la vista del proxy cubriera el rango del invitado
(`{aspect, base_level, levels, 0, 1}`), con el relleno y el resolve por nivel. Es un
cambio de estructura (hoy hay una imagen proxy por nivel) y queda propuesto, no aplicado:
sin una sesión que lo mida no se puede saber si el ruido que se ve es esto o el LOD bias
del parche A.

## 6. Cómo medirlo (para la sesión que hace el director)

Antes (el estado actual, sin *proxy*):

```powershell
tools\run-baseline.ps1 -DurationSec 300 -Tag scene-proxy-antes
```

Después (con el *proxy*):

```powershell
tools\run-baseline.ps1 -DurationSec 300 -Tag scene-proxy-despues -IniExtra @{ preset = '1' }
```

Lo que hay que mirar en los dos logs:

* `reduced-size draws X/frame of Y`: con preset 1 debe salir X > 0 (hoy, con el código
  viejo, 65-73 de ~180).
* La línea nueva `Scene proxy:`: `eligible` y los motivos (`size`, `format`, `mips`...) y
  los `size changes` (las pasadas que mezclan tamaños).
* `Frame stats:` para el FPS y el % del hilo GPU.
* Las capturas (`-ScreenshotIntervalSec 30`) de la misma escena: la imagen **no** puede
  salir como la de `20261019-213303-g3-preset-calidad` (ruido, rectángulos, bandas).

## 7. Lo que queda abierto

* La imagen corrupta con el *proxy*: los parches A y D son las dos sospechas medibles; el
  contador `size` de §4.1 dirá si el juego tiene objetivos que hasta ahora no eran
  elegibles (que era una de las causas: reducir unos búferes y no otros).
* Por qué solo se reducen ~70 de los ~180 draws de la escena: lo dicen los contadores
  nuevos y el parche B.
* El coste extra de la GPU con el *proxy* (las copias y el resample del depth): con el
  *depth* muestreado desde la proxy (en vez de resolverlo a la nativa) se ahorraría el
  *resolve* por frame, pero es un cambio de coherencia que hay que medir.
* El `tlg_settings.*` no se ha tocado: el preset es lo que enciende el *proxy* y no hace
  falta un conmutador nuevo. El conmutador `SceneHalfRes` (bit 50) sigue siendo el que
  decide si los objetivos más pequeños que el frame siguen a la escena.
