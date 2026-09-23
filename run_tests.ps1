[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('tcp', 'friend', 'message', 'sqlite', 'capacity')]
    [string[]]$Tests
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$selectedTests = @()
$exitCode = 0

$testOptions = @(
    [pscustomobject]@{
        Key = 'tcp'
        Label = 'TCP 半包、粘包与帧解析'
        CTestRegex = 'TcpStreamParserTest'
        Capacity = $false
    }
    [pscustomobject]@{
        Key = 'friend'
        Label = '好友申请事务与回滚'
        CTestRegex = 'FriendTransactionIntegration'
        Capacity = $false
    }
    [pscustomobject]@{
        Key = 'message'
        Label = '消息重试与离线同步'
        CTestRegex = 'MessageRetryOfflineSyncTest'
        Capacity = $false
    }
    [pscustomobject]@{
        Key = 'sqlite'
        Label = '客户端 SQLite 缓存'
        CTestRegex = 'LocalChatStorageTest'
        Capacity = $false
    }
    [pscustomobject]@{
        Key = 'capacity'
        Label = '连接容量（需要 ChatServer 运行）'
        CTestRegex = ''
        Capacity = $true
    }
)

if ($PSBoundParameters.ContainsKey('Tests')) {
    $selectedTests = @($testOptions | Where-Object { $Tests -contains $_.Key })
} else {
    Write-Host '选择要运行的测试，输入 y 运行；直接回车或输入 n 跳过。'
    foreach ($testOption in $testOptions) {
        $answer = Read-Host "$($testOption.Label)? [y/N]"
        if ($answer -match '^(?i:y|yes)$') {
            $selectedTests += $testOption
        }
    }
}

if ($selectedTests.Count -eq 0) {
    Write-Host '没有选择测试，未执行构建或测试。'
    exit 0
}

Push-Location $repoRoot
try {
    $ctestManifest = Join-Path $repoRoot 'build\CTestTestfile.cmake'
    if (-not (Test-Path -LiteralPath $ctestManifest)) {
        throw "没有找到根目录 CTest 配置：$ctestManifest。请先执行 cmake --preset windows-vcpkg，然后执行 cmake --build --preset $($Configuration.ToLowerInvariant())。"
    }

    $regularTests = @($selectedTests | Where-Object { -not $_.Capacity })
    if ($regularTests.Count -gt 0) {
        $testRegex = ($regularTests | ForEach-Object { $_.CTestRegex }) -join '|'
        Write-Host ''
        Write-Host "运行所选 CTest 用例：$testRegex"
        & ctest --test-dir build -C $Configuration -R $testRegex --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            $exitCode = $LASTEXITCODE
            Write-Host "CTest 运行失败（退出码 $exitCode）。请确认根目录默认构建已成功完成。" -ForegroundColor Red
        }
    }

    if ($selectedTests | Where-Object { $_.Capacity }) {
        Write-Host ''
        Write-Host '运行连接容量测试...'
        & ctest --test-dir build -C $Configuration -L capacity --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            $exitCode = $LASTEXITCODE
        }
    }
} catch {
    Write-Host "发生错误：$_" -ForegroundColor Red
    if ($exitCode -eq 0) {
        $exitCode = 1
    }
} finally {
    Pop-Location
}

exit $exitCode
