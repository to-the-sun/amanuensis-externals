@echo off
set "DEFAULT_PATH=D:\[Library]\[Documents]\Max 8\Library\analyze~\python\analyze_files.py"

rem Try the default library path first
if exist "%DEFAULT_PATH%" (
    set "SCRIPT_PATH=%DEFAULT_PATH%"
) else (
    rem Fallback to the same folder as the batch file
    set "SCRIPT_PATH=%~dp0analyze_files.py"
)

if not exist "%SCRIPT_PATH%" (
    echo CRITICAL ERROR: analyze_files.py not found.
    echo.
    echo Looked in: "%DEFAULT_PATH%"
    echo And:      "%~dp0analyze_files.py"
    echo.
    echo Please ensure the script is installed in the Max 8 Library.
    pause
    exit /b 1
)

rem If no arguments provided (double-click), pass "." to force analysis of the current folder
if "%~1"=="" (
    python "%SCRIPT_PATH%" .
) else (
    rem Pass all drag-and-dropped files/folders
    python "%SCRIPT_PATH%" %*
)

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Analysis encountered an error (Code: %ERRORLEVEL%).
    pause
)
