@echo off
setlocal
where cmake >nul 2>nul || (echo ERROR: cmake not found & exit /b 1)
where cl >nul 2>nul || (
  echo ERROR: cl.exe not found.
  echo Open "x64 Native Tools Command Prompt for VS 2022" and run this script again.
  exit /b 1
)
cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
echo.
echo BUILD OK. Executables are under build\bin\
endlocal
