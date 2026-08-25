[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $AgentDll,

    [Parameter(Mandatory = $true)]
    [string] $NativeLoader,

    [string] $JavaHome = $env:JAVA_HOME,

    [ValidateRange(5, 120)]
    [int] $TimeoutSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Keep a deliberately crashing private test child from showing a system-modal
# WER/critical-error dialog on the user's desktop. Child processes inherit the
# process error mode unless explicitly created with CREATE_DEFAULT_ERROR_MODE.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class McOverlaySmokeErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
'@
$previousErrorMode = [McOverlaySmokeErrorMode]::SetErrorMode(0x0003)

function ConvertTo-NativeArgument {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string] $Value
    )

    # ProcessStartInfo.Arguments is a Windows command line, not a shell. Use
    # CommandLineToArgvW-compatible quoting so spaces, quotes, and trailing
    # backslashes survive on Windows PowerShell 5.1 and PowerShell 7.
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }

    $result = [System.Text.StringBuilder]::new()
    [void] $result.Append([char] 34)
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ([int] $character -eq 92) {
            ++$backslashes
            continue
        }

        if ([int] $character -eq 34) {
            [void] $result.Append([char] 92, (2 * $backslashes) + 1)
            [void] $result.Append([char] 34)
        }
        else {
            if ($backslashes -gt 0) {
                [void] $result.Append([char] 92, $backslashes)
            }
            [void] $result.Append($character)
        }
        $backslashes = 0
    }

    if ($backslashes -gt 0) {
        [void] $result.Append([char] 92, 2 * $backslashes)
    }
    [void] $result.Append([char] 34)
    return $result.ToString()
}

function Join-NativeArguments {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]] $ArgumentList
    )

    $quoted = foreach ($argument in $ArgumentList) {
        ConvertTo-NativeArgument -Value $argument
    }
    return [string]::Join(' ', [string[]] $quoted)
}

function Start-NativeProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Executable,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]] $ArgumentList,

        [switch] $RedirectInput
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.Arguments = Join-NativeArguments -ArgumentList $ArgumentList
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.RedirectStandardInput = $RedirectInput.IsPresent

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        $process.Dispose()
        throw "Could not start '$Executable'."
    }
    return $process
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Executable,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]] $ArgumentList,

        [Parameter(Mandatory = $true)]
        [int] $TimeoutMilliseconds
    )

    $process = $null
    try {
        $process = Start-NativeProcess -Executable $Executable -ArgumentList $ArgumentList
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $process.Kill()
            [void] $process.WaitForExit(5000)
            throw "'$Executable' did not exit within $TimeoutMilliseconds ms."
        }
        $process.WaitForExit()

        return [pscustomobject] @{
            ExitCode = $process.ExitCode
            Stdout = $stdoutTask.GetAwaiter().GetResult()
            Stderr = $stderrTask.GetAwaiter().GetResult()
        }
    }
    finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
    }
}

function Resolve-FilePath {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $Description
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    $fullPath = [System.IO.Path]::GetFullPath($resolved.ProviderPath)
    if (-not [System.IO.File]::Exists($fullPath)) {
        throw "$Description is not a regular file: '$fullPath'."
    }
    return $fullPath
}

function Resolve-JdkTool {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,

        [AllowEmptyString()]
        [string] $ConfiguredJavaHome
    )

    if (-not [string]::IsNullOrWhiteSpace($ConfiguredJavaHome)) {
        $candidate = Join-Path -Path $ConfiguredJavaHome -ChildPath "bin\$Name.exe"
        if ([System.IO.File]::Exists($candidate)) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    $command = Get-Command "$Name.exe" -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) {
        throw "Could not find $Name.exe. Set -JavaHome to a full JDK installation."
    }
    return [System.IO.Path]::GetFullPath($command.Source)
}

function Get-RemainingMilliseconds {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Stopwatch] $Stopwatch,

        [Parameter(Mandatory = $true)]
        [int] $LimitSeconds,

        [Parameter(Mandatory = $true)]
        [string] $Operation
    )

    $remaining = ([long] $LimitSeconds * 1000L) - $Stopwatch.ElapsedMilliseconds
    if ($remaining -le 0) {
        throw "Timed out while $Operation."
    }
    return [int] [Math]::Min($remaining, [int]::MaxValue)
}

function Wait-TaskResult {
    param(
        [Parameter(Mandatory = $true)]
        [System.Threading.Tasks.Task] $Task,

        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Stopwatch] $Stopwatch,

        [Parameter(Mandatory = $true)]
        [int] $LimitSeconds,

        [Parameter(Mandatory = $true)]
        [string] $Operation
    )

    try {
        if (-not $Task.IsCompleted) {
            $remaining = Get-RemainingMilliseconds -Stopwatch $Stopwatch `
                -LimitSeconds $LimitSeconds -Operation $Operation
            if (-not $Task.Wait($remaining)) {
                throw "Timed out while $Operation."
            }
        }
        return $Task.GetAwaiter().GetResult()
    }
    catch [System.AggregateException] {
        throw ($_.Exception.GetBaseException())
    }
}

$agentPath = $null
$loaderPath = $null
$javaExecutable = $null
$javacExecutable = $null
$temporaryDirectory = $null
$pipe = $null
$reader = $null
$writer = $null
$targetProcess = $null
$targetErrorTask = $null
$loaderProcess = $null
$loaderOutputTask = $null
$loaderErrorTask = $null
$detachSent = $false
$detachComplete = $false
$failure = $null

try {
    if ($env:OS -ne 'Windows_NT') {
        throw 'This smoke test is Windows-only.'
    }

    $agentPath = Resolve-FilePath -Path $AgentDll -Description 'Agent DLL'
    $loaderPath = Resolve-FilePath -Path $NativeLoader -Description 'Native loader'
    if ([System.IO.Path]::GetExtension($agentPath) -ine '.dll') {
        throw "Agent path must end in .dll: '$agentPath'."
    }
    if ([System.IO.Path]::GetExtension($loaderPath) -ine '.exe') {
        throw "Native loader path must end in .exe: '$loaderPath'."
    }

    $javaExecutable = Resolve-JdkTool -Name 'java' -ConfiguredJavaHome $JavaHome
    $javacExecutable = Resolve-JdkTool -Name 'javac' -ConfiguredJavaHome $JavaHome

    $temporaryDirectory = Join-Path -Path ([System.IO.Path]::GetTempPath()) `
        -ChildPath ("McOverlayNativeLoaderSmoke-{0}" -f [guid]::NewGuid().ToString('N'))
    $classesDirectory = Join-Path -Path $temporaryDirectory -ChildPath 'classes'
    [void] [System.IO.Directory]::CreateDirectory($classesDirectory)

    $targetSource = Join-Path -Path $PSScriptRoot -ChildPath 'AttachSmokeTarget.java'
    $compile = Invoke-CapturedProcess -Executable $javacExecutable -ArgumentList @(
        '-encoding', 'UTF-8', '-d', $classesDirectory, $targetSource
    ) -TimeoutMilliseconds 30000
    if ($compile.ExitCode -ne 0) {
        throw "javac failed with exit code $($compile.ExitCode): $($compile.Stderr.Trim())"
    }

    # This is intentionally the only PID ever supplied to the native loader.
    # The script owns its Process object and explicitly disables HotSpot's
    # Attach mechanism to exercise the OS-loader fallback, not JDK Attach.
    $targetProcess = Start-NativeProcess -Executable $javaExecutable -ArgumentList @(
        '-XX:+DisableAttachMechanism',
        '-cp', $classesDirectory, 'com.mcoverlay.tests.AttachSmokeTarget'
    ) -RedirectInput
    $targetErrorTask = $targetProcess.StandardError.ReadToEndAsync()

    $startupWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $readyLine = Wait-TaskResult -Task $targetProcess.StandardOutput.ReadLineAsync() `
        -Stopwatch $startupWatch -LimitSeconds 10 -Operation 'waiting for the private JVM target'
    if ($readyLine -ne 'MC_OVERLAY_ATTACH_SMOKE_READY') {
        throw "Private JVM target did not report readiness (received '$readyLine')."
    }

    $token = [guid]::NewGuid().ToString('N').ToLowerInvariant()
    $pipeName = "McOverlayNativeLoaderSmoke-$PID-$([guid]::NewGuid().ToString('N'))"
    $fullPipeName = "\\.\pipe\$pipeName"
    $agentOptions = "pipe=$fullPipeName;token=$token;protocol=1"

    # Listen before starting the loader; McOverlay_Start can connect as soon as
    # the remote bootstrap thread transfers control to the agent DLL.
    $pipe = [System.IO.Pipes.NamedPipeServerStream]::new(
        $pipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        1,
        [System.IO.Pipes.PipeTransmissionMode]::Byte,
        [System.IO.Pipes.PipeOptions]::Asynchronous,
        4096,
        4096)
    $connectionTask = $pipe.WaitForConnectionAsync()

    $loaderProcess = Start-NativeProcess -Executable $loaderPath -ArgumentList @(
        $targetProcess.Id.ToString(), $agentPath, $agentOptions
    )
    $loaderOutputTask = $loaderProcess.StandardOutput.ReadToEndAsync()
    $loaderErrorTask = $loaderProcess.StandardError.ReadToEndAsync()

    $protocolWatch = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $connectionTask.IsCompleted) {
        if ($loaderProcess.HasExited -and $loaderProcess.ExitCode -ne 0) {
            $loaderProcess.WaitForExit()
            $earlyStdout = $loaderOutputTask.GetAwaiter().GetResult().Trim()
            $earlyStderr = $loaderErrorTask.GetAwaiter().GetResult().Trim()
            throw "Native loader exited with code $($loaderProcess.ExitCode) before the agent connected. stdout: $earlyStdout stderr: $earlyStderr"
        }

        $connectionRemaining = Get-RemainingMilliseconds -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for the agent pipe connection'
        [void] $connectionTask.Wait([Math]::Min(100, $connectionRemaining))
    }
    [void] (Wait-TaskResult -Task $connectionTask -Stopwatch $protocolWatch `
        -LimitSeconds $TimeoutSeconds -Operation 'waiting for the agent pipe connection')

    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $reader = [System.IO.StreamReader]::new($pipe, $utf8, $false, 1024, $true)
    $writer = [System.IO.StreamWriter]::new($pipe, $utf8, 1024, $true)
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true

    $expectedHello = "HELLO 1 $($targetProcess.Id) $token"
    $helloSeen = $false
    $hookReadySeen = $false
    $received = [System.Collections.Generic.List[string]]::new()

    while (-not ($helloSeen -and $hookReadySeen)) {
        $line = Wait-TaskResult -Task $reader.ReadLineAsync() -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for HELLO and HOOK_READY'
        if ($null -eq $line) {
            throw 'The agent disconnected before completing its startup protocol.'
        }
        if ([System.Text.Encoding]::UTF8.GetByteCount($line) -gt 1024) {
            throw 'The agent sent a protocol line larger than 1024 bytes.'
        }
        [void] $received.Add($line)

        if ($line.StartsWith('HELLO ', [System.StringComparison]::Ordinal)) {
            if ($line -cne $expectedHello) {
                throw "Agent authentication mismatch: expected '$expectedHello', received '$line'."
            }
            $helloSeen = $true
        }
        elseif ($line.StartsWith('HOOK_READY ', [System.StringComparison]::Ordinal)) {
            if ($line -cne 'HOOK_READY OpenGL') {
                throw "Unexpected hook backend in '$line'."
            }
            $hookReadySeen = $true
        }
        elseif ($line.StartsWith('ERROR ', [System.StringComparison]::Ordinal)) {
            throw "Agent reported an error: $line"
        }
        # STATUS is informational. The private AWT target deliberately has no
        # HGLRC, so RENDERER_READY is not expected in this smoke test.
    }

    $remaining = Get-RemainingMilliseconds -Stopwatch $protocolWatch `
        -LimitSeconds $TimeoutSeconds -Operation 'waiting for the native loader to exit'
    if (-not $loaderProcess.WaitForExit($remaining)) {
        throw 'Native loader did not exit after starting the agent.'
    }
    $loaderProcess.WaitForExit()
    $loaderStdout = $loaderOutputTask.GetAwaiter().GetResult()
    $loaderStderr = $loaderErrorTask.GetAwaiter().GetResult()
    if ($loaderProcess.ExitCode -ne 0) {
        throw "Native loader exited with code $($loaderProcess.ExitCode). stdout: $($loaderStdout.Trim()) stderr: $($loaderStderr.Trim())"
    }

    # Exercise the same controller commands used by the desktop application.
    # DETACH stops hooks and IPC; the private JVM then exits and lets Windows
    # unload the process image normally.
    $writer.WriteLine('STATE 0 0')
    $writer.WriteLine('DETACH')
    $detachSent = $true

    # The agent keeps the pipe open until OpenGL hooks, renderer state, and JNI
    # references have finished teardown. Never let the private JVM exit while
    # a remote/bootstrap thread may still be using those resources.
    $cleanupWatch = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $detachComplete) {
        $line = Wait-TaskResult -Task $reader.ReadLineAsync() -Stopwatch $cleanupWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for DETACH_COMPLETE'
        if ($null -eq $line) {
            throw 'The agent disconnected before reporting DETACH_COMPLETE.'
        }
        [void] $received.Add($line)
        if ($line -ceq 'DETACH_COMPLETE') {
            $detachComplete = $true
        }
        elseif ($line.StartsWith('ERROR ', [System.StringComparison]::Ordinal)) {
            throw "Agent reported an error while detaching: $line"
        }
    }

    # Regression for controller "Browse processes -> Return": after DETACH the
    # DLL remains in the private JVM. Reuse the same exact-path image and pipe
    # endpoint; McOverlayNativeLoader must call the resident McOverlay_Start
    # export instead of returning the historical AGENT_ALREADY_LOADED error.
    $writer.Dispose()
    $reader.Dispose()
    $writer = $null
    $reader = $null
    if ($pipe.IsConnected) {
        $pipe.Disconnect()
    }
    $loaderProcess.Dispose()
    $loaderProcess = $null
    $detachSent = $false
    $detachComplete = $false
    [void] $received.Add('REATTACH_BEGIN')

    $connectionTask = $pipe.WaitForConnectionAsync()
    $loaderProcess = Start-NativeProcess -Executable $loaderPath -ArgumentList @(
        $targetProcess.Id.ToString(), $agentPath, $agentOptions
    )
    $loaderOutputTask = $loaderProcess.StandardOutput.ReadToEndAsync()
    $loaderErrorTask = $loaderProcess.StandardError.ReadToEndAsync()
    $reattachWatch = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $connectionTask.IsCompleted) {
        if ($loaderProcess.HasExited -and $loaderProcess.ExitCode -ne 0) {
            $loaderProcess.WaitForExit()
            $earlyStdout = $loaderOutputTask.GetAwaiter().GetResult().Trim()
            $earlyStderr = $loaderErrorTask.GetAwaiter().GetResult().Trim()
            throw "Resident native loader exited with code $($loaderProcess.ExitCode). stdout: $earlyStdout stderr: $earlyStderr"
        }
        $remaining = Get-RemainingMilliseconds -Stopwatch $reattachWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for resident-agent reconnection'
        [void] $connectionTask.Wait([Math]::Min(100, $remaining))
    }
    [void] (Wait-TaskResult -Task $connectionTask -Stopwatch $reattachWatch `
        -LimitSeconds $TimeoutSeconds -Operation 'waiting for resident-agent reconnection')

    $reader = [System.IO.StreamReader]::new($pipe, $utf8, $false, 1024, $true)
    $writer = [System.IO.StreamWriter]::new($pipe, $utf8, 1024, $true)
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true
    $helloSeen = $false
    $hookReadySeen = $false
    while (-not ($helloSeen -and $hookReadySeen)) {
        $line = Wait-TaskResult -Task $reader.ReadLineAsync() -Stopwatch $reattachWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for resident HELLO and HOOK_READY'
        if ($null -eq $line) {
            throw 'The resident agent disconnected before completing its startup protocol.'
        }
        [void] $received.Add($line)
        if ($line.StartsWith('HELLO ', [System.StringComparison]::Ordinal)) {
            if ($line -cne $expectedHello) {
                throw "Resident agent authentication mismatch: expected '$expectedHello', received '$line'."
            }
            $helloSeen = $true
        }
        elseif ($line -ceq 'HOOK_READY OpenGL') {
            $hookReadySeen = $true
        }
        elseif ($line.StartsWith('ERROR ', [System.StringComparison]::Ordinal)) {
            throw "Resident agent reported an error: $line"
        }
    }
    $remaining = Get-RemainingMilliseconds -Stopwatch $reattachWatch `
        -LimitSeconds $TimeoutSeconds -Operation 'waiting for resident native loader to exit'
    if (-not $loaderProcess.WaitForExit($remaining)) {
        throw 'Resident native loader did not exit after restarting the agent.'
    }
    $loaderProcess.WaitForExit()
    $loaderStdout = $loaderOutputTask.GetAwaiter().GetResult()
    $loaderStderr = $loaderErrorTask.GetAwaiter().GetResult()
    if ($loaderProcess.ExitCode -ne 0) {
        throw "Resident native loader exited with code $($loaderProcess.ExitCode). stdout: $($loaderStdout.Trim()) stderr: $($loaderStderr.Trim())"
    }

    $writer.WriteLine('STATE 0 0')
    $writer.WriteLine('DETACH')
    $detachSent = $true
    while (-not $detachComplete) {
        $line = Wait-TaskResult -Task $reader.ReadLineAsync() -Stopwatch $reattachWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for resident DETACH_COMPLETE'
        if ($null -eq $line) {
            throw 'The resident agent disconnected before reporting DETACH_COMPLETE.'
        }
        [void] $received.Add($line)
        if ($line -ceq 'DETACH_COMPLETE') {
            $detachComplete = $true
        }
        elseif ($line.StartsWith('ERROR ', [System.StringComparison]::Ordinal)) {
            throw "Resident agent reported an error while detaching: $line"
        }
    }

    Write-Host 'PASS: Native loader initial load and resident reattach both completed.'
    Write-Host "Target PID: $($targetProcess.Id) (private child, Attach disabled)"
    Write-Host "Protocol: $([string]::Join(' | ', $received))"
}
catch {
    $failure = $_
}
finally {
    if ($null -ne $writer -and -not $detachSent) {
        try { $writer.WriteLine('DETACH') } catch { }
    }

    if ($null -ne $loaderProcess) {
        try {
            if (-not $loaderProcess.HasExited) {
                # McOverlayNativeLoader has a 15-second remote-call deadline.
                # Wait beyond it before terminating the local helper; killing
                # the helper cannot cancel a thread already inside the JVM.
                if (-not $loaderProcess.WaitForExit(20000)) {
                    $loaderProcess.Kill()
                    [void] $loaderProcess.WaitForExit(5000)
                }
            }
        }
        catch { }
        finally { $loaderProcess.Dispose() }
    }

    if ($null -ne $writer -and -not $detachComplete) {
        # Best-effort bounded grace period for failure paths where a complete
        # protocol reader was never established. The success path above uses
        # the explicit DETACH_COMPLETE boundary instead.
        Start-Sleep -Milliseconds 3500
    }

    foreach ($resource in @($writer, $reader, $pipe)) {
        if ($null -ne $resource) {
            try { $resource.Dispose() } catch { }
        }
    }

    if ($null -ne $targetProcess) {
        try {
            if (-not $targetProcess.HasExited) {
                try {
                    $targetProcess.StandardInput.WriteLine('QUIT')
                    $targetProcess.StandardInput.Flush()
                    $targetProcess.StandardInput.Close()
                }
                catch { }

                if (-not $targetProcess.WaitForExit(15000)) {
                    $targetProcess.Kill()
                    [void] $targetProcess.WaitForExit(5000)
                }
            }
        }
        catch { }
        finally { $targetProcess.Dispose() }
    }

    # Recursively remove only our GUID-named directory immediately below the
    # current Windows temp root. Never widen this cleanup to a caller path.
    if (-not [string]::IsNullOrWhiteSpace($temporaryDirectory)) {
        $expectedParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
        $actualParent = [System.IO.Path]::GetDirectoryName(
            [System.IO.Path]::GetFullPath($temporaryDirectory)).TrimEnd('\')
        $leaf = [System.IO.Path]::GetFileName($temporaryDirectory)
        if ($actualParent -ieq $expectedParent -and
            $leaf -cmatch '^McOverlayNativeLoaderSmoke-[0-9a-f]{32}$' -and
            [System.IO.Directory]::Exists($temporaryDirectory)) {
            Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
        }
    }

    [void] [McOverlaySmokeErrorMode]::SetErrorMode($previousErrorMode)
}

if ($null -ne $failure) {
    [Console]::Error.WriteLine("FAIL: $($failure.Exception.Message)")
    exit 1
}

exit 0
