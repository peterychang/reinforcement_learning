@echo off
setlocal

CALL .scripts\init.cmd

REM TODO: Is this necessary?
call "%InstallDir%\Common7\Tools\VsDevCmd.bat"

CALL .scripts\build.cmd
