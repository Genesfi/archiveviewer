$file = Get-ChildItem "F:\Game\cos" -Filter "*Hoshilily*" | Select-Object -First 1
Write-Host "File found: $($file.FullName)"
& "C:\Program Files\7-Zip\7z.exe" l $file.FullName | Select-Object -First 40
