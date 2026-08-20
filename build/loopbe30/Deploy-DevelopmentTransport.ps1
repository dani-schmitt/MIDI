[CmdletBinding()]
param(
    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$RepositoryRoot,

    [switch]$ForceTerminateHungService
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}
else {
    $RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
}

$source = Join-Path $RepositoryRoot "src\api\VSFiles\$Platform\$Configuration\Midi2.LoopBe30MidiTransport.dll"
$installDirectory = Join-Path $env:ProgramFiles "nerds.de\LoopBe30"
$target = Join-Path $installDirectory "Midi2.LoopBe30MidiTransport.dll"
$backup = Join-Path $installDirectory "Midi2.LoopBe30MidiTransport.dev-backup.dll"

if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "The development transport was not found at '$source'. Build it first."
}
if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
    throw "The installed LoopBe30 transport was not found at '$target'. Install LoopBe30 first."
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an Administrator PowerShell."
}

$service = Get-Service -Name midisrv -ErrorAction Stop
$serviceWasRunning = $service.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped
$monitorWasStopped = $false

$monitorProcesses = @(Get-Process -Name loough -ErrorAction SilentlyContinue)
if ($monitorProcesses.Count -gt 0) {
    Write-Host "Closing the LoopBe30 monitor so it cannot reconnect during service shutdown..."
    $monitorProcesses | Stop-Process -Force -ErrorAction Stop
    $monitorProcesses | Wait-Process -Timeout 10 -ErrorAction Stop
    $monitorWasStopped = $true
}

if ($serviceWasRunning) {
    Write-Host "Stopping Windows MIDI Service..."
    try {
        Stop-Service -Name midisrv -Force -ErrorAction Stop
    }
    catch {
        if (-not $ForceTerminateHungService) {
            throw "Windows MIDI Service did not stop normally. Close every MIDI application and retry. If it remains hung, rerun with -ForceTerminateHungService. The original error was: $($_.Exception.Message)"
        }

        $serviceProcessId = (Get-CimInstance Win32_Service -Filter "Name='midisrv'" -ErrorAction Stop).ProcessId
        if ($serviceProcessId -eq 0) {
            throw "Windows MIDI Service reported no running process after its stop command failed."
        }
        Write-Warning "Forcibly terminating hung Windows MIDI Service process $serviceProcessId."
        Stop-Process -Id $serviceProcessId -Force -ErrorAction Stop
    }
    (Get-Service -Name midisrv).WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Stopped,
        [TimeSpan]::FromSeconds(30))
}

try {
    if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
        Copy-Item -LiteralPath $target -Destination $backup -ErrorAction Stop
        Write-Host "Backed up the installed transport to '$backup'."
    }

    Copy-Item -LiteralPath $source -Destination $target -Force -ErrorAction Stop

    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($sourceHash -ne $installedHash) {
        throw "The installed DLL hash does not match the development build."
    }

    Write-Host "Installed development transport SHA-256: $installedHash"
}
finally {
    if ($serviceWasRunning) {
        Write-Host "Starting Windows MIDI Service..."
        Start-Service -Name midisrv -ErrorAction Stop
        (Get-Service -Name midisrv).WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Running,
            [TimeSpan]::FromSeconds(30))
    }
}

Start-Sleep -Seconds 2
$parameters = Get-ItemProperty -LiteralPath "HKLM:\SOFTWARE\nerds.de\LoopBe30\Parameters" -ErrorAction Stop
Write-Host "LoopBe30 development transport deployed. Actual ports: $($parameters.ActualPorts)."
if ($monitorWasStopped) {
    Write-Host "The tray monitor was closed for deployment. Start LoopBe30 again from the Start menu."
}
