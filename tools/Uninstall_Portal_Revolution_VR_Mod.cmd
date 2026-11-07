@ECHO OFF
rem Removes the Portal Revolution VR mod. Lives in the game folder.
cd /d "%~dp0"

:choice
set /P c=Uninstall the Portal Revolution VR mod, including VR\config.txt [Y/N]?
if /I "%c%" EQU "Y" goto :remove
if /I "%c%" EQU "N" goto :keep
goto :choice

:remove
del /Q ".\bin\win64\d3d11.dll"
RD /Q /S ".\VR"
echo The mod is uninstalled. You can delete this file.
pause
exit

:keep
echo Nothing was changed.
pause
exit
