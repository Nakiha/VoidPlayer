param(
    [ValidateSet("debug", "release", "all")]
    [string]$Mode = "debug",
    [string]$ArtifactDirectory = $env:VOIDPLAYER_FLUTTER_WINDOWS_ENGINE_ARTIFACT_DIR
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$Lock = Get-Content (Join-Path $Root "toolchains/flutter.lock.json") -Raw |
    ConvertFrom-Json
if (-not $Lock.windowsLocalEngineArtifacts) {
    throw "windowsLocalEngineArtifacts is not published in flutter.lock.json"
}
$Repo = $Lock.forkRemote -replace "^https://github.com/", "" -replace "\.git$", ""
$ReleaseTag = $Lock.windowsEngineReleaseTag
$Out = Join-Path $Root ".toolchains/flutter/engine/src/out"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Temp = Join-Path ([System.IO.Path]::GetTempPath()) "voidplayer-windows-engine-$PID"
New-Item -ItemType Directory -Force -Path $Temp | Out-Null
try {
    $modes = if ($Mode -eq "all") { @("debug", "release") } else { @($Mode) }
    foreach ($item in $modes) {
        $spec = $Lock.windowsLocalEngineArtifacts.$item
        if (-not $spec) { throw "Missing Windows engine artifact spec: $item" }
        $archive = Join-Path $Temp $spec.asset
        if ($ArtifactDirectory) {
            $source = Join-Path $ArtifactDirectory $spec.asset
            if (-not (Test-Path $source)) {
                throw "Windows engine artifact not found: $source"
            }
            Copy-Item -LiteralPath $source -Destination $archive
        } else {
            if (-not $ReleaseTag) {
                throw "No windowsEngineReleaseTag is published. Pass -ArtifactDirectory or set VOIDPLAYER_FLUTTER_WINDOWS_ENGINE_ARTIFACT_DIR."
            }
            & gh release download $ReleaseTag -R $Repo --pattern $spec.asset --dir $Temp --clobber
            if ($LASTEXITCODE -ne 0) { throw "Failed to download $($spec.asset)" }
        }
        $actual = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
        if ($actual -ne $spec.sha256.ToLowerInvariant()) {
            throw "SHA-256 mismatch for $($spec.asset)"
        }
        Expand-Archive -Path $archive -DestinationPath $Out -Force
    }
} finally {
    if (Test-Path $Temp) { Remove-Item -LiteralPath $Temp -Recurse -Force }
}
