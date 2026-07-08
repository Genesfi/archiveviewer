$clsid = "{C3861219-5E79-4B52-8706-03761D98357F}"
$providerIID = "{E357F5F9-A113-4973-B299-0F16C7D041EC}" # Correct GUID
$oldProviderIID = "{E357F5D9-A425-4581-85DB-385906E103E7}" # Incorrect old GUID

Write-Host "Removing COM registration..."
$hkcuClsidPath = "HKCU:\Software\Classes\CLSID\$clsid"
if (Test-Path $hkcuClsidPath) {
    Remove-Item -Path $hkcuClsidPath -Recurse -Force
}

$approvedPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved"
if (Test-Path $approvedPath) {
    Remove-ItemProperty -Path $approvedPath -Name $clsid -ErrorAction SilentlyContinue
}

$extensions = @(".zip", ".rar", ".7z")
foreach ($ext in $extensions) {
    # Remove both new and old keys
    Remove-Item -Path "HKCU:\Software\Classes\$ext\ShellEx\$providerIID" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path "HKCU:\Software\Classes\$ext\ShellEx\$oldProviderIID" -Recurse -Force -ErrorAction SilentlyContinue

    $progId = $null
    if (Test-Path "HKCU:\Software\Classes\$ext") {
        $progId = (Get-ItemProperty -Path "HKCU:\Software\Classes\$ext" -Name "(default)" -ErrorAction SilentlyContinue)."(default)"
    }
    if (!$progId -and (Test-Path "HKLM:\Software\Classes\$ext")) {
        $progId = (Get-ItemProperty -Path "HKLM:\Software\Classes\$ext" -Name "(default)" -ErrorAction SilentlyContinue)."(default)"
    }

    if ($progId) {
        Remove-Item -Path "HKCU:\Software\Classes\$progId\ShellEx\$providerIID" -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -Path "HKCU:\Software\Classes\$progId\ShellEx\$oldProviderIID" -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "Unregistration completed successfully!"
Write-Host "Restarting Windows Explorer..."
Stop-Process -Name explorer -Force
