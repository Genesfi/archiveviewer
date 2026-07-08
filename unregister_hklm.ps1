# Ensure script runs as Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (!$isAdmin) {
    Write-Host "Requesting Administrator privileges..."
    Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

$clsid = "{C3861219-5E79-4B52-8706-03761D98357F}"
$providerIID = "{E357F5F9-A113-4973-B299-0F16C7D041EC}"
$progId = "ArchivePreviewer.AssocFile"

# Stop explorer to release locks on the DLL/EXE in Program Files
Write-Host "Restarting Windows Explorer to release DLL/EXE locks..."
Stop-Process -Name explorer -Force
Start-Sleep -Seconds 1

Write-Host "Removing HKLM and HKCU COM registrations..."
$hives = @("HKLM:", "HKCU:")
foreach ($hive in $hives) {
    # Remove CLSID
    $clsidPath = "$hive\Software\Classes\CLSID\$clsid"
    if (Test-Path $clsidPath) {
        Remove-Item -Path $clsidPath -Recurse -Force
    }

    # Remove ProgID
    $progIdPath = "$hive\Software\Classes\$progId"
    if (Test-Path $progIdPath) {
        Remove-Item -Path $progIdPath -Recurse -Force
    }

    $approvedPath = "$hive\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved"
    if (Test-Path $approvedPath) {
        Remove-ItemProperty -Path $approvedPath -Name $clsid -ErrorAction SilentlyContinue
    }

    $extensions = @(".zip", ".rar", ".7z")
    foreach ($ext in $extensions) {
        # Remove direct extensions
        Remove-Item -Path "$hive\Software\Classes\$ext\ShellEx\$providerIID" -Recurse -Force -ErrorAction SilentlyContinue

        # Remove from OpenWithProgids
        $openWithList = "$hive\Software\Classes\$ext\OpenWithProgids"
        if (Test-Path $openWithList) {
            Remove-ItemProperty -Path $openWithList -Name $progId -ErrorAction SilentlyContinue
        }

        # Reset default association if it was ours
        $currentDefault = (Get-ItemProperty -Path "$hive\Software\Classes\$ext" -Name "(default)" -ErrorAction SilentlyContinue)."(default)"
        if ($currentDefault -eq $progId) {
            Remove-ItemProperty -Path "$hive\Software\Classes\$ext" -Name "(default)" -ErrorAction SilentlyContinue
        }
    }
}

# Clean up Program Files directory
$destFolder = "C:\Program Files\ArchivePreviewer"
if (Test-Path $destFolder) {
    Write-Host "Cleaning up files in $destFolder..."
    Remove-Item -Path $destFolder -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Unregistration completed successfully!"
