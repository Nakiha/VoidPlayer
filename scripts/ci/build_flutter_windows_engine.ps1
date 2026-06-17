param(
    [ValidateSet("debug", "release", "all")]
    [string]$Mode = "debug",
    [string]$DepotTools = $env:DEPOT_TOOLS
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$FlutterRoot = if ($env:VOIDPLAYER_FLUTTER_BIN) {
    Split-Path (Split-Path $env:VOIDPLAYER_FLUTTER_BIN -Parent) -Parent
} else {
    Join-Path $Root ".toolchains/flutter"
}
$EngineSrc = Join-Path $FlutterRoot "engine/src"
if (-not $DepotTools) {
    $autoninja = Get-Command autoninja.bat -ErrorAction SilentlyContinue
    if ($autoninja) {
        $DepotTools = Split-Path $autoninja.Source -Parent
    }
}
if (-not $DepotTools -or -not (Test-Path (Join-Path $DepotTools "autoninja.bat"))) {
    throw "depot_tools is required. Pass -DepotTools <path> or set DEPOT_TOOLS."
}
$env:PATH = "$DepotTools;$env:PATH"
if (-not $env:DEPOT_TOOLS_WIN_TOOLCHAIN) {
    $env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"
}
if (-not $env:GYP_MSVS_VERSION) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found; install Visual Studio C++ tools or set GYP_MSVS_VERSION."
    }
    $installationVersion = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion
    $major = [int]($installationVersion -split "\.")[0]
    $env:GYP_MSVS_VERSION = if ($major -ge 18) { "2026" } else { "2022" }
}

foreach ($required in @("flutter/third_party/skia", "flutter/third_party/dart")) {
    if (-not (Test-Path (Join-Path $EngineSrc $required))) {
        throw "Incomplete engine checkout: missing $required. Sync Flutter engine dependencies before building."
    }
}

$targets = if ($Mode -eq "all") { @("debug", "release") } else { @($Mode) }
foreach ($target in $targets) {
    $name = if ($target -eq "debug") { "host_debug_unopt" } else { "host_release" }
    $arguments = @(
        (Join-Path $EngineSrc "flutter/tools/gn"),
        "--out-dir", $EngineSrc,
        "--target-dir", $name
    )
    if ($target -eq "debug") {
        $arguments += "--unoptimized"
    } else {
        $arguments += @("--runtime-mode", "release")
    }
    Push-Location $EngineSrc
    try {
        & python @arguments
        if ($LASTEXITCODE -ne 0) { throw "GN generation failed for $name" }
        & autoninja -C (Join-Path $EngineSrc "out/$name") `
            flutter_windows `
            gen_snapshot `
            flutter_patched_sdk `
            "flutter/build/dart:dart_sdk" `
            flutter_export.h `
            flutter_windows.h `
            "flutter/shell/platform/windows/client_wrapper:publish_wrapper_windows"
        if ($LASTEXITCODE -ne 0) { throw "Engine build failed for $name" }
    } finally {
        Pop-Location
    }
}
