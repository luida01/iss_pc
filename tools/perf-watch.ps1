#Requires -Version 7.0
<#
.SYNOPSIS
    Vigilancia de rendimiento: repite el juego con la configuracion dada y anota, en un CSV,
    los FPS estaticos (la intro, identica en cada ejecucion) y el uso de la GPU en PORCENTAJE
    (no memoria), el de la CPU, la VRAM y los draws por segundo.

.DESCRIPTION
    Cada pasada lanza tools\run-baseline.ps1 y despues lee:
      - result.json  -> FPS de mediana y p95 de la sesion entera
      - samples.csv  -> utilizacion de la GPU (mediana), de la CPU, VRAM
      - stdout.log   -> los primeros bloques de "Frame stats" (la intro: la misma secuencia en
                        todas las ejecuciones, por eso es la medicion comparable) y el pipe de
                        grabacion (draws/s)

    El CSV queda en evidence/perf-watch-<timestamp>.csv.

.PARAMETER Runs
    Cuantas pasadas repetir (por defecto 3).

.PARAMETER DurationSec
    Segundos de juego por pasada (por defecto 100).

.PARAMETER Tag
    Etiqueta de las pasadas.

.PARAMETER Env
    Variables de entorno para la pasada (p.ej. @{ TLG_ENGINE_FPS = '30' }).

.PARAMETER Ini
    Claves de tlg.ini para la pasada (p.ej. @{ preset = '1' }).

.EXAMPLE
    .\tools\perf-watch.ps1 -Runs 3 -DurationSec 100
    .\tools\perf-watch.ps1 -Runs 2 -Env @{ TLG_NO_PAGE_TRACK = '1' } -Tag 'sin-tracker'
#>
[CmdletBinding()]
param(
    [ValidateRange(1, 20)][int]$Runs = 3,
    [ValidateRange(30, 3600)][int]$DurationSec = 100,
    [string]$Tag = 'watch',
    [hashtable]$Env = @{},
    [hashtable]$Ini = @{},
    [string]$Scenario = ''
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)

$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$csv = Join-Path (Join-Path (Get-Location) 'evidence') "perf-watch-$stamp.csv"
'run,tag,estado,dur_s,fps_mediana,fps_p95,intro_fps_1,intro_fps_2,intro_fps_3,intro_fps_4,gpu_mediano_pct,gpu_max_pct,cpu_mediano_pct,vram_max_mb,draws_s,evidencia' |
    Out-File $csv -Encoding ascii

for ($i = 1; $i -le $Runs; $i++) {
    Write-Host "=== pasada $i/$Runs ($Tag) ===" -ForegroundColor Cyan
    $args = @{ DurationSec = $DurationSec; Resolution = '720p'; Tag = "$Tag-$i"; ExtraEnv = $Env }
    if ($Ini.Count -gt 0) { $args['IniExtra'] = $Ini }
    if ($Scenario) { $args['AutoPlayScenario'] = $Scenario }
    & '.\tools\run-baseline.ps1' @args | Out-Null

    $dir = Get-ChildItem 'evidence' -Directory -Filter "*-$Tag-$i" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $dir) { Write-Warning "sin evidencia para la pasada $i"; continue }

    $j = Get-Content (Join-Path $dir.FullName 'result.json') -Raw | ConvertFrom-Json
    $samples = Import-Csv (Join-Path $dir.FullName 'samples.csv') -ErrorAction SilentlyContinue
    $gpu = @($samples | ForEach-Object { [double]$_.gpu_util_percent } | Sort-Object)
    $cpu = @($samples | ForEach-Object { [double]$_.cpu_percent } | Sort-Object)
    $median = { param($a) if ($a.Count) { $a[[int]($a.Count / 2)] } else { 0 } }

    $intro = @(1..4 | ForEach-Object {
        $m = Select-String -Path (Join-Path $dir.FullName 'stdout.log') -Pattern 'Frame stats: ([0-9.]+) FPS' -ErrorAction SilentlyContinue |
            Select-Object -Skip ($_ - 1) -First 1
        if ($m) { $m.Matches[0].Groups[1].Value } else { '' }
    })
    $draws = 0
    $pipe = Select-String -Path (Join-Path $dir.FullName 'stdout.log') -Pattern '(\d+) draws/s' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($pipe) { $draws = $pipe.Matches[0].Groups[1].Value }

    $row = "{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15}" -f `
        $i, $Tag, $j.status, $j.duration_sec, $j.metrics.fps.median, $j.metrics.fps.p95,
        $intro[0], $intro[1], $intro[2], $intro[3],
        (& $median $gpu), ($gpu | Select-Object -Last 1), (& $median $cpu), $j.metrics.memory.vram_max_mb,
        $draws, $dir.Name
    $row | Out-File $csv -Append -Encoding ascii
    Write-Host ("    FPS estaticos (intro): {0} | GPU {1}% | CPU {2}% | draws/s {3} | VRAM {4} MB" -f `
        ($intro -join ', '), (& $median $gpu), (& $median $cpu), $draws, $j.metrics.memory.vram_max_mb)
}

Write-Host ""
Write-Host "CSV: $csv" -ForegroundColor Green
Import-Csv $csv | Format-Table run,estado,fps_mediana,intro_fps_1,intro_fps_2,gpu_mediano_pct,cpu_mediano_pct,draws_s -AutoSize
