@echo off
rem Build VoiceTypingSwitcher.exe and publish to GitHub Releases.
rem Usage: release.bat [version] [release_notes]
setlocal EnableExtensions
cd /d "%~dp0"
chcp 65001 >nul

set "SELF_BAT=%~f0"
set "EXE_PATH=bin\VoiceTypingSwitcher.exe"
set "WAS_RUNNING=0"

if "%~1"=="-h" goto :usage
if "%~1"=="--help" goto :usage
if "%~1"=="/?" goto :usage

set "CUR_VER="
for /f "usebackq tokens=1,2,3" %%A in ("src\version.h") do if "%%A"=="#define" if "%%B"=="APP_VER_STR" set "CUR_VER=%%~C"
if not defined CUR_VER (
    echo [LỖI] Không đọc được APP_VER_STR trong src\version.h.
    goto :fail
)

echo ====================================================
echo   VoiceTypingSwitcher - Build ^& GitHub Release
echo ====================================================
echo Phiên bản hiện tại (src\version.h): %CUR_VER%

set "RAW_VER=%~1"
if defined RAW_VER goto :parse_ver
set /p "RAW_VER=Nhập số phiên bản mới (vd: 1.2.0, Enter để giữ %CUR_VER%): "

:parse_ver
set "TRIMMED="
set "SP_EXTRA="
if defined RAW_VER for /f "tokens=1,2" %%A in ("%RAW_VER%") do (
    set "TRIMMED=%%A"
    set "SP_EXTRA=%%B"
)
if defined SP_EXTRA goto :bad_ver
set "RAW_VER=%TRIMMED%"
if not defined RAW_VER set "RAW_VER=%CUR_VER%"
if /i "%RAW_VER:~0,1%"=="v" set "RAW_VER=%RAW_VER:~1%"

set "MAJ="
set "MIN="
set "PAT="
set "EXTRA="
for /f "tokens=1-4 delims=." %%a in ("%RAW_VER%") do (
    set "MAJ=%%a"
    set "MIN=%%b"
    set "PAT=%%c"
    set "EXTRA=%%d"
)

if defined EXTRA goto :bad_ver
if not "%MAJ%.%MIN%.%PAT%"=="%RAW_VER%" goto :bad_ver
echo(%MAJ%| findstr /r "^[0-9][0-9]*$" >nul || goto :bad_ver
echo(%MIN%| findstr /r "^[0-9][0-9]*$" >nul || goto :bad_ver
echo(%PAT%| findstr /r "^[0-9][0-9]*$" >nul || goto :bad_ver
echo(%MAJ%| findstr /r "^0[0-9]" >nul && goto :bad_ver
echo(%MIN%| findstr /r "^0[0-9]" >nul && goto :bad_ver
echo(%PAT%| findstr /r "^0[0-9]" >nul && goto :bad_ver

set "VER=%MAJ%.%MIN%.%PAT%"
set "TAG=v%VER%"

set "NOTES=%~2"
if not defined NOTES (
    set /p "NOTES=Ghi chú phát hành (Enter để GitHub tự tạo từ commit): "
)

echo.
echo [1/5] Cập nhật phiên bản %VER% vào src\version.h...
if "%VER%"=="%CUR_VER%" (
    echo       Giữ nguyên phiên bản %VER%.
) else (
    call :run_ps update-version || goto :fail
    echo       Đã cập nhật src\version.h: %CUR_VER% -^> %VER%
)

echo.
echo [2/5] Build %EXE_PATH%...
tasklist /FI "IMAGENAME eq VoiceTypingSwitcher.exe" /NH 2>nul | find /I "VoiceTypingSwitcher.exe" >nul
if not errorlevel 1 (
    set "WAS_RUNNING=1"
    echo       Đang tạm dừng VoiceTypingSwitcher.exe để ghi đè file build...
    taskkill /IM VoiceTypingSwitcher.exe /F >nul 2>&1
    ping -n 2 127.0.0.1 >nul
)
call build.cmd <nul || goto :fail
if not exist "%EXE_PATH%" (
    echo [LỖI] Không tìm thấy %EXE_PATH% sau khi build.
    goto :fail
)

echo.
echo [3/5] Chạy kiểm thử (test.cmd)...
call test.cmd <nul || goto :fail

echo.
echo [4/5] Đẩy code và tag %TAG% lên GitHub...
where git >nul 2>&1 || (
    echo [LỖI] Không tìm thấy lệnh git trong PATH.
    goto :fail
)

set "HAS_CHANGES="
for /f "delims=" %%L in ('git status --porcelain') do set "HAS_CHANGES=1"
if not defined HAS_CHANGES goto :push_branch

echo       Các thay đổi chưa commit:
git status --short
set "DO_COMMIT="
set /p "DO_COMMIT=      Commit tất cả với thông điệp 'Release %TAG%'? [Y/n]: "
if /i "%DO_COMMIT%"=="n" (
    echo [HỦY] Đã hủy phát hành theo yêu cầu.
    goto :fail
)
git add -A || goto :fail
git commit -m "Release %TAG%" || goto :fail

:push_branch
git push origin HEAD || goto :fail

set "TAG_EXISTS=0"
git rev-parse -q --verify "refs/tags/%TAG%" >nul 2>&1 && set "TAG_EXISTS=1"
git ls-remote --exit-code --tags origin "refs/tags/%TAG%" >nul 2>&1 && set "TAG_EXISTS=1"
if "%TAG_EXISTS%"=="0" goto :create_tag

set "OVERWRITE_TAG="
set /p "OVERWRITE_TAG=      Tag %TAG% đã tồn tại. Ghi đè tag và cập nhật lại Release? [y/N]: "
if /i not "%OVERWRITE_TAG%"=="y" (
    echo [HỦY] Tag %TAG% đã tồn tại. Hãy chạy lại với số phiên bản khác.
    goto :fail
)
git tag -f -a "%TAG%" -m "Release %TAG%" || goto :fail
git push -f origin "refs/tags/%TAG%" || goto :fail
goto :publish

:create_tag
git tag -a "%TAG%" -m "Release %TAG%" || goto :fail
git push origin "refs/tags/%TAG%" || goto :fail

:publish
echo.
echo [5/5] Tạo GitHub Release %TAG% và tải lên %EXE_PATH%...
call :run_ps publish || goto :fail

if "%WAS_RUNNING%"=="1" (
    echo.
    echo Khởi chạy lại %EXE_PATH%...
    start "" "%EXE_PATH%"
)

echo.
echo ====================================================
echo   Phát hành %TAG% thành công!
echo ====================================================
call :maybe_pause
exit /b 0

:bad_ver
echo [LỖI] Số phiên bản không hợp lệ: "%RAW_VER%"
echo       Định dạng đúng là MAJOR.MINOR.PATCH (vd: 1.2.0 hoặc v1.2.0).
goto :fail

:usage
echo Cách dùng:
echo   release.bat                         ^(nhập phiên bản và ghi chú tương tác^)
echo   release.bat 1.2.0                   ^(chỉ định phiên bản, hỏi ghi chú^)
echo   release.bat 1.2.0 "Nội dung..."     ^(chỉ định cả phiên bản và ghi chú^)
exit /b 0

:run_ps
powershell -NoProfile -ExecutionPolicy Bypass -Command "& ([ScriptBlock]::Create(([IO.File]::ReadAllText($env:SELF_BAT, [Text.Encoding]::UTF8) -split '(?m)^:PS_SCRIPT_START\r?\n', 2)[1])) '%~1'"
exit /b %errorlevel%

:maybe_pause
echo(%cmdcmdline%| findstr /i /c:"\" \"%~f0\" \"" >nul 2>&1 && pause
exit /b 0

:fail
if "%WAS_RUNNING%"=="1" if exist "%EXE_PATH%" start "" "%EXE_PATH%"
echo.
echo Phát hành thất bại.
call :maybe_pause
exit /b 1

:PS_SCRIPT_START
param([string]$Mode)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if ($Mode -eq 'update-version') {
    $path = Join-Path $PWD.Path 'src\version.h'
    $content = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
    $updated = $content
    $updated = [regex]::Replace($updated, '(?m)^(#define\s+APP_VER_MAJOR\s+)\d+', "`${1}$($env:MAJ)")
    $updated = [regex]::Replace($updated, '(?m)^(#define\s+APP_VER_MINOR\s+)\d+', "`${1}$($env:MIN)")
    $updated = [regex]::Replace($updated, '(?m)^(#define\s+APP_VER_PATCH\s+)\d+', "`${1}$($env:PAT)")
    $updated = [regex]::Replace($updated, '(?m)^(#define\s+APP_VER_STR\s+)"[^"]*"', "`${1}`"$($env:VER)`"")
    $updated = [regex]::Replace($updated, '(?m)^(#define\s+APP_VER_WSTR\s+)L"[^"]*"', "`${1}L`"$($env:VER)`"")
    [IO.File]::WriteAllText($path, $updated, (New-Object Text.UTF8Encoding $false))
    exit 0
}

if ($Mode -ne 'publish') {
    throw "Unknown mode: $Mode"
}

$tag = $env:TAG
$notes = $env:NOTES
$exePath = (Resolve-Path $env:EXE_PATH).Path
$assetName = [IO.Path]::GetFileName($exePath)
$title = "VoiceTypingSwitcher $tag"

function Get-LocalPropsToken {
    $propFile = Join-Path $PWD.Path 'local.properties'
    if (-not (Test-Path $propFile)) { return $null }
    foreach ($line in [IO.File]::ReadAllLines($propFile, [Text.Encoding]::UTF8)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#') -or $t.StartsWith(';')) { continue }
        if ($t -match '^(?:github\.token|GITHUB_TOKEN|GH_TOKEN|git\.token|token)\s*=\s*(.+)$') {
            $val = $Matches[1].Trim().Trim('"').Trim("'")
            if (-not [string]::IsNullOrWhiteSpace($val)) { return $val }
        }
    }
    return $null
}

$token = Get-LocalPropsToken
$tokenSource = 'local.properties'
if ([string]::IsNullOrWhiteSpace($token)) {
    $token = $env:GITHUB_TOKEN
    $tokenSource = 'GITHUB_TOKEN'
}
if ([string]::IsNullOrWhiteSpace($token)) {
    $token = $env:GH_TOKEN
    $tokenSource = 'GH_TOKEN'
}

# 1) If no token configured in local.properties/env, try GitHub CLI (gh) if installed and authenticated
if ([string]::IsNullOrWhiteSpace($token)) {
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $gh -and (Test-Path "$env:ProgramFiles\GitHub CLI\gh.exe")) {
        $gh = Get-Command "$env:ProgramFiles\GitHub CLI\gh.exe"
    }
    if ($gh) {
        & $gh.Source auth status *> $null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "      Sử dụng GitHub CLI (gh)..."
            & $gh.Source release view $tag *> $null
            if ($LASTEXITCODE -eq 0) {
                & $gh.Source release upload $tag $exePath --clobber
                if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
                if (-not [string]::IsNullOrWhiteSpace($notes)) {
                    & $gh.Source release edit $tag --title $title --notes $notes
                    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
                }
            } else {
                $ghArgs = @('release', 'create', $tag, $exePath, '--title', $title)
                if ([string]::IsNullOrWhiteSpace($notes)) {
                    $ghArgs += '--generate-notes'
                } else {
                    $ghArgs += @('--notes', $notes)
                }
                & $gh.Source @ghArgs
                if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            }
            $url = (& $gh.Source release view $tag --json url --jq .url 2>$null)
            if ($url) { Write-Host "      Release URL : $url" }
            exit 0
        }
    }
}

# 2) GitHub REST API using local.properties / env / Git Credential Manager / PAT prompt
$remote = (git remote get-url origin).Trim()
if ($remote -notmatch 'github\.com[:/]([^/]+)/([^/]+?)(?:\.git)?$') {
    throw "Không nhận diện được GitHub repository từ remote origin: $remote"
}
$owner = $Matches[1]
$repo = $Matches[2]

function Get-GcmToken {
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = 'git'
        $psi.Arguments = 'credential fill'
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.UseShellExecute = $false
        $p = [System.Diagnostics.Process]::Start($psi)
        $p.StandardInput.Write("protocol=https`nhost=github.com`n`n")
        $p.StandardInput.Close()
        $out = $p.StandardOutput.ReadToEnd()
        $p.WaitForExit(15000) | Out-Null
        if ($out -match '(?m)^password=(.+)$') { return $Matches[1].Trim() }
    } catch {}
    return $null
}

function Prompt-Token {
    Write-Host "      Cần GitHub Personal Access Token (quyền repo / Contents: Read & Write)."
    Write-Host "      Mẹo: dán vào local.properties (github.token=...) để tự dùng cho các lần sau."
    $sec = Read-Host "      Nhập GitHub Token" -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr).Trim() }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

if ([string]::IsNullOrWhiteSpace($token)) {
    $token = Get-GcmToken
    $tokenSource = 'Git Credential Manager'
}
if ([string]::IsNullOrWhiteSpace($token)) {
    $token = Prompt-Token
    $tokenSource = 'nhập thủ công'
}
if ([string]::IsNullOrWhiteSpace($token)) {
    throw 'Không có GitHub token để tạo Release.'
}

function New-Headers([string]$t) {
    return @{
        Authorization          = "Bearer $t"
        Accept                 = 'application/vnd.github+json'
        'X-GitHub-Api-Version' = '2022-11-28'
        'User-Agent'           = 'VoiceTypingSwitcher-Release'
    }
}

function Get-HttpStatus($err) {
    try {
        if ($err.Exception.Response -and $err.Exception.Response.StatusCode) {
            return [int]$err.Exception.Response.StatusCode
        }
    } catch {}
    return 0
}

function Publish-ViaApi([string]$t) {
    $headers = New-Headers $t
    $apiBase = "https://api.github.com/repos/$owner/$repo"

    $rel = $null
    try {
        $rel = Invoke-RestMethod -Method Get -Uri "$apiBase/releases/tags/$tag" -Headers $headers
    } catch {
        if ((Get-HttpStatus $_) -ne 404) { throw }
    }

    if ($null -eq $rel) {
        $payload = @{
            tag_name   = $tag
            name       = $title
            draft      = $false
            prerelease = $false
        }
        if ([string]::IsNullOrWhiteSpace($notes)) {
            $payload.generate_release_notes = $true
        } else {
            $payload.body = $notes
        }
        $bodyBytes = [Text.Encoding]::UTF8.GetBytes(($payload | ConvertTo-Json -Depth 4))
        $rel = Invoke-RestMethod -Method Post -Uri "$apiBase/releases" `
            -Headers $headers -ContentType 'application/json; charset=utf-8' -Body $bodyBytes
    } else {
        if (-not [string]::IsNullOrWhiteSpace($notes)) {
            $bodyBytes = [Text.Encoding]::UTF8.GetBytes((@{ name = $title; body = $notes } | ConvertTo-Json -Depth 4))
            $rel = Invoke-RestMethod -Method Patch -Uri "$apiBase/releases/$($rel.id)" `
                -Headers $headers -ContentType 'application/json; charset=utf-8' -Body $bodyBytes
        }
        foreach ($a in @($rel.assets)) {
            if ($a -and $a.name -eq $assetName) {
                Invoke-RestMethod -Method Delete -Uri "$apiBase/releases/assets/$($a.id)" -Headers $headers | Out-Null
            }
        }
    }

    $uploadBase = ($rel.upload_url -replace '\{.*$', '')
    $uploadUri = "${uploadBase}?name=$([Uri]::EscapeDataString($assetName))"
    $asset = Invoke-RestMethod -Method Post -Uri $uploadUri -Headers $headers `
        -ContentType 'application/octet-stream' -InFile $exePath

    Write-Host "      Release URL : $($rel.html_url)"
    Write-Host "      Download URL: $($asset.browser_download_url)"
}

Write-Host "      Sử dụng GitHub API ($owner/$repo, xác thực qua $tokenSource)..."
try {
    Publish-ViaApi $token
} catch {
    $status = Get-HttpStatus $_
    if (($status -eq 401 -or $status -eq 403) -and $tokenSource -ne 'nhập thủ công') {
        Write-Warning "Token từ $tokenSource không được chấp nhận (HTTP $status)."
        $token = Prompt-Token
        if ([string]::IsNullOrWhiteSpace($token)) { throw 'Đã hủy do không nhập token.' }
        Publish-ViaApi $token
    } else {
        throw
    }
}
