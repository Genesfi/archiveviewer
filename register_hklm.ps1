# Ensure script runs as Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (!$isAdmin) {
    Write-Host "Requesting Administrator privileges..."
    Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

$sourceDll = Resolve-Path "ArchiveThumbnailProvider.dll" | Select-Object -ExpandProperty Path
if (!(Test-Path $sourceDll)) {
    Write-Error "Could not find ArchiveThumbnailProvider.dll. Please compile the project first."
    exit 1
}

$sourceExe = Resolve-Path "ArchivePreviewer.exe" | Select-Object -ExpandProperty Path
if (!(Test-Path $sourceExe)) {
    Write-Error "Could not find ArchivePreviewer.exe. Please compile the project first."
    exit 1
}

# Create a trusted folder in Program Files
$destFolder = "C:\Program Files\ArchivePreviewer"
if (!(Test-Path $destFolder)) {
    New-Item -ItemType Directory -Path $destFolder -Force | Out-Null
}

$dllPath = Join-Path $destFolder "ArchiveThumbnailProvider.dll"
$exePath = Join-Path $destFolder "ArchivePreviewer.exe"

# IMPORTANT: Stop Windows Explorer BEFORE copying, otherwise it locks the DLL and fails to update it!
Write-Host "Stopping Windows Explorer to release locks on the DLL..."
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host "Copying DLL to trusted folder: $dllPath"
Copy-Item -Path $sourceDll -Destination $dllPath -Force

Write-Host "Copying EXE to trusted folder: $exePath"
Copy-Item -Path $sourceExe -Destination $exePath -Force

$clsid = "{C3861219-5E79-4B52-8706-03761D98357F}"
$providerIID = "{E357F5F9-A113-4973-B299-0F16C7D041EC}"

# Function to write registrations to a target hive
function Write-Registry($hive) {
    $clsidPath = "$hive\Software\Classes\CLSID\$clsid"
    $inprocPath = "$clsidPath\InprocServer32"
    
    Write-Host "Registering CLSID in $hive..."
    if (!(Test-Path $clsidPath)) {
        New-Item -Path $clsidPath -Force | Out-Null
    }
    New-ItemProperty -Path $clsidPath -Name "(default)" -Value "Archive Thumbnail Provider" -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $clsidPath -Name "DisableProcessIsolation" -Value 1 -PropertyType DWord -Force | Out-Null

    if (!(Test-Path $inprocPath)) {
        New-Item -Path $inprocPath -Force | Out-Null
    }
    New-ItemProperty -Path $inprocPath -Name "(default)" -Value $dllPath -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $inprocPath -Name "ThreadingModel" -Value "Apartment" -PropertyType String -Force | Out-Null

    # Register our own ProgID: ArchivePreviewer.AssocFile
    $progId = "ArchivePreviewer.AssocFile"
    $progIdPath = "$hive\Software\Classes\$progId"
    Write-Host "Registering ProgID $progId in $hive..."
    if (!(Test-Path $progIdPath)) {
        New-Item -Path $progIdPath -Force | Out-Null
    }
    New-ItemProperty -Path $progIdPath -Name "(default)" -Value "Archive Previewer File" -PropertyType String -Force | Out-Null

    $iconPath = "$progIdPath\DefaultIcon"
    if (!(Test-Path $iconPath)) {
        New-Item -Path $iconPath -Force | Out-Null
    }
    New-ItemProperty -Path $iconPath -Name "(default)" -Value "$exePath,0" -PropertyType String -Force | Out-Null

    $commandPath = "$progIdPath\shell\open\command"
    if (!(Test-Path $commandPath)) {
        New-Item -Path $commandPath -Force | Out-Null
    }
    New-ItemProperty -Path $commandPath -Name "(default)" -Value "`"$exePath`" `"%1`"" -PropertyType String -Force | Out-Null

    # Register thumbnail handler under our ProgID
    $progIdShellexPath = "$progIdPath\ShellEx\$providerIID"
    if (!(Test-Path $progIdShellexPath)) {
        New-Item -Path $progIdShellexPath -Force | Out-Null
    }
    New-ItemProperty -Path $progIdShellexPath -Name "(default)" -Value $clsid -PropertyType String -Force | Out-Null

    # Register extensions
    $extensions = @(".zip", ".rar", ".7z")
    foreach ($ext in $extensions) {
        Write-Host "Registering extension $ext in $hive..."
        
        # Direct extension registry
        $shellexPath = "$hive\Software\Classes\$ext\ShellEx\$providerIID"
        if (!(Test-Path $shellexPath)) {
            New-Item -Path $shellexPath -Force | Out-Null
        }
        New-ItemProperty -Path $shellexPath -Name "(default)" -Value $clsid -PropertyType String -Force | Out-Null

        # Also add ArchivePreviewer.AssocFile to OpenWithProgids
        $openWithList = "$hive\Software\Classes\$ext\OpenWithProgids"
        if (!(Test-Path $openWithList)) {
            New-Item -Path $openWithList -Force | Out-Null
        }
        New-ItemProperty -Path $openWithList -Name $progId -Value "" -PropertyType String -Force | Out-Null
    }
}

# Write to both HKLM and HKCU
Write-Registry "HKLM:"
Write-Registry "HKCU:"

# Approve in Shell Extensions (HKLM and HKCU)
$approvedPaths = @(
    "HKLM:\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved",
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved"
)
foreach ($path in $approvedPaths) {
    if (!(Test-Path $path)) {
        New-Item -Path $path -Force | Out-Null
    }
    New-ItemProperty -Path $path -Name $clsid -Value "Archive Thumbnail Provider" -PropertyType String -Force | Out-Null
}

Write-Host "Registration completed successfully!"

# Force start explorer if it didn't start automatically
Start-Process explorer -ErrorAction SilentlyContinue
