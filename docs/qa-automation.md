# QA automation: capturas de ventana, autoplay y feedback de Codex

Este documento describe la automatización opcional del harness para el Gate G0.
Los scripts viven en `tools/` y no forman parte del runtime ni del juego:

| Archivo | Papel |
|---|---|
| `tools/GameWindow.ps1` | Localiza/enfoca la ventana, captura y envía teclas. |
| `tools/autoplay/intro-g0.json` | Escenario del Gate G0 (intro/menús + 12 min de gameplay). |
| `tools/Invoke-CodexFeedback.ps1` | Analiza una captura con el CLI local de Codex. |
| `tools/run-baseline.ps1` | Orquesta todo dentro de una sesión de evidencia. |

Todo requiere **PowerShell 7** y no añade dependencias externas. Sin los flags
nuevos, `run-baseline.ps1` mide exactamente igual que antes: input manual, sin
capturas ni feedback.

## 1. Flujo recomendado para el Gate G0

```powershell
pwsh
cd C:\Users\luisd\proyectos\infamous-port

.\tools\run-baseline.ps1 -DurationSec 960 -Resolution native -Tag g0-intro `
  -AutoPlayScenario intro-g0 -ScreenshotIntervalSec 30 -BringToFront -CodexFeedback 2
```

1. El harness lanza `python port\run.py` como siempre y crea
   `evidence\<timestamp>-<tag>\`.
2. Cuando aparece `tlg-probe.exe`, empieza el reloj de `-DurationSec` y la
   automatización:
   - `-BringToFront` enfoca la ventana del juego al arrancar y cada 30 s
     (`Set-GameWindowFocus`, con reintentos y sin depender del ratón).
   - `-AutoPlayScenario intro-g0` ejecuta la timeline del JSON por
     `elapsed_sec` (segundos desde el arranque del juego). Los pasos vencidos
     con más de `max_lateness_sec` (5 s) se marcan `skipped` en lugar de
     dispararse en ráfaga.
   - `-ScreenshotIntervalSec 30` guarda una captura de la **ventana** del juego
     cada 30 s como `automation-<n>s.png` (n = segundo planificado).
   - `-CodexFeedback 2` lanza `Invoke-CodexFeedback.ps1` en segundo plano cada
     2 capturas; el análisis no bloquea la sesión.
3. Al terminar se escribe `automation.json` con timeline, capturas, estado del
   foco, feedback y errores, y `result.json` como siempre (mismo formato;
   `notes` resume la automatización).

## 2. Escenario `intro-g0`

`tools/autoplay/intro-g0.json` (639 pasos, 960 s planificados):

- **Fase 1 (0-234 s):** `Space` cada 6 s para saltar logos, intro y menús
  (Cross en el juego).
- **Fase 2 (240-960 s):** ciclo de 24 s repetido 30 veces que mezcla movimiento
  continuo, cámara y acciones.

| Tecla del harness | Botón PS4 | Uso en el ciclo |
|---|---|---|
| `W`/`A`/`S`/`D` (y cuerdas `W+A`, `S+D`) | stick izquierdo | moverse en cuadrado/diagonal |
| `Up`/`Down`/`Left`/`Right` | stick derecho | girar cámara |
| `Space` | Cross | saltar, avanzar diálogos |
| `Enter` | Options | (no se usa en el ciclo; abre pausa) |
| `E` | Square | ataque cuerpo a cuerpo |
| `Q` | Triangle | acción de contexto |
| `R` | L2 | apuntar |
| `F` | R2 | disparar |
| `Z` | L3 | pulsación del stick izquierdo |
| `C` | R3 | pulsación del stick derecho |

Cada paso del JSON tiene `elapsed_sec`, `key`, `hold_ms` y `delay_ms` (pausa
tras soltar la tecla). Para escenarios propios: copia el JSON, mantén la
timeline ordenada y usa solo las teclas soportadas; `run-baseline.ps1` valida y
rechaza el resto.

## 3. Capturas de la ventana

`Save-WindowScreenshot` intenta primero `PrintWindow` con
`PW_RENDERFULLCONTENT`; si el bitmap sale casi negro (se muestrean píxeles) o la
llamada falla, enfoca la ventana y usa `Graphics.CopyFromScreen` con el
rectángulo en píxeles físicos. `GameWindow.ps1` activa DPI per-monitor v2 al
cargarse para que el rectángulo no salga escalado. El método usado en cada
captura queda en `automation.json` (`captures[].method`).

Uso suelto:

```powershell
. .\tools\GameWindow.ps1
$win = Get-GameWindow -ProcessId (Get-Process tlg-probe).Id
Set-GameWindowFocus -Window $win
Save-WindowScreenshot -Window $win -Path .\evidence\shot.png
Send-GameKey -ProcessId $win.ProcessId -Key Space -HoldMs 150
Save-WindowScreenshot -Path .\evidence\full.png -FullScreen
Send-GameKeyScript -Steps @( @{ key = 'W+A'; hold_ms = 1500; delay_ms = 200 } )
```

Notas de input: `Send-GameKey` intenta `SendInput` con scancodes
(`MapVirtualKey`), comprueba que la ventana esté en primer plano y solo entonces
escribe; si no puede, usa `PostMessage` como respaldo (puede no llegar a juegos
con Raw Input). Nunca escribe en la ventana equivocada a propósito.

## 4. Feedback del agente local (Codex)

```powershell
.\tools\Invoke-CodexFeedback.ps1 `
  -ImagePath .\evidence\<run>\automation-300s.png `
  -EvidenceDir .\evidence\<run> `
  -ContextFile .\evidence\<run>\automation.json `
  -TimeoutSec 240
```

- Lanza `codex.exe exec -C <repo> --skip-git-repo-check -s read-only -i <png>
  -o <raw> --json` con los plugins de computer-use/browser desactivados.
- El prompt pide **solo** un JSON con `state`, `visible_text`, `hud_fps`,
  `gameplay_confidence`, `next_keys` y `notes`, y prohíbe controlar ChatGPT,
  Spotify u otras apps: es un analista de captura, no un agente de acción; no
  modifica archivos.
- Salidas en `EvidenceDir` (o junto a la imagen): `chatgpt-feedback.json`,
  `chatgpt-feedback.md`, `chatgpt-feedback.raw.txt`,
  `chatgpt-feedback.stdout.log` / `chatgpt-feedback.stderr.log`.
- Si codex no existe, falla o agota el timeout: `chatgpt-feedback.error.txt`
  con motivo, comando y cola de salida, y **código 0** para no romper el
  harness.
- Cuando lo lanza `run-baseline.ps1` cada N capturas, las salidas anteriores se
  archivan en `feedback\chatgpt-feedback-<captura>.{json,md,...}` y los logs del
  CLI en `codex-feedback-<captura>.out.log` / `.err.log`.

## 5. `automation.json`

```text
automation.json
  schema, started_at, finished_at
  config            flags efectivos (intervalo, bring_to_front, feedback)
  scenario          nombre, origen, pasos, duración planificada
  window            handle, título, rect, dpi, foreground
  timeline[]        un registro por paso: status pending|done|failed|skipped,
                    at_sec real, ok, note
  captures[]        automation-<n>s.png, planned_sec, at_sec, method, size,
                    foreground, error
  feedback[]        image, at_sec, status, pid, out_log/err_log, exit_code
  focus             attempts/failures, último resultado y ventana en primer plano
  counters          step_index, next_capture_at, skipped_captures, ...
  summary           pasos ejecutados/fallidos/saltados, capturas, feedback
  errors[]          incidencias (ventana perdida, paso fallido, etc.)
```

## 6. Limitaciones

- **Foco:** `SendInput` necesita que el juego esté en primer plano. Si otra
  ventana lo tapa y el foco no se puede recuperar, las teclas caen a
  `PostMessage` y pueden no tener efecto; en ese caso el timeline registra
  `failed`. `-BringToFront` reduce el problema, no lo elimina.
- **`Enter` abre el menú de pausa (Options):** el ciclo de `intro-g0` no lo usa.
  Un escenario que lo pulse debe cerrarlo después.
- **Timeline a ciegas:** la automatización no ve el juego; si el estado real no
  es el esperado (carga larga, muerte, diálogo), los pasos siguen igual. El
  feedback por captura está para detectarlo, no para corregirlo en caliente.
- **Primera captura:** el primer `PrintWindow`/System.Drawing tiene coste de
  JIT (~1 s); una captura puede llegar tarde y otra saltarse
  (`skipped_captures`).
- **Codex:** tarda y consume cuota del ChatGPT del usuario. Se lanza **un**
  análisis a la vez; si el anterior sigue vivo se pospone. Los archivos
  `chatgpt-feedback.*` son del **último** análisis; el historial por captura
  queda en `feedback\`.
- **Un solo proceso `tlg-probe`:** como el harness original. Reiniciar el juego
  desde el menú crea otro proceso que no se controla ni se muestrea.
- **Privacidad:** `automation.json`, los logs de codex y las capturas pueden
  contener rutas locales; redáctalos antes de publicarlos.
