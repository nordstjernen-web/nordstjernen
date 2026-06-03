param(
    [string]$Url = "about:start",
    [int]$Seconds = 5,
    [string]$MingwBin = "C:\msys64\mingw64\bin",
    [string]$Exe = ""
)

$ErrorActionPreference = "Stop"

if (-not $Exe) {
    $root = Resolve-Path (Join-Path $PSScriptRoot "..")
    $Exe = Join-Path $root "builddir\src\nordstjernen.exe"
}

if (-not (Test-Path -LiteralPath $Exe)) {
    throw "Browser binary not found: $Exe"
}

if (-not (Test-Path -LiteralPath $MingwBin)) {
    throw "MSYS2 MINGW64 bin directory not found: $MingwBin"
}

Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class NdWin32ErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint uMode);
}
"@

[NdWin32ErrorMode]::SetErrorMode(0x8003) | Out-Null
$env:PATH = "$MingwBin;$env:PATH"

$process = Start-Process -FilePath $Exe -ArgumentList @($Url) -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Seconds $Seconds
    if (Get-Process -Id $process.Id -ErrorAction SilentlyContinue) {
        Stop-Process -Id $process.Id -Force
        "smoke-launch-ok"
    } else {
        throw "Browser exited before the smoke interval elapsed"
    }
} finally {
    $leftover = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
    if ($leftover) {
        Stop-Process -Id $process.Id -Force
    }
}
