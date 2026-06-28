@echo off
set /p commit_id="Enter the commit ID to reset to: "

if "%commit_id%"=="" (
    echo Error: No commit ID entered. Exiting.
    timeout /t 3 >nul
    exit /b
)

echo.
echo Resetting repository to %commit_id%...
git reset --hard %commit_id%

echo.
echo Overwriting remote repository...
git push origin --force

echo.
echo Success! Closing in 3 seconds...
timeout /t 3 >nul
