# Sonic Loader wake-from-rest auto-redeployer (Windows PowerShell).
#
# Run on a PC that stays on. Polls the PS5 elfldr port every few
# seconds and re-sends elf-arsenal.elf the moment the PS5 wakes from
# rest mode and the elfldr daemon is back.
#
# Usage:
#   .\sonic-watchdog.ps1 -Ps5Ip 10.0.0.189 -ElfPath C:\path\elf-arsenal.elf
#
# Knobs you can override:
#   -ElfldrPort 9021
#   -LoaderPort 6969      (used to confirm rest mode actually happened
#                          before re-firing — prevents spurious
#                          redeploys when the watchdog itself restarts)
#   -ProbeIntervalSec 5
#   -SettleDelaySec   2
#
# Ctrl-C to stop.

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)] [string] $Ps5Ip,
    [Parameter(Mandatory=$true)] [string] $ElfPath,
    [int] $ElfldrPort       = 9021,
    [int] $LoaderPort       = 6969,
    [int] $ProbeIntervalSec = 5,
    [int] $SettleDelaySec   = 2
)

if (-not (Test-Path $ElfPath)) {
    throw "ELF not found: $ElfPath"
}

function Test-PortOpen {
    param([string]$Host, [int]$Port, [int]$TimeoutMs = 1500)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect($Host, $Port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs, $false)) {
            return $false
        }
        $client.EndConnect($iar)
        return $true
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

function Send-Elf {
    param([string]$Host, [int]$Port, [string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.Connect($Host, $Port)
        $stream = $client.GetStream()
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        # half-close so elfldr sees EOF and finishes loading.
        $client.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
        Start-Sleep -Milliseconds 500
        return $true
    } catch {
        Write-Host "sonic-watchdog: deploy failed: $_"
        return $false
    } finally {
        $client.Close()
    }
}

Write-Host "sonic-watchdog: PS5_IP=$Ps5Ip  ELF=$ElfPath"
Write-Host ("sonic-watchdog: probing :{0} every {1}s — Ctrl-C to stop" `
            -f $ElfldrPort, $ProbeIntervalSec)

$prevElfldrUp = $false
$loaderWasUp  = $false

while ($true) {
    $loaderUp = Test-PortOpen -Host $Ps5Ip -Port $LoaderPort
    if ($loaderUp) { $loaderWasUp = $true }

    $elfldrUp = Test-PortOpen -Host $Ps5Ip -Port $ElfldrPort
    if ($elfldrUp) {
        if (-not $prevElfldrUp) {
            # closed → open transition.
            $skip = $loaderWasUp -and (Test-PortOpen -Host $Ps5Ip -Port $LoaderPort)
            if ($skip) {
                Write-Host "sonic-watchdog: elfldr open + loader still up — first boot or no rest cycle. Skipping."
            } else {
                Start-Sleep -Seconds $SettleDelaySec
                $size = (Get-Item $ElfPath).Length
                $stamp = Get-Date -Format "HH:mm:ss"
                Write-Host ("sonic-watchdog: {0} sending {1} bytes to {2}:{3}…" `
                            -f $stamp, $size, $Ps5Ip, $ElfldrPort)
                if (Send-Elf -Host $Ps5Ip -Port $ElfldrPort -Path $ElfPath) {
                    Write-Host ("sonic-watchdog: {0} ✔ sent." -f (Get-Date -Format "HH:mm:ss"))
                    $loaderWasUp = $false
                }
            }
        }
        $prevElfldrUp = $true
    } else {
        if ($prevElfldrUp) {
            Write-Host ("sonic-watchdog: {0} PS5 unreachable — likely rest mode." `
                        -f (Get-Date -Format "HH:mm:ss"))
        }
        $prevElfldrUp = $false
        $loaderWasUp  = $false
    }

    Start-Sleep -Seconds $ProbeIntervalSec
}
