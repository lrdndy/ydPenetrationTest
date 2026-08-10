@echo off
REM WARNING: the following tests send real order/cancel requests to the configured YD environment.
set EXE=build\bin
%EXE%\test_02_basic_trade.exe --accounts config\accounts.local.csv --live || exit /b 1
%EXE%\test_10_batch_cancel.exe --accounts config\accounts.local.csv --live || exit /b 1
REM Error-message cases depend on account/market conditions and should be run deliberately:
REM %EXE%\test_08_error_message.exe --accounts config\accounts.local.csv --live --case no-position
REM %EXE%\test_08_error_message.exe --accounts config\accounts.local.csv --live --case insufficient-funds
REM %EXE%\test_08_error_message.exe --accounts config\accounts.local.csv --live --case market-state
