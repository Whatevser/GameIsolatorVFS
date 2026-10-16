@echo off
setlocal EnableExtensions
rem GameIsolatorVFS offline installer.
rem Put install.bat, steam_api64.vfs.dll and vfsproxy.ini into the game's
rem   ...\bin\Win64_Shipping_Client\  folder, then double-click this file.
cd /d "%~dp0"

if not exist "steam_api64.vfs.dll" (
    echo [X] steam_api64.vfs.dll is missing - keep it next to install.bat.
    pause & exit /b 1
)
if not exist "steam_api64.dll" if not exist "steam_api64_orig.dll" (
    echo [X] No steam_api64.dll here. Run this inside the game's
    echo     ...\bin\Win64_Shipping_Client\ folder.
    pause & exit /b 1
)

if exist "steam_api64_orig.dll" (
    echo [=] steam_api64_orig.dll already present - refreshing the proxy.
) else (
    ren "steam_api64.dll" "steam_api64_orig.dll"
    echo [+] renamed steam_api64.dll -^> steam_api64_orig.dll
)

copy /y "steam_api64.vfs.dll" "steam_api64.dll" >nul
echo [+] installed steam_api64.dll

if exist "vfscore.dll" ( del /q "vfscore.dll" & echo [+] removed obsolete vfscore.dll )

if not exist "vfsproxy.ini" (
    echo [!] vfsproxy.ini is missing - copy it here and set your real-^>virtual paths,
    echo     or nothing will be redirected.
) else (
    echo [i] Make sure vfsproxy.ini has your real-^>virtual path pairs.
)

echo [OK] Done. Launch the game; check vfsproxy.log for "attach ... hooks enabled".
pause
