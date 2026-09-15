# SPDX-License-Identifier: MIT
#
# Сборка Fwd81.
#
# Скрипт сам находит компилятор и сам поднимает его окружение — отдельная
# «командная строка разработчика» не нужна. Работает и в Windows PowerShell 5.1,
# и в PowerShell 7 (в CI), поэтому здесь нет операторов && и ??.
#
# Примеры:
#   .\build.ps1                    собрать Debug и Release, прогнать проверки
#   .\build.ps1 -Config Release    только Release
#   .\build.ps1 -Clean             снести каталог build и собрать заново
#   .\build.ps1 -NoChecks          собрать без проверок (быстрее)

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Config = 'Both',

    [switch]$Clean,
    [switch]$NoChecks
)

$ErrorActionPreference = 'Stop'

# Вывод в UTF-8, иначе русский текст от cmake превратится в кракозябры.
try {
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false
} catch {
    # Не смертельно: собраться это не мешает.
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$buildDir = Join-Path $root 'build'

function Write-Step([string]$text) {
    Write-Host ''
    Write-Host "==> $text" -ForegroundColor Cyan
}

function Stop-WithError([string]$text) {
    Write-Host ''
    Write-Host "ОШИБКА: $text" -ForegroundColor Red
    exit 1
}

# --- 1. Компилятор ------------------------------------------------------------

Write-Step 'Ищу Visual Studio'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Stop-WithError @"
не нашёлся vswhere.exe по пути
    $vswhere
Значит, Visual Studio на этой машине не установлена.
Нужна Visual Studio 2022 с рабочей нагрузкой "Разработка классических приложений на C++".
"@
}

# -products * обязателен: без него не видно Build Tools, у них другой идентификатор.
$installations = & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$installations = @($installations | Where-Object { $_ -and (Test-Path $_) })

if ($installations.Count -eq 0) {
    Stop-WithError @"
Visual Studio нашлась, но без компилятора C++ для x64.
Открой "Visual Studio Installer" -> "Изменить" и поставь галочку
"Разработка классических приложений на C++".
"@
}

$vsPath = $installations | Where-Object { $_ -like '*2022*Professional*' } | Select-Object -First 1
if (-not $vsPath) {
    $vsPath = $installations | Where-Object { $_ -like '*2022*' } | Select-Object -First 1
}
if (-not $vsPath) {
    $vsPath = $installations[0]
}

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    Stop-WithError "в установке $vsPath нет файла vcvars64.bat — похоже, не установлены средства C++ для x64."
}

Write-Host "    $vsPath"

# --- 2. Окружение компилятора -------------------------------------------------

Write-Step 'Поднимаю окружение компилятора (vcvars64)'

# vcvars64.bat внутри себя зовёт vswhere.exe без указания пути. На этой машине
# включён NoDefaultCurrentDirectoryInExePath, поэтому cmd его не находит и пишет
# "is not recognized". Кладём каталог установщика в PATH — окружение поднимется молча.
$env:PATH = (Split-Path -Parent $vswhere) + ';' + $env:PATH

$environmentLines = & cmd.exe /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) {
    Stop-WithError 'vcvars64.bat отработал с ошибкой. Запусти его руками и посмотри, что он пишет.'
}

foreach ($line in $environmentLines) {
    if ($line -match '^([A-Za-z_][^=]*)=(.*)$') {
        Set-Item -Path ('Env:' + $matches[1]) -Value $matches[2]
    }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Stop-WithError 'после vcvars64 компилятор cl.exe всё равно не виден в PATH.'
}

# --- 3. cmake и ninja ---------------------------------------------------------

Write-Step 'Ищу cmake и ninja'

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) {
    Stop-WithError 'cmake.exe не найден. Поставь CMake и добавь его в PATH.'
}
$cmakePath = $cmake.Source

$ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
if ($ninja) {
    $ninjaPath = $ninja.Source
} else {
    # Visual Studio привозит свой ninja вместе с компонентом CMake.
    $bundled = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    if (Test-Path $bundled) {
        $ninjaPath = $bundled
    } else {
        Stop-WithError 'ninja.exe не найден ни в PATH, ни в составе Visual Studio.'
    }
}

Write-Host "    cmake: $cmakePath"
Write-Host "    ninja: $ninjaPath"

# --- 4. Сборка ----------------------------------------------------------------

if ($Clean -and (Test-Path $buildDir)) {
    Write-Step 'Убираю прошлую сборку'
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

Write-Step 'Настройка сборки'

& $cmakePath -S $root -B $buildDir -G 'Ninja Multi-Config' `
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath" `
    '-DCMAKE_C_COMPILER=cl'
if ($LASTEXITCODE -ne 0) {
    Stop-WithError 'cmake не смог настроить сборку. Текст ошибки выше.'
}

if ($Config -eq 'Both') {
    $configurations = @('Debug', 'Release')
} else {
    $configurations = @($Config)
}

foreach ($configuration in $configurations) {
    Write-Step "Сборка: $configuration"
    & $cmakePath --build $buildDir --config $configuration
    if ($LASTEXITCODE -ne 0) {
        Stop-WithError "сборка конфигурации $configuration не прошла. Текст ошибки выше."
    }
}

# --- 5. Проверки --------------------------------------------------------------

if (-not $NoChecks) {
    $python = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($python) {
        $pythonCommand = @($python.Source, '-3')
    } else {
        $python = Get-Command python.exe -ErrorAction SilentlyContinue
        if ($python) {
            $pythonCommand = @($python.Source)
        } else {
            $pythonCommand = $null
        }
    }

    if (-not $pythonCommand) {
        Write-Host ''
        Write-Host 'ВНИМАНИЕ: Python не найден, проверки пропущены.' -ForegroundColor Yellow
    } else {
        $interpreter = $pythonCommand[0]
        $pythonArguments = @()
        if ($pythonCommand.Count -gt 1) {
            $pythonArguments = $pythonCommand[1..($pythonCommand.Count - 1)]
        }

        Write-Step 'Проверка лицензионной границы'
        & $interpreter @pythonArguments (Join-Path $root 'tools\check_licenses.py')
        if ($LASTEXITCODE -ne 0) {
            Stop-WithError 'лицензионная граница нарушена, список выше. Правила — в docs/licensing.md.'
        }

        foreach ($configuration in $configurations) {
            $binDirectory = Join-Path $buildDir "bin\$configuration"

            Write-Step "Проверка fwd81core.dll ($configuration): зависимости и требуемая версия Windows"
            & $interpreter @pythonArguments (Join-Path $root 'tools\check_image.py') `
                (Join-Path $binDirectory 'fwd81core.dll') --imports ntdll.dll
            if ($LASTEXITCODE -ne 0) {
                Stop-WithError 'ядро не прошло проверку, подробности выше.'
            }

            Write-Step "Проверка fwd81cli.exe ($configuration): требуемая версия Windows"
            & $interpreter @pythonArguments (Join-Path $root 'tools\check_image.py') `
                (Join-Path $binDirectory 'fwd81cli.exe')
            if ($LASTEXITCODE -ne 0) {
                Stop-WithError 'утилита не прошла проверку, подробности выше.'
            }
        }
    }
}

# --- 6. Итог ------------------------------------------------------------------

Write-Host ''
Write-Host 'Готово.' -ForegroundColor Green
foreach ($configuration in $configurations) {
    Write-Host "    build\bin\$configuration\"
}
