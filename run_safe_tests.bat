@echo off
set EXE=build\bin
%EXE%\test_01_connect.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_12_market_position.exe --accounts config\accounts.local.csv --instrument au2612 || exit /b 1
%EXE%\test_03_reconnect.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_05_duplicate_order.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_06_threshold.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_07_instruction_check.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_09_pause_trade.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_11_logging.exe --accounts config\accounts.local.csv || exit /b 1
