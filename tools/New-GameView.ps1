<#
.SYNOPSIS
    Crea una vista del juego CUSA00223 (base 1.00 + update 1.07) con hardlinks.
.DESCRIPTION
    El port nativo necesita una sola carpeta con el update aplicado encima del juego base.
    Copiar los ~33 GB no es necesario: en NTFS, los archivos se pueden enlazar con hardlinks,
    que comparten los mismos datos y no ocupan espacio extra.

    El script crea una carpeta nueva, enlaza todos los archivos del juego base y luego
    reemplaza los que vengan en el update. Se niega a borrar una carpeta existente que no
    haya sido creada por él (marcador .infamous-game-view).
.PARAMETER Base
    Carpeta del dump base CUSA00223 (con eboot.bin 1.00).
.PARAMETER Patch
    Carpeta del update CUSA00223-patch (con eboot.bin 1.07).
.PARAMETER Out
    Carpeta de salida. Debe estar en el mismo volumen que Base y Patch.
.PARAMETER ParamSfo
    Opcional. param.sfo de la versión 1.07. Si no se indica, se usa el del update.
.EXAMPLE
    .\tools\New-GameView.ps1 `
      -Base  'C:\...\work\games\CUSA00223' `
      -Patch 'C:\...\work\games\CUSA00223-patch' `
      -Out   'C:\...\work\games\CUSA00223-view'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Base,
    [Parameter(Mandatory)][string]$Patch,
    [Parameter(Mandatory)][string]$Out,
    [string]$ParamSfo
)

$ErrorActionPreference = 'Stop'
$notGame = @('patch0', 'patch-meta', 'system_lib', 'itemzflow-dumper.log', '.filelist')

function Get-NormalizedPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd('\')
}

$Base = Get-NormalizedPath $Base
$Patch = Get-NormalizedPath $Patch
$Out = Get-NormalizedPath $Out

foreach ($pair in @(@('Base', $Base), @('Patch', $Patch))) {
    if (-not (Test-Path -LiteralPath $pair[1] -PathType Container)) {
        throw "$($pair[0]) no existe o no es una carpeta: $($pair[1])"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $Base 'eboot.bin') -PathType Leaf)) {
    throw "Base no parece un dump del juego: falta eboot.bin en $Base"
}
if (-not (Test-Path -LiteralPath (Join-Path $Patch 'eboot.bin') -PathType Leaf)) {
    throw "Patch no parece el update: falta eboot.bin en $Patch"
}

$baseRoot = [IO.Path]::GetPathRoot($Base)
$patchRoot = [IO.Path]::GetPathRoot($Patch)
$outRoot = [IO.Path]::GetPathRoot($Out)
if ($baseRoot -ne $patchRoot -or $baseRoot -ne $outRoot) {
    throw "Los hardlinks exigen el mismo volumen. Base=$baseRoot Patch=$patchRoot Out=$outRoot"
}

$marker = Join-Path $Out '.infamous-game-view'
if (Test-Path -LiteralPath $Out) {
    if (-not (Test-Path -LiteralPath $marker)) {
        throw "La salida existe y no tiene marcador de vista: $Out. No la borro."
    }
    Write-Host "Borrando vista anterior: $Out"
    Remove-Item -LiteralPath $Out -Recurse -Force
}

New-Item -ItemType Directory -Path $Out -Force | Out-Null
@(
    "base=$Base"
    "patch=$Patch"
    "created=$((Get-Date).ToString('o'))"
) | Set-Content -LiteralPath $marker -Encoding UTF8

$script:baseCount = 0
$script:patchCount = 0

function Link-Tree {
    param(
        [string]$Source,
        [string]$Destination,
        [bool]$Replace,
        [bool]$Top = $false
    )
    foreach ($entry in Get-ChildItem -LiteralPath $Source -Force) {
        if ($Top -and $notGame -contains $entry.Name) { continue }
        $dest = Join-Path $Destination $entry.Name
        if ($entry.PSIsContainer) {
            if (-not (Test-Path -LiteralPath $dest)) {
                New-Item -ItemType Directory -Path $dest -Force | Out-Null
            }
            Link-Tree -Source $entry.FullName -Destination $dest -Replace $Replace -Top $false
            continue
        }
        if (Test-Path -LiteralPath $dest) {
            if (-not $Replace) { continue }
            Remove-Item -LiteralPath $dest -Force
        }
        New-Item -ItemType HardLink -Path $dest -Target $entry.FullName | Out-Null
        if ($Replace) { $script:patchCount++ } else { $script:baseCount++ }
    }
}

Write-Host "Enlazando juego base..."
Link-Tree -Source $Base -Destination $Out -Replace $false -Top $true
Write-Host "Aplicando update encima..."
Link-Tree -Source $Patch -Destination $Out -Replace $true -Top $true

$paramTarget = Join-Path $Out 'sce_sys\param.sfo'
$candidates = @()
if ($ParamSfo) { $candidates += $ParamSfo }
$candidates += (Join-Path $Patch 'sce_sys\param.sfo')
$candidates += (Join-Path $Patch 'patch-meta\param.sfo')
$candidates += (Join-Path $Base 'patch-meta\param.sfo')
$chosen = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($chosen) {
    if (Test-Path -LiteralPath $paramTarget) { Remove-Item -LiteralPath $paramTarget -Force }
    New-Item -ItemType HardLink -Path $paramTarget -Target $chosen | Out-Null
    Write-Host "param.sfo: $chosen"
} else {
    Write-Warning 'No se encontró un param.sfo 1.07; el launcher puede avisar.'
}

$eboot = Join-Path $Out 'eboot.bin'
$ebootHash = (Get-FileHash -LiteralPath $eboot -Algorithm SHA256).Hash
$logical = (Get-ChildItem -LiteralPath $Out -Recurse -File -Force | Measure-Object Length -Sum)
Write-Host ''
Write-Host "Vista creada: $Out"
Write-Host "  archivos base enlazados : $script:baseCount"
Write-Host "  archivos update aplicados: $script:patchCount"
Write-Host "  tamaño lógico            : $([math]::Round($logical.Sum / 1GB, 2)) GiB (hardlinks: no ocupa eso)"
Write-Host "  eboot.bin SHA256         : $ebootHash"
Write-Host ''
Write-Host "Siguiente paso:"
Write-Host "  `$env:TLG_GAME_DIR = '$Out'"
Write-Host "  python port\run.py"
