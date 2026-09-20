[CmdletBinding()]
param(
    [switch]$Signed,
    [switch]$ConfirmSimpleSignReady,
    [switch]$X64RetailInstallerOnly,
    [switch]$X64TrialInstallerOnly,
    [string]$RepositoryRoot,
    [string]$MonitorRepositoryRoot,
    [string]$CertificateThumbprint
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($X64RetailInstallerOnly -and $X64TrialInstallerOnly) {
    throw "Select either X64RetailInstallerOnly or X64TrialInstallerOnly, not both."
}

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}
else {
    $RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
}

if ([string]::IsNullOrWhiteSpace($MonitorRepositoryRoot)) {
    $MonitorRepositoryRoot = (Resolve-Path (Join-Path $RepositoryRoot "..\midi2")).Path
}
else {
    $MonitorRepositoryRoot = (Resolve-Path -LiteralPath $MonitorRepositoryRoot).Path
}

$apiRoot = Join-Path $RepositoryRoot "src\api"
$setupRoot = Join-Path $RepositoryRoot "src\oob-setup-loopbe30"
$stagingRoot = Join-Path $RepositoryRoot "build\staging"
$signingRoot = Join-Path $RepositoryRoot "build\signing"
$signScript = Join-Path $signingRoot "Sign-LoopBe30File.ps1"
$manifestScript = Join-Path $signingRoot "New-LoopBe30SigningManifest.ps1"
$releaseRoot = Join-Path $RepositoryRoot (
    "build\release\LoopBe30 2.0.1 {0} ({1})" -f `
        $(if ($Signed) { "Signed" } elseif ($X64RetailInstallerOnly) { "Unsigned x64 Retail" } elseif ($X64TrialInstallerOnly) { "Unsigned x64 Trial" } else { "Unsigned" }),
        (Get-Date -Format "yyyy-MM-dd HH-mm-ss"))

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "Visual Studio Installer's vswhere.exe was not found."
}

$installationPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if ([string]::IsNullOrWhiteSpace($installationPath)) {
    throw "A Visual Studio installation containing MSBuild was not found."
}

$msbuild = Join-Path $installationPath "MSBuild\Current\Bin\amd64\MSBuild.exe"
if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
    throw "MSBuild was not found at '$msbuild'."
}

$env:MIDI_REPO_ROOT = $RepositoryRoot.TrimEnd('\') + '\'
if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $env:LOOPBE30_SIGNING_THUMBPRINT = $CertificateThumbprint.Replace(" ", "")
}

function Invoke-MsBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Project,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$Platform,
        [hashtable]$Properties = @{},
        [string]$Target = "Rebuild",
        [switch]$Restore
    )

    $arguments = @(
        $Project,
        "/t:$Target",
        "/m:1",
        "/v:minimal",
        "/p:Configuration=$Configuration",
        "/p:Platform=$Platform",
        "/p:WindowsTargetPlatformVersion=10.0.26100.0"
    )
    if ($Restore) {
        $arguments += "/restore"
    }
    foreach ($item in $Properties.GetEnumerator()) {
        $arguments += "/p:$($item.Key)=$($item.Value)"
    }

    Write-Host "MSBuild $([IO.Path]::GetFileName($Project)) $Configuration|$Platform"
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed for '$Project' ($Configuration|$Platform)."
    }
}

function Invoke-Sign {
    param([string]$Path, [string]$Description)

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $signScript `
        -Path $Path `
        -Description $Description `
        -SubjectName "Daniel Schmitt" `
        -TimestampUrl "http://time.certum.pl" `
        -ConfirmSimpleSignReady
    if ($LASTEXITCODE -ne 0) {
        throw "Signing failed for '$Path'."
    }
}

function Assert-MsiUpgradeSequence {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The LoopBe30 MSI was not found for sequence validation: '$Path'."
    }

    $installer = $null
    $database = $null
    $view = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = $installer.OpenDatabase($Path, 0)
        $view = $database.OpenView(
            'SELECT `Action`,`Sequence` FROM `InstallExecuteSequence`')
        $view.Execute()

        $sequences = @{}
        while ($null -ne ($record = $view.Fetch())) {
            $action = $record.StringData(1)
            if ($action -in @(
                "InstallInitialize",
                "RemoveExistingProducts",
                "InstallFiles",
                "InstallFinalize",
                "Action_LaunchLoopBe30Mon")) {
                $sequences[$action] = $record.IntegerData(2)
            }
        }

        $requiredActions = @(
            "InstallInitialize",
            "RemoveExistingProducts",
            "InstallFiles",
            "InstallFinalize",
            "Action_LaunchLoopBe30Mon"
        )
        foreach ($action in $requiredActions) {
            if (-not $sequences.ContainsKey($action)) {
                throw "The LoopBe30 MSI is missing the '$action' execute-sequence action."
            }
        }

        if (-not (
            $sequences["InstallInitialize"] -lt $sequences["RemoveExistingProducts"] -and
            $sequences["RemoveExistingProducts"] -lt $sequences["InstallFiles"] -and
            $sequences["InstallFiles"] -lt $sequences["InstallFinalize"] -and
            $sequences["InstallFinalize"] -lt $sequences["Action_LaunchLoopBe30Mon"])) {
            throw "The LoopBe30 MSI upgrade sequence is unsafe. Related products must be removed before new files are installed, and the Monitor must launch only after installation finalizes."
        }

        Write-Host "Validated safe MSI upgrade sequence in '$Path'."
    }
    finally {
        if ($null -ne $view) {
            try { $view.Close() } catch { }
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
        }
        if ($null -ne $database) {
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
        }
        if ($null -ne $installer) {
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
        }
    }
}

if ($Signed) {
    if (-not $ConfirmSimpleSignReady) {
        throw "Sign in to Certum SimplySign Desktop and rerun with -ConfirmSimpleSignReady."
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $signScript `
        -SubjectName "Daniel Schmitt" `
        -TimestampUrl "http://time.certum.pl" `
        -ConfirmSimpleSignReady `
        -PreflightOnly
    if ($LASTEXITCODE -ne 0) {
        throw "The Certum SimplySign certificate preflight failed."
    }
}

New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null

$dependencyProjects = @(
    (Join-Path $apiRoot "idl\IDL.vcxproj"),
    (Join-Path $apiRoot "Libs\MidiEndpointNamingLib\MidiEndpointNamingLib.vcxproj"),
    (Join-Path $apiRoot "Libs\MidiPluginConfigurationLib\MidiPluginConfigurationLib.vcxproj")
)
$transportProject = Join-Path $apiRoot "Transport\LoopBe30MidiTransport\Midi2.LoopBe30Transport.vcxproj"
$testProject = Join-Path $apiRoot "Test\LoopBe30.Transport.unittests\LoopBe30.Transport.unittests.vcxproj"
$monitorProject = Join-Path $MonitorRepositoryRoot "loough\loough.vcxproj"
$customActionProject = Join-Path $setupRoot "custom-actions\customactions.csproj"
$msiProject = Join-Path $setupRoot "api-package\api-package.wixproj"
$bundleProject = Join-Path $setupRoot "main-bundle\main-bundle.wixproj"

$helpCompiler = Join-Path ${env:ProgramFiles(x86)} "HTML Help Workshop\hhc.exe"
$helpProject = Join-Path $MonitorRepositoryRoot "loough\hlp\loough.hhp"
if (-not (Test-Path -LiteralPath $helpCompiler -PathType Leaf)) {
    throw "HTML Help Workshop is required to build loough.chm."
}
& $helpCompiler $helpProject
# hhc.exe historically returns 1 after a successful compile.
if ($LASTEXITCODE -notin @(0, 1) -or
    -not (Test-Path -LiteralPath (Join-Path $MonitorRepositoryRoot "loough\hlp\loough.chm") -PathType Leaf)) {
    throw "The LoopBe30 help project did not produce loough.chm."
}

$x64InstallerOnly = $X64RetailInstallerOnly -or $X64TrialInstallerOnly
$platforms = if ($x64InstallerOnly) { @("x64") } else { @("x64", "ARM64") }
$coreConfigurations = if ($x64InstallerOnly) { @("Release") } else { @("Debug", "Release") }
$monitorConfigurations = if ($X64RetailInstallerOnly) { @("Release") } elseif ($X64TrialInstallerOnly) { @("Release_Demo") } else { @("Debug", "Release", "Release_Demo") }
$editions = if ($X64RetailInstallerOnly) { @("Retail") } elseif ($X64TrialInstallerOnly) { @("Trial") } else { @("Retail", "Trial") }

foreach ($platform in $platforms) {
    foreach ($configuration in $coreConfigurations) {
        foreach ($dependencyProject in $dependencyProjects) {
            Invoke-MsBuild -Project $dependencyProject -Configuration $configuration -Platform $platform `
                -Properties @{ SolutionDir = "$apiRoot\" }
        }

        Invoke-MsBuild -Project $transportProject -Configuration $configuration -Platform $platform `
            -Properties @{ SolutionDir = "$apiRoot\"; LoopBe30Edition = "Retail" }
        Invoke-MsBuild -Project $testProject -Configuration $configuration -Platform $platform `
            -Properties @{ SolutionDir = "$apiRoot\" }

        $testExecutable = Join-Path $apiRoot "VSFiles\$platform\$configuration\LoopBe30.Transport.unittests.exe"
        if ($platform -eq "x64") {
            & $testExecutable
            if ($LASTEXITCODE -ne 0) {
                throw "LoopBe30 transport tests failed for $configuration|$platform."
            }
        }
    }

    foreach ($monitorConfiguration in $monitorConfigurations) {
        Invoke-MsBuild -Project $monitorProject -Configuration $monitorConfiguration -Platform $platform `
            -Properties @{ SolutionDir = "$MonitorRepositoryRoot\" }
    }

    if (-not $X64RetailInstallerOnly) {
        Invoke-MsBuild -Project $transportProject -Configuration "Release" -Platform $platform `
            -Properties @{ SolutionDir = "$apiRoot\"; LoopBe30Edition = "Trial" }
    }

    foreach ($edition in $editions) {
        $transportConfiguration = if ($edition -eq "Trial") { "Release_Trial" } else { "Release" }
        $transportOutput = Join-Path $apiRoot "VSFiles\$platform\$transportConfiguration\Midi2.LoopBe30MidiTransport.dll"
        $stageRootForEdition = if ($edition -eq "Trial") {
            Join-Path $stagingRoot "loopbe30-trial"
        }
        else {
            $stagingRoot
        }
        $stageFolder = Join-Path $stageRootForEdition "api\$platform"
        New-Item -ItemType Directory -Path $stageFolder -Force | Out-Null

        if ($Signed) {
            Invoke-Sign -Path $transportOutput -Description "LoopBe30 $edition Transport"
        }
        Copy-Item -LiteralPath $transportOutput -Destination $stageFolder -Force
    }

    if ($Signed) {
        if (-not $X64TrialInstallerOnly) {
            Invoke-Sign -Path (Join-Path $MonitorRepositoryRoot "bin\$platform\Release\loough.exe") `
                -Description "LoopBe30 Monitor"
        }
        if (-not $X64RetailInstallerOnly) {
            Invoke-Sign -Path (Join-Path $MonitorRepositoryRoot "bin\$platform\Release_Demo\loough.exe") `
                -Description "LoopBe30 Trial Monitor"
        }
    }

    $signingProperties = @{
        MIDI_REPO_ROOT = "$RepositoryRoot\"
        LoopBe30Sign = $Signed.ToString().ToLowerInvariant()
        LoopBe30SigningConfirmed = $Signed.ToString().ToLowerInvariant()
        LoopBe30SigningSubject = "Daniel Schmitt"
        LoopBe30TimestampUrl = "http://time.certum.pl"
        LoopBe30SigningScript = $signScript
    }
    Invoke-MsBuild -Project $customActionProject -Configuration "Release" -Platform $platform `
        -Properties $signingProperties -Restore

    foreach ($edition in $editions) {
        $installerProperties = $signingProperties.Clone()
        $installerProperties["SolutionDir"] = "$setupRoot\"
        $installerProperties["LoopBe30Edition"] = $edition
        $installerProperties["LoopBe30RequireDeveloperMode"] = (-not $Signed).ToString().ToLowerInvariant()
        $installerProperties["BuildProjectReferences"] = "false"

        Invoke-MsBuild -Project $msiProject -Configuration "Release" -Platform $platform `
            -Properties $installerProperties -Restore

        $suffix = if ($platform -eq "ARM64") { "-arm64" } else { "" }
        $editionFolder = if ($edition -eq "Trial") { "Trial" } else { "" }
        $msiName = if ($edition -eq "Trial") {
            "LoopBe30TrialSetup$suffix.msi"
        }
        else {
            "LoopBe30Setup$suffix.msi"
        }
        $msiOutput = Join-Path $setupRoot "api-package\bin\$platform\Release"
        if ($editionFolder) {
            $msiOutput = Join-Path $msiOutput $editionFolder
        }
        Assert-MsiUpgradeSequence -Path (Join-Path $msiOutput $msiName)

        Invoke-MsBuild -Project $bundleProject -Configuration "Release" -Platform $platform `
            -Properties $installerProperties -Restore

        $setupName = if ($edition -eq "Trial") {
            "setuploopbe30trial$suffix.exe"
        }
        else {
            "setuploopbe30$suffix.exe"
        }
        $bundleOutput = Join-Path $setupRoot "main-bundle\bin\$platform\Release"
        if ($editionFolder) {
            $bundleOutput = Join-Path $bundleOutput $editionFolder
        }
        Copy-Item -LiteralPath (Join-Path $bundleOutput $setupName) `
            -Destination (Join-Path $releaseRoot $setupName) -Force
    }
}

$expectedInstallers = if ($X64RetailInstallerOnly) {
    @("setuploopbe30.exe")
}
elseif ($X64TrialInstallerOnly) {
    @("setuploopbe30trial.exe")
}
else {
    @(
        "setuploopbe30.exe",
        "setuploopbe30trial.exe",
        "setuploopbe30-arm64.exe",
        "setuploopbe30trial-arm64.exe"
    )
}
$actualInstallers = @(Get-ChildItem -LiteralPath $releaseRoot -Filter "*.exe" | Select-Object -ExpandProperty Name)
if (@(Compare-Object $expectedInstallers $actualInstallers).Count -ne 0) {
    throw "The release folder does not contain exactly the four required installers."
}

if ($Signed) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $manifestScript `
        -RepositoryRoot $RepositoryRoot `
        -LoopBe30MonRoot $MonitorRepositoryRoot `
        -ReleaseRoot $releaseRoot `
        -SubjectName "Daniel Schmitt"
    if ($LASTEXITCODE -ne 0) {
        throw "Signed artifact verification or manifest generation failed."
    }
}

Write-Host "LoopBe30 release artifacts: $releaseRoot"
Get-ChildItem -LiteralPath $releaseRoot | Select-Object Name, Length
