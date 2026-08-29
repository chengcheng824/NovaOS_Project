@echo off
rem count.nsh - if/goto loop demo
set n=3
:loop
echo count = %n%
if %n%==1 goto end
if %n%==3 goto three
if %n%==2 goto two
:three
set n=2
goto loop
:two
set n=1
goto loop
:end
echo done!
pause
