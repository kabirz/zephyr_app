$softwareList = @(
    'Git.Git',
    '7zip.7zip',
    'Kitware.CMake',
    'GitHub.cli',
    'GnuPG.Gpg4win',
    'GnuWin32.Make',
    'JernejSimoncic.Wget',
    'Ninja-build.Ninja',
    'Python.Python.3.10'
)

foreach ($software in $softwareList) {
    winget list --id $software
    if (!$?) {
        Write-Host "Installing $software..." -ForegroundColor Green
        winget install --id $software
        if ($?) {
            Write-Host "$software Install successfully!" -ForegroundColor Green
        } else {
            Write-Host "$software Install Failed." -ForegroundColor Red
        }
    }
}

Write-Host "All software Installed." -ForegroundColor Green

