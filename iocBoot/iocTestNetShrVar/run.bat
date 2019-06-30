setlocal
call %~dp0dllPath.bat
if "%EPICS_HOST_ARCH%" == "" (
    if exist "%~dp0..\..\bin\windows-x64\TestNetShrVar.exe" (
        set EPICS_HOST_ARCH=windows-x64
    )
    if exist "%~dp0..\..\bin\windows-x64-debug\TestNetShrVar.exe" (
        set EPICS_HOST_ARCH=windows-x64-debug
    )
    if exist "%~dp0..\..\bin\windows-x64-static\TestNetShrVar.exe" (
        set EPICS_HOST_ARCH=windows-x64-static
    )
)
%~dp0..\..\bin\%EPICS_HOST_ARCH%\TestNetShrVar.exe st.cmd
