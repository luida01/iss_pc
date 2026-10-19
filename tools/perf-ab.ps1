#Requires -Version 7.0
<#
.SYNOPSIS
    Compara configuraciones graficas del port: lanza el juego con cada una, mide y saca tabla.

.DESCRIPTION
    Cada configuracion es una etiqueta y un hashtable con tres claves opcionales:
      resolution  'native', '480p', '720p', ... (se escribe en la copia de tlg.ini de la evidencia)
      ini         hashtable de claves de tlg.ini (preset, live_resolution, rcas, ...)
      env         hashtable de variables de entorno (ISS_UPSCALER, ISS_GC_BUDGET_MB, ...)

    Para cada configuracion lanza run-baseline.ps1 (que mide FPS, frame time, VRAM, RAM, GPU y
    guarda capturas) y al terminar lee su result.json. Deja:

      evidence/perf-ab-<timestamp>/summary.csv   una fila por configuracion
      evidence/perf-ab-<timestamp>/summary.md    la misma tabla en Markdown

    Los lanzamientos son secuenciales: nunca hay dos juegos a la vez.

.PARAMETER Configs
    Configuraciones a comparar. Por defecto, la matriz de palancas de 4 GB.

.PARAMETER DurationSec
    Segundos de juego por configuracion (por defecto 120).

.PARAMETER Tag
    Etiqueta base de la carpeta de evidencia.

.PARAMETER OutDir
    Carpeta donde se deja el resumen (por defecto evidence/perf-ab-<timestamp>).

.PARAMETER KeepOpen
    No cierra la ventana al terminar cada pasada (para sesiones manuales).

.EXAMPLE
    .\tools\perf-ab.ps1 -DurationSec 180
    .\tools\perf-ab.ps1 -Configs @{ 'prueba' = @{ resolution = '720p'; env = @{ ISS_UPSCALER = 'fsr3' } } } -DurationSec 90
#>
[CmdletBinding()]
param(
    [hashtable]$Configs = @{},
    [ValidateRange(10, 7200)][int]$DurationSec = 120,
    [string]$Tag = 'perf-ab',
    [string]$OutDir = '',
    [switch]$KeepOpen,
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)

if ($Configs.Count -eq 0) {
    $Configs = [ordered]@{
        '720p-nativo'       = @{ resolution = '720p'; env = @{ ISS_UPSCALER = 'off' }; ini = @{ preset = '0' } }
        '720p-fsr3'         = @{ resolution = '720p'; env = @{ ISS_UPSCALER = 'fsr3' }; ini = @{ preset = '0' } }
        '720p-fsr3-jitter'  = @{ resolution = '720p'; env = @{ ISS_UPSCALER = 'fsr3'; ISS_JITTER = '1' }; ini = @{ preset = '0' } }
        '720p-fsr411'       = @{ resolution = '720p'; env = @{ ISS_UPSCALER = 'fsr411' }; ini = @{ preset = '0' } }
        '720p-fsr3-calidad' = @{ resolution = '720p'; env = @{ ISS_UPSCALER = 'fsr3' }; ini = @{ preset = '1' } }
        '480p-nativo'       = @{ resolution = '480p'; env = @{ ISS_UPSCALER = 'off' }; ini = @{ preset = '0' } }
    }
}

if (-not $OutDir) {
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
    $OutDir = Join-Path (Join-Path (Get-Location) 'evidence') "perf-ab-$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$rows = @()
foreach ($name in $Configs.Keys) {
    $cfg = $Configs[$name]
    # Forma simple: un hashtable plano son variables de entorno.
    if (-not ($cfg -is [System.Collections.IDictionary]) -or
        -not ($cfg.Contains('env') -or $cfg.Contains('ini') -or $cfg.Contains('resolution'))) {
        $cfg = @{ env = $cfg }
    }
    $envMap = if ($cfg.Contains('env')) { $cfg['env'] } else { @{} }
    $iniMap = if ($cfg.Contains('ini')) { $cfg['ini'] } else { @{} }
    $res = if ($cfg.Contains('resolution')) { [string]$cfg['resolution'] } else { '720p' }

    Write-Host "=== $name ===" -ForegroundColor Cyan
    Write-Host ("    resolucion: $res")
    Write-Host ("    env: " + (($envMap.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))
    Write-Host ("    ini: " + (($iniMap.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))

    $args = @{
        DurationSec = $DurationSec
        Resolution  = $res
        Tag         = [string]$name
        ExtraEnv    = $envMap
        IniExtra    = $iniMap
        Python      = $Python
    }
    if ($KeepOpen) { $args['KeepOpen'] = $true }
    & '.\tools\run-baseline.ps1' @args | Out-Null

    $result = Get-ChildItem 'evidence' -Directory -Filter "*-$name" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $result) {
        Write-Warning "sin evidencia para $name"
        continue
    }
    $json = Get-Content (Join-Path $result.FullName 'result.json') -Raw | ConvertFrom-Json
    $m = $json.metrics
    $rows += [pscustomobject]@{
        config          = $name
        status          = $json.status
        stop_reason     = $json.stop_reason
        duration_sec    = $json.duration_sec
        fps_median      = $m.fps.median
        fps_p95         = $m.fps.p95
        frame_median_ms = $m.frame_pacing.median_ms
        frame_p99_ms    = $m.frame_pacing.p99_ms
        vram_max_mb     = $m.memory.vram_max_mb
        ram_max_mb      = $m.memory.ram_max_mb
        gpu_median_pct  = $m.gpu.utilization_median_pct
        cpu_median_pct  = $m.cpu.median_pct
        resolution      = $res
        evidence        = $result.Name
        env             = (($envMap.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')
        ini             = (($iniMap.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')
    }
}

$csv = Join-Path $OutDir 'summary.csv'
$rows | Export-Csv -Path $csv -NoTypeInformation -Encoding utf8

$md = @()
$md += "# Comparativa grafica ($Tag)"
$md += ''
$md += "Pasadas de $DurationSec s cada una. Evidencia completa en `evidence/`."
$md += ''
$md += '| Configuracion | FPS mediana | FPS p95 | Frame ms (med/p99) | VRAM max | GPU med | CPU med | Estado |'
$md += '|---|---|---|---|---|---|---|---|'
foreach ($r in $rows) {
    $md += ('| {0} | {1} | {2} | {3} / {4} | {5} MB | {6}% | {7}% | {8} |' -f `
        $r.config, $r.fps_median, $r.fps_p95, $r.frame_median_ms, $r.frame_p99_ms,
        $r.vram_max_mb, $r.gpu_median_pct, $r.cpu_median_pct, $r.status)
}
$md += ''
$md += '## Variables por configuracion'
$md += ''
foreach ($r in $rows) {
    $md += ("- **$($r.config)** (resolucion $($r.resolution)): env `$( $($r.env) )`, ini `$( $($r.ini) )` (evidencia: ``$($r.evidence)``)")
}
$mdFile = Join-Path $OutDir 'summary.md'
$md | Out-File -FilePath $mdFile -Encoding utf8

Write-Host ''
Write-Host "Resumen: $csv" -ForegroundColor Green
Write-Host "Tabla:   $mdFile" -ForegroundColor Green
$rows | Format-Table config, fps_median, fps_p95, frame_median_ms, vram_max_mb, gpu_median_pct -AutoSize
