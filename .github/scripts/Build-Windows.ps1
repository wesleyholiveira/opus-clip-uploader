[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ($DebugPreference -eq 'Continue') {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ($env:CI -eq $null) {
    throw "Build-Windows.ps1 requires CI environment"
}

if (-not [System.Environment]::Is64BitOperatingSystem) {
    throw "A 64-bit system is required to build the project."
}

if ($PSVersionTable.PSVersion -lt '7.2.0') {
    Write-Warning 'The obs-studio PowerShell build script requires PowerShell Core 7.'
    exit 2
}

function Build {
    trap {
        Pop-Location -Stack BuildTemp -ErrorAction 'SilentlyContinue'
        Write-Error $_
        Log-Group
        exit 2
    }

    $ProjectRoot = (Resolve-Path -Path "$PSScriptRoot/../..").Path

    $UtilityFunctions = Get-ChildItem -Path "$PSScriptRoot/utils.pwsh/*.ps1" -Recurse

    foreach ($Utility in $UtilityFunctions) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    Push-Location -Stack BuildTemp
    Ensure-Location $ProjectRoot

    $ConfigurePreset = "windows-ci-${Target}"
    $BuildPreset = "windows-ci-${Target}"
    $BuildDir = "$ProjectRoot/build_${Target}"
    $IsNinjaPreset = $false

    if ($Target -eq "x64") {
        $ConfigurePreset = "windows-ninja-cuda-x64"
        $BuildPreset = "windows-ninja-cuda-x64"
        $BuildDir = "$ProjectRoot/build_cuda"
        $IsNinjaPreset = $true
    }

    $CmakeArgs = @(
        '--preset', $ConfigurePreset
    )

    if (-not [string]::IsNullOrWhiteSpace($env:CLIP_CROPPER_FFMPEG_RUNTIME_DIR)) {
        $FfmpegRuntimeDir = $env:CLIP_CROPPER_FFMPEG_RUNTIME_DIR.Replace('\', '/')
        $CmakeArgs += "-DCLIP_CROPPER_FFMPEG_RUNTIME_DIR=${FfmpegRuntimeDir}"
    }

    $CmakeBuildArgs = @(
        '--build'
        '--preset', $BuildPreset
        '--parallel'
    )

    $CmakeInstallArgs = @(
        '--install', $BuildDir
        '--prefix', "$ProjectRoot/release/${Configuration}"
    )

    if (-not $IsNinjaPreset) {
        $CmakeBuildArgs += @('--config', $Configuration)
        $CmakeInstallArgs += @('--config', $Configuration)
    }

    if ($DebugPreference -eq 'Continue') {
        $CmakeArgs += '--debug-output'
        $CmakeBuildArgs += '--verbose'
        $CmakeInstallArgs += '--verbose'
    }

    Log-Group "Configuring ${ProductName}..."
    Invoke-External cmake @CmakeArgs

    Log-Group "Building ${ProductName}..."
    Invoke-External cmake @CmakeBuildArgs

    Log-Group "Installing ${ProductName}..."
    Invoke-External cmake @CmakeInstallArgs

    Pop-Location -Stack BuildTemp
    Log-Group
}

Build
