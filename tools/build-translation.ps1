# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidatePattern('^$|^v[0-9]+$')]
    [string]$PlatformToolset = '',
    [ValidatePattern('^$|^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$WindowsSdkVersion = '',
    [string]$VisualStudioPath = '',
    [ValidateRange(1, 64)]
    [int]$Jobs = 4,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
$sumatraRoot = Split-Path -Parent $PSScriptRoot
$solutionPath = Join-Path $sumatraRoot 'vs2022\SumatraPDF.sln'
if (-not (Test-Path -LiteralPath $solutionPath -PathType Leaf)) {
    throw "Source solution not found: $solutionPath"
}

function Test-VsRoot([string]$Candidate) {
    return $Candidate -and
        (Test-Path -LiteralPath (Join-Path $Candidate 'MSBuild\Current\Bin\MSBuild.exe') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Candidate 'VC\Tools\MSVC') -PathType Container)
}

function Find-VsRoot {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath -PathType Leaf) {
        $candidate = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (Test-VsRoot $candidate) { return $candidate }
    }

    $msbuild = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($msbuild) {
        $candidate = Split-Path -Parent $msbuild.Source
        while ($candidate) {
            if (Test-VsRoot $candidate) { return $candidate }
            $parent = Split-Path -Parent $candidate
            if ($parent -eq $candidate) { break }
            $candidate = $parent
        }
    }

    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Select-Object -Unique) {
        foreach ($version in @('18', '2022')) {
            foreach ($edition in @('BuildTools', 'Community', 'Professional', 'Enterprise')) {
                $candidate = Join-Path $base "Microsoft Visual Studio\$version\$edition"
                if (Test-VsRoot $candidate) { return $candidate }
            }
        }
    }
    throw 'Visual Studio C++ tools were not found. Install the Desktop development with C++ workload or set -VisualStudioPath.'
}

if ($VisualStudioPath) {
    if (-not (Test-VsRoot $VisualStudioPath)) {
        throw "Visual Studio C++ tools were not found at: $VisualStudioPath"
    }
} else {
    $VisualStudioPath = Find-VsRoot
}
$msbuildPath = Join-Path $VisualStudioPath 'MSBuild\Current\Bin\MSBuild.exe'
$toolsetRoots = @(Get-ChildItem -Path (Join-Path $VisualStudioPath 'MSBuild\Microsoft\VC\v*\Platforms\x64\PlatformToolsets\*') -Directory -ErrorAction SilentlyContinue)
$availableToolsets = @($toolsetRoots | Where-Object { $_.Name -match '^v[0-9]+$' } | Select-Object -ExpandProperty Name -Unique | Sort-Object { [int]$_.Substring(1) } -Descending)
if (-not $PlatformToolset) {
    if ($availableToolsets.Count -eq 0) { throw 'No MSVC x64 platform toolset was found.' }
    $PlatformToolset = $availableToolsets[0]
} elseif ($PlatformToolset -notin $availableToolsets) {
    throw "Platform toolset $PlatformToolset is not installed. Available: $($availableToolsets -join ', ')"
}

$sdkRoots = @($env:WindowsSdkDir, (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10')) | Where-Object { $_ } | Select-Object -Unique
$installedSdks = @(
    foreach ($sdkRoot in $sdkRoots) {
        foreach ($sdk in Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory -ErrorAction SilentlyContinue) {
            if ($sdk.Name -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') { continue }
            $umHeader = Join-Path $sdk.FullName 'um\Windows.h'
            $ucrtHeader = Join-Path $sdk.FullName 'ucrt\stdio.h'
            $umLibrary = Join-Path $sdkRoot "Lib\$($sdk.Name)\um\x64\kernel32.lib"
            $ucrtLibrary = Join-Path $sdkRoot "Lib\$($sdk.Name)\ucrt\x64\ucrt.lib"
            if ((Test-Path -LiteralPath $umHeader) -and (Test-Path -LiteralPath $ucrtHeader) -and
                (Test-Path -LiteralPath $umLibrary) -and (Test-Path -LiteralPath $ucrtLibrary)) {
                [pscustomobject]@{ Version = $sdk.Name; Root = $sdkRoot }
            }
        }
    }
)
if ($WindowsSdkVersion) {
    $selectedSdk = $installedSdks | Where-Object { $_.Version -eq $WindowsSdkVersion } | Select-Object -First 1
} else {
    $selectedSdk = $installedSdks | Sort-Object { [version]$_.Version } -Descending | Select-Object -First 1
}
if (-not $selectedSdk) {
    throw 'A complete Windows 10/11 SDK for x64 was not found. Set -WindowsSdkVersion to an installed version.'
}
$WindowsSdkVersion = $selectedSdk.Version
$configDirectory = if ($Configuration -eq 'Debug') { 'dbg64' } else { 'rel64' }
$artifactPath = Join-Path $sumatraRoot "out\$configDirectory\SumatraPDF-static.exe"
$logDirectory = Join-Path $sumatraRoot 'out\build-translation'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $logDirectory "$($Configuration.ToLowerInvariant())-$stamp.log"
$receiptPath = Join-Path $logDirectory "$($Configuration.ToLowerInvariant())-$stamp.json"
$properties = "Configuration=$Configuration;Platform=x64;PlatformToolset=$PlatformToolset;WindowsTargetPlatformVersion=$WindowsSdkVersion;MultiProcessorCompilation=false"
$buildArguments = @(
    $solutionPath,
    '/t:SumatraPDF-static',
    "/p:$properties",
    "/m:$Jobs",
    '/v:minimal',
    '/nologo',
    "/flp:logfile=$logPath;verbosity=normal;encoding=UTF-8"
)

Write-Host "MSBuild: $msbuildPath"
Write-Host "Configuration: $Configuration x64; toolset: $PlatformToolset; SDK: $WindowsSdkVersion"
Write-Host "Output: $artifactPath"
if ($ValidateOnly) {
    [pscustomobject]@{
        Validation = 'Passed'
        MSBuild = $msbuildPath
        Arguments = $buildArguments
        Artifact = $artifactPath
        Log = $logPath
    } | ConvertTo-Json -Depth 4
    return
}

New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$startedUtc = [DateTime]::UtcNow.ToString('o')
Push-Location -LiteralPath $sumatraRoot
try {
    & $msbuildPath @buildArguments
    $buildExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($buildExitCode -ne 0) { throw "MSBuild failed with exit code $buildExitCode. Log: $logPath" }
if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
    throw "MSBuild returned success, but the executable was not found: $artifactPath"
}
$artifact = Get-Item -LiteralPath $artifactPath
[pscustomobject]@{
    SourceBaseline = 'b1a8e22577c9e35064315d279fc33ba6b67c5bef'
    Configuration = $Configuration
    Platform = 'x64'
    PlatformToolset = $PlatformToolset
    WindowsSdkVersion = $WindowsSdkVersion
    MSBuild = $msbuildPath
    Arguments = $buildArguments
    StartedUtc = $startedUtc
    FinishedUtc = [DateTime]::UtcNow.ToString('o')
    ExitCode = $buildExitCode
    Artifact = $artifactPath
    Bytes = $artifact.Length
    SHA256 = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Log = $logPath
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receiptPath -Encoding utf8
Write-Host "Build passed. Receipt: $receiptPath"
