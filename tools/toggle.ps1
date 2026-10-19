#Requires -Version 7.0
<#
.SYNOPSIS
    A/B en caliente de las optimizaciones del port (sin reiniciar el juego).

.DESCRIPTION
    El port lee TLG_TOGGLE_FILE cada 250 ms: el numero decimal del fichero es la mascara de
    optimizaciones DESACTIVADAS (bit = 1 desactiva). Con este script se listan y se cambian por
    nombre, mirando el HUD (F10) para ver el efecto inmediato.

    El juego debe arrancar con TLG_TOGGLE_FILE apuntando a un fichero (el launcher lo hace si se
    le pasa la variable). Ejemplo de sesion:

        $env:TLG_TOGGLE_FILE = 'C:\Users\luisd\proyectos\infamous-port\port\out\toggles.txt'
        # ... lanzar el juego ...
        .\tools\toggle.ps1 -List
        .\tools\toggle.ps1 -Disable HotPages
        .\tools\toggle.ps1 -Reset

.PARAMETER List
    Lista las optimizaciones con su bit.

.PARAMETER Disable
    Nombres a desactivar (se acumulan con lo que ya este desactivado).

.PARAMETER Enable
    Nombres a reactivar (se quitan de la mascara).

.PARAMETER Mask
    Mascara decimal completa (tiene prioridad sobre -Disable/-Enable).

.PARAMETER Reset
    Deja la mascara a 0: todas las optimizaciones activas.

.PARAMETER File
    Fichero de toggles (por defecto, $env:TLG_TOGGLE_FILE).

.EXAMPLE
    .\tools\toggle.ps1 -List
    .\tools\toggle.ps1 -Disable RegionCache,HotPages
    .\tools\toggle.ps1 -Reset
#>
[CmdletBinding()]
param(
    [switch]$List,
    [string[]]$Disable = @(),
    [string[]]$Enable = @(),
    [string]$Mask,
    [switch]$Reset,
    [string]$File = $env:TLG_TOGGLE_FILE
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$header = Join-Path $root 'port\gpu\shim\tlg_toggles.h'

$bits = [ordered]@{}
foreach ($line in Get-Content $header) {
    if ($line -match '^\s*([A-Z][A-Za-z0-9]*)\s*=\s*(1ull\s*<<\s*\d+|1u\s*<<\s*\d+|\d+)\s*,') {
        $name = $Matches[1]
        $expr = $Matches[2] -replace '\s+', ''
        if ($expr -match '^1(?:ull|u)<<(\d+)$') {
            $bits[$name] = [uint64]1 -shl [int]$Matches[1]
        } else {
            $bits[$name] = [uint64]$expr
        }
    }
}

if ($List -or (-not $Disable -and -not $Enable -and -not $Mask -and -not $Reset)) {
    Write-Host 'Optimizaciones (bit = 1 -> desactivada):' -ForegroundColor Cyan
    foreach ($k in $bits.Keys) {
        Write-Host ('  {0,-26} {1,14}' -f $k, $bits[$k])
    }
    Write-Host ''
    Write-Host ("Fichero: " + $(if ($File) { $File } else { '(sin TLG_TOGGLE_FILE)' }))
    if (-not $File) { return }
}

if (-not $File) {
    throw 'Falta TLG_TOGGLE_FILE: indica -File o arranca el juego con la variable puesta.'
}

$current = 0
if (Test-Path -LiteralPath $File -PathType Leaf) {
    $text = (Get-Content -LiteralPath $File -Raw).Trim()
    if ($text -match '^\d+$') { $current = [uint64]$text }
}

$mask = $current
if ($Reset) {
    $mask = 0
} elseif ($Mask) {
    $mask = [uint64]$Mask
} else {
    foreach ($name in $Disable) {
        if (-not $bits.Contains($name)) { throw "No conozco la optimizacion '$name' (-List para verlas)" }
        $mask = $mask -bor $bits[$name]
    }
    foreach ($name in $Enable) {
        if (-not $bits.Contains($name)) { throw "No conozco la optimizacion '$name' (-List para verlas)" }
        $mask = $mask -band (-bnot $bits[$name])
    }
}

$dir = Split-Path -Parent $File
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
Set-Content -LiteralPath $File -Value $mask -Encoding ascii
Write-Host ("Mascara escrita en {0}: {1} (antes {2})" -f $File, $mask, $current) -ForegroundColor Green
foreach ($k in $bits.Keys) {
    if (($mask -band $bits[$k]) -ne 0) { Write-Host ("  desactivada: {0}" -f $k) }
}
