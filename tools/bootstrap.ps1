$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed with exit code $LASTEXITCODE" }
}

$dependencies = @(
    @{ Path = '3rdparty/SDL';      Url = 'https://github.com/libsdl-org/SDL';  Commit = 'f00dc919ca35b9756316e86c4777392de442f9b2'; Label = 'release-3.4.20' },
    @{ Path = '3rdparty/imgui';    Url = 'https://github.com/ocornut/imgui';   Commit = 'c1aaac65e896a45f81f47a4d68f9959e0f86c859'; Label = 'v1.93.0' },
    @{ Path = '3rdparty/json';     Url = 'https://github.com/nlohmann/json';   Commit = '55f93686c01528224f448c19128836e7df245f72'; Label = 'v3.12.0' },
    @{ Path = '3rdparty/zstd';     Url = 'https://github.com/facebook/zstd';   Commit = 'f8745da6ff1ad1e7bab384bd1f9d742439278e99'; Label = 'v1.5.7' },
    @{ Path = '3rdparty/ZArchive'; Url = 'https://github.com/Exzap/ZArchive';  Commit = '965b66c8d67b6b7e30fd63b3b75aa91a99ff303b'; Label = 'master' },
    @{ Path = '3rdparty/stb';      Url = 'https://github.com/nothings/stb';    Commit = '6b5799bae8a1fe3792691ef4b72ae26434c3e799'; Label = 'master' }
)

if (-not (Test-Path .git)) {
    Invoke-Git init -b main
}

foreach ($dependency in $dependencies) {
    $path = $dependency.Path
    if (-not (Test-Path "$path/.git")) {
        Write-Host "Adding $path ($($dependency.Label))"
        Invoke-Git submodule add --depth 1 $dependency.Url $path
        Invoke-Git config -f .gitmodules "submodule.$path.shallow" true
    }
    Invoke-Git -C $path fetch --depth 1 origin $dependency.Commit
    Invoke-Git -C $path checkout --quiet $dependency.Commit
    Invoke-Git add $path
}
Invoke-Git add .gitmodules

Write-Host ''
Write-Host 'Submodules are pinned. Next:'
Write-Host '  cmake --preset release'
Write-Host '  cmake --build --preset release'
Write-Host '  ctest --preset release'
