@echo off
setlocal DisableDelayedExpansion
rem G.O.R.E: Morrowind 1.0 - Guts Overhaul ^& Ragdoll Extension.
pushd "%~dp0" || exit /b 1
"%~dp0openmw.exe" %*
set "gore_exit_code=%errorlevel%"
popd
exit /b %gore_exit_code%
