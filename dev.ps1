# Development environment for SwitchGamesRecomp (Windows).
# Usage, in PowerShell:  . .\dev.ps1
# Finds the newest Visual Studio (Build Tools) with the C++ tools: 2022 (17) or 18. clang-cl comes from that install's
# "C++ Clang tools" when present, else from MSYS2 UCRT64 (C:\msys64\ucrt64\bin, LLVM targeting x86_64-pc-windows-msvc),
# which uses the Visual Studio headers and libraries of the shell set up here.
$vsRoots = @("${env:ProgramFiles(x86)}\Microsoft Visual Studio", "$env:ProgramFiles\Microsoft Visual Studio")
$vs = Get-ChildItem $vsRoots -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending |
    ForEach-Object { Get-ChildItem $_.FullName -Directory } |
    Where-Object { Test-Path "$($_.FullName)\Common7\Tools\Microsoft.VisualStudio.DevShell.dll" } |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $vs) { throw "No Visual Studio with C++ tools found" }
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"  # vswhere, which VsDevCmd calls
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
# clang, cmake and ninja from Visual Studio ahead of MinGW
$cm = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
$env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$cm\CMake\bin;$cm\Ninja;$env:PATH"
if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue) -and (Test-Path C:\msys64\ucrt64\bin\clang-cl.exe)) {
    $env:PATH = "$env:PATH;C:\msys64\ucrt64\bin"  # after VS's tools: its clang-cl and lld-link only
}
$env:PATH = "$PSScriptRoot\private\tools\nstool;$env:PATH"
# Vulkan SDK: headers, vk.xml (tools/gen_vk.py), validation layer (NX_VK_VALIDATION=1)
if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = (Get-ChildItem C:\VulkanSDK -Directory | Sort-Object Name | Select-Object -Last 1).FullName }
$env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH"
Set-Location $PSScriptRoot
Write-Host "SwitchGamesRecomp dev env: $vs | clang-cl $((Get-Command clang-cl).Source) | $(Get-Location)"
