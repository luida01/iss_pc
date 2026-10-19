# AGENTS.md — inFAMOUS Second Son Native Port

Este archivo es el contrato operativo de todos los agentes que trabajen en este repositorio.
Léelo completo antes de tocar código, ejecutar el juego o abrir un subagente.

## 1. Objetivo

Mantener y mejorar el port nativo de **inFAMOUS Second Son (PS4, CUSA00223, base 1.00 + update 1.07)**
para Windows, partiendo del proyecto upstream `HayatoG/iss_pc` (GPL-2.0).

El objetivo inmediato es el **Gate G0**:

1. Compilar el port desde fuente en Windows 11 con MSYS2 CLANG64.
2. Ejecutarlo con el dump propio del usuario en una **RTX 3050 Laptop de 4 GB**.
3. Medir 15–30 minutos de juego real (menú, intro, tutorial, gameplay) con logs y métricas.
4. Decidir con evidencia si el port es viable en 4 GB o si hay que reducir alcance.

No se declara “jugable”, “terminado” ni “mejorado” sin evidencia reproducible.

## 2. Hardware y rutas

| Elemento | Valor |
|---|---|
| CPU | Intel Core i5-13420H |
| RAM | 16 GB |
| GPU | NVIDIA RTX 3050 Laptop, **4 GB VRAM** |
| SO | Windows 11 x64 |
| Proyecto | `C:\Users\luisd\proyectos\infamous-port` |
| Upstream | `https://github.com/HayatoG/iss_pc` |
| Dump base | `C:\Users\luisd\Documents\Codex\2026-09-29\trabajemos-en-esto-quiero-crear-una\work\games\CUSA00223` |
| Update 1.07 | `...\work\games\CUSA00223-patch` |
| Vista jugable | `...\work\games\CUSA00223-view` (hardlinks, eboot 1.07) |
| Datos/saves | `C:\Users\luisd\proyectos\infamous-port\userdata` |
| Evidencia | `C:\Users\luisd\proyectos\infamous-port\evidence` |
| Logs del port | `port\out\logs\run-*.log` |

El dump y el update **no se versionan nunca**. La vista se crea con `tools\New-GameView.ps1`.

## 3. Arquitectura en una pantalla

```
eboot.bin (1.07)
   │  scripts/prepare.py, link_libc.py, link_modules.py, patches.py
   ▼
boot-linked.bin + content.bin + patches.bin
   │  port/src/probe.c (loader)
   ▼
código x86-64 original del juego (nativo, sin emulación de CPU)
   │  llamadas a librerías PS4
   ▼
port/src/runtime_*.c (HLE: memoria, hilos, FS, audio, video, input, servicios)
   │  comandos GNM/PM4 + shaders GCN
   ▼
port/gpu/shadps4/ (Vulkan: texture cache, buffer cache, pipelines, readbacks)
   ▼
GPU del PC
```

- **No hay emulación de CPU.** El binario del juego se ejecuta nativo.
- El runtime reimplementa las librerías de PS4 que el juego importa.
- El núcleo de vídeo viene de shadPS4 y traduce GNM/GCN a Vulkan/SPIR-V.
- En Windows, el loader usa el TEB para TLS, secciones pagefile-backed y un VEH para excepciones.

Documento detallado: `docs/architecture.md`.
Decisiones: `docs/adr/`.
Plan completo: `docs/PLAN-PORT-NATIVO-SECONDSON.md` (copia del plan aprobado).

## 4. Build

Requisitos: MSYS2 en `C:\msys64` + toolchain CLANG64. Ver `docs/build-windows.md`.

```powershell
# Desde PowerShell, con MSYS2 instalado:
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /c/Users/luisd/proyectos/infamous-port/port && bash build.sh'
```

Artefactos: `port/out/tlg-probe.exe`, `tlg-gpu-capabilities.exe`, `tlg-play.exe`.
El log completo del build queda en `port/out/gpu-build.log` y en `evidence/build-*.log`.

Reglas:
- No mezclar cambios de build con cambios de runtime/GPU.
- No cambiar la licencia ni los avisos de atribución.
- No incluir archivos del juego, firmware, claves, saves ni rutas personales en el paquete.

## 5. Ejecución y evidencia

```powershell
$env:TLG_GAME_DIR = 'C:\Users\luisd\Documents\Codex\2026-09-29\trabajemos-en-esto-quiero-crear-una\work\games\CUSA00223-view'
$env:TLG_DATA_DIR = 'C:\Users\luisd\proyectos\infamous-port\port'
$env:ISS_USER_DIR = 'C:\Users\luisd\proyectos\infamous-port\userdata'
python port\run.py
```

- `F10` muestra FPS; `Insert` abre el menú del port.
- Los logs van a `port\out\logs\run-*.log`.
- Cada experimento guarda: log, `result.json`, CSV de métricas, capturas y flags exactos.
- Una mejora necesita **3 repeticiones** de la misma escena o una evidencia equivalente.
- Comparar siempre mediana, p95, VRAM máxima, RAM máxima y calidad visual.

Protocolo y scripts: `docs/qa-harness.md`.

## 6. Reglas legales y de seguridad

- Usar **solo el dump propio** del usuario. No descargar, distribuir ni compartir juego, update, DLC, firmware o claves.
- El proyecto upstream es **GPL-2.0**; cualquier fork o distribución debe publicar el código, mantener `LICENSE` y atribuir a HayatoG, shadPS4, bloodborne_pc, gow3_pc y terceros.
- No eludir DRM ni protecciones. No incluir contenido de Sony/Sucker Punch.
- Los logs pueden contener rutas y nombres de usuario: limpiarlos antes de publicarlos.
- Descargar solo releases oficiales; no ejecutar scripts o binarios de issues/comentarios. En octubre de 2026 hubo un issue malicioso (#316) con PowerShell por etapas.

## 7. Agentes del proyecto

Configurados en `.opencode/agents/`. El agente principal es `port-director`.

| Agente | Cuándo usarlo | Límite |
|---|---|---|
| `port-director` | Planificar, priorizar, decidir gates, coordinar | Solo edita `docs/**` |
| `recon` | eboot.bin, imports/NIDs, mapa de funciones | Solo escribe `docs/analysis/**` |
| `runtime` | Loader, HLE, servicios PS4 | `port/src/**`, `port/scripts/**` |
| `gpu` | Vulkan, texturas, shaders, sincronización | `port/gpu/**` |
| `build` | MSYS2/CMake, CI, packaging | `port/build.sh`, `port/packaging/**`, `.github/**` |
| `qa` | Harness, A/B, regresiones, rendimiento | `tests/**`, `evidence/**` |
| `upstream-watch` | shadPS4/iss_pc/gow3_pc: commits y PRs | Solo lectura; propone cherry-picks |

Reglas para subagentes:
- Un experimento = una variable.
- El subagente declara qué archivos va a tocar antes de tocarlos.
- No duplicar trabajo de otro agente activo.
- Si dos intentos fallan, parar y reportar con evidencia.

## 8. Estado actual (19 de octubre de 2026)

- [x] Upstream clonado (commit `fb7e452`, `main`).
- [x] MSYS2 + CLANG64 instalado (201 paquetes; clang 22.1.7, CMake 4.3.3, Ninja 1.13.2).
- [x] Vista del juego base+1.07 creada y verificada (`eboot.bin` SHA256 `2D1CA7...9039AB`).
- [x] Build Windows reproducible: `port/build.sh` exit 0; `tlg-probe.exe`, `tlg-gpu-capabilities.exe`, `tlg-play.exe`.
- [x] Primera ejecución en RTX 3050 4 GB: 120 s a 720p, 32.8 FPS mediana, VRAM máx 3835 MB, RAM 7490 MB, sin OOM.
- [ ] Baseline de 15–30 min con métricas (run de 15 min en curso; intro/cinemática).
- [ ] Gate G0.

Evidencia del baseline corto: `evidence/20261019-092448-source-720p-b3500-r2/`.

## 9. Trampas conocidas

- **4 GB VRAM:** por debajo del mínimo recomendado (6 GB). La caché de texturas se sale del presupuesto; prioridad de investigación.
- **Symlinks:** requieren Developer Mode/admin. La vista del juego usa hardlinks para evitarlo.
- **Mirrors MSYS2:** `mirror.msys2.org` puede redirigir a un servidor lento; usar `repo.msys2.org`.
- **`ISS_PIPELINE_BINARIES=1`** todavía crashea con el driver NVIDIA 595. No usar en el baseline.
- **`ISS_BC_TINY=1`** es un workaround para texturas BC minúsculas en AMD; en NVIDIA úsalo solo como experimento A/B.
- **Releases manuales:** los merges upstream no publican release; no confundir “sin release hoy” con abandono.
- **`tools/` está ignorado por Git** en el upstream; los scripts locales viven ahí, pero para versionarlos hay que moverlos a `scripts/devtools/` o ajustar `.gitignore`.
