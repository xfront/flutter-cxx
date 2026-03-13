@echo off
rem ------------------------------------------------------------------------
rem cui one-click build entry (pure CMake script, no Git Bash needed).
rem
rem   build.cmd                     Windows debug full build + ctest
rem   build.cmd release             Windows release
rem   build.cmd android --configure-only
rem   build.cmd --help              list platforms / profiles / options
rem
rem Implementation lives in cmake\build.cmake: arguments are translated into
rem CMakePresets invocations, so script builds match IDE / CI preset builds.
rem (ASCII only here on purpose: cmd's batch parser chokes on UTF-8 text and
rem  on LF-only line endings.)
rem ------------------------------------------------------------------------
setlocal
cmake -P "%~dp0cmake\build.cmake" -- %*
exit /b %ERRORLEVEL%
