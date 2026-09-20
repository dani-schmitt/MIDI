[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$LoopBe30MonRoot,

    [Parameter(Mandatory = $true)]
    [string]$ReleaseRoot,

    [string]$SubjectName = "Daniel Schmitt",
    [string]$SignToolPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-SignToolPath {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    $sdkSignTool = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
    if (Test-Path -LiteralPath $sdkSignTool -PathType Leaf) {
        return $sdkSignTool
    }

    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    throw "SignTool was not found."
}

function Get-MsiVersion {
    param([string]$Path)

    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.OpenDatabase($Path, 0)
    $view = $database.OpenView(
        "SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductVersion'")
    [void]$view.Execute()
    $record = $view.Fetch()
    if ($null -eq $record) {
        throw "ProductVersion was not found in '$Path'."
    }

    $version = $record.StringData(1)
    [void]$view.Close()
    return $version
}

function Get-Sha256 {
    param([string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha256.ComputeHash($stream))).Replace("-", "")
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function New-ManifestEntry {
    param(
        [string]$Path,
        [string]$Type,
        [string]$Edition,
        [string]$Architecture,
        [string]$ExpectedSubject,
        [string]$ResolvedSignTool
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Expected signed artifact was not found: '$Path'."
    }

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $verifyOutput = & $ResolvedSignTool verify /pa /all /v $resolvedPath 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool verification failed for '$resolvedPath'."
    }

    $verifyText = $verifyOutput -join [Environment]::NewLine
    if ($verifyText -notmatch "Number of warnings:\s+0" -or
        $verifyText -notmatch "Number of errors:\s+0") {
        throw "SignTool reported warnings or errors for '$resolvedPath'."
    }

    $timestampMatch = [regex]::Match(
        $verifyText,
        "The signature is timestamped:\s*(?<timestamp>[^\r\n]+)")
    if (-not $timestampMatch.Success) {
        throw "No timestamp was reported for '$resolvedPath'."
    }

    $signedFileCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate]::CreateFromSignedFile(
        $resolvedPath)
    $signerCertificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(
        $signedFileCertificate)
    $signerSimpleName = $signerCertificate.GetNameInfo(
        [System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName,
        $false)
    if ($signerSimpleName -ne $ExpectedSubject) {
        throw "Unexpected signer '$signerSimpleName' on '$resolvedPath'."
    }

    $timestampAuthorityMatch = [regex]::Match(
        $verifyText,
        "Timestamp Verified by:\s*\r?\n\s*Issued to:\s*(?<authority>[^\r\n]+)")
    if (-not $timestampAuthorityMatch.Success) {
        throw "No timestamp authority was reported for '$resolvedPath'."
    }

    $extension = [System.IO.Path]::GetExtension($resolvedPath).ToLowerInvariant()
    if ($extension -eq ".msi") {
        $version = Get-MsiVersion -Path $resolvedPath
    }
    else {
        $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($resolvedPath).FileVersion
    }

    return [ordered]@{
        file = [System.IO.Path]::GetFileName($resolvedPath)
        type = $Type
        edition = $Edition
        architecture = $Architecture
        version = $version
        bytes = (Get-Item -LiteralPath $resolvedPath).Length
        sha256 = Get-Sha256 -Path $resolvedPath
        signerSubject = $signerCertificate.Subject
        signerThumbprint = $signerCertificate.Thumbprint
        timestamp = $timestampMatch.Groups["timestamp"].Value.Trim()
        timestampAuthority = $timestampAuthorityMatch.Groups["authority"].Value.Trim()
        signatureStatus = "Valid"
        path = $resolvedPath
    }
}

$repositoryRootPath = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$monitorRootPath = (Resolve-Path -LiteralPath $LoopBe30MonRoot).Path
$releaseRootPath = (Resolve-Path -LiteralPath $ReleaseRoot).Path
$resolvedSignTool = Resolve-SignToolPath -RequestedPath $SignToolPath

$artifactDefinitions = @(
    @{ Path = "$repositoryRootPath\src\api\VSFiles\x64\Release\Midi2.LoopBe30MidiTransport.dll"; Type = "Transport"; Edition = "Retail"; Architecture = "x64" },
    @{ Path = "$repositoryRootPath\src\api\VSFiles\x64\Release_Trial\Midi2.LoopBe30MidiTransport.dll"; Type = "Transport"; Edition = "Trial"; Architecture = "x64" },
    @{ Path = "$repositoryRootPath\src\api\VSFiles\ARM64\Release\Midi2.LoopBe30MidiTransport.dll"; Type = "Transport"; Edition = "Retail"; Architecture = "ARM64" },
    @{ Path = "$repositoryRootPath\src\api\VSFiles\ARM64\Release_Trial\Midi2.LoopBe30MidiTransport.dll"; Type = "Transport"; Edition = "Trial"; Architecture = "ARM64" },
    @{ Path = "$monitorRootPath\bin\x64\Release\loough.exe"; Type = "Monitor"; Edition = "Retail"; Architecture = "x64" },
    @{ Path = "$monitorRootPath\bin\x64\Release_Demo\loough.exe"; Type = "Monitor"; Edition = "Trial"; Architecture = "x64" },
    @{ Path = "$monitorRootPath\bin\ARM64\Release\loough.exe"; Type = "Monitor"; Edition = "Retail"; Architecture = "ARM64" },
    @{ Path = "$monitorRootPath\bin\ARM64\Release_Demo\loough.exe"; Type = "Monitor"; Edition = "Trial"; Architecture = "ARM64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\custom-actions\bin\x64\Release\net472\customactions.dll"; Type = "CustomActionManaged"; Edition = "Shared"; Architecture = "x64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\custom-actions\bin\x64\Release\net472\customactions.CA.dll"; Type = "CustomActionPackage"; Edition = "Shared"; Architecture = "x64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\custom-actions\bin\ARM64\Release\net472\customactions.dll"; Type = "CustomActionManaged"; Edition = "Shared"; Architecture = "ARM64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\custom-actions\bin\ARM64\Release\net472\customactions.CA.dll"; Type = "CustomActionPackage"; Edition = "Shared"; Architecture = "ARM64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\api-package\bin\x64\Release\LoopBe30Setup.msi"; Type = "MSI"; Edition = "Retail"; Architecture = "x64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\api-package\bin\x64\Release\Trial\LoopBe30TrialSetup.msi"; Type = "MSI"; Edition = "Trial"; Architecture = "x64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\api-package\bin\ARM64\Release\LoopBe30Setup-arm64.msi"; Type = "MSI"; Edition = "Retail"; Architecture = "ARM64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\api-package\bin\ARM64\Release\Trial\LoopBe30TrialSetup-arm64.msi"; Type = "MSI"; Edition = "Trial"; Architecture = "ARM64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\main-bundle\obj\x64\Release\setuploopbe30-engine.exe"; Type = "BurnEngine"; Edition = "Retail"; Architecture = "x64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\main-bundle\obj\x64\Release\Trial\setuploopbe30trial-engine.exe"; Type = "BurnEngine"; Edition = "Trial"; Architecture = "x64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\main-bundle\obj\ARM64\Release\setuploopbe30-arm64-engine.exe"; Type = "BurnEngine"; Edition = "Retail"; Architecture = "ARM64" },
    @{ Path = "$repositoryRootPath\src\oob-setup-loopbe30\main-bundle\obj\ARM64\Release\Trial\setuploopbe30trial-arm64-engine.exe"; Type = "BurnEngine"; Edition = "Trial"; Architecture = "ARM64" },
    @{ Path = "$releaseRootPath\setuploopbe30.exe"; Type = "BurnBundle"; Edition = "Retail"; Architecture = "x64" },
    @{ Path = "$releaseRootPath\setuploopbe30trial.exe"; Type = "BurnBundle"; Edition = "Trial"; Architecture = "x64" },
    @{ Path = "$releaseRootPath\setuploopbe30-arm64.exe"; Type = "BurnBundle"; Edition = "Retail"; Architecture = "ARM64" },
    @{ Path = "$releaseRootPath\setuploopbe30trial-arm64.exe"; Type = "BurnBundle"; Edition = "Trial"; Architecture = "ARM64" }
)

$entries = foreach ($definition in $artifactDefinitions) {
    New-ManifestEntry `
        -Path $definition.Path `
        -Type $definition.Type `
        -Edition $definition.Edition `
        -Architecture $definition.Architecture `
        -ExpectedSubject $SubjectName `
        -ResolvedSignTool $resolvedSignTool
}

$manifest = [ordered]@{
    schemaVersion = 1
    product = "LoopBe30 for Windows MIDI Services"
    productVersion = "2.0.1"
    generatedUtc = (Get-Date).ToUniversalTime().ToString("o")
    signer = $SubjectName
    timestampService = "http://time.certum.pl"
    artifacts = @($entries)
}

$manifestPath = Join-Path $releaseRootPath "loopbe30-signing-manifest.json"
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Wrote signing manifest '$manifestPath'."

