@echo off
setlocal DisableDelayedExpansion
rem Russian Morrowind data: select Windows-1251 and bundled Unicode fonts.
rem Command-line overrides apply to this run; no user configuration is rewritten.
pushd "%~dp0" || exit /b 1
"%~dp0openmw.exe" --encoding win1251 --fallback Fonts_Font_0,MysticCards Fonts_Font_2,MysticCards %*
set "openmw_exit_code=%errorlevel%"
popd
exit /b %openmw_exit_code%
