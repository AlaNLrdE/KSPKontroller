@echo off
rem Windows launcher for the ./ci bash script (PowerShell / cmd).
rem Prefers Git for Windows' bash; plain "bash" may be WSL, which can't see these paths.
setlocal
set "GITBASH=%ProgramFiles%\Git\bin\bash.exe"
if not exist "%GITBASH%" set "GITBASH=%ProgramFiles(x86)%\Git\bin\bash.exe"
if not exist "%GITBASH%" set "GITBASH=%LocalAppData%\Programs\Git\bin\bash.exe"
if not exist "%GITBASH%" (
  echo error: Git for Windows bash not found. Install Git or run ./ci from Git Bash. 1>&2
  exit /b 1
)
"%GITBASH%" "%~dp0ci" %*
exit /b %ERRORLEVEL%
