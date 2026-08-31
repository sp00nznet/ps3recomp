@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "G:\recomp\ps3games\ps3recomp"
cl /nologo /O1 /W3 /I runtime\ppu /Fe:"G:\recomp\ps3games\ps3recomp\scratch\ppu_conformance.exe" "G:\recomp\ps3games\ps3recomp\scratch\ppu_conformance.cpp" > "G:\recomp\ps3games\ps3recomp\scratch\ppu_conformance.log" 2>&1
if errorlevel 1 exit /b 2
"G:\recomp\ps3games\ps3recomp\scratch\ppu_conformance.exe" >> "G:\recomp\ps3games\ps3recomp\scratch\ppu_conformance.log" 2>&1
