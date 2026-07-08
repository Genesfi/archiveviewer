$dllPath = Resolve-Path "ArchiveThumbnailProvider.dll" | Select-Object -ExpandProperty Path
if (!(Test-Path $dllPath)) {
    Write-Error "Could not find ArchiveThumbnailProvider.dll. Please compile the project first."
    exit 1
}

$clsid = "{C3861219-5E79-4B52-8706-03761D98357F}"
$providerIID = "{E357F5F9-A113-4973-B299-0F16C7D041EC}" # Correct Registry GUID for IThumbnailProvider ShellEx

$hkcuClsidPath = "HKCU:\Software\Classes\CLSID\$clsid"
$hkcuInprocPath = "$hkcuClsidPath\InprocServer32"

Write-Host "Registering COM CLSID in HKCU..."
if (!(Test-Path $hkcuClsidPath)) {
    New-Item -Path $hkcuClsidPath -Force | Out-Null
}
New-ItemProperty -Path $hkcuClsidPath -Name "(default)" -Value "Archive Thumbnail Provider" -PropertyType String -Force | Out-Null
# Disable Process Isolation to force Windows Explorer to load the DLL directly into its process
New-ItemProperty -Path $hkcuClsidPath -Name "DisableProcessIsolation" -Value 1 -PropertyType DWord -Force | Out-Null

if (!(Test-Path $hkcuInprocPath)) {
    New-Item -Path $hkcuInprocPath -Force | Out-Null
}
New-ItemProperty -Path $hkcuInprocPath -Name "(default)" -Value $dllPath -PropertyType String -Force | Out-Null
New-ItemProperty -Path $hkcuInprocPath -Name "ThreadingModel" -Value "Apartment" -PropertyType String -Force | Out-Null

# Register in Approved Shell Extensions for current user
$approvedPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved"
Write-Host "Approving Shell Extension for current user..."
if (!(Test-Path $approvedPath)) {
    New-Item -Path $approvedPath -Force | Out-Null
}
New-ItemProperty -Path $approvedPath -Name $clsid -Value "Archive Thumbnail Provider" -PropertyType String -Force | Out-Null

$extensions = @(".zip", ".rar", ".7z")
foreach ($ext in $extensions) {
    Write-Host "Registering extension $ext..."
    
    # 1. Register extension directly
    $shellexPath = "HKCU:\Software\Classes\$ext\ShellEx\$providerIID"
    if (!(Test-Path $shellexPath)) {
        New-Item -Path $shellexPath -Force | Out-Null
    }
    New-ItemProperty -Path $shellexPath -Name "(default)" -Value $clsid -PropertyType String -Force | Out-Null

    # 2. Register under the active associated Program ID (ProgID)
    $progId = $null
    if (Test-Path "HKCU:\Software\Classes\$ext") {
        $progId = (Get-ItemProperty -Path "HKCU:\Software\Classes\$ext" -Name "(default)" -ErrorAction SilentlyContinue)."(default)"
    }
    if (!$progId -and (Test-Path "HKLM:\Software\Classes\$ext")) {
        $progId = (Get-ItemProperty -Path "HKLM:\Software\Classes\$ext" -Name "(default)" -ErrorAction SilentlyContinue)."(default)"
    }

    if ($progId) {
        Write-Host "Registering associated ProgID ($progId) for $ext..."
        $progIdPath = "HKCU:\Software\Classes\$progId"
        if (!(Test-Path $progIdPath)) {
            New-Item -Path $progIdPath -Force | Out-Null
        }
        $progIdShellexPath = "$progIdPath\ShellEx\$providerIID"
        if (!(Test-Path $progIdShellexPath)) {
            New-Item -Path $progIdShellexPath -Force | Out-Null
        }
        New-ItemProperty -Path $progIdShellexPath -Name "(default)" -Value $clsid -PropertyType String -Force | Out-Null
    }
}

Write-Host "Registration completed successfully!"
Write-Host "Restarting Windows Explorer to clear caches and apply changes..."
Stop-Process -Name explorer -Force
