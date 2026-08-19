@echo off
set EXE=build\bin
%EXE%\test_01_connect.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_12_market_position.exe --accounts config\accounts.local.csv --instrument au2612 || exit /b 1
%EXE%\test_03_reconnect.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_05_duplicate_order.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_06_threshold.exe --accounts config\accounts.local.csv || exit /b 1
%EXE%\test_07_1_invalid_instrument.exe --accounts config\accounts.local.csv --instrument au2617 || exit /b 1
%EXE%\test_07_2_invalid_price.exe --accounts config\accounts.local.csv --instrument au2612 || exit /b 1
%EXE%\test_07_3_invalid_volume.exe --accounts config\accounts.local.csv --instrument au2612 || exit /b 1
%EXE%\test_09_pause_trade.exe --accounts config\accounts.local.csv || exit /b 1
rem Run test_11_logging separately after real order/cancel and cabinet-error logs exist.
