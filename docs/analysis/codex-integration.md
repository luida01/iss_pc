# Integración de la app ChatGPT/Codex como agente QA

**Fecha:** 2026-10-19
**Alcance:** determinar si la app local ChatGPT/Codex (CLI `codex.exe` + computer-use) puede usarse como agente QA del port: feedback por test y control del PC, sin que el usuario tenga que mover los controles.
**Método:** reconocimiento de solo lectura de `C:\Users\luisd\.codex` y `C:\Users\luisd\AppData\Local\OpenAI\Codex`, ayuda del CLI, historial de hilos, artefactos de plugins y **una única** llamada de prueba `codex exec` con imagen. Este documento es el único archivo creado; no se modificó nada más.

> **Actualización 2026-10-19 10:20 UTC:** el smoke test de 45 s (`evidence/20261019-101754-fps-hud-smoke/`) validó el flujo completo. `Save-WindowScreenshot` capturó la ventana real del juego con `foreground=true`, el HUD mostró `38 FPS / 26.6 ms / GPU 72% / VRAM 4.6 de 3.3 GB`, y `Invoke-CodexFeedback.ps1` devolvió JSON válido (`state=cutscene`, `hud_fps=38`, `next_keys=["Space"]`) en 14.3 s. Computer-use sigue bloqueado sin aprobación UI; el feedback por imagen ya es la ruta operativa.

## 1. Resumen ejecutivo

| Pregunta | Respuesta |
|---|---|
| ¿Puede el agente dar feedback automático por captura? | **Sí, y ya está integrado.** `tools/Invoke-CodexFeedback.ps1` lanza `codex exec -s read-only -i <png>` por captura desde `tools/run-baseline.ps1` (`-CodexFeedback N`). La prueba real de este recon terminó con exit 0 y JSON válido. |
| ¿Puede computer-use pilotar el juego sin intervención del usuario? | **No.** Computer-use exige una aprobación por aplicación en la UI de la app («Allow Codex to use \<app\>?»), no hay ninguna persistida y `codex exec` no puede responderla («permissions approval is not supported in exec mode»). |
| ¿Hace falta computer-use para el objetivo de QA? | **No para el feedback** (el análisis de capturas ya funciona). Para el input, el port tiene rutas nativas más fiables (`TLG_PAD_FILE/RECORD/REPLAY`, `TLG_MOUSE_TEST`) que no dependen del foco de ventana. |
| ¿Qué bloquea hoy el bucle completo? | Solo la calidad de las capturas (la última sesión de evidencia capturó el escritorio con la terminal delante) y detalles menores del harness. La autenticación ya está resuelta («Logged in using ChatGPT»). |

## 2. Inventario de la instalación local

| Elemento | Valor observado |
|---|---|
| CLI | `C:\Users\luisd\AppData\Local\OpenAI\Codex\bin\896a0166b774037a\codex.exe`, versión `0.163.0-alpha.10.2`. **No está en `PATH`** en este equipo; hay que usar la ruta completa o la búsqueda que ya hace el harness. |
| App | Paquete MSIX `OpenAI.Codex_26.1013.4894.0_x64__2p2nqsd0c76g0`, en ejecución como proceso. |
| Autenticación | `codex login status` → `Logged in using ChatGPT` (sin API key; la llamada de prueba no pidió credenciales). |
| Config global | `C:\Users\luisd\.codex\config.toml`. |

### 2.1 `config.toml`

- Modelo por defecto `gpt-6-luna`, `model_reasoning_effort` alto.
- Hook `notify` → `codex-computer-use.exe turn-ended` (la app notifica el fin de turno; se dispara también en llamadas `exec` normales, no solo en computer-use).
- Plugins habilitados: `computer-use`, `unified-computer-use`, `browser`, `codex-app-tools`, `visualize`, `code-review`; runtimes `docs`/`pdf`/`spreadsheets`/`presentations`/`template-creator`.
- `[mcp_servers.node_repl]` con variables de entorno de CUA.
- `[windows] sandbox = "elevated"`.
- Lista de proyectos de confianza: `infamous-port` **no** está incluido (por eso el harness usa `--skip-git-repo-check`).

### 2.2 Estado global y computer-use

- `C:\Users\luisd\.codex\.codex-global-state.json`:
  - `agent-mode-by-host-id = {"local":"full-access"}`
  - `permission-selection-by-host-id:local = {"kind":"agent-mode","agentMode":"full-access"}`
  - `computer-use-bundled-plugin-auto-install-disabled = false`
  - **Sin entradas de aprobación de aplicaciones.**
- `C:\Users\luisd\.codex\computer-use\config.json`: solo cadenas de UI (`accentColor`, locale `es`). No contiene aprobaciones.
- Bases SQLite (`state_5.sqlite`, `logs_2.sqlite`, `thread_history_1.sqlite`): proyectos, hilos y logs de la app. No hay ninguna aprobación de computer-use persistida.
- **`C:\Users\luisd\.codex\computer-use\config.toml` no existe** (verificado con `Test-Path`): cero aprobaciones `[sessions]` guardadas.

### 2.3 Evidencia de denegaciones previas (historial de hilos)

- Hilo `01a15464-187b-7f92-a1c4-77d684bc793c`: `sky.get_window_state` falló con «Computer Use was not approved to use ChatGPT» y «…not approved to use Spotify». Es decir: la app ya intentó computer-use y el usuario (o la ausencia de respuesta) dejó las apps sin aprobar.
- Hilo `01a10021-70ee-7192-a533-e9e57cce4835` (proyecto anterior): «Las entradas absolutas del ratón mediante Computer Use no produjeron delta SDL». Relevante porque este port usa ratón relativo (`SDL_SetWindowRelativeMouseMode`).

## 3. Cómo funciona la aprobación de computer-use

Extraído de las fuentes del runtime CUA (`@oai/sky`, `helper_transport.js`, minificado) y del runtime `node_repl.exe`:

1. La primera vez que una sesión intenta controlar una app, la app crea una **elicitación** en la UI: «Allow Codex to use \<app\>?», con opciones de aprobar/denegar y ámbito (`conversation` o `global`).
2. Las apps aprobadas viajan en la cabecera `x-oai-cua-approved-app` en las llamadas siguientes.
3. El estado se persiste como `computer-use-persisted-state` (permitidas/denegadas por ámbito) y, en disco, en **`~/.codex/computer-use/config.toml` sección `[sessions]`** (hoy inexistente).
4. **`codex exec` no puede responder elicitaciones**: el binario contiene la cadena «permissions approval is not supported in exec mode». Sin aprobación persistida previa, cualquier flujo headless de computer-use queda bloqueado en el primer intento.

Comprobaciones de soporte:

- `codex mcp list --json`: `cua_repl` **enabled**, `node_repl` **enabled**, `codex_app` y `code-review` deshabilitados.
- `codex doctor`: autenticación OK; sandbox `elevated`/`restricted`; app-server no corriendo; advertencia de MCP: **`CODEX_WINDOWS_REGISTERED_CORE` sin definir** (afecta a `node_repl`, y por extensión al camino CUA).
- Catálogo de apps para computer-use: `C:\Users\luisd\.codex\cache\computer-use\computer-use-app-catalog\01a15464-187b-7f92-a1c4-77d684bc793c\user-assist.json`. Ahí aparecen `tlg-probe` con `use_count: 0` y `last_used: null` (nunca controlada), y también `gow3-probe` (`use_count: 0`).
- Ayuda del CLI: `codex exec` (con `resume`/`fork`), `codex app-server daemon/proxy`, `codex remote-control start/pair` (sin enrollments), `codex agents` (TUI), `codex debug models` (9 modelos con visión: `gpt-6.1-sol`, `gpt-6-astra`, `gpt-6-sol`, `gpt-6-luna`, `gpt-reserve`, `gpt-5.6-sol`, `gpt-5.6-terra`, `gpt-5.6-luna`, `codex-auto-review`).

**Conclusión de la sección:** la vía computer-use existe y funcionaría *después* de una aprobación UI, pero no se puede pre-aprobar `tlg-probe.exe` de forma desatendida con la instalación actual.

## 4. Prueba única de análisis de imagen (la llamada permitida)

Se hizo **una sola** invocación, sin computer-use, con sandbox de solo lectura, sin tocar el historial del usuario y con la config del usuario ignorada para que no cargara plugins ni hooks:

```powershell
& "$env:LOCALAPPDATA\OpenAI\Codex\bin\896a0166b774037a\codex.exe" exec `
  -s read-only --skip-git-repo-check --ephemeral --ignore-user-config `
  -m gpt-6-luna -c 'model_reasoning_effort="low"' `
  --json -C 'C:\Users\luisd\proyectos\infamous-port' `
  -i 'C:\Users\luisd\proyectos\infamous-port\evidence\20261019-092725-source-720p-b3500-15m\screenshot-15m-start.png' `
  -o 'C:\Users\luisd\AppData\Local\Temp\opencode\codex-image-analysis-20261019.txt' `
  'Analiza la captura adjunta. Responde SOLO un objeto JSON con summary, scene, visible_ui, rendering_issues y confidence.'
```

(`--ephemeral` evita dejar hilo persistente; la imagen era el PNG de 204 KB `screenshot-15m-start.png` del run de 15 min.)

**Resultado:**

- **Exit 0.** Autenticación vía cuenta ChatGPT, sin prompts interactivos.
- **Consumo:** `input_tokens = 19136` (11 776 en caché), `output_tokens = 181`, `reasoning_output_tokens = 0`. Coste bajo y acotado por llamada.
- **Formato:** stdout `--json` = 4 eventos JSONL (`thread.started` `01a15477-b5e1-7a51-9722-90fa89f53d73`, `turn.started`, `item.completed` con el mensaje, `turn.completed` con usage). El archivo `-o` quedó en UTF-8 y **es lo que el harness debe consumir**.
- **Calidad del análisis:** el modelo describió correctamente que la imagen era una *terminal*, no el juego, y lo dijo explícitamente («No se ve el juego: la ventana muestra una sesión de terminal…», `confidence 0.99`). Es decir: **ve y razona sobre la captura sin alucinar el contenido**, que es exactamente lo que necesita el QA.

**Caveats observados:**

1. La captura analizada es de un run cuya evidencia visual ya estaba marcada como no válida (`docs/analysis/gate-g0-baseline.md`): se capturó el escritorio completo con la terminal delante. La prueba valida el mecanismo, no la evidencia.
2. stderr mostró `Reading additional input from stdin...` (PowerShell lo convirtió en `NativeCommandError`, sin afectar al exit 0). Conviene cerrar stdin en futuras llamadas (`RedirectStandardInput` + `Close`, o `< NUL`).
3. La redirección `>` de PowerShell genera el JSONL en UTF-16LE; usar `-o` (UTF-8) o escribir con `[IO.File]::WriteAllText`. El harness ya escribe en UTF-8 sin BOM.

## 5. Estado actual del harness (la integración ya existe)

### 5.1 `tools/run-baseline.ps1`

Parámetros de automatización: `-AutoPlayScenario`, `-ScreenshotIntervalSec`, `-BringToFront`, `-CodexFeedback N`.

- Sin flags, el harness mide igual que siempre (input manual).
- Con automatización, carga `tools/GameWindow.ps1` y escribe **`automation.json`** en la evidencia: `timeline` (pasos con `elapsed_sec`/`key`/`hold_ms`/`status`), `captures` (archivo, método, tamaño, `foreground`), `feedback` (captura, pid, estado), `focus` (intentos, fallos, título en primer plano) y `summary` final.
- Las capturas se guardan como `automation-<n>s.png` con **`Save-WindowScreenshot`** (ventana del juego: `PrintWindow` con respaldo `CopyFromScreen`; el `automation.json` registra cuál se usó y si la ventana estaba en primer plano).
- `-CodexFeedback N`: cada N capturas archiva `chatgpt-feedback.*` en `feedback/` (con sufijo de la captura) y lanza en segundo plano `tools/Invoke-CodexFeedback.ps1`; al cerrar espera hasta 15 s a los análisis pendientes.

Ejemplo:

```powershell
pwsh -NoProfile -File .\tools\run-baseline.ps1 -DurationSec 960 -Resolution 720p -Tag g0-intro `
  -AutoPlayScenario intro-g0 -ScreenshotIntervalSec 30 -BringToFront -CodexFeedback 2
```

### 5.2 `tools/Invoke-CodexFeedback.ps1`

Ya implementa el patrón correcto: localiza el `codex.exe` más reciente bajo `%LOCALAPPDATA%\OpenAI\Codex\bin`, y ejecuta:

```
codex.exe exec -C <repo> --skip-git-repo-check -s read-only -i <captura> -o <raw.txt> --json
  -c plugins."unified-computer-use@openai-bundled".enabled=false
  -c plugins."computer-use@openai-bundled".enabled=false
  -c plugins."browser@openai-bundled".enabled=false
  <prompt QA>
```

- Timeout 240 s con `taskkill /T /F` del árbol; cualquier fallo escribe `chatgpt-feedback.error.txt` y **devuelve 0** para no romper la sesión.
- Parsea del mensaje del modelo un JSON con `state` (title/menu/cutscene/gameplay/error), `visible_text`, `hud_fps`, `gameplay_confidence`, `next_keys`, `notes`; escribe `chatgpt-feedback.json/.md/.raw.txt/.stdout.log/.stderr.log`.
- El prompt prohíbe explícitamente controlar otras apps o tocar archivos: es análisis puro de la captura.

### 5.3 Input nativo del port (alternativa a SendInput)

- `port/src/runtime_pad.c`: `TLG_PAD_FILE`, `TLG_PAD_RECORD`, `TLG_PAD_REPLAY` — input de mando por guion, determinista y sin depender del foco de ventana.
- `port/gpu/shim/window.cpp`: `TLG_MOUSE_TEST=1` — ratón sintético en bucle.
- `tools/GameWindow.ps1` (usado por el autoplay): `Get-GameWindow`, `Set-GameWindowFocus`, `Save-WindowScreenshot`, `Send-GameKey`/`Send-GameKeyScript` (SendInput/PostMessage).
- `tools/autoplay/intro-g0.json`: escenario de pasos (`elapsed_sec`, `key`, `hold_ms`, `delay_ms`) para cubrir menús/título con teclado.

## 6. Recomendación

**Ruta primaria (sin computer-use, sin aprobaciones, sin que el usuario mueva nada):**

1. **Input:** escenarios del harness (`-AutoPlayScenario`) y, cuando la escena lo permita, guiones nativos del port (`TLG_PAD_FILE`/`TLG_PAD_REPLAY`) por ser inmunes a pérdidas de foco.
2. **Captura:** `-ScreenshotIntervalSec 30 -BringToFront` → `automation-<n>s.png` de la **ventana del juego**. Verificar en `automation.json` que cada captura tiene `foreground=true` y método razonable; si `CopyFromScreen` capturó algo encima, la captura no vale como evidencia.
3. **Feedback por test:** `-CodexFeedback 2` con el script actual. Cada captura produce `chatgpt-feedback.json` parseado y auditable, con coste ~19 k tokens por llamada.
4. **Cierre:** revisar `automation.json` (pasos ejecutados/fallidos, capturas, foco) + `result.json` (FPS, VRAM, RAM) + `feedback/`.

**Mejoras menores, opcionales (para otro agente; este recon no las aplicó):**

- En `Invoke-CodexFeedback.ps1`: cerrar stdin (`RedirectStandardInput` + `Close`) para eliminar el aviso «Reading additional input from stdin…»; considerar `--output-schema` para forzar el JSON del análisis por schema en vez de parsear texto; considerar `--ephemeral` para no acumular hilos; `-c model_reasoning_effort="low"` si el coste importa.
- Validar el brillo/contenido de cada captura antes de gastar una llamada al modelo (detectar capturas negras de `PrintWindow`).

**Ruta computer-use (solo si el usuario quiere que la app pilote el juego directamente):** no es viable en modo desatendido hoy. Si se quiere explorar:

1. El usuario abre la app ChatGPT y lanza una conversación pidiendo controlar la ventana del juego (proceso `tlg-probe`).
2. La app mostrará la elicitación «Allow Codex to use tlg-probe?»; aprobar con ámbito **global** persiste la aprobación en `~/.codex/computer-use/config.toml [sessions]`, y a partir de ahí computer-use podría repetirse sin volver a preguntar.
3. Limitaciones a documentar: `codex exec` jamás podrá aprobar elicitaciones; el ratón absoluto de computer-use **no genera delta SDL** en este port (usa ratón relativo) — solo teclado sería fiable; `guidance.md` prohíbe automatizar terminales (el agente debe limitarse a la ventana del juego); y sigue pendiente el aviso `CODEX_WINDOWS_REGISTERED_CORE`.

## 7. Bloqueos y pasos que requieren al usuario

| Bloqueo | ¿Requiere al usuario? | Detalle |
|---|---|---|
| Sin aprobación persistida de computer-use | Sí, solo si se quiere computer-use | Aprobar «Allow Codex to use tlg-probe?» en la UI con ámbito global; hoy `computer-use/config.toml` no existe. |
| Elicitaciones en modo exec | No tiene solución | `codex exec` no soporta aprobaciones («permissions approval is not supported in exec mode»). |
| `CODEX_WINDOWS_REGISTERED_CORE` sin definir | Sí, si se usa `cua_repl`/MCP de node | Definirla en el entorno y reiniciar la app; sin esto, doctor avisa y el camino CUA puede fallar. |
| Ratón absoluto sin delta SDL | No (evitar la ruta) | Usar teclado/escenarios; el port usa ratón relativo. |
| Capturas de escritorio en vez de la ventana | No (ya mitigado en el harness) | El run de 15 min quedó invalidado visualmente por esto; el camino `Save-WindowScreenshot` + `automation.json` lo cubre, pero hay que revisar `foreground`/método por captura. |
| No hay nada más para el feedback por imagen | No | Autenticación ya presente («Logged in using ChatGPT»); no se necesita aprobación alguna para `codex exec -i`. |

## 8. Anexo: comandos de referencia

```powershell
# Estado y diagnóstico (usar la ruta completa: codex no está en PATH)
& "$env:LOCALAPPDATA\OpenAI\Codex\bin\896a0166b774037a\codex.exe" --version
& "$env:LOCALAPPDATA\OpenAI\Codex\bin\896a0166b774037a\codex.exe" login status
& "$env:LOCALAPPDATA\OpenAI\Codex\bin\896a0166b774037a\codex.exe" doctor
& "$env:LOCALAPPDATA\OpenAI\Codex\bin\896a0166b774037a\codex.exe" mcp list --json
& "$env:LOCALAPPDATA\OpenAI\Codex\bin\896a0166b774037a\codex.exe" debug models

# QA por captura, manual
pwsh -NoProfile -File .\tools\Invoke-CodexFeedback.ps1 `
  -ImagePath .\evidence\<run>\automation-300s.png `
  -EvidenceDir .\evidence\<run> `
  -ContextFile .\evidence\<run>\automation.json

# Sesión con autoplay + capturas + feedback
pwsh -NoProfile -File .\tools\run-baseline.ps1 -DurationSec 960 -Resolution 720p -Tag g0-intro `
  -AutoPlayScenario intro-g0 -ScreenshotIntervalSec 30 -BringToFront -CodexFeedback 2
```
