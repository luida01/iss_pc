# Plan de implementación — Port nativo de inFAMOUS Second Son

**Juego objetivo:** inFAMOUS Second Son, PS4, CUSA00223, base 1.00 + update 1.07  
**Referencia principal:** port nativo de God of War III Remastered (`FelipePrado0/gow3_pc`)  
**Equipo objetivo:** 2–4 personas, o 1 persona + agentes de OpenCode  
**Hardware de referencia:** i5-13420H, 16 GB RAM, RTX 3050 Laptop 4 GB  
**Fecha:** 19 de octubre de 2026  
**Estado:** plan de implementación; no incluye código ni archivos del juego.

> Este documento no distribuye ni promueve la distribución de código, arte, audio, vídeo, firmware ni datos de Sony Interactive Entertainment o Sucker Punch Productions. El port trabaja únicamente con un volcado propio del juego y no debe eludir protecciones ni DRM. Revisa la legislación de tu país antes de publicar cualquier resultado.

---

## 0. Decisión en una página

### 0.1 Hallazgo que cambia el plan

El port de God of War III **no es una reimplementación desde cero del motor del juego**. Es un **runtime nativo específico del juego**:

- El `eboot.bin` original de PS4 se carga y se ejecuta **nativamente** en el CPU x86-64 del PC. No hay emulación de CPU.
- Un runtime propio reimplementa las bibliotecas del sistema PS4 (memoria, hilos, ficheros, audio, vídeo, mandos, saves, servicios).
- La GPU no se emula: los comandos GNM/PM4 y los shaders GCN se traducen a **Vulkan/SPIR-V** usando el núcleo de vídeo de shadPS4.
- Encima hay trabajo específico del juego: parches en memoria, hooks, correcciones de shaders, texturas, sincronización y rendimiento.

Y el hallazgo importante: **ya existe un port nativo de inFAMOUS Second Son construido exactamente con esa misma receta**:

- Repo: <https://github.com/HayatoG/iss_pc>
- Licencia: GPL-2.0 (ver `port/LICENSE`).
- Juego: CUSA00223, base 1.00 + update 1.07.
- Estado: boot, título, intro y gameplay; probado hasta “Go to Longhouse”.
- Rendimiento: 20–30 FPS típicos en una RTX 2060, inestable.
- Windows: probado por jugadores; tiene límites conocidos con GPUs de 4 GB de VRAM.
- Base: `tlg_pc`, `gr2_pc`, `gow3_pc`, `bloodborne_pc` y el núcleo de vídeo de shadPS4.
- Incluye `port/src/runtime_infamous.c`, audio propio, saves, sensor de movimiento, touchpad simulado, launcher Qt y 12 correcciones experimentales (BP01–BP12).

Por tanto, “replicar el port de GOW3 para inFAMOUS” ya no significa empezar de cero. Significa:

1. **Usar `iss_pc` como base de producción** (es el mismo enfoque, ya aplicado al juego correcto).
2. **Usar `gow3_pc` como referencia de arquitectura y de ingeniería** para entender el loader, el runtime, los parches y el packaging.
3. **Aprovechar tu fork de shadPS4** como laboratorio de GPU, rendimiento y pruebas A/B, y portar a `iss_pc` las mejoras que hayas validado.

### 0.2 Recomendación

**Ruta A híbrida (recomendada):**

- **Producción:** fork de `iss_pc`.
- **Laboratorio:** tu fork `Emulador-SecondSon` de shadPS4.
- **Aprendizaje:** una spike de 1–2 semanas con `gow3_pc`, sin intentar reescribir lo que ya existe.
- **Primer paso obligatorio:** probar la release oficial de `iss_pc` en tu RTX 3050 de 4 GB. Si no arranca o no es jugable por VRAM, el plan cambia antes de invertir meses.

**No recomendado:** empezar un port desde cero copiando solo la idea de GOW3. Es un proyecto de 12–24 meses para una persona, con alto riesgo de reimplementar lo que `iss_pc` ya resolvió.

### 0.3 Criterio de éxito inicial

Antes de escribir código nuevo, debes conseguir:

- Build reproducible de `iss_pc` desde fuente en Windows/MSYS2 CLANG64.
- Ejecución de 15–30 minutos en tu equipo, con logs y métricas.
- Identificación clara del cuello de botella principal en 4 GB de VRAM.
- Un harness que ejecute, registre y compare pruebas con una sola variable.

Si esto no se cumple, no tiene sentido empezar la fase de rendimiento.

### 0.4 Qué hacer con tu trabajo actual

Tu carpeta `Emulador-SecondSon` no se tira: se convierte en el laboratorio de GPU y rendimiento.

Mantener y reutilizar:

- El fork de shadPS4 0.18.1 con `second-son-laptop`.
- La ruta de caché host para rangos grandes y el crecimiento canónico de caché.
- Los modos de readback preciso/relaxed y su efecto en la iluminación.
- `SHADPS4_FAST_DIRTY_TRACKER`, `SHADPS4_FAST_CLEAN_REGION_CHECK` y las métricas de CPU/render/pipelines.
- `Test.ps1`, `CompareFrames.ps1`, `result.json`, capturas ABBA y el protocolo de una sola variable.
- Los controles Xbox, el sensor de movimiento simulado y el touchpad.

Plan de fusión:

1. Validar cada mejora en el fork de shadPS4 con una escena fija.
2. Portarla a `iss_pc` como flag aislado, no como cambio global.
3. Si la mejora es genérica del núcleo de vídeo, proponerla también a shadPS4.
4. No mantener dos renderers completos en paralelo: un solo núcleo compartido, con flags por juego.
5. Enfocar primero las mejoras que atacan el problema de 4 GB de VRAM, porque es el riesgo principal del hardware.

### 0.5 Estado real de `iss_pc` (19 de octubre de 2026)

No es un proyecto abandonado ni parado. Los datos del repositorio:

- Creado el **13 de octubre de 2026**; en menos de una semana tenía **622 estrellas**, **41 forks** y **168 issues abiertos**.
- En los últimos 7 días: **más de 100 commits**, **293 issues creados** y **123 cerrados**.
- Releases casi diarias entre el 16 y el 19 de octubre. La última incluye correcciones de niebla/agua/fuego, 30 idiomas, tope de presupuesto de texturas en GPUs integradas, mejoras de rendimiento sin cambiar la imagen y actualizador del launcher.
- El repo no está archivado.

Por qué puede *parecer* que se ha frenado:

- **Un solo mantenedor** (`HayatoG`, Guilherme) mantiene varios ports: `iss_pc`, `tlg_pc`, `gr2_pc`, `tlou_pc` y el nuevo `ps4_core`.
- **`ps4_core`**: está unificando la base de todos los ports. La roadmap dice que cuando todos usen ese núcleo, el prefijo interno pasará a `PSCORE_`. Es un refactor grande que consume tiempo y explica que algunas mejoras game-specific tarden.
- **Releases manuales**: los merges y push a `main` no publican nada; el mantenedor lanza el workflow de release a mano. Si pasan días sin publicar, el repo puede parecer quieto aunque haya commits.
- **Volumen de reportes**: 293 issues en 7 días, en su mayoría problemas de hardware (RX 580, GPUs de 4 GB, Intel iGPU, Arc, RTX 30/50). La triage consume mucho tiempo; parte la ayuda un colaborador de la comunidad (`Andretuta`).
- **Trabajo asistido por agentes**: hay commits y PRs con ramas `codex/...`; el ritmo es alto, pero la revisión humana sigue siendo el cuello de botella.

Qué sigue abierto y por qué:

- **4 GB de VRAM**: sigue siendo el problema principal. La recomendación oficial es 6 GB. En el issue #75, una RX 6500 XT de 4 GB muestra caché de texturas de ~3.1 GB contra ~1.7 GB disponibles: el juego expulsa y recarga texturas constantemente. El último release limita el presupuesto en GPUs integradas, pero **no valida una RTX 3050 discreta de 4 GB**. En el issue #72 el mantenedor pide precisamente ese dato y si `ISS_BC_TINY=1` mejora las texturas.
- **Rendimiento**: la thread que procesa draws se satura en escenas densas; las lecturas GPU -> CPU y la compilación de shaders siguen en la roadmap.
- **Cobertura**: “Go to Longhouse” es el punto más lejano **verificado**, no una barrera declarada. Hay crashes en puntos concretos (por ejemplo, el issue #47, tras la misión del grafiti) y avisos de que algunas versiones del juego no están soportadas: el port solo garantiza **CUSA00223 con update 1.07**.
- **Seguridad**: el 19 de octubre se detectó un issue malicioso (#316) con un script PowerShell por etapas. El repo añadió un guardián automático. Descarga solo releases oficiales y no ejecutes scripts de issues.

Implicación para el plan:

1. El upstream se mueve rápido: tu fork debe tener **rebase frecuente** y parches aislados.
2. Contribuir upstream puede ser mejor que mantener un fork privado, sobre todo en el problema de 4 GB y rendimiento.
3. No esperes a que `iss_pc` esté “terminado” para empezar: nunca lo estará mientras siga en desarrollo.
4. Tu RTX 3050 de 4 GB es un **caso de prueba poco validado**; eso es una oportunidad real de contribución.

---

## 1. Objetivo, alcance y no objetivos

### 1.1 Objetivo

Conseguir un port nativo de inFAMOUS Second Son para Windows que:

- Use el `eboot.bin` 1.07 de un volcado propio.
- Arranque, llegue al título y al gameplay.
- Permita jugar la historia con controles, audio, vídeo, saves y ajustes gráficos.
- Tenga un rendimiento aceptable en el hardware objetivo.
- Sea reproducible, testeable y publicable con la licencia correcta.

### 1.2 Objetivos medibles

| Nivel | Resultado | Criterio |
|---|---|---|
| M0 | Boot y título | Arranque reproducible + logs + captura |
| M1 | Gameplay estable | 30 minutos sin crash crítico; save/load; mando y audio |
| M2 | Primer arco argumental | Hasta “Go to Longhouse” o equivalente, con cinemáticas |
| M3 | Historia principal | Según alcance acordado; 2 horas de soak sin fugas críticas |
| M4 | Rendimiento | 900p, 30 FPS de mediana, p95 < 50 ms, VRAM < 3,8 GB en RTX 3050 4 GB |
| M5 | Release | Instalador/launcher, checksums, fuente GPL publicada, sin archivos del juego |

M4 es el objetivo realista para 4 GB. 1080p/60 FPS es un “stretch goal”, no una promesa.

### 1.3 No objetivos

- Distribuir el juego, sus assets, firmware, claves o saves.
- Eludir DRM, firmas o protecciones de la consola.
- Reescribir el motor Decima desde cero.
- Emular el CPU de PS4 (el código ya es x86-64).
- Compatibilidad perfecta con todas las GPUs y todos los drivers.
- Una reimplementación clean-room si se usa código GPL de `iss_pc`/`gow3_pc`.

### 1.4 Restricción de hardware

`iss_pc` recomienda **6 GB de VRAM** y tiene un problema abierto con GPUs de **4 GB**. Tu RTX 3050 Laptop tiene 4 GB. Esto no impide el proyecto, pero cambia las prioridades:

- 720p/900p primero, 1080p después.
- Presupuesto explícito de VRAM y texturas.
- Reducción de resolución interna, FSR y cache de memoria pueden ser necesarios.
- No se debe confundir “bajar la ventana” con “bajar la resolución interna del juego”.

---

## 2. Qué significa técnicamente “replicar GOW3”

### 2.1 Arquitectura real

```text
eboot.bin (SELF/ELF) + libc.prx + libSceFios2.prx + assets
        |
        v
scripts/prepare.py  ->  relocations + link libc/Fios2 + imagen cargable
        |
        v
src/probe.c         ->  loader + hooks + arranque del código original x86-64
        |
        +--> src/runtime_*.c   -> HLE de bibliotecas PS4
        |       memoria, hilos, mutex, sema, ficheros, saves, audio,
        |       vídeo, mando, servicios, trofeos, PlayGo, AppContent...
        |
        +--> gpu/shadps4/      -> GNM/PM4 + shaders GCN -> Vulkan/SPIR-V
        |       buffer cache, texture cache, sincronización, readbacks,
        |       pipeline cache, formatos BC, barreras, presentación...
        |
        +--> launcher + packaging + logs + tests
```

### 2.2 Capas y equivalencias

| Capa | GOW3 (`gow3_pc`) | inFAMOUS (`iss_pc`) | Qué se reutiliza |
|---|---|---|---|
| Preparación | `scripts/prepare.py`, `link_modules.py`, `link_libc.py` | `port/scripts/prepare.py`, `link_modules.py`, `link_libc.py` | Misma idea; cambia el eboot y los módulos |
| Loader | `src/probe.c` | `port/src/probe.c` | Relocations, mapeo, hooks, entry point |
| Runtime | `src/runtime_*.c` | `port/src/runtime_*.c` + `runtime_infamous.c` | HLE de libs PS4; lo específico del juego cambia |
| GPU | `gpu/shadps4/` | `port/gpu/shadps4/` | Núcleo shadPS4 + fixes del juego |
| Parches | `patches/*.xml`, `scripts/patches.py` | `port/patches/*.xml`, `port/scripts/infamous_patches.py` | Parches por firma, en memoria |
| Launcher | `launcher/gow3_launcher_win.py` | `port/launcher/qt/` | Configuración, mods, saves, updates |
| Pruebas | `tests/`, `run.py` | `port/tests/`, `run.py` | Unit tests + pruebas con juego preparado |

### 2.3 Lo que es específico de inFAMOUS

No se copia tal cual de GOW3:

- `eboot.bin` y layout de memoria de CUSA00223 1.07.
- Imports/NIDs y bibliotecas que usa el juego.
- `runtime_infamous.c` y los servicios que el juego necesita.
- Shaders y estados GNM del motor Decima.
- Formatos de textura, iluminación, oclusión, partículas, niebla, agua y fuego.
- Save data, trofeos, DLC, idiomas y PlayGo.
- Parches y hooks del juego.

### 2.4 Lo que sí se aprende de GOW3

- Cómo se prepara un `eboot.bin` y se enlazan `libc`/`Fios2`.
- Cómo se implementa un loader que ejecuta código PS4 en Windows.
- Cómo se organiza un runtime HLE por módulos.
- Cómo se integra el núcleo de shadPS4 sin el resto del emulador.
- Cómo se aplican parches por firma sin tocar los archivos del juego.
- Cómo se empaqueta un port GPL con launcher, logs y tests.
- Qué problemas de Windows son inevitables: red zone, SSE4A/F16C, page file, drivers.

---

## 3. Rutas posibles

| Ruta | Base | Esfuerzo | Tiempo realista | Riesgo | Resultado | Cuándo elegirla |
|---|---|---|---|---|---|---|
| **A. Fork `iss_pc`** | Port nativo existente | Medio | 3–6 meses con 2–4 personas; 6–12 meses solo | Medio | Port nativo jugable y mantenible | Recomendada |
| **B. Fork `gow3_pc` desde cero** | Runtime de otro juego | Muy alto | 12–24 meses solo | Muy alto | Aprendizaje profundo; probablemente reimplementa `iss_pc` | Solo si el objetivo es aprender o si `iss_pc` no sirve |
| **C. Seguir con shadPS4** | Emulador | Bajo–medio | Ya funciona; mejoras continuas | Bajo | Juego jugable en emulador, no port nativo | Si priorizas jugar ya o experimentar GPU |
| **A+C híbrida** | `iss_pc` + tu fork shadPS4 | Medio | 3–6 meses | Medio | Port nativo + laboratorio de rendimiento | **Recomendada** |

### 3.1 Por qué la ruta B no es racional como primera opción

- `iss_pc` ya tiene loader, runtime, audio, saves, controles, launcher y GPU para el juego correcto.
- Tu fork de shadPS4 ya tiene trabajo validado de memoria, readbacks, controles y métricas.
- Empezar de cero obliga a redescubrir imports, formatos, shaders y bugs que ya están resueltos.
- El código de `gow3_pc` y `iss_pc` es GPL-2.0; si vas a reutilizarlo, tendrás que publicar tu fuente bajo GPL. Si quieres evitarlo, necesitas clean-room, que es aún más caro.

### 3.2 Cuándo sí hacer la ruta B

- Quieres aprender cómo funciona un port nativo de PS4, no solo tener el juego.
- Quieres una base propia con arquitectura distinta.
- `iss_pc` tiene un problema de licencia o de calidad que no puedes aceptar.
- Tienes 12+ meses y experiencia en C/C++, Vulkan, reversing y PS4.

En ese caso, haz solo una **spike de 2 semanas** con `gow3_pc` antes de decidir. No empieces la fase larga sin haber compilado, ejecutado y documentado GOW3.

---

## 4. Agentes necesarios

### 4.1 Topología

```text
                    +----------------+
                    | port-director  |
                    | plan y gates   |
                    +--------+-------+
                             |
        +--------------------+--------------------+
        |                    |                    |
  +-----v------+      +------v------+      +------v------+
  | recon      |      | runtime     |      | gpu         |
  | eboot/NIDs |      | HLE/loader  |      | Vulkan/GCN  |
  +-----+------+      +------+------+      +------+------+
        |                    |                    |
        +--------------------+--------------------+
                             |
                    +--------v--------+
                    | qa + build      |
                    | tests/CI/release|
                    +-----------------+
                             |
                    +--------v--------+
                    | upstream-watch  |
                    | shadPS4/ports   |
                    +-----------------+
```

No empieces con 8 agentes. Empieza con **4**: director, recon, runtime, gpu. Añade build/qa cuando haya builds; upstream cuando haya releases.

### 4.2 Roles

| Agente | Misión | Entregables | Skills/MCP | Permisos |
|---|---|---|---|---|
| `port-director` | Planifica, prioriza, decide gates y coordina | Roadmap, ADRs, informes de gate | `ps4-native-port`; GitHub/gh | Edita docs; lanza subagentes; shell con aprobación |
| `recon` | Analiza `eboot.bin`, imports/NIDs, funciones y diferencias | `docs/analysis/imports.md`, `functions.md`, `runtime-gap.md` | REA, GhidraMCP, radare2 | Solo lectura de código; escribe en `docs/analysis/` |
| `runtime` | Loader, relocations, HLE, memoria, hilos, ficheros, saves, audio | Parches en `port/src/`, `port/scripts/`; tests | REA, GhidraMCP, x64dbg | Edita `port/src/`, `port/scripts/`, `tests/` |
| `gpu` | GNM/Vulkan, shaders GCN/SPIR-V, texturas, barreras, readbacks | Parches en `port/gpu/`, capturas RenderDoc, informes | RenderDoc MCP, REA, shadPS4 docs | Edita `port/gpu/`, `tests/` |
| `build` | MSYS2/CMake/CI, dependencias, packaging, reproducibilidad | Build scripts, CI, instalador, checksums | Shell, GitHub Actions | Edita `port/build.sh`, `packaging/`, `.github/` |
| `qa` | Harness, A/B, logs, regresiones, rendimiento | `outputs/evidence/`, `tests/`, informes | Harness MCP, RenderDoc | Edita `tests/`, `outputs/` |
| `upstream-watch` | Vigila shadPS4, `iss_pc`, `gow3_pc`, `tlg_pc`, `gr2_pc` | Lista de commits/PRs candidatos; parches | GitHub/gh, web | Solo lectura; no hace merge |
| `legal-docs` | Licencias, atribución, avisos, privacidad | `LICENSE`, `NOTICE`, `docs/legal.md` | GitHub | Edita docs; revisión humana |

### 4.3 Reglas de operación para todos los agentes

1. **Una variable por experimento.** No mezclar un cambio de memoria con un cambio de shader.
2. **Sin evidencia no hay afirmación.** Cada mejora necesita log, hash, captura o `result.json`.
3. **Conservar originales.** Nunca sobrescribir logs, binarios o capturas de una prueba anterior.
4. **No meter el juego en Git.** `eboot.bin`, `*.prx`, `*.pkg`, `art/`, `user/`, saves, claves y rutas personales quedan fuera.
5. **Parar tras dos intentos fallidos.** Reportar hipótesis, evidencia y siguiente paso; no entrar en bucles.
6. **Revisión humana en los gates.** Un agente no declara “jugable” ni “terminado” sin validación del usuario.
7. **GPL.** Si se copia o modifica código de `iss_pc`/`gow3_pc`, mantener licencia, atribución y fuente.
8. **Seguridad.** Solo releases oficiales; verificar checksums; no ejecutar binarios de mirrors desconocidos.

---

## 5. Skills

### 5.1 Skills externas recomendadas

| Skill | Para qué | Instalación | Notas |
|---|---|---|---|
| **REA / Reverse Engineer Anything** (`morluto/rea`) | Orquestar reversing con Ghidra; binarios nativos, JS, .NET, APK | `npx rea-agents setup --client opencode` | Es MCP + skill. En Windows, el análisis nativo con Ghidra es experimental (PE x86/x64/ARM); Hopper no está disponible. Para PS4 ELF conviene Ghidra directo. |
| **reverse-skill** (`zhaoxuya520/reverse-skill`) | Router de metodologías: binario, APK, JS, .NET, CTF, malware | Seguir README; en Windows `powershell -File skills/scripts/refresh-tool-index.ps1` | Útil como catálogo, no sustituye el conocimiento de PS4. No instalar todos los subskills. |
| **ps4-native-port** (propia) | Flujo del proyecto: arquitectura, builds, reglas, evidencias | Crear en `.opencode/skills/ps4-native-port/SKILL.md` | **Obligatoria**. Es el “manual operativo” de los agentes. |
| **decima-gpu** (propia, opcional) | Capturas RenderDoc, shaders, formatos, sincronización | Crear en `.opencode/skills/decima-gpu/SKILL.md` | Añadir cuando empiece la fase GPU. |

### 5.2 Qué debe contener `ps4-native-port`

- Mapa de capas (loader, runtime, GPU, launcher).
- Comandos exactos de build y test en Windows/MSYS2.
- Lista de archivos que nunca se commitean.
- Protocolo de experimentos A/B y formato de informe.
- Criterios de gate.
- Limitaciones de Windows: red zone, SSE4A/F16C, page file, 4 GB VRAM.
- Enlaces a `iss_pc`/`gow3_pc` y a los ADRs.
- Regla: no copiar código de fuentes no GPL.

### 5.3 Qué no hace una skill

Una skill **no añade herramientas**. Si el agente no tiene Ghidra, RenderDoc o el harness, la skill solo describe el flujo. Hay que instalar el MCP/CLI correspondiente.

---

## 6. MCPs

### 6.1 Mínimo recomendado

| MCP | Función | Instalación | Cuándo |
|---|---|---|---|
| **REA** | Reversing asistido + evidencias | `npx rea-agents setup --client opencode` | Fase 0–2 |
| **GhidraMCP** | Decompilar, xrefs, renombrar, analizar `eboot.bin` | `uvx --from git+https://github.com/LaurieWired/GhidraMCP ghidra-mcp --ghidra-server http://127.0.0.1:8080/` | Fase 1–2 |
| **RenderDoc MCP** | Capturas de frame, pipelines, recursos, shaders | `python -m renderdoc_mcp` + `RENDERDOC_MODULE_PATH` | Fase 3–4 |
| **ps4-port-harness** (propio) | Ejecutar, loguear, comparar, capturar, validar | Python/FastMCP en `tools/port_mcp` | Fase 1 en adelante |

### 6.2 Opcionales

| MCP | Función | Coste/riesgo | Cuándo |
|---|---|---|---|
| **radare2 MCP** | Triage rápido, scripting, gratis | Bajo; r2pm en Windows | Si no quieres abrir Ghidra para todo |
| **IDA Pro MCP** | Decompilación superior | Requiere licencia IDA Pro | Si ya la tienes |
| **x64dbg MCP** | Debug dinámico en Windows | Medio; plugin + permisos | Para crashes nativos |
| **Frida MCP** | Instrumentación dinámica | Medio; no esencial para este port | Hooks y experimentos dinámicos |
| **GitHub MCP** | Issues/PRs | Bajo; `gh` CLI ya basta | Fase 5 |

### 6.3 MCP propio: `ps4-port-harness`

Es el MCP de mayor valor porque cierra el ciclo de ingeniería. Debe exponer, como mínimo:

- `build(profile)` -> binario, SHA256, warnings.
- `run(label, duration_s, env)` -> `result.json`, log, captura.
- `tail_log(label, pattern)` -> líneas filtradas.
- `compare(label_a, label_b)` -> FPS, frame time, VRAM, RAM, diferencias de imagen.
- `capture_frame(label)` -> `.rdc` + resumen de pipelines.
- `shader_report(label)` -> pipelines nuevos, tiempos de compilación, cache hits.
- `check_no_game_assets()` -> falla si hay archivos del juego en Git.
- `issue_report(label)` -> informe listo para GitHub, sin rutas personales.

Puede reutilizar tu `second-son/Test.ps1`, `CompareFrames.ps1` y los `result.json` existentes. No reinventes el harness: envuélvelo.

### 6.4 Configuración de OpenCode

Añadir servidores locales en `opencode.jsonc`:

```jsonc
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "servers": {
      "ghidra": {
        "type": "local",
        "command": [
          "uvx",
          "--from",
          "git+https://github.com/LaurieWired/GhidraMCP",
          "ghidra-mcp",
          "--ghidra-server",
          "http://127.0.0.1:8080/"
        ],
        "timeout": { "startup": 60000 }
      },
      "renderdoc": {
        "type": "local",
        "command": ["python", "-m", "renderdoc_mcp"],
        "environment": {
          "RENDERDOC_MODULE_PATH": "C:\\Program Files\\RenderDoc"
        }
      },
      "port-harness": {
        "type": "local",
        "command": ["python", "-m", "tools.port_mcp"],
        "cwd": "."
      }
    }
  }
}
```

REA se configura solo con `npx rea-agents setup --client opencode`; no lo escribas a mano si el instalador ya lo hace.

Para comprobar:

```powershell
opencode mcp list
```

Si un servidor muestra `needs authentication`, abre `/mcps` y completa el flujo. No pegues tokens en `opencode.jsonc`; usa `{env:NAME}`.

### 6.5 Regla de contexto

Cada MCP consume contexto y tokens. No instales los seis a la vez. Empieza con **REA + Ghidra + harness**; añade RenderDoc cuando haya frames que depurar.

---

## 7. Fases, gates y entregables

### Fase 0 — Viabilidad y baseline (3–5 días)

**Objetivo:** saber si el port existente funciona en tu hardware antes de invertir en código.

Tareas:

1. Inventario del volcado: CUSA00223 1.00 + 1.07, `eboot.bin`, `libc.prx`, `libSceFios2.prx`, `sce_sys/`, `art/`, `playgo.pgm`, `sce_discmap*.plt`. Hashes SHA256.
2. Revisar licencia y requisitos de `iss_pc`; descargar solo la release oficial.
3. Ejecutar `iss_pc` en tu RTX 3050 4 GB:
   - 720p, 900p y 1080p.
   - “GPU accuracy: Fast” vs preciso.
   - Perfil recomendado y “como la release anterior”.
   - Medir FPS, frame time, VRAM, RAM, crashes.
4. Instalar MSYS2 CLANG64 y compilar `iss_pc` desde fuente.
5. Compilar `gow3_pc` (opcional, para aprender) y ejecutarlo con tu volcado propio.
6. Crear el informe de baseline.

**Agentes:** director, build, qa.  
**Skills/MCP:** `ps4-native-port`, shell, REA (si hay que inspeccionar el eboot).  
**Entregables:** `docs/baseline-isspc-4gb.md`, logs, capturas, hashes, decisión.  
**Gate G0:** 15–30 minutos de gameplay en 4 GB, o bloqueo documentado con workaround. Si no se puede, decidir: seguir con shadPS4, usar otra GPU, o reducir alcance.

### Fase 1 — Teardown y harness (1–2 semanas)

**Objetivo:** entender la arquitectura y automatizar la experimentación.

Tareas:

1. Fork de `iss_pc` con nombre de trabajo (`iss-pc-lab`), conservando LICENSE y atribución.
2. Leer y documentar:
   - `port/scripts/prepare.py`, `link_modules.py`, `link_libc.py`.
   - `port/src/probe.c`, `runtime.c`, `runtime_infamous.c`, `runtime_memory.c`, `runtime_thread.c`, `runtime_file.c`, `runtime_pad.c`, `runtime_audio.c`.
   - `port/gpu/shadps4/` y `common/backports.h`.
   - `port/launcher/` y `docs/ROADMAP.md`.
3. Analizar `eboot.bin` con Ghidra/REA: imports, NIDs, TLS, entry point, funciones de inicialización.
4. Construir el mapa de imports -> implementación -> stubs.
5. Crear `ps4-port-harness` MCP y el protocolo A/B.
6. Crear ADRs: build, licencia, harness, estrategia de fork.
7. Configurar CI que compile y ejecute tests sin juego (unit tests).

**Agentes:** recon, runtime, gpu, build, qa.  
**Skills/MCP:** REA, GhidraMCP, harness.  
**Entregables:** `docs/architecture.md`, `docs/analysis/imports.md`, `docs/adr/0001..0004`, harness funcionando.  
**Gate G1:** build reproducible + harness ejecuta una prueba + mapa de imports + 3 ADRs.

### Fase 2 — Bring-up y estabilización (2–4 semanas)

**Objetivo:** port nativo estable en tu equipo, sin perseguir aún el rendimiento máximo.

Tareas:

1. Compilar y ejecutar el fork en Windows/MSYS2.
2. Resolver diferencias de toolchain/dependencias.
3. Verificar loader, relocations, libc/Fios2, memoria virtual y page file.
4. Reproducir el arranque hasta título e intro.
5. Implementar/corregir imports que falten, en orden de dependencia.
6. Validar mando Xbox, audio, vídeo, saves y carga de partida.
7. Prueba de 30 minutos con una escena fija y logs.

**Agentes:** runtime, build, qa.  
**Skills/MCP:** `ps4-native-port`, REA/Ghidra, harness.  
**Entregables:** `docs/bringup.md`, lista de imports resueltos, `result.json` de 30 min.  
**Gate G2:** 30 min de gameplay, save/load, mando/audio, sin crash crítico; 720p/900p.

### Fase 3 — Rendimiento y VRAM (4–8 semanas)

**Objetivo:** atacar los cuellos de botella reales, no optimizar a ciegas.

Prioridades alineadas con el roadmap de `iss_pc`:

1. **Readbacks GPU -> CPU:** la thread del juego espera `memcpy` de páginas modificadas por GPU. Medir y reducir.
2. **Compilación de shaders:** stutter de 64–118 compilaciones por ventana; cache de pipelines, compilación diferida.
3. **VRAM 4 GB:** presupuesto, texturas, BC 1D, fallback host cache, FSR.
4. **Sincronización:** barreras, semáforos, presentación; no eliminar sincronización necesaria.
5. **Oclusión:** modo 3 es una aproximación; medir occlusion queries reales.
6. **Efectos:** niebla, agua, fuego, brillo del dreno; ballot wave64, stencil, readbacks.

Aportes concretos de tu trabajo actual:

- `SHADPS4_LARGE_BUFFER_HOST_CACHE`, crecimiento canónico y presupuesto de 3200 MiB.
- Readbacks precisos/relaxed y su efecto en iluminación.
- `SHADPS4_FAST_DIRTY_TRACKER`, `SHADPS4_FAST_CLEAN_REGION_CHECK`, métricas de CPU/render.
- `Test.ps1`, `CompareFrames.ps1`, `result.json`, capturas ABBA.
- Correcciones de controles Xbox y sensor de movimiento.

**Agentes:** gpu, runtime, qa.  
**Skills/MCP:** RenderDoc, harness, REA.  
**Entregables:** informe de perfilado, top 5 cambios, curvas FPS/VRAM, capturas antes/después.  
**Gate G3:** 900p, 30 FPS mediana, p95 < 50 ms, VRAM < 3,8 GB, sin `DeviceLost`; checklist visual.

### Fase 4 — Contenido y paridad (4–8 semanas)

**Objetivo:** que el juego sea jugable más allá de la primera hora.

Tareas:

1. Avanzar capítulos y cinemáticas; registrar el primer bloqueo de cada zona.
2. Corregir HLE específico de inFAMOUS: servicios, saves, trofeos, DLC, PlayGo.
3. Validar audio (AudioOut/Ajm), vídeo (H.264), subtítulos e idiomas.
4. Validar sensor de movimiento, touchpad simulado y mando Xbox.
5. Pruebas de regresión por zona; 2 horas de soak.
6. Documentar límites conocidos.

**Agentes:** runtime, gpu, qa.  
**Skills/MCP:** harness, RenderDoc, Ghidra.  
**Entregables:** matriz de cobertura, lista de bugs, saves de prueba, informe de soak.  
**Gate G4:** alcance acordado (primer arco o historia principal) sin bloqueos críticos.

### Fase 5 — Release (2–3 semanas)

**Objetivo:** paquete público, legal y reproducible.

Tareas:

1. Launcher/installer, configuración, saves, actualizador.
2. Crash reporter y logs sin rutas personales.
3. Checksums SHA256, CI de release, build reproducible.
4. LICENSE, NOTICE, atribución a `iss_pc`, `gow3_pc`, shadPS4 y terceros.
5. Verificar que no se distribuyen archivos del juego.
6. Documentación de instalación y solución de problemas.

**Agentes:** build, qa, legal-docs.  
**Skills/MCP:** shell, GitHub.  
**Entregables:** RC, fuente GPL, checksums, notas.  
**Gate G5:** instalación limpia en otra máquina; sin assets; fuente publicada.

### Fase 6 — Mantenimiento (continuo)

- Seguir commits de `iss_pc`, `gow3_pc`, `tlg_pc`, `gr2_pc`, `bloodborne_pc`, shadPS4.
- Rebase mensual; mantener parches aislados con flags.
- Triage de issues; releases cuando haya hitos.
- No romper la compatibilidad de saves.

### Cronograma resumido

| Fase | Duración | Acumulado |
|---|---:|---:|
| 0. Viabilidad | 3–5 días | 1 semana |
| 1. Teardown + harness | 1–2 semanas | 2–3 semanas |
| 2. Bring-up | 2–4 semanas | 4–7 semanas |
| 3. Rendimiento | 4–8 semanas | 8–15 semanas |
| 4. Contenido | 4–8 semanas | 12–23 semanas |
| 5. Release | 2–3 semanas | 14–26 semanas |
| 6. Mantenimiento | continuo | — |

Con 2–4 personas: **3–6 meses** hasta un RC serio. Una persona sola: **6–12 meses**, según experiencia y alcance. La ruta B desde cero: **12–24 meses**.

---

## 8. Primeros 10 días (sprint 0)

| Día | Tarea | Salida |
|---|---|---|
| 1 | Checklist legal, inventario del dump, hashes | `docs/legal.md`, `docs/dump-inventory.md` |
| 2 | Instalar MSYS2 CLANG64; revisar `port/packaging/windows/README.md` | Entorno listo |
| 3 | Clonar y compilar `iss_pc` | Binario + SHA256 |
| 4 | Ejecutar release oficial en 4 GB; 720p/900p; logs | Baseline de FPS/VRAM |
| 5 | Repetir con build propio; comparar | Diferencia release vs fuente |
| 6 | Clonar/compilar `gow3_pc`; leer `README` y `docs/` | Nota de arquitectura |
| 7 | Instalar REA + GhidraMCP; abrir `eboot.bin` | Primer mapa de imports |
| 8 | Crear harness mínimo (`run`, `tail`, `compare`) | MCP local |
| 9 | Prueba A/B de una sola variable | Informe con evidencia |
| 10 | Gate G0/G1; decidir ruta | Go/No-Go |

Comandos de referencia (MSYS2 CLANG64):

```bash
git clone --recursive https://github.com/HayatoG/iss_pc.git iss_pc-lab
cd iss_pc-lab/port
ISS_LTO=OFF bash build.sh
```

```bash
git clone --recursive https://github.com/FelipePrado0/gow3_pc.git gow3_pc-lab
cd gow3_pc-lab
git checkout gow3   # rama de desarrollo; omite para main
bash build.sh
```

En PowerShell, para abrir el entorno:

```powershell
C:\msys64\msys2_shell.cmd -defterm -here -no-start -clang64
```

No copies rutas personales ni archivos del juego dentro del repo. Usa `paths.local.json` ignorado por Git, como ya haces en `Emulador-SecondSon`.

---

## 9. Backlog por épica

| Épica | Tareas | Agente | Criterio de aceptación |
|---|---|---|---|
| **E0. Legal/base** | Licencia, dump, hashes, inventario, política de no assets | director/legal | Checklist firmado; repo limpio |
| **E1. Build** | MSYS2, CMake, dependencias, build reproducible, CI | build | `build.sh` produce binario con SHA256; CI verde |
| **E2. Harness** | `run`, `tail`, `compare`, `capture`, `check_no_game_assets` | qa | 3 pruebas reproducibles; informes automáticos |
| **E3. Loader** | SELF/ELF, relocations, TLS, libc/Fios2, entry point | runtime | Arranque hasta título |
| **E4. HLE** | Memoria, hilos, mutex, sema, ficheros, servicios, saves, pad, audio, vídeo | runtime | 30 min de gameplay; save/load |
| **E5. GPU** | GNM/PM4, shaders, texturas, barreras, readbacks, presentación | gpu | Imagen correcta en escena fija; sin DeviceLost |
| **E6. VRAM** | Presupuesto, host cache, BC 1D, FSR, texturas | gpu | VRAM < 3,8 GB; sin OOM en 4 GB |
| **E7. Performance** | Perfilado CPU/GPU, shader cache, frame pacing | gpu/qa | 900p 30 FPS mediana; p95 < 50 ms |
| **E8. Contenido** | Capítulos, cinemáticas, DLC, trofeos, idiomas | runtime/qa | Alcance acordado; 2 h soak |
| **E9. Release** | Launcher, installer, updater, GPL, checksums | build/legal | RC instalable; fuente publicada |

---

## 10. Métricas y protocolo de pruebas

### 10.1 Métricas

| Métrica | Herramienta | Objetivo |
|---|---|---|
| Build hash | SHA256 | Reproducible |
| Tiempo a título | Log del port | < 60 s |
| Tiempo a gameplay | Log/captura | < 3 min |
| FPS mediana | `result.json`/overlay | ≥ 30 en 900p |
| Frame time p95 | `result.json` | < 50 ms |
| VRAM pico | nvidia-smi/overlay | < 3,8 GB |
| RAM privada | Process/overlay | < 8 GB |
| Crash-free | soak | ≥ 2 h |
| Stutter | pipeline metrics | < 10 compilaciones nuevas/min en zona caliente |
| Calidad visual | capturas fijas | Sin artefactos críticos |
| Cobertura | saves/checkpoints | Según alcance |

### 10.2 Protocolo A/B

1. Misma partida guardada, misma zona, misma cámara.
2. Misma configuración salvo **una** variable.
3. 180 segundos por ejecución; 3 repeticiones.
4. Mediana de FPS, p95, VRAM y RAM.
5. Registrar driver, temperatura/frecuencias y estado de caché.
6. Conservar logs y capturas originales.
7. No atribuir causalidad a una diferencia si el entorno cambió.

Tu `second-son/Test.ps1` y `CompareFrames.ps1` ya implementan gran parte de esto; reutilízalos.

---

## 11. Riesgos y mitigaciones

| Riesgo | Prob. | Impacto | Mitigación |
|---|---|---|---|
| GPU de 4 GB por debajo del mínimo | Alta | Alto | Probar primero; 720p/900p; host cache; presupuesto de texturas; FSR |
| Complejidad de Decima/open world | Alta | Alto | No prometer historia completa al inicio; gates por arco |
| Rendimiento insuficiente | Alta | Alto | Perfilar; atacar top 5; aceptar 30 FPS; no optimizar a ciegas |
| Stutter de shaders | Alta | Medio | Cache de pipelines, compilación diferida, prewarm |
| Divergencia con upstream | Alta | Medio | Fork pinned; rebase mensual; parches con flags |
| Licencia GPL | Media | Alto | Publicar fuente; atribución; no mezclar código incompatible |
| Legal/DMCA | Media | Alto | Volcado propio; sin assets; sin DRM; asesoría local |
| Windows: red zone/SSE4A | Media | Medio | Ya resuelto en `iss_pc`; tests en Intel |
| Drivers NVIDIA/AMD | Media | Medio | Matriz de GPUs; fallbacks; no prometer todas |
| Errores de agentes | Media | Medio | Evidencia, tests, revisión humana, stop tras 2 fallos |
| Scope creep | Alta | Alto | Backlog por gates; “no” a features fuera de alcance |
| Seguridad de binarios | Media | Medio | Solo releases oficiales; checksums; escaneo |

---

## 12. Legal y licencias

- **Volcado propio:** el usuario debe extraer el juego de su propia consola. No se distribuyen `eboot.bin`, `*.prx`, `*.pkg`, `art/`, audio, vídeo, fuentes ni saves.
- **Firmware:** `iss_pc` no necesita firmware; no lo incluyas ni lo pidas.
- **DRM:** no se debe eludir DRM ni protecciones. Si tu jurisdicción no permite el reverse engineering, no publiques el proyecto.
- **GPL-2.0:** `iss_pc` y `gow3_pc` son GPL-2.0. Si haces fork y distribuyes, debes:
  - Entregar el código fuente completo bajo GPL-2.0.
  - Mantener LICENSE y avisos de copyright.
  - Atribuir a HayatoG, FelipePrado0, shadPS4, bloodborne_pc y terceros.
  - No imponer restricciones adicionales incompatibles.
- **Marcas:** no afiliación con Sony/Sucker Punch; no uses logos oficiales; el icono debe ser original.
- **Privacidad:** los logs pueden contener rutas y nombres de usuario; límpialos antes de publicar.
- **No es asesoría legal.** Consulta a un abogado si vas a publicar.

---

## Anexo A — Plantillas de agentes OpenCode

Guardar en `.opencode/agents/`. El cuerpo Markdown es el system prompt.

### A.1 `port-director.md`

```md
---
description: Director del port nativo; planifica, prioriza y decide gates
mode: primary
permissions:
  - action: subagent
    resource: "*"
    effect: allow
  - action: edit
    resource: "*"
    effect: deny
  - action: edit
    resource: "docs/**"
    effect: allow
  - action: shell
    resource: "*"
    effect: ask
---
Eres el director del port nativo de inFAMOUS Second Son. Trabaja en español.

Lee `AGENTS.md`, `PLAN-PORT-NATIVO-SECONDSON.md`, `SECOND_SON.md` y `RESULTADOS.md` antes de decidir.

Responsabilidades:
- Mantener el roadmap y los gates.
- Dividir trabajo en tareas de una sola variable.
- Lanzar subagentes de recon, runtime, gpu, build y qa.
- Exigir evidencia (log, hash, captura, result.json) antes de aceptar una mejora.
- No declarar "jugable" ni "terminado" sin validación humana.

Nunca commitees archivos del juego, claves, saves ni rutas personales.
```

### A.2 `recon.md`

```md
---
description: Analiza eboot.bin, imports/NIDs y mapa de dependencias
mode: subagent
permissions:
  - action: edit
    resource: "*"
    effect: deny
  - action: edit
    resource: "docs/analysis/**"
    effect: allow
  - action: read
    resource: "*"
    effect: allow
  - action: shell
    resource: "*"
    effect: ask
---
Eres el agente de reconocimiento binario. Trabaja en español.

Objetivo: producir un mapa verificable de `eboot.bin` (CUSA00223 1.07):
- Entry point, TLS, relocations, secciones.
- Imports/NIDs y módulos.
- Funciones de inicialización y bucles principales.
- Diferencias con GOW3/iss_pc.

Usa Ghidra/REA/radare2. Cada afirmación debe incluir dirección, función o evidencia.
Escribe en `docs/analysis/`; no modifiques código de producción.
```

### A.3 `runtime.md`

```md
---
description: Implementa loader, HLE y servicios PS4
mode: subagent
permissions:
  - action: edit
    resource: "*"
    effect: deny
  - action: edit
    resource: "port/src/**"
    effect: allow
  - action: edit
    resource: "port/scripts/**"
    effect: allow
  - action: edit
    resource: "tests/**"
    effect: allow
  - action: shell
    resource: "*"
    effect: ask
---
Eres el ingeniero de runtime y loader. Trabaja en español.

Ámbito: `port/src/probe.c`, `runtime_*.c`, `runtime_infamous.c`, `port/scripts/`.
Método: reproduce el fallo, formula una hipótesis, cambia una sola cosa, mide y documenta.
Nunca inventes imports ni stubs sin evidencia del binario.
Mantén compatibilidad con saves y con el código GPL existente.
```

### A.4 `gpu.md`

```md
---
description: GPU, GNM/Vulkan, shaders GCN/SPIR-V, texturas y sincronización
mode: subagent
permissions:
  - action: edit
    resource: "*"
    effect: deny
  - action: edit
    resource: "port/gpu/**"
    effect: allow
  - action: edit
    resource: "tests/**"
    effect: allow
  - action: shell
    resource: "*"
    effect: ask
---
Eres el ingeniero de GPU del port. Trabaja en español.

Ámbito: `port/gpu/shadps4/`, backports, pipeline cache, texture cache, buffer cache, readbacks.
Usa RenderDoc para capturas y el harness para A/B.
No elimines sincronización necesaria para ganar FPS.
Documenta cada cambio con captura, métricas y una hipótesis causal.
```

### A.5 `build.md`

```md
---
description: Build MSYS2/CMake, CI, packaging y reproducibilidad
mode: subagent
permissions:
  - action: edit
    resource: "*"
    effect: deny
  - action: edit
    resource: "port/build.sh"
    effect: allow
  - action: edit
    resource: "port/packaging/**"
    effect: allow
  - action: edit
    resource: ".github/**"
    effect: allow
  - action: shell
    resource: "*"
    effect: ask
---
Eres el ingeniero de build y release. Trabaja en español.

Mantén builds reproducibles en Windows/MSYS2 CLANG64 y Linux.
Verifica que el paquete no incluya archivos del juego.
Documenta dependencias, versiones y checksums.
No cambies la licencia ni los avisos de atribución.
```

### A.6 `qa.md`

```md
---
description: Harness, pruebas A/B, regresiones y rendimiento
mode: subagent
permissions:
  - action: edit
    resource: "*"
    effect: deny
  - action: edit
    resource: "tests/**"
    effect: allow
  - action: edit
    resource: "outputs/evidence/**"
    effect: allow
  - action: shell
    resource: "*"
    effect: ask
---
Eres el ingeniero de QA y rendimiento. Trabaja en español.

Diseña experimentos de una sola variable, con 3 repeticiones y misma escena.
Conserva logs, capturas y result.json originales.
Compara FPS mediana, p95, VRAM, RAM y calidad visual.
No presentes una mejora no reproducida como solución.
```

### A.7 `upstream-watch.md`

```md
---
description: Vigila shadPS4 y ports upstream; propone cherry-picks
mode: subagent
permissions:
  - action: edit
    resource: "*"
    effect: deny
  - action: read
    resource: "*"
    effect: allow
  - action: shell
    resource: "*"
    effect: ask
---
Eres el observador de upstream. Trabaja en español.

Vigila: shadPS4, HayatoG/iss_pc, FelipePrado0/gow3_pc, HayatoG/tlg_pc,
HayatoG/gr2_pc, deadinside28/bloodborne_pc.
Clasifica commits/PRs por relevancia: estabilidad, VRAM, shaders, rendimiento, Windows.
Propone cherry-picks con evidencia; no hagas merge sin revisión humana.
```

---

## Anexo B — Skill `ps4-native-port`

Crear en `.opencode/skills/ps4-native-port/SKILL.md`:

```md
---
name: PS4 Native Port
description: Flujo de trabajo para port nativo de PS4 con loader, HLE y núcleo Vulkan de shadPS4
---
# PS4 Native Port

## Antes de tocar código
1. Lee `AGENTS.md`, `PLAN-PORT-NATIVO-SECONDSON.md` y `docs/adr/`.
2. Identifica la capa: loader, runtime, GPU, launcher o tests.
3. Comprueba que el juego y los assets están fuera de Git.

## Arquitectura
- `eboot.bin` -> `prepare.py` -> `probe.c` -> código original x86-64.
- `runtime_*.c` -> HLE de bibliotecas PS4.
- `gpu/shadps4/` -> GNM/PM4 + GCN -> Vulkan/SPIR-V.

## Build
- Windows: MSYS2 CLANG64; sigue `port/packaging/windows/README.md`.
- Linux: `cd port && ISS_LTO=OFF bash build.sh`.
- Nunca mezcles un cambio de build con un cambio de runtime/GPU.

## Experimento
1. Reproduce el fallo con una etiqueta nueva.
2. Formula una hipótesis.
3. Cambia una sola variable.
4. Ejecuta 3 veces la misma escena.
5. Guarda log, captura, hash y `result.json`.
6. Compara mediana, p95, VRAM y RAM.
7. Si no hay evidencia, no lo declares mejora.

## Reglas
- No archivos del juego, claves, saves ni rutas personales.
- No elimines sincronización sin medir.
- No copies código de fuentes no GPL.
- Para tras dos intentos fallidos y reporta.
```

---

## Anexo C — Configuración mínima de MCPs

Ver sección 6.4. Añade solo los servidores que vayas a usar. Comprueba con:

```powershell
opencode mcp list
```

Para REA:

```powershell
npx rea-agents setup --client opencode
```

Para GhidraMCP, primero instala el plugin en Ghidra y arranca el servidor del plugin en `127.0.0.1:8080`. Después:

```powershell
opencode mcp add ghidra -- uvx --from git+https://github.com/LaurieWired/GhidraMCP ghidra-mcp --ghidra-server http://127.0.0.1:8080/
```

Para RenderDoc:

```powershell
pip install -e .   # desde el repo de renderdoc-mcp
opencode mcp add renderdoc --env RENDERDOC_MODULE_PATH="C:\Program Files\RenderDoc" -- python -m renderdoc_mcp
```

No instales IDA Pro MCP si no tienes licencia. No instales Frida/x64dbg hasta que un crash lo justifique.

---

## Anexo D — Checklist de gate

### Gate G0 — Viabilidad

- [ ] Volcado CUSA00223 1.00 + 1.07 verificado.
- [ ] `iss_pc` release oficial ejecutado en el equipo.
- [ ] 15–30 min de gameplay o bloqueo documentado.
- [ ] FPS/VRAM/RAM registrados a 720p, 900p y 1080p.
- [ ] Build desde fuente funciona.
- [ ] Decisión: ruta A, B o C.

### Gate G1 — Arquitectura y harness

- [ ] `docs/architecture.md` revisado.
- [ ] `docs/analysis/imports.md` con NIDs y stubs.
- [ ] Harness ejecuta, loguea y compara.
- [ ] CI compila y pasa tests unitarios.
- [ ] ADRs de build, licencia y estrategia.

### Gate G2 — Bring-up

- [ ] Boot -> título -> gameplay.
- [ ] 30 min sin crash crítico.
- [ ] Save/load.
- [ ] Mando Xbox y audio.
- [ ] Logs limpios de rutas personales.

### Gate G3 — Rendimiento

- [ ] 900p, 30 FPS mediana, p95 < 50 ms.
- [ ] VRAM < 3,8 GB.
- [ ] Sin `DeviceLost`/OOM en 30 min.
- [ ] Capturas antes/después de la misma escena.
- [ ] Top 5 bottlenecks documentados.

### Gate G4 — Contenido

- [ ] Alcance acordado completado.
- [ ] Cinemáticas, audio, subtítulos.
- [ ] DLC/trofeos/idiomas según alcance.
- [ ] 2 h de soak.
- [ ] Lista de bugs conocidos publicada.

### Gate G5 — Release

- [ ] Instalación limpia en otra máquina.
- [ ] Sin archivos del juego en el paquete.
- [ ] LICENSE/NOTICE y atribución.
- [ ] Fuente GPL publicada.
- [ ] Checksums y notas de release.

---

## Anexo E — Fuentes y enlaces

Consultados el 19 de octubre de 2026:

- God of War III Remastered PC: <https://github.com/FelipePrado0/gow3_pc>
- README de GOW3: <https://github.com/FelipePrado0/gow3_pc/blob/main/README.md>
- inFAMOUS Second Son native PC port: <https://github.com/HayatoG/iss_pc>
- README de `iss_pc`: <https://github.com/HayatoG/iss_pc/blob/main/README.md>
- Roadmap de `iss_pc`: <https://github.com/HayatoG/iss_pc/blob/main/docs/ROADMAP.md>
- shadPS4: <https://github.com/shadps4-emu/shadPS4>
- Compatibilidad CUSA00223: <https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/952>
- REA: <https://github.com/morluto/rea>
- reverse-skill: <https://github.com/zhaoxuya520/reverse-skill>
- GhidraMCP: <https://github.com/LaurieWired/GhidraMCP>
- radare2 MCP: <https://github.com/radareorg/radare2-mcp>
- RenderDoc MCP: <https://github.com/Linkingooo/renderdoc-mcp>
- x64dbg MCP: <https://github.com/duty1g/x64dbg-mcp-server>
- Frida MCP: <https://github.com/dnakov/frida-mcp>
- OpenCode V2 docs: <https://opencode.ai/v2/docs/>

---

## Resumen final

La forma correcta de “replicar GOW3 para inFAMOUS” en octubre de 2026 no es escribir otro port desde cero: es **forkear `iss_pc`**, entender su arquitectura con `gow3_pc` como referencia, y usar tu fork de shadPS4 como laboratorio para portar las mejoras de VRAM, readbacks y rendimiento que ya validaste. El primer gate es tu GPU de 4 GB: si el port existente no es jugable ahí, hay que ajustar el alcance antes de empezar.
