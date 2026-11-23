@echo off
rem What cmd.exe asks of the NT personality (docs/shell/s6.md step 6). Every line
rem starts W: and tests/corpus/cmd-expected.txt is what this same cmd.exe printed
rem under Wine; the boot gate compares the two, in order and complete.
setlocal enabledelayedexpansion
set work=%~dp0cmdt%RANDOM%
mkdir "%work%"
if not exist "%work%" (echo W:no-work-directory & exit /b 1)
cd /d "%work%"

rem ---- the language
set name=World
echo W:echo Hello %name%
set /a n=6*7
echo W:arith %n%
set /a m=(n+8)/5
echo W:arith2 %m%
set text=abcdefgh
echo W:substr %text:~2,3%
echo W:replace %text:cd=XY%
if %n%==42 (echo W:if-equal) else (echo W:no)
if not %n% GTR 50 echo W:if-compare
if /i "ABC"=="abc" echo W:if-nocase
if defined name echo W:if-defined
if not defined nothing echo W:if-undefined
for %%i in (one two three) do echo W:for %%i
for /l %%i in (1,2,5) do echo W:forl %%i
set total=0
for /l %%i in (1,1,4) do set /a total+=%%i
echo W:delayed !total!
goto skip
echo W:not-reached
:skip
echo W:goto
call :sub first second
echo W:after-call %errorlevel%
goto files
:sub
echo W:sub %1 %2
exit /b 7

:files
rem ---- files
echo line one> a.txt
echo line two>> a.txt
type a.txt > b.txt
for /f "tokens=2" %%w in (b.txt) do echo W:forf %%w
for /f "delims=" %%w in (a.txt) do echo W:line %%w
if exist a.txt echo W:exists
if not exist nope.txt echo W:missing
copy a.txt c.txt > nul
if exist c.txt echo W:copied
ren c.txt d.txt
if exist d.txt echo W:renamed
move d.txt e.txt > nul
if exist e.txt echo W:moved
del e.txt
if not exist e.txt echo W:deleted
mkdir sub\deep
echo x> sub\deep\f.txt
if exist sub\deep\f.txt echo W:nested
for %%f in (*.txt) do echo W:glob %%f
rmdir /s /q sub
if not exist sub echo W:tree-removed
find "two" < a.txt > nul 2>&1
echo W:pipe-done

rem ---- status
cmd /c exit 3
echo W:child %errorlevel%
ver > nul
echo W:ver %errorlevel%

cd /d "%~dp0"
if /i "%cd%\"=="%~dp0" echo W:back
rmdir /s /q "%work%"
if not exist "%work%" echo W:cleaned
echo W:done
