param(
    [ValidateSet("debug", "release")]
    [string]$Mode = "debug"
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$Lock = Get-Content (Join-Path $Root "toolchains/flutter.lock.json") -Raw |
    ConvertFrom-Json
$EngineSrc = if ($env:VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH) {
    $env:VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH
} else {
    Join-Path $Root ".toolchains/flutter/engine/src"
}
$Name = if ($Mode -eq "debug") { "host_debug_unopt" } else { "host_release" }
$Source = Join-Path $EngineSrc "out/$Name"
if (-not (Test-Path $Source)) {
    throw "Local engine output not found: $Source"
}

$Output = Join-Path $Root "build/flutter-engine-artifacts"
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Asset = "$($Lock.name)-$($Lock.forkCommit.Substring(0,12))-windows-$Name.zip"
$Archive = Join-Path $Output $Asset
if (Test-Path $Archive) { Remove-Item -LiteralPath $Archive }

$StageRoot = Join-Path ([System.IO.Path]::GetTempPath()) "voidplayer-windows-engine-package-$PID"
$Stage = Join-Path $StageRoot $Name
if (Test-Path $StageRoot) { Remove-Item -LiteralPath $StageRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
try {
    foreach ($dir in @("cpp_client_wrapper", "dart-sdk", "flutter_patched_sdk", "gen", "shader_lib")) {
        $path = Join-Path $Source $dir
        if (Test-Path $path) {
            Copy-Item -LiteralPath $path -Destination $Stage -Recurse
        }
    }

    $topLevelPatterns = @(
        "*.dat",
        "*.dill",
        "*.dll",
        "*.exe",
        "*.exp",
        "*.h",
        "*.json",
        "*.lib",
        "flutter_windows.dll.pdb"
    )
    foreach ($pattern in $topLevelPatterns) {
        Get-ChildItem -LiteralPath $Source -File -Filter $pattern |
            Copy-Item -Destination $Stage -Force
    }

    Compress-Archive -Path $Stage -DestinationPath $Archive -CompressionLevel Optimal
} finally {
    if (Test-Path $StageRoot) { Remove-Item -LiteralPath $StageRoot -Recurse -Force }
}

$Hash = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
"$Hash  $Asset" | Set-Content -Encoding ascii "$Archive.sha256"
Write-Host "$Archive"
Write-Host "sha256=$Hash"
