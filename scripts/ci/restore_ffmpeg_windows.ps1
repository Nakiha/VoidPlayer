param(
    [string]$ArtifactDir = "build/ci-ffmpeg"
)

$ErrorActionPreference = "Stop"

$artifactName = "voidplayer-ffmpeg-windows-x64-n8.1"
$unpackDir = Join-Path $ArtifactDir "unpacked"
$zip = Get-ChildItem $ArtifactDir -Recurse -Filter "$artifactName.zip" | Select-Object -First 1

if (-not $zip) {
    throw "Windows FFmpeg artifact zip not found under $ArtifactDir"
}

Remove-Item -Recurse -Force $unpackDir -ErrorAction SilentlyContinue
Expand-Archive -Force $zip.FullName $unpackDir
Remove-Item -Recurse -Force "windows/libs/ffmpeg" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "windows/libs/ffmpeg" | Out-Null
Move-Item (Join-Path $unpackDir "*") "windows/libs/ffmpeg/"

$dll = "windows/libs/ffmpeg/bin/avcodec-62.dll"
if (-not (Test-Path $dll)) {
    throw "Windows FFmpeg DLL is missing: $dll"
}

if ((Get-Item $dll).Length -lt 1000000) {
    throw "Windows FFmpeg DLL is unexpectedly small: $dll"
}

$bytes = [System.IO.File]::ReadAllBytes($dll)
$prefix = [System.Text.Encoding]::ASCII.GetString($bytes, 0, [Math]::Min($bytes.Length, 64))
if ($prefix.StartsWith("version https://git-lfs.github.com/spec/v1")) {
    throw "Windows FFmpeg DLL is still a Git LFS pointer: $dll"
}
