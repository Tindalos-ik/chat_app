[CmdletBinding()]
param(
    [string]$Distribution = 'Ubuntu-22.04',
    [string]$MediaServer = '/home/tindalos/ZLMediaKit/release/linux/Release/MediaServer',
    [string]$Config = '/mnt/d/temp/chat-video-probe/zlm.ini',
    [string]$LogDirectory = '/mnt/d/temp/chat-video-probe/zlm-logs'
)

$ErrorActionPreference = 'Stop'

$Host.UI.RawUI.WindowTitle = 'ZLMediaKit (WSL)'
# Keep stdin for heartbeats; the Linux supervisor cleans up even if closing the window leaves WSL alive.
$launcher = Join-Path $PSScriptRoot 'start_zlmediakit.sh'
$linuxLauncher = & wsl.exe --distribution $Distribution --exec wslpath -u $launcher
if ($LASTEXITCODE -ne 0) {
    throw 'Cannot resolve the ZLMediaKit launcher path in WSL.'
}

$arguments = @('--distribution', $Distribution, '--exec', 'bash', $linuxLauncher,
    $MediaServer, $Config, $LogDirectory)
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = 'wsl.exe'
# Windows PowerShell 5.1 lacks ArgumentList; quote each argument using Windows argv rules.
$startInfo.Arguments = ($arguments | ForEach-Object {
    if ($_ -match '[\s"]' -or $_ -eq '') {
        '"' + [regex]::Replace(([regex]::Replace($_, '(\\*)"', '$1$1\"')), '(\\+)$', '$1$1') + '"'
    }
    else { $_ }
}) -join ' '
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $startInfo
$started = $false

try {
    $started = $process.Start()
    while (-not $process.HasExited) {
        try {
            $process.StandardInput.WriteLine('heartbeat')
            $process.StandardInput.Flush()
        }
        catch {
            if (-not $process.HasExited) { throw }
            break
        }
        if ($process.WaitForExit(1000)) { break }
    }
    $exitCode = $process.ExitCode
}
finally {
    if ($started) {
        # Normal exits close stdin immediately; forced console closure has a 5-second Linux deadline.
        $process.StandardInput.Close()
        $process.WaitForExit()
    }
    $process.Dispose()
}

if ($exitCode -ne 0) {
    throw "ZLMediaKit exited with code $exitCode."
}
