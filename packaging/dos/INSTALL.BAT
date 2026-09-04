@ECHO OFF
REM Install the v9x DOS remote agent to C:\V9XREMOT
IF NOT EXIST C:\V9XREMOT\NUL MD C:\V9XREMOT
IF NOT EXIST C:\V9XREMOT\TEMP\NUL MD C:\V9XREMOT\TEMP
COPY /Y V9XAGNT.EXE C:\V9XREMOT\
COPY /Y DOS4GW.EXE C:\V9XREMOT\
IF NOT EXIST C:\V9XREMOT\AGENT.INI COPY AGENT.INI C:\V9XREMOT\
IF NOT EXIST C:\V9XREMOT\WATTCP.CFG COPY WATTCP.CFG C:\V9XREMOT\
ECHO.
ECHO Installed to C:\V9XREMOT
ECHO.
ECHO 1. Edit C:\V9XREMOT\WATTCP.CFG for this machine's IP settings.
ECHO 2. Make sure a packet driver for your NIC loads in AUTOEXEC.BAT.
ECHO 3. Add these lines to AUTOEXEC.BAT, after the packet driver:
ECHO.
ECHO      SET WATTCP.CFG=C:\V9XREMOT
ECHO      C:\V9XREMOT\V9XAGNT.EXE
ECHO.
ECHO The agent runs in the foreground and does not return, so it must be
ECHO the last line of AUTOEXEC.BAT. The box is then a dedicated agent.
ECHO.
