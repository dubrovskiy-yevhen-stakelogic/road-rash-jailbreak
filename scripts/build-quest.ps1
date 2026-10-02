# Builds the VR targets (docs\QUEST.md, docs\PCVR.md):
#   1. (with -Windows) the Windows tree -WindowsBuildDir, whose rrgame.exe has --vr / --vr-mock, next to
#      openxr_loader.dll;
#   2. the Quest native library: this repository's CMake tree configured for the NDK (arm64-v8a, android-32, OpenGL ES
#      3.2) in -BuildDir, target rrgame_quest (the game itself; -Target rrvrtest builds the minimal OpenXR test app)
#      -> <BuildDir>/jniLibs/arm64-v8a/{librrgame.so, libopenxr_loader.so} (the folder is emptied first, so the APK
#      carries only what was built now);
#   3. the APK: android/ packaged and signed by Gradle, offline, with a debug keystore generated under work/android.
# Only locally installed tools are used: the Android toolchain folder (-Toolchain; default: android-toolchain next to
# this repository, holding sdk\ with NDK 27.2.12479018, jdk21\ and gradle\) and CMake / Ninja from Visual Studio 2022.
# Nothing is downloaded (Gradle runs --offline from its local cache).
#
#   powershell -ExecutionPolicy Bypass -File scripts\build-quest.ps1 [-Windows] [-NoApk] [-BuildDir <dir>] [-WindowsBuildDir <dir>]
param(
    [string]$Toolchain = (Join-Path (Split-Path $PSScriptRoot | Split-Path) 'android-toolchain'),
    [string]$BuildDir = 'build_quest',
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$BuildType = 'Release',
    [ValidateSet('release', 'debug')][string]$Variant = 'release',
    [string]$Target = 'rrgame_quest',
    [string]$WindowsBuildDir = 'build_vr',
    [switch]$Windows,
    [switch]$NoApk
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$sdk = Join-Path $Toolchain 'sdk'
$ndk = Join-Path $sdk 'ndk\27.2.12479018'
$jdk = Join-Path $Toolchain 'jdk21'
$gradle = Join-Path $Toolchain 'gradle\bin\gradle.bat'
foreach ($p in @($ndk, $jdk, $gradle)) { if (!(Test-Path $p)) { throw "missing: $p" } }

if ($Windows) {
    & cmd /c (Join-Path $repo 'build.cmd') $WindowsBuildDir $BuildType
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed ($LASTEXITCODE)" }
    Get-Item (Join-Path $repo "$WindowsBuildDir\rrgame.exe")
}

# CMake >= 3.24 (the root CMakeLists.txt) and Ninja: the ones Visual Studio 2022 ships.
$vsCMake = $null
foreach ($edition in 'Community', 'Professional', 'Enterprise') {
    $c = "C:\Program Files\Microsoft Visual Studio\2022\$edition\Common7\IDE\CommonExtensions\Microsoft\CMake"
    if (Test-Path "$c\CMake\bin\cmake.exe") { $vsCMake = $c; break }
}
if (!$vsCMake) { throw 'Visual Studio 2022 CMake not found' }
$cmake = "$vsCMake\CMake\bin\cmake.exe"
$ninja = ("$vsCMake\Ninja\ninja.exe").Replace('\', '/')
$build = Join-Path $repo $BuildDir
& $cmake -S $repo -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$($ndk.Replace('\','/'))/build/cmake/android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 -DANDROID_STL=c++_static "-DCMAKE_BUILD_TYPE=$BuildType"
if ($LASTEXITCODE -ne 0) { throw "Quest configure failed ($LASTEXITCODE)" }
& $cmake --build $build --target $Target
if ($LASTEXITCODE -ne 0) { throw "Quest native build failed ($LASTEXITCODE)" }
# The APK's libraries: exactly the target's and the Khronos loader (an up-to-date target runs no POST_BUILD step,
# so they are gathered here every time; a library of another target left in the folder is removed).
$jni = Join-Path $build 'jniLibs\arm64-v8a'
New-Item -ItemType Directory -Force $jni | Out-Null
# Only remove libraries from the selected build's staging directory.
$jni = [IO.Path]::GetFullPath($jni)
if (!$jni.StartsWith([IO.Path]::GetFullPath($build).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'JNI staging directory is outside the build folder.'
}
Get-ChildItem -LiteralPath $jni -Filter '*.so' | Remove-Item -Force
$lib = if ($Target -eq 'rrgame_quest') { 'librrgame.so' } else { "lib$Target.so" }
Copy-Item (Join-Path $build $lib) $jni
Copy-Item (Join-Path $build 'openxr_aar\prefab\modules\openxr_loader\libs\android.arm64-v8a\libopenxr_loader.so') $jni
Get-ChildItem (Join-Path $build 'jniLibs\arm64-v8a') | Select-Object Name, Length | Format-Table | Out-String | Write-Host
if ($NoApk) { return }

# The debug keystore: generated once, locally, never committed (work/ is ignored).
$keyDir = Join-Path $repo 'work\android'
$keystore = Join-Path $keyDir 'debug.keystore'
if (!(Test-Path $keystore)) {
    New-Item -ItemType Directory -Force $keyDir | Out-Null
    & (Join-Path $jdk 'bin\keytool.exe') -genkeypair -keystore $keystore -storepass android -alias androiddebugkey `
        -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=Android Debug,O=Android,C=US'
    if ($LASTEXITCODE -ne 0) { throw 'keytool failed' }
}
$env:JAVA_HOME = $jdk
$env:ANDROID_HOME = $sdk
[IO.File]::WriteAllText((Join-Path $repo 'android\local.properties'), 'sdk.dir=' + $sdk.Replace('\', '/'),
    [Text.UTF8Encoding]::new($false))
$task = if ($Variant -eq 'release') { 'assembleRelease' } else { 'assembleDebug' }
& $gradle -p (Join-Path $repo 'android') --offline --no-daemon $task `
    "-PrrjbJniDir=$(Join-Path $build 'jniLibs')"
if ($LASTEXITCODE -ne 0) { throw "Gradle failed ($LASTEXITCODE)" }
Get-Item (Join-Path $repo "android\app\build\outputs\apk\$Variant\app-$Variant.apk")
