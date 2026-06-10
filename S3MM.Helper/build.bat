@echo off
REM Build S3MM.Helper.dll using .NET 2.0 compiler (same as mod compilation)
REM This matches the settings in mod_compiler.cpp

set CSC=C:\Windows\Microsoft.NET\Framework\v2.0.50727\csc.exe

if not exist "%CSC%" (
    echo ERROR: .NET 2.0 csc.exe not found at %CSC%
    exit /b 1
)

REM Check for mscorlib.dll - we need it as a reference
REM Look in common locations
set MSCORLIB=

if exist "%~dp0..\S3MM\Libs\mscorlib.dll" (
    set MSCORLIB=%~dp0..\S3MM\Libs\mscorlib.dll
) else if exist "C:\Windows\Microsoft.NET\Framework\v2.0.50727\mscorlib.dll" (
    set MSCORLIB=C:\Windows\Microsoft.NET\Framework\v2.0.50727\mscorlib.dll
)

if "%MSCORLIB%"=="" (
    echo ERROR: mscorlib.dll not found
    echo Please copy mscorlib.dll from Sims 3 game files to S3MM\Libs\
    exit /b 1
)

echo Building S3MM.Helper.dll...
echo Using mscorlib: %MSCORLIB%

"%CSC%" /noconfig /target:library /nostdlib /platform:x86 /nowarn:1701,1702 /out:S3MM.Helper.dll /reference:"%MSCORLIB%" S3MM.cs

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build failed!
    exit /b 1
)

echo.
echo Build successful!
echo.

REM Copy to S3MM\Libs if it exists
if exist "%~dp0..\S3MM\Libs" (
    copy /Y S3MM.Helper.dll "%~dp0..\S3MM\Libs\"
    echo Copied to S3MM\Libs\
)

echo.
echo Done! Mods can now reference S3MM.Helper.dll
