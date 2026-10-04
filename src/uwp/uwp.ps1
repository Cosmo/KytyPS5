<#
.SYNOPSIS
Builds, installs and runs the KytyPS5 UWP app on this PC.

.DESCRIPTION
One verb per step; each stops with the reason when it fails:
  configure  CMake configure of the UWP build directory (Ninja, clang-cl)
  build      builds kyty_uwp, updates the package layout and checks its imports for the Xbox
  deploy     installs missing framework packages and registers the layout (Developer Mode)
  package    a signed .msix of the layout and its framework packages, for the Xbox
  xbox       sets up the Xbox's Device Portal for logs: -Address <IP>, then its sign-in
             (kept in -XboxDir or $env:KYTY_XBOX_DIR, else build/uwp/xbox)
  install    installs the newest package on the Xbox (Device Portal)
  launch     starts the app on the Xbox afresh: the launcher, or a game with -Title or -Game
  screenshot saves a screenshot of the Xbox in the Xbox folder's screenshots
  logs       downloads the app's logs from the Xbox and prints their ends
  upload     copies -Files into the app's LocalState on the Xbox, or into its folder -To
             (created if missing), e.g. -To _Patches for game patches
  run        starts the app (a game with -Title or -Game through <protocol>://run), keeps its window
             in front (Windows slows down hidden UWP apps), reports the frame rate, prints the logs
  all        build, deploy, run (the default)
  folders    lists the game folders; -Add or -Remove changes them
  games      lists the games of the last scan (title IDs for run -Title)
  stacks     prints the call stacks of the running app's threads (cdb)
  debug      attaches cdb to the running app in a new window

.EXAMPLE
src\uwp\uwp.ps1 configure
src\uwp\uwp.ps1 folders -Add <games folder>
src\uwp\uwp.ps1 run -Title PPSA00000 -Seconds 60
src\uwp\uwp.ps1 upload -Files <patches folder>\PPSA00000.json -To _Patches
#>
param(
    [ValidateSet('configure', 'build', 'deploy', 'package', 'xbox', 'install', 'launch', 'screenshot', 'logs', 'upload', 'run', 'all', 'folders', 'games', 'stacks', 'debug')]
    [string]$Verb = 'all',
    [string]$BuildDir = '',
    [string]$Game = '',
    [string]$Title = '',
    [string]$Add = '',
    [string]$Remove = '',
    [string]$Address = '',
    [string]$XboxDir = '',
    [string[]]$Files = @(),
    [string]$To = '',
    [int]$Seconds = 15,
    [switch]$Keep,
    # Dependencies (see README.md): the extracted NuGet packages. The defaults are under $env:KYTY_DEPS, else under build\deps.
    [string]$WinUIRoot = '',
    [string]$WebView2Root = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$deps = if ($env:KYTY_DEPS) { $env:KYTY_DEPS } else { "$root\build\deps" }
if (-not $BuildDir) { $BuildDir = if ($env:KYTY_UWP_BUILD) { $env:KYTY_UWP_BUILD } else { "$root\build\uwp" } }
if (-not $WinUIRoot) { $WinUIRoot = "$deps\nuget\microsoft.ui.xaml.2.8.7" }
if (-not $WebView2Root) { $WebView2Root = "$deps\nuget\microsoft.web.webview2.1.0.2849.39" }
$layout = "$BuildDir\uwp-layout"
# The package identity and URI scheme come from the manifest of the layout (see KYTY_UWP_PACKAGE_NAME and KYTY_UWP_PROTOCOL in CMakeLists.txt).
$packageName = 'KytyPS5'
$protocol = 'kyty'
$displayName = 'KytyPS5'
if (Test-Path "$layout\AppxManifest.xml") {
    $manifestXml = [xml](Get-Content "$layout\AppxManifest.xml" -Raw)
    $packageName = $manifestXml.Package.Identity.Name
    $displayName = $manifestXml.Package.Properties.DisplayName
    $protocolNode = $manifestXml.SelectNodes("//*[local-name()='Protocol']") | Select-Object -First 1
    if ($protocolNode) { $protocol = $protocolNode.Name }
}

function Step($text) { Write-Host "== $text" -ForegroundColor Cyan }

# The last lines of a log, read from its last bytes: a game's output can make a log hundreds of MB, and Get-Content -Tail reads all of it.
function Get-LogTail([string]$Path, [int]$Lines) {
    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $count = [Math]::Min([long]131072, $stream.Length)
        $null = $stream.Seek(-$count, 'End')
        $buffer = New-Object byte[] $count
        $null = $stream.Read($buffer, 0, $count)
    } finally { $stream.Dispose() }
    [Text.Encoding]::UTF8.GetString($buffer) -split "`r?`n" | Select-Object -Last $Lines
}
function Fail($text) { Write-Host "FAILED: $text" -ForegroundColor Red; exit 1 }

function Enter-DevShell {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
    if (-not $vs) { Fail 'Visual Studio not found' }
    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    $env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$env:PATH"
}

function Invoke-Configure {
    Step "configure $BuildDir"
    foreach ($path in @($WinUIRoot, $WebView2Root)) {
        if (-not (Test-Path $path)) { Fail "missing dependency: $path (see README.md)" }
    }
    if (-not $env:KYTY_WINPTHREAD_DLL -or -not (Test-Path $env:KYTY_WINPTHREAD_DLL)) {
        Fail '$env:KYTY_WINPTHREAD_DLL must name the UCRT build of libwinpthread-1.dll (see README.md)'
    }
    if (-not $env:KYTY_SPIRV_TO_DXIL_ROOT -or -not (Test-Path (Join-Path $env:KYTY_SPIRV_TO_DXIL_ROOT 'include\spirv_to_dxil.h'))) {
        Fail '$env:KYTY_SPIRV_TO_DXIL_ROOT must name a spirv_to_dxil build (include, lib, bin; see README.md)'
    }
    Enter-DevShell
    # Sources of the libraries upstream fetches, when they are kept in $env:KYTY_DEPS\<name>-src instead of being downloaded.
    $fetched = foreach ($name in 'opus', 'xbyak', 'zydis', 'zstd') {
        if (Test-Path "$deps\$name-src") { "-DFETCHCONTENT_SOURCE_DIR_$($name.ToUpper())=$deps\$name-src" }
    }
    cmake -S $root -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl `
        -DCMAKE_CXX_COMPILER=clang-cl -DKYTY_BUILD_UWP=ON "-DKYTY_WINUI_ROOT=$WinUIRoot" "-DKYTY_WEBVIEW2_ROOT=$WebView2Root" `
        "-DKYTY_WINPTHREAD_DLL=$env:KYTY_WINPTHREAD_DLL" $fetched `
        "-DKYTY_SPIRV_TO_DXIL_ROOT=$env:KYTY_SPIRV_TO_DXIL_ROOT" `
        $(if ($env:KYTY_UWP_PACKAGE_NAME) { "-DKYTY_UWP_PACKAGE_NAME=$env:KYTY_UWP_PACKAGE_NAME" }) `
        $(if ($env:KYTY_UWP_DISPLAY_NAME) { "-DKYTY_UWP_DISPLAY_NAME=$env:KYTY_UWP_DISPLAY_NAME" }) `
        $(if ($env:KYTY_UWP_PROTOCOL) { "-DKYTY_UWP_PROTOCOL=$env:KYTY_UWP_PROTOCOL" }) `
        $(if ($env:KYTY_ZARCHIVE_SOURCE) { "-DFETCHCONTENT_SOURCE_DIR_ZARCHIVE_SOURCE=$env:KYTY_ZARCHIVE_SOURCE" }) `
        $(if ($env:KYTY_FFMPEG_DIR) { "-DFFMPEG_PREBUILT_DIR=$env:KYTY_FFMPEG_DIR" }) `
        -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
    if ($LASTEXITCODE -ne 0) { Fail 'configure' }
}

function Invoke-Build {
    Step 'build kyty_uwp'
    if (-not (Test-Path "$BuildDir\CMakeCache.txt")) { Invoke-Configure } else { Enter-DevShell }
    cmake --build $BuildDir --target kyty_uwp_layout
    if ($LASTEXITCODE -ne 0) { Fail 'build' }
    $null = Test-Imports
}

function Test-Imports([string]$Folder = $layout) {
    # Every import of the folder's binaries (the package layout) has to be an API UWP apps have: WindowsApp.lib, the
    # store C runtime or a DLL in the package. The Xbox has no desktop-only DLLs, and an import it
    # can't resolve stops the app from starting there (Windows still runs it). Imports by
    # ordinal are not checked. Returns whether every import is fine.
    $lib = "$env:WindowsSdkDir\Lib\$($env:WindowsSDKVersion.TrimEnd('\'))\um\x64\WindowsApp.lib"
    $available = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($line in (llvm-nm $lib 2>$null)) {
        if ($line -match '__imp_(\S+)$') { [void]$available.Add($Matches[1]) }
    }
    if ($available.Count -eq 0) { Write-Host "(imports not checked: no $lib)"; return $true }
    $packaged = (Get-ChildItem $Folder -Filter *.dll).Name
    $missing = foreach ($binary in Get-ChildItem "$Folder\*" -Include *.exe, *.dll) {
        $dll = ''
        foreach ($line in (llvm-readobj --coff-imports $binary.FullName)) {
            if ($line -match '^\s*Name: (\S+)') { $dll = $Matches[1]; continue }
            if ($line -notmatch '^\s*Symbol: ([^(\s]\S*)') { continue }
            $symbol = $Matches[1]
            if ($dll -match '^(api-ms-win-crt-|vcruntime140(_1)?_app|msvcp140.*_app)' -or
                $packaged -contains $dll -or $available.Contains($symbol)) { continue }
            "$($binary.Name): $dll $symbol"
        }
    }
    if ($missing) {
        Write-Host 'WARNING: imports UWP apps on the Xbox do not have (it would not start there):' `
            -ForegroundColor Yellow
        $missing | Group-Object { $_ -replace ' \S+$' } | ForEach-Object {
            Write-Host ("  {0} ({1}): {2}" -f $_.Name, $_.Count,
                (($_.Group | ForEach-Object { $_ -replace '^.* ' }) -join ' '))
        }
    }
    return -not $missing
}

function Install-Framework($name, $appx) {
    if (Get-AppxPackage -Name $name | Where-Object Architecture -eq 'X64') { return }
    if (-not (Test-Path $appx)) { Fail "framework package $name is not installed and $appx is missing" }
    Step "install framework package $name"
    Add-AppxPackage -Path $appx
}

function Invoke-Deploy {
    Step "register $layout"
    if (-not (Test-Path "$layout\AppxManifest.xml")) { Fail 'no package layout; run the build first' }
    if (-not (Get-AppxPackage -Name 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64')) {
        Fail 'Microsoft.VCLibs.140.00 (x64) is not installed'
    }
    Install-Framework 'Microsoft.UI.Xaml.2.8' "$WinUIRoot\tools\AppX\x64\Release\Microsoft.UI.Xaml.2.8.appx"
    Get-Process kyty_uwp -ErrorAction SilentlyContinue | Stop-Process -Force
    # A package of this identity registered from another folder (an older build directory) is replaced, keeping the app's data.
    $registered = Get-AppxPackage -Name $packageName
    if ($registered -and $registered.InstallLocation -ne $layout) {
        Step "registered from $($registered.InstallLocation): replacing the registration (app data kept)"
        $registered | Remove-AppxPackage -PreserveApplicationData
    }
    # Registering in place: the app runs from the layout, so a rebuild needs no new deploy
    # unless the manifest changed. A changed manifest with the same version is refused; the
    # old registration is then removed first, keeping the app's data (LocalState).
    try {
        Add-AppxPackage -Register "$layout\AppxManifest.xml" -ForceApplicationShutdown
    } catch {
        if ($_.Exception.Message -notmatch '0x80073CFB') { throw }
        Step 'manifest changed: replacing the registration (app data kept)'
        Get-AppxPackage -Name $packageName | Remove-AppxPackage -PreserveApplicationData
        Add-AppxPackage -Register "$layout\AppxManifest.xml" -ForceApplicationShutdown
    }
    $package = Get-AppxPackage -Name $packageName
    if (-not $package) { Fail 'registration did not install the package' }
    Write-Host "installed $($package.PackageFullName)"
}

function Invoke-Package {
    # An .msix of the layout for the Xbox, with the framework packages it depends on, signed with
    # the certificate CN=KytyPS5 of the current user (README.md). Each package gets a newer
    # version than the last, so it installs over it.
    if (-not (Test-Path "$layout\AppxManifest.xml")) { Fail 'no package layout; run the build first' }
    # The app is stamped with the git revision when it is built; a build from a tree with uncommitted changes is "dirty", and the emulator then keeps no
    # pipeline cache (the shaders are translated again at every start).
    $stamp = Select-String -Path "$layout\kyty_uwp.exe" -Pattern '-dirty' -SimpleMatch -Quiet
    if ($stamp) { Write-Host 'WARNING: this build is dirty (made from uncommitted changes): the pipeline cache is off. Commit and run build again.' -ForegroundColor Yellow }
    $certificate = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
        $_.Subject -eq 'CN=KytyPS5' -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date)
    } | Select-Object -First 1
    if (-not $certificate) { Fail 'no certificate CN=KytyPS5 to sign with (see README.md)' }
    $vclibs = "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs" +
        '\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx'
    $winui = "$WinUIRoot\tools\AppX\x64\Release\Microsoft.UI.Xaml.2.8.appx"
    foreach ($path in @($vclibs, $winui)) {
        if (-not (Test-Path $path)) { Fail "missing framework package: $path" }
    }
    Enter-DevShell
    $out = "$BuildDir\uwp-package"
    $staging = "$out\layout"
    Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $staging | Out-Null
    # The layout without what registering it on this PC added.
    Get-ChildItem $layout -Exclude 'microsoft.system.package.metadata' |
        Copy-Item -Destination $staging -Recurse
    $now = Get-Date
    $version = '1.0.{0}.{1}' -f ($now.Year % 100 * 1000 + $now.DayOfYear), [int]$now.ToString('HHmm')
    $manifest = "$staging\AppxManifest.xml"
    (Get-Content $manifest -Raw) -replace '(<Identity [^>]*Version=")[^"]+', "`${1}$version" |
        Set-Content $manifest -Encoding utf8 -NoNewline
    $msix = "$out\${packageName}_${version}_x64.msix"
    Step "package $msix"
    makeappx pack /d $staging /p $msix /o | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { Fail 'makeappx' }
    signtool sign /q /fd SHA256 /sha1 $certificate.Thumbprint $msix
    if ($LASTEXITCODE -ne 0) { Fail 'signtool' }
    Remove-Item $staging -Recurse -Force
    New-Item -ItemType Directory "$out\Dependencies" | Out-Null
    Copy-Item $vclibs, $winui "$out\Dependencies\"
    Get-ChildItem $out -Recurse -File | ForEach-Object {
        '{0,-60} {1,8:N1} MB' -f $_.FullName.Substring($out.Length + 1), ($_.Length / 1MB)
    }
}

# The Xbox's Device Portal (Dev Mode) for the xbox and logs verbs: its address, its sign-in
# encrypted for this Windows user (DPAPI) and the downloaded logs, in $XboxDir (by default
# $env:KYTY_XBOX_DIR, else the build directory's xbox folder).
if (-not $XboxDir) { $XboxDir = if ($env:KYTY_XBOX_DIR) { $env:KYTY_XBOX_DIR } else { "$BuildDir\xbox" } }
$xboxConfig = "$XboxDir\xbox.json"
$xboxCredential = "$XboxDir\xbox-credential.xml"

function Get-Xbox {
    if (-not (Test-Path $xboxConfig)) { Fail 'no Xbox set up: run uwp.ps1 xbox -Address <IP>' }
    $address = (Get-Content $xboxConfig -Raw | ConvertFrom-Json).address
    # The Device Portal's certificate is self-signed: accepted from this address only.
    if (-not ('KytyXboxCertificate' -as [type])) {
        Add-Type @'
public static class KytyXboxCertificate {
    public static string Host;
    public static bool Check(object sender, System.Security.Cryptography.X509Certificates.X509Certificate certificate,
                             System.Security.Cryptography.X509Certificates.X509Chain chain,
                             System.Net.Security.SslPolicyErrors errors) {
        var request = sender as System.Net.HttpWebRequest;
        return errors == System.Net.Security.SslPolicyErrors.None ||
               (request != null && request.RequestUri.Host == Host);
    }
}
'@
    }
    [KytyXboxCertificate]::Host = $address
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    [Net.ServicePointManager]::ServerCertificateValidationCallback = [Delegate]::CreateDelegate(
        [Net.Security.RemoteCertificateValidationCallback], [KytyXboxCertificate].GetMethod('Check'))
    $credential = if (Test-Path $xboxCredential) { Import-Clixml $xboxCredential } else { $null }
    return @{ Uri = "https://${address}:11443"; Credential = $credential }
}

function Invoke-XboxApi($xbox, $path, $outFile = '') {
    $request = @{ Uri = $xbox.Uri + $path; UseBasicParsing = $true }
    if ($xbox.Credential) { $request.Credential = $xbox.Credential }
    if ($outFile) { Invoke-WebRequest @request -OutFile $outFile } else { Invoke-RestMethod @request }
}

function Invoke-XboxSetup {
    if (-not $Address) { Fail 'give the address Dev Home shows: uwp.ps1 xbox -Address <IP>' }
    $xboxAddress = $Address -replace '^https?://' -replace '[:/].*$'
    New-Item -ItemType Directory $XboxDir -Force | Out-Null
    @{ address = $xboxAddress } | ConvertTo-Json | Set-Content $xboxConfig -Encoding utf8
    Remove-Item $xboxCredential -ErrorAction SilentlyContinue
    Step "Device Portal at $xboxAddress"
    $credential = Get-Credential -Message 'The Xbox Device Portal sign-in (Cancel if it has none)'
    if ($credential) { $credential | Export-Clixml $xboxCredential }
    $info = Invoke-XboxApi (Get-Xbox) '/api/os/info'
    Write-Host "connected: $($info.ComputerName), $($info.OsEdition) $($info.OsVersion)"
}

function New-XboxClient($xbox) {
    # An HTTP client for the Device Portal's changes (POST), which need its CSRF token.
    Add-Type -AssemblyName System.Net.Http
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.CookieContainer = [Net.CookieContainer]::new()
    if ($xbox.Credential) { $handler.Credentials = $xbox.Credential.GetNetworkCredential() }
    $client = [Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(10)
    # The token is a cookie; the start page sets it for the whole site (an API call would set it
    # for its own path only).
    [void]$client.GetAsync("$($xbox.Uri)/").Result
    $token = $handler.CookieContainer.GetCookies([Uri]$xbox.Uri) | Where-Object Name -eq 'CSRF-Token'
    if ($token) { $client.DefaultRequestHeaders.Add('X-CSRF-Token', $token.Value) }
    return $client
}

function Get-XboxPackage($xbox) {
    $package = (Invoke-XboxApi $xbox '/api/app/packagemanager/packages').InstalledPackages |
        Where-Object PackageFullName -like "${packageName}_*" |
        Sort-Object { [version]('{0}.{1}.{2}.{3}' -f $_.Version.Major, $_.Version.Minor, $_.Version.Build,
            $_.Version.Revision) } | Select-Object -Last 1
    # (Right after an install, the replaced version may still be listed.)
    if (-not $package) { Fail "$packageName is not installed on the Xbox" }
    return $package
}

function Invoke-XboxInstall {
    # The newest package from the package step; the framework packages are on the Xbox already
    # (the first install through the Device Portal's page brings them).
    $xbox = Get-Xbox
    $msix = Get-ChildItem "$BuildDir\uwp-package\*.msix" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $msix) { Fail 'no package: run the package step first' }
    $client = New-XboxClient $xbox
    Step "install $($msix.Name) on the Xbox"
    $stream = [IO.File]::OpenRead($msix.FullName)
    try {
        # The file part as browsers send it (quoted names, no filename*), which the Device Portal
        # needs to recognize the package.
        $file = [Net.Http.StreamContent]::new($stream)
        $file.Headers.ContentDisposition = [Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data')
        $file.Headers.ContentDisposition.Name = "`"$($msix.Name)`""
        $file.Headers.ContentDisposition.FileName = "`"$($msix.Name)`""
        $file.Headers.ContentType = [Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
        $form = [Net.Http.MultipartFormDataContent]::new()
        $form.Add($file)
        $response = $client.PostAsync(
            "$($xbox.Uri)/api/app/packagemanager/package?package=$([Uri]::EscapeDataString($msix.Name))",
            $form).Result
    } finally {
        $stream.Dispose()
    }
    if (-not $response.IsSuccessStatusCode) {
        Fail "upload: $([int]$response.StatusCode) $($response.Content.ReadAsStringAsync().Result)"
    }
    # The installation goes on after the upload; its state is 204 until it ends.
    do {
        Start-Sleep -Seconds 1
        $state = $client.GetAsync("$($xbox.Uri)/api/app/packagemanager/state").Result
    } while ([int]$state.StatusCode -eq 204)
    $result = $state.Content.ReadAsStringAsync().Result | ConvertFrom-Json
    if (-not $result.Success) { Fail "install: $($result.CodeText) $($result.Reason)" }
    Write-Host 'installed'
}

function Get-AppFolderQuery($package, [string]$folder) {
    # A folder of the app's data (LocalAppData\Packages\<package>), e.g. \LocalState.
    return 'knownfolderid=LocalAppData&packagefullname={0}&path={1}' -f
        [Uri]::EscapeDataString($package.PackageFullName), [Uri]::EscapeDataString($folder)
}

function Send-XboxFile($xbox, $client, $package, [string]$folder, [string]$name, [byte[]]$bytes) {
    $content = [Net.Http.ByteArrayContent]::new($bytes)
    $content.Headers.ContentDisposition = [Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data')
    $content.Headers.ContentDisposition.Name = "`"$name`""
    $content.Headers.ContentDisposition.FileName = "`"$name`""
    $form = [Net.Http.MultipartFormDataContent]::new()
    $form.Add($content)
    $response = $client.PostAsync(
        "$($xbox.Uri)/api/filesystem/apps/file?$(Get-AppFolderQuery $package $folder)", $form).Result
    if (-not $response.IsSuccessStatusCode) {
        Fail "${name}: $([int]$response.StatusCode) $($response.Content.ReadAsStringAsync().Result)"
    }
}

function New-XboxFolder($xbox, $client, $package, [string]$parent, [string]$name) {
    # Creates the folder `name` in the app's folder `parent` unless it exists.
    $query = Get-AppFolderQuery $package $parent
    $items = (Invoke-XboxApi $xbox "/api/filesystem/apps/files?$query").Items
    if ($items | Where-Object Name -eq $name) { return }
    $response = $client.PostAsync(
        "$($xbox.Uri)/api/filesystem/apps/folder?$query&newfoldername=$([Uri]::EscapeDataString($name))",
        $null).Result
    if (-not $response.IsSuccessStatusCode) {
        Fail "folder ${name}: $([int]$response.StatusCode) $($response.Content.ReadAsStringAsync().Result)"
    }
}

function Invoke-XboxUpload {
    if (-not $Files) { Fail 'give the files: uwp.ps1 upload -Files <file>[,<file>...] [-To <folder>]' }
    $items = foreach ($pattern in $Files) {
        $found = @(Get-Item $pattern -ErrorAction SilentlyContinue)
        if (-not $found) { Fail "no such file: $pattern" }
        $found | ForEach-Object {
            if ($_.PSIsContainer) { Fail "a folder, not a file: $($_.FullName)" }
            $_
        }
    }
    $xbox = Get-Xbox
    $package = Get-XboxPackage $xbox
    $client = New-XboxClient $xbox
    $folder = '\LocalState'
    foreach ($part in ($To -split '[\\/]' | Where-Object { $_ })) {
        New-XboxFolder $xbox $client $package $folder $part
        $folder += "\$part"
    }
    Step "upload to $folder on the Xbox"
    foreach ($item in $items) {
        Send-XboxFile $xbox $client $package $folder $item.Name ([IO.File]::ReadAllBytes($item.FullName))
        Write-Host "  $($item.Name) ($($item.Length) bytes)"
    }
}

function Invoke-XboxLaunch {
    # Starts the app, afresh: the launcher, or a game with -Title or -Game. The Device Portal
    # starts apps without arguments, so they go to LocalState\launch.txt, which the app reads
    # (and deletes) when it starts.
    $xbox = Get-Xbox
    $package = Get-XboxPackage $xbox
    $client = New-XboxClient $xbox
    $encode = { param($text) [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($text)) }
    $stop = $client.DeleteAsync("$($xbox.Uri)/api/taskmanager/app?package=" +
        [Uri]::EscapeDataString((& $encode $package.PackageFullName))).Result
    if ($stop.IsSuccessStatusCode) { Start-Sleep -Seconds 2 }
    $arguments = if ($Title) { "${protocol}://run?title=" + [Uri]::EscapeDataString($Title) }
        elseif ($Game) { "${protocol}://run?game=" + [Uri]::EscapeDataString($Game) } else { '' }
    if ($arguments) {
        Send-XboxFile $xbox $client $package '\LocalState' 'launch.txt' ([Text.Encoding]::UTF8.GetBytes($arguments))
    }
    Step "start $($package.PackageFullName) on the Xbox $arguments"
    $response = $client.PostAsync(("$($xbox.Uri)/api/taskmanager/app?appid={0}&package={1}" -f
        [Uri]::EscapeDataString((& $encode $package.PackageRelativeId)),
        [Uri]::EscapeDataString((& $encode $package.PackageFullName))), $null).Result
    if (-not $response.IsSuccessStatusCode) {
        Fail "start: $([int]$response.StatusCode) $($response.Content.ReadAsStringAsync().Result)"
    }
    Write-Host 'started'
}

function Invoke-XboxScreenshot {
    $xbox = Get-Xbox
    $out = "$XboxDir\screenshots"
    New-Item -ItemType Directory $out -Force | Out-Null
    $file = "$out\$((Get-Date).ToString('yyyy-MM-dd_HH-mm-ss')).png"
    Invoke-XboxApi $xbox '/ext/screenshot?download=true&hdr=false' $file
    Write-Host $file
}

function Invoke-XboxLogs {
    $xbox = Get-Xbox
    $package = Get-XboxPackage $xbox
    $out = "$XboxDir\logs"
    New-Item -ItemType Directory $out -Force | Out-Null
    foreach ($name in 'kyty-uwp.txt', 'kyty-emulator.txt') {
        $query = "$(Get-AppFolderQuery $package '\LocalState')&filename=$([Uri]::EscapeDataString($name))"
        Remove-Item "$out\$name" -ErrorAction SilentlyContinue
        try {
            Invoke-XboxApi $xbox "/api/filesystem/apps/file?$query" "$out\$name"
        } catch {
            Write-Host "${name}: $($_.Exception.Message)" -ForegroundColor Yellow
            continue
        }
        Step "$name (the end; the whole file is in $out)"
        Get-LogTail "$out\$name" 40
    }
}

function Get-LocalState {
    $package = Get-AppxPackage -Name $packageName
    if (-not $package) { Fail 'the app is not installed; run deploy first' }
    return "$env:LOCALAPPDATA\Packages\$($package.PackageFamilyName)\LocalState"
}

function Get-AppWindow {
    # A UWP app's window belongs to ApplicationFrameHost; the emulator titles it with the frame rate.
    return Get-Process ApplicationFrameHost -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -like "*$displayName*" } | Select-Object -First 1
}

function Invoke-Run {
    $package = Get-AppxPackage -Name $packageName
    if (-not $package) { Fail 'the app is not installed; run deploy first' }
    $localState = Get-LocalState
    $log = "$localState\kyty-uwp.txt"
    $emulatorLog = "$localState\kyty-emulator.txt"
    Get-Process kyty_uwp -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Remove-Item $log, $emulatorLog -ErrorAction SilentlyContinue
    if ($Title) {
        $uri = "${protocol}://run?title=" + [Uri]::EscapeDataString($Title)
    } elseif ($Game) {
        $uri = "${protocol}://run?game=" + [Uri]::EscapeDataString($Game)
    } else {
        $uri = "shell:AppsFolder\$($package.PackageFamilyName)!App"
    }
    Step "run $uri"
    Start-Process $uri
    Add-Type -Namespace KytyUwp -Name Window -ErrorAction SilentlyContinue -MemberDefinition `
        '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr window);'
    $exited = $false
    for ($t = 1; $t -le $Seconds; $t++) {
        Start-Sleep 1
        if ($t -gt 3 -and -not (Get-Process kyty_uwp -ErrorAction SilentlyContinue)) { $exited = $true; break }
        $window = Get-AppWindow
        if ($window -and $t % 5 -eq 0) {
            [void][KytyUwp.Window]::SetForegroundWindow($window.MainWindowHandle)
            $progress = if ($window.MainWindowTitle -match 'frame: \d+, fps: \d+') { $Matches[0] } else { '' }
            Write-Host ('{0,4}s  {1}' -f $t, $progress)
        }
    }
    if ($exited) { Step "app exited after ~${t}s" } else { Step "app running after ${Seconds}s" }
    # Stopped before the logs are read, so the run lasts as long as asked.
    if (-not $exited -and -not $Keep) { Get-Process kyty_uwp -ErrorAction SilentlyContinue | Stop-Process -Force }
    if (Test-Path $log) { Get-LogTail $log 20 } else { Write-Host '(no app log written)' }
    if (Test-Path $emulatorLog) {
        Step "emulator log (last lines; $([int]((Get-Item $emulatorLog).Length / 1MB)) MB in all)"
        Get-LogTail $emulatorLog 30
    }
}

function Invoke-Folders {
    # The app's settings (LocalState\kyty-uwp.json, also edited by its settings page); only
    # game_folders changes here. LocalState\Games is always searched as well.
    $settingsFile = "$(Get-LocalState)\kyty-uwp.json"
    $settings = [pscustomobject]@{}
    if (Test-Path $settingsFile) {
        $settings = Get-Content $settingsFile -Raw | ConvertFrom-Json
    }
    $folders = @($settings.game_folders | Where-Object { $_ })
    if ($Add) {
        $path = (Resolve-Path $Add -ErrorAction SilentlyContinue).Path
        if (-not $path) { Fail "no such folder: $Add" }
        $folders = @($folders | Where-Object { $_ -ne $path }) + $path
    }
    if ($Remove) { $folders = @($folders | Where-Object { $_ -ne $Remove }) }
    if ($Add -or $Remove) {
        $settings | Add-Member -NotePropertyName game_folders -NotePropertyValue @($folders) -Force
        [IO.File]::WriteAllText($settingsFile, (ConvertTo-Json $settings))
    }
    Step "game folders (and $(Get-LocalState)\Games)"
    $folders | ForEach-Object { Write-Host "  $_" }
}

function Invoke-Games {
    $library = "$(Get-LocalState)\library.json"
    if (-not (Test-Path $library)) { Fail 'no scan yet; start the app once (run)' }
    (Get-Content $library -Raw -Encoding UTF8 | ConvertFrom-Json).games |
        Format-Table title_id, name, version -AutoSize | Out-String | Write-Host
}

function Get-Cdb {
    $cdb = "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe"
    if (-not (Test-Path $cdb)) { Fail 'cdb not found: install the Debugging Tools for Windows (Windows SDK)' }
    return $cdb
}

function Invoke-Stacks {
    $process = Get-Process kyty_uwp -ErrorAction SilentlyContinue
    if (-not $process) { Fail 'the app is not running' }
    $out = "$BuildDir\uwp-stacks.txt"
    # Non-invasive: reads the threads without stopping the app.
    & (Get-Cdb) -pv -p $process.Id -y "$BuildDir\uwp" -c '~*kc 24;q' > $out 2>&1
    Step "stacks of the emulator's threads (all threads: $out)"
    $thread = ''
    foreach ($line in Get-Content $out) {
        if ($line -match '^\s*[.#]?\s*\d+\s+Id: ') { $thread = $line.Trim(); continue }
        if ($thread -and $line -match 'kyty_uwp!') {
            Write-Host $thread; $thread = ''
        }
        if ($line -match 'kyty_uwp!') { Write-Host "    $($line.Trim())" }
    }
}

function Invoke-Debug {
    $process = Get-Process kyty_uwp -ErrorAction SilentlyContinue
    if (-not $process) { Fail 'the app is not running' }
    Start-Process (Get-Cdb) -ArgumentList "-p $($process.Id) -y `"$BuildDir\uwp`""
}

switch ($Verb) {
    'configure' { Invoke-Configure }
    'build' { Invoke-Build }
    'deploy' { Invoke-Deploy }
    'package' { Invoke-Package }
    'xbox' { Invoke-XboxSetup }
    'install' { Invoke-XboxInstall }
    'launch' { Invoke-XboxLaunch }
    'screenshot' { Invoke-XboxScreenshot }
    'logs' { Invoke-XboxLogs }
    'upload' { Invoke-XboxUpload }
    'run' { Invoke-Run }
    'all' { Invoke-Build; Invoke-Deploy; Invoke-Run }
    'folders' { Invoke-Folders }
    'games' { Invoke-Games }
    'stacks' { Invoke-Stacks }
    'debug' { Invoke-Debug }
}
