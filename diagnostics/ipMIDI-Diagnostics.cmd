@echo off
setlocal EnableExtensions DisableDelayedExpansion

title ipMIDI Diagnostics

set "IPMIDI_KEY=HKLM\SOFTWARE\nerds.de\ipMIDI\Parameters"
set "LOG_FILE=%~dp0ipMIDI.log"
set "TEMP_REPORT=%TEMP%\ipMIDI-%RANDOM%-%RANDOM%.tmp"

call :WriteReport > "%TEMP_REPORT%" 2>&1

copy /y "%TEMP_REPORT%" "%LOG_FILE%" >nul 2>&1
if not errorlevel 1 goto LogReady

set "LOG_FILE=%USERPROFILE%\ipMIDI.log"
copy /y "%TEMP_REPORT%" "%LOG_FILE%" >nul 2>&1
if not errorlevel 1 goto LogReady

set "LOG_FILE="

:LogReady
type "%TEMP_REPORT%"
del /q "%TEMP_REPORT%" >nul 2>&1

echo.
if defined LOG_FILE (
    echo The report was written to:
    echo %LOG_FILE%
) else (
    echo WARNING: The report could not be saved to ipMIDI.log.
)

echo.
echo Please send the ipMIDI.log file to ipMIDI support if requested.
echo.
if /i "%~1"=="--no-pause" exit /b 0
pause
exit /b 0


:WriteReport
echo ============================================================
echo ipMIDI diagnostic report
echo ============================================================
echo Created: %DATE% %TIME%
echo Computer: %COMPUTERNAME%
for /f "delims=" %%V in ('ver') do echo Windows: %%V
echo Architecture: %PROCESSOR_ARCHITECTURE%
echo.

echo ipMIDI settings
echo ----------------
echo Registry location: %IPMIDI_KEY%
echo.

reg.exe query "%IPMIDI_KEY%" /reg:64 >nul 2>&1
if errorlevel 1 goto IpMidiKeyMissing

call :ShowRegistryValue WantedPorts "Ports after reboot"
call :ShowRegistryValue ActualPorts "Actual ports"
call :ShowRegistryValue MuteMask "Muted-port mask"
call :ShowLoopbackValue
goto IpMidiKeyComplete

:IpMidiKeyMissing
echo The ipMIDI settings were not found.
echo ipMIDI may not be installed, or it may not have been started yet.

:IpMidiKeyComplete
echo.
echo Windows MIDI Service
echo --------------------

where.exe powershell.exe >nul 2>&1
if errorlevel 1 goto PowerShellMissing

powershell.exe -NoLogo -NoProfile -Command ^
    "$ErrorActionPreference = 'Stop';" ^
    "try {" ^
    "  $service = Get-Service -Name 'midisrv' -ErrorAction SilentlyContinue;" ^
    "  if ($null -eq $service) { Write-Output 'Status: Not installed'; exit 0 };" ^
    "  $serviceKey = Get-ItemProperty -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\midisrv';" ^
    "  Write-Output ('Status: ' + $service.Status);" ^
    "  $startModes = @{ 2 = 'Automatic'; 3 = 'Manual (Trigger Start)'; 4 = 'Disabled' };" ^
    "  $startMode = $startModes[[int]$serviceKey.Start];" ^
    "  if ($null -eq $startMode) { $startMode = 'Other (' + $serviceKey.Start + ')' };" ^
    "  Write-Output ('Startup type: ' + $startMode);" ^
    "  Write-Output ('Service account: ' + $serviceKey.ObjectName);" ^
    "  $path = [Environment]::ExpandEnvironmentVariables($serviceKey.ImagePath.Trim([char]34));" ^
    "  Write-Output ('Executable: ' + $path);" ^
    "  if (Test-Path -LiteralPath $path) {" ^
    "    $version = (Get-Item -LiteralPath $path).VersionInfo;" ^
    "    Write-Output ('File version: ' + $version.FileVersion);" ^
    "    Write-Output ('Product version: ' + $version.ProductVersion);" ^
    "  } else { Write-Output 'Version: Executable not found' }" ^
    "} catch { Write-Output ('Unable to read MIDI Service information: ' + $_.Exception.Message) }"
goto ReportComplete

:PowerShellMissing
echo Status and version could not be read because Windows PowerShell was not found.

:ReportComplete
echo.
echo This report only reads information. It does not change Windows,
echo the MIDI Service, or any ipMIDI setting.
echo ============================================================
exit /b 0


:ShowRegistryValue
setlocal EnableDelayedExpansion
set "VALUE_NAME=%~1"
set "VALUE_LABEL=%~2"
set "VALUE_FOUND="
set "VALUE_TYPE="
set "VALUE_DATA="

for /f "tokens=1,2,*" %%A in ('reg.exe query "%IPMIDI_KEY%" /v "%~1" /reg:64 2^>nul') do (
    if /i "%%A"=="%~1" (
        set "VALUE_FOUND=1"
        set "VALUE_TYPE=%%B"
        set "VALUE_DATA=%%C"
    )
)

if not defined VALUE_FOUND (
    echo !VALUE_LABEL! ^(!VALUE_NAME!^): Not found
    endlocal
    exit /b 0
)

if /i not "!VALUE_TYPE!"=="REG_DWORD" (
    echo !VALUE_LABEL! ^(!VALUE_NAME!^): !VALUE_DATA! [!VALUE_TYPE!]
    endlocal
    exit /b 0
)

set /a "VALUE_DECIMAL=!VALUE_DATA!" >nul 2>&1
echo !VALUE_LABEL! ^(!VALUE_NAME!^): !VALUE_DECIMAL! [!VALUE_DATA!]
endlocal
exit /b 0


:ShowLoopbackValue
setlocal EnableDelayedExpansion
set "VALUE_FOUND="
set "VALUE_TYPE="
set "VALUE_DATA="

for /f "tokens=1,2,*" %%A in ('reg.exe query "%IPMIDI_KEY%" /v "Loopback" /reg:64 2^>nul') do (
    if /i "%%A"=="Loopback" (
        set "VALUE_FOUND=1"
        set "VALUE_TYPE=%%B"
        set "VALUE_DATA=%%C"
    )
)

if not defined VALUE_FOUND (
    echo Local loopback ^(Loopback^): Not found
    endlocal
    exit /b 0
)

if /i not "!VALUE_TYPE!"=="REG_DWORD" (
    echo Local loopback ^(Loopback^): !VALUE_DATA! [!VALUE_TYPE!]
    endlocal
    exit /b 0
)

if "!VALUE_DATA!"=="0x1" (
    echo Local loopback ^(Loopback^): On [!VALUE_DATA!]
) else if "!VALUE_DATA!"=="0x0" (
    echo Local loopback ^(Loopback^): Off [!VALUE_DATA!]
) else (
    echo Local loopback ^(Loopback^): Invalid value [!VALUE_DATA!]
)

endlocal
exit /b 0
