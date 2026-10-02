@echo off
rem Development launcher: starts the build in build_m0 on the disc image named in DISC below.
rem Usage: play.bat                 the game's own front end (title, menus, career, save to rrjb_card.mcr)
rem        play.bat [set] [race]    straight into one race, e.g. play.bat 1 20
rem        play.bat jailbreak       straight into the Jailbreak mode (career venue 5) through the menus
rem Race keys: arrows or WASD - Up throttle, Down brake, Left/Right steer; Z/X punch, Q kick;
rem C camera, V look back; Esc quits (back to the menu when the race was started from it).
rem Menu keys: arrows, Enter select, Esc/Backspace back. Full list: rrgame --keys.
rem F2 4:3 with bars / widescreen, F3 dither, F4 15-bit colour.
rem Gamepad - Xbox pad, DualSense over USB or Bluetooth, or any DirectInput pad:
rem   R2 or Cross throttle, L2 or Square brake, d-pad or left stick steer,
rem   R1 punch / swing, L1 second punch, Triangle kick, hold d-pad Up / Down with them for the other attacks,
rem   Circle look back, Create or touchpad camera, L3 taunt, Options pause. Xbox: A B X Y = Cross Circle Square
rem   Triangle, RB / LB / RT / LT = R1 / L1 / R2 / L2. Rebind in controls.ini next to rrgame.exe; rrgame --keys.
rem No parenthesised blocks below on purpose: the disc path contains "(USA)", and cmd ends a
rem block at the first ")" it expands inside one.

setlocal
cd /d "%~dp0"

rem The disc image: RRJB_DISC, else the first line of disc.txt next to this file.
set "DISC=%RRJB_DISC%"
if "%DISC%"=="" if exist "%~dp0disc.txt" set /p DISC=<"%~dp0disc.txt"
set "GAME=%~dp0build_m0\rrgame.exe"
set "SET=%~1"
set "RACE=%~2"
rem Everything after the race number goes to rrgame unchanged (e.g. --camera 1).
set "EXTRA="
shift
shift
:collect
if "%~1"=="" goto collected
set "EXTRA=%EXTRA% %1"
shift
goto collect
:collected
if not exist "%GAME%" goto nogame
if not exist "%DISC%" goto nodisc

rem The window normally opens without taking the keyboard (so scripted runs never steal focus);
rem started by hand it should get the keyboard at once.
set "RRGAME_FOCUS=1"

if "%SET%"=="" goto frontend
if /i "%SET%"=="jailbreak" goto jailbreak
if "%RACE%"=="" set "RACE=20"
"%GAME%" "%DISC%" --race %SET% %RACE%%EXTRA%
if errorlevel 1 goto failed
goto end

:jailbreak
"%GAME%" "%DISC%" --jailbreak
if errorlevel 1 goto failed
goto end

:frontend
"%GAME%" "%DISC%"
if errorlevel 1 goto failed
goto end

:nogame
echo rrgame.exe not found: "%GAME%"
echo Build it first: build.cmd build_m0
pause
goto end

:nodisc
echo Disc image not found: "%DISC%"
pause
goto end

:failed
echo.
echo rrgame exited with an error, see the output above.
pause

:end
endlocal
