@echo off
setlocal DisableDelayedExpansion
rem Enable bundled Russian-capable Unicode fonts without changing data encoding.
rem Use this for Cyrillic input with English data or an already configured game.
pushd "%~dp0" || exit /b 1
"%~dp0openmw.exe" --fallback Fonts_Font_0,MysticCards Fonts_Font_2,MysticCards %*
set "openmw_exit_code=%errorlevel%"
popd
exit /b %openmw_exit_code%
