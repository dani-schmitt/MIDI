[CmdletBinding()]
param(
    [string[]]$Path = @(),
    [string]$Description = "LoopBe30 component",
    [string]$SubjectName = "Daniel Schmitt",
    [string]$CertificateThumbprint = $env:LOOPBE30_SIGNING_THUMBPRINT,
    [string]$TimestampUrl = "http://time.certum.pl",
    [string]$SignToolPath,
    [switch]$ConfirmSimpleSignReady,
    [switch]$PreflightOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-SignToolPath {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Leaf)) {
            throw "SignTool was not found at '$RequestedPath'."
        }

        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    $windowsKitsRoot = ${env:ProgramFiles(x86)}
    $sdkSignTool = Join-Path $windowsKitsRoot "Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
    if (Test-Path -LiteralPath $sdkSignTool -PathType Leaf) {
        return $sdkSignTool
    }

    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    throw "SignTool was not found. Install the Windows SDK 10.0.26100.0 signing tools."
}

function Test-CodeSigningEku {
    param([System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate)

    foreach ($usage in $Certificate.EnhancedKeyUsageList) {
        $objectId = if ($usage.ObjectId -is [System.Security.Cryptography.Oid]) {
            $usage.ObjectId.Value
        }
        else {
            [string]$usage.ObjectId
        }

        if ($objectId -eq "1.3.6.1.5.5.7.3.3") {
            return $true
        }
    }

    return $false
}

if (-not $ConfirmSimpleSignReady) {
    throw "Signing confirmation is missing. Sign in to Certum SimplySign Desktop, then rerun with -ConfirmSimpleSignReady."
}

$resolvedSignTool = Resolve-SignToolPath -RequestedPath $SignToolPath
$now = Get-Date
$normalizedRequestedThumbprint = $CertificateThumbprint -replace "\s", ""

$matchingCertificates = @(
    Get-ChildItem -Path Cert:\CurrentUser\My |
        Where-Object {
            $_.GetNameInfo(
                [System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName,
                $false) -eq $SubjectName -and
            $_.NotBefore -le $now -and
            $_.NotAfter -gt $now -and
            $_.HasPrivateKey -and
            (Test-CodeSigningEku -Certificate $_)
        }
)

if (-not [string]::IsNullOrWhiteSpace($normalizedRequestedThumbprint)) {
    $matchingCertificates = @(
        $matchingCertificates |
            Where-Object {
                ($_.Thumbprint -replace "\s", "") -eq $normalizedRequestedThumbprint
            }
    )
}

if ($matchingCertificates.Count -eq 0) {
    throw "No valid CurrentUser\My code-signing certificate for '$SubjectName' with private-key access is available. Confirm that Certum SimplySign Desktop is signed in."
}

if ($matchingCertificates.Count -gt 1) {
    $candidateList = ($matchingCertificates | ForEach-Object {
        "$($_.Thumbprint) (expires $($_.NotAfter.ToString('u')))"
    }) -join "; "

    throw "Multiple valid code-signing certificates match '$SubjectName': $candidateList. Set LOOPBE30_SIGNING_THUMBPRINT for this signing run."
}

$selectedCertificate = $matchingCertificates[0]
Write-Host "Signing certificate: $($selectedCertificate.Subject)"
Write-Host "Certificate thumbprint: $($selectedCertificate.Thumbprint)"
Write-Host "Certificate expires: $($selectedCertificate.NotAfter.ToString('u'))"
Write-Host "Timestamp service: $TimestampUrl"

if ($PreflightOnly) {
    Write-Host "Certum SimplySign signing preflight succeeded."
    return
}

if ($Path.Count -eq 0) {
    throw "At least one file path is required unless -PreflightOnly is specified."
}

foreach ($inputPath in $Path) {
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
        throw "Signing input was not found: '$inputPath'."
    }

    $resolvedPath = (Resolve-Path -LiteralPath $inputPath).Path
    $extension = [System.IO.Path]::GetExtension($resolvedPath).ToLowerInvariant()
    if ($extension -notin @(".dll", ".exe", ".msi")) {
        throw "Authenticode signing is not enabled for '$extension' files: '$resolvedPath'."
    }

    $signArguments = @(
        "sign",
        "/v",
        "/s", "My",
        "/fd", "SHA256",
        "/tr", $TimestampUrl,
        "/td", "SHA256",
        "/d", $Description
    )

    if ([string]::IsNullOrWhiteSpace($normalizedRequestedThumbprint)) {
        $signArguments += @("/a", "/n", $SubjectName)
    }
    else {
        $signArguments += @("/sha1", $selectedCertificate.Thumbprint)
    }

    $signArguments += $resolvedPath

    Write-Host "Signing '$resolvedPath'..."
    $signOutput = & $resolvedSignTool @signArguments 2>&1
    $signExitCode = $LASTEXITCODE
    $signOutput | ForEach-Object { Write-Host $_ }

    if ($signExitCode -ne 0) {
        throw "SignTool sign failed for '$resolvedPath' with exit code $signExitCode."
    }

    $verifyOutput = & $resolvedSignTool verify /pa /all /v $resolvedPath 2>&1
    $verifyExitCode = $LASTEXITCODE
    $verifyOutput | ForEach-Object { Write-Host $_ }

    if ($verifyExitCode -ne 0) {
        throw "SignTool verification failed for '$resolvedPath' with exit code $verifyExitCode."
    }

    $verifyText = $verifyOutput -join [Environment]::NewLine
    if ($verifyText -notmatch "Number of warnings:\s+0" -or
        $verifyText -notmatch "Number of errors:\s+0" -or
        $verifyText -notmatch "The signature is timestamped:") {
        throw "Signature verification for '$resolvedPath' did not report a clean RFC 3161 timestamped signature."
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $resolvedPath
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "PowerShell reports signature status '$($signature.Status)' for '$resolvedPath'."
    }

    if ($null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $selectedCertificate.Thumbprint) {
        throw "The signer certificate for '$resolvedPath' is not the selected '$SubjectName' certificate."
    }

    if ($null -eq $signature.TimeStamperCertificate) {
        throw "The signature for '$resolvedPath' does not contain a timestamp certificate."
    }

    Write-Host "Signed and verified '$resolvedPath'."
}

