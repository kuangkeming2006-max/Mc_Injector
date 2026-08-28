[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $HarnessExe,

    [Parameter(Mandatory = $true)]
    [string] $AgentDll,

    [string] $JavaHome = $env:JAVA_HOME,

    [switch] $SplitThreads,

    [ValidateRange(10, 120)]
    [int] $TimeoutSeconds = 45
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not ('McOverlayOpenGlSmokeNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class McOverlayOpenGlSmokeNative
{
    public const uint WmClose = 0x0010;
    private const uint WmKeyDown = 0x0100;
    private const uint WmKeyUp = 0x0101;

    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessageW(
        IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindow(IntPtr window);

    public static bool PostVirtualKey(IntPtr window, uint virtualKey, uint scanCode)
    {
        long down = 1L | ((long)scanCode << 16);
        long up = down | (1L << 30) | (1L << 31);
        return PostMessageW(window, WmKeyDown, new UIntPtr(virtualKey), new IntPtr(down)) &&
               PostMessageW(window, WmKeyUp, new UIntPtr(virtualKey), new IntPtr(up));
    }
}
'@
}

$previousErrorMode = [McOverlayOpenGlSmokeNative]::SetErrorMode(
    0x0001 -bor 0x0002 -bor 0x8000)

function ConvertTo-NativeArgument {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string] $Value
    )

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

function Resolve-JvmDll {
    param(
        [AllowEmptyString()]
        [string] $ConfiguredJavaHome
    )

    $jdkRoot = $ConfiguredJavaHome
    if ([string]::IsNullOrWhiteSpace($jdkRoot)) {
        $java = Get-Command 'java.exe' -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -eq $java) {
            throw 'Could not find a 64-bit JDK. Set -JavaHome explicitly.'
        }
        $jdkRoot = [System.IO.Directory]::GetParent(
            [System.IO.Path]::GetDirectoryName($java.Source)).FullName
    }

    $candidate = Join-Path -Path $jdkRoot -ChildPath 'bin\server\jvm.dll'
    if (-not [System.IO.File]::Exists($candidate)) {
        throw "Could not find bin\server\jvm.dll below JavaHome '$jdkRoot'."
    }
    return [System.IO.Path]::GetFullPath($candidate)
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

function Read-ProtocolLine {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.StreamReader] $Reader,

        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Stopwatch] $Stopwatch,

        [Parameter(Mandatory = $true)]
        [int] $LimitSeconds,

        [Parameter(Mandatory = $true)]
        [string] $Operation,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]] $Received
    )

    $line = Wait-TaskResult -Task $Reader.ReadLineAsync() -Stopwatch $Stopwatch `
        -LimitSeconds $LimitSeconds -Operation $Operation
    if ($null -eq $line) {
        throw "The agent disconnected while $Operation. Received: $([string]::Join(' | ', $Received))"
    }
    if ([System.Text.Encoding]::UTF8.GetByteCount($line) -gt 1024) {
        throw 'The agent sent a protocol line larger than 1024 bytes.'
    }
    [void] $Received.Add($line)
    if ($line.StartsWith('ERROR ', [System.StringComparison]::Ordinal)) {
        throw "Agent reported an error: $line"
    }
    return $line
}

$harnessPath = $null
$agentPath = $null
$jvmPath = $null
$pipe = $null
$reader = $null
$writer = $null
$targetProcess = $null
$targetErrorTask = $null
$readyTask = $null
$windowHandle = [IntPtr]::Zero
$detachSent = $false
$detachComplete = $false
$failure = $null
$received = [System.Collections.Generic.List[string]]::new()
$diagnosticExitCode = $null
$diagnosticStderr = ''

try {
    if ($env:OS -ne 'Windows_NT') {
        throw 'This smoke test is Windows-only.'
    }

    $harnessPath = Resolve-FilePath -Path $HarnessExe -Description 'OpenGL JVM harness'
    $agentPath = Resolve-FilePath -Path $AgentDll -Description 'Agent DLL'
    $jvmPath = Resolve-JvmDll -ConfiguredJavaHome $JavaHome
    if ([System.IO.Path]::GetExtension($harnessPath) -ine '.exe') {
        throw "Harness path must end in .exe: '$harnessPath'."
    }
    if ([System.IO.Path]::GetExtension($agentPath) -ine '.dll') {
        throw "Agent path must end in .dll: '$agentPath'."
    }

    $token = [guid]::NewGuid().ToString('N').ToLowerInvariant()
    $pipeName = "McOverlayOpenGlJvmSmoke-$PID-$([guid]::NewGuid().ToString('N'))"
    $fullPipeName = "\\.\pipe\$pipeName"
    $threadMode = if ($SplitThreads.IsPresent) { 'split' } else { 'unified' }

    # The pipe exists before the private child starts. The harness has no PID
    # parameter and no discovery code: it creates its JVM, WGL context, and
    # agent runtime inside this one owned child process.
    $pipe = [System.IO.Pipes.NamedPipeServerStream]::new(
        $pipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        1,
        [System.IO.Pipes.PipeTransmissionMode]::Byte,
        [System.IO.Pipes.PipeOptions]::Asynchronous,
        4096,
        4096)
    $connectionTask = $pipe.WaitForConnectionAsync()

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $harnessPath
    $startInfo.Arguments = Join-NativeArguments -ArgumentList @(
        '--jvm', $jvmPath,
        '--agent', $agentPath,
        '--pipe', $fullPipeName,
        '--token', $token,
        '--thread-mode', $threadMode
    )
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $targetProcess = [System.Diagnostics.Process]::new()
    $targetProcess.StartInfo = $startInfo
    if (-not $targetProcess.Start()) {
        throw "Could not start '$harnessPath'."
    }
    $targetErrorTask = $targetProcess.StandardError.ReadToEndAsync()
    $readyTask = $targetProcess.StandardOutput.ReadLineAsync()

    $protocolWatch = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $connectionTask.IsCompleted) {
        if ($targetProcess.HasExited) {
            $earlyError = $targetErrorTask.GetAwaiter().GetResult().Trim()
            throw "Private OpenGL JVM harness exited with code $($targetProcess.ExitCode) before connecting. stderr: $earlyError"
        }
        $remaining = Get-RemainingMilliseconds -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for the agent pipe connection'
        [void] $connectionTask.Wait([Math]::Min(100, $remaining))
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
    while (-not ($helloSeen -and $hookReadySeen)) {
        $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for HELLO and HOOK_READY' `
            -Received $received
        if ($line.StartsWith('HELLO ', [System.StringComparison]::Ordinal)) {
            if ($line -cne $expectedHello) {
                throw "Agent authentication mismatch: expected '$expectedHello', received '$line'."
            }
            $helloSeen = $true
        }
        elseif ($line -ceq 'HOOK_READY OpenGL') {
            $hookReadySeen = $true
        }
    }

    $readyLine = Wait-TaskResult -Task $readyTask -Stopwatch $protocolWatch `
        -LimitSeconds $TimeoutSeconds -Operation 'waiting for the private WGL window'
    $readyMatch = [regex]::Match(
        [string] $readyLine,
        '^MC_OVERLAY_OPENGL_SMOKE_READY (?<pid>[0-9]+) (?<hwnd>[0-9a-fA-F]+) (?<windowThread>[0-9]+) (?<renderThread>[0-9]+) (?<mode>unified|split)$')
    if (-not $readyMatch.Success -or
        [int] $readyMatch.Groups['pid'].Value -ne $targetProcess.Id) {
        throw "Harness readiness line was invalid: '$readyLine'."
    }
    $windowThreadId = [uint32]::Parse($readyMatch.Groups['windowThread'].Value)
    $renderThreadId = [uint32]::Parse($readyMatch.Groups['renderThread'].Value)
    $reportedMode = $readyMatch.Groups['mode'].Value
    if ($reportedMode -cne $threadMode) {
        throw "Harness reported thread mode '$reportedMode', expected '$threadMode'."
    }
    if ($SplitThreads.IsPresent -and $windowThreadId -eq $renderThreadId) {
        throw 'Split-thread invariant failed: HWND and HGLRC used the same thread.'
    }
    if (-not $SplitThreads.IsPresent -and $windowThreadId -ne $renderThreadId) {
        throw 'Unified-thread invariant failed: HWND and HGLRC used different threads.'
    }
    $handleValue = [Int64]::Parse(
        $readyMatch.Groups['hwnd'].Value,
        [System.Globalization.NumberStyles]::AllowHexSpecifier,
        [System.Globalization.CultureInfo]::InvariantCulture)
    $windowHandle = [IntPtr]::new($handleValue)
    if (-not [McOverlayOpenGlSmokeNative]::IsWindow($windowHandle)) {
        throw 'The private WGL HWND was not valid.'
    }

    $writer.WriteLine('STATE 1 1')
    $stateAppliedSeen = $false
    $rendererReadySeen = $false
    while (-not ($stateAppliedSeen -and $rendererReadySeen)) {
        $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for the first ImGui frame' `
            -Received $received
        if ($line -ceq 'STATE_APPLIED 1 1') {
            $stateAppliedSeen = $true
        }
        elseif ($line -ceq 'RENDERER_READY OpenGL') {
            $rendererReadySeen = $true
        }
    }

    # Exercise the complete persisted feature-state grammar: fifteen flags,
    # defense/threat radii, hold key, card opacity, and three packed 0xRRGGBB
    # values. The synthetic JVM
    # has no Minecraft classes, but the renderer/control protocol must still
    # accept and acknowledge the settings atomically.
    $writer.WriteLine('FEATURE_STATE 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 10 32 164 78 16777215 0 1644065')
    do {
        $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'applying the complete feature snapshot' `
            -Received $received
    } while ($line -cne 'FEATURE_STATE_APPLIED')

    # The synthetic JVM intentionally has no Minecraft classes, so telemetry
    # must still arrive with valid=0. This checks that mapping failure never
    # suppresses the controller dashboard protocol or blocks the render hook.
    $gameStateLine = $received |
        Where-Object { $_.StartsWith('GAME_STATE ', [System.StringComparison]::Ordinal) } |
        Select-Object -First 1
    while ($null -eq $gameStateLine) {
        $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for GAME_STATE telemetry' `
            -Received $received
        if ($line.StartsWith('GAME_STATE ', [System.StringComparison]::Ordinal)) {
            $gameStateLine = $line
        }
    }
    $gameStateFields = $gameStateLine.Split(
        [char[]] @(' '), [System.StringSplitOptions]::RemoveEmptyEntries)
    if ($gameStateFields.Count -ne 15 -or
        $gameStateFields[0] -cne 'GAME_STATE' -or
        $gameStateFields[1] -cne '1' -or
        $gameStateFields[4] -cne '0') {
        throw "Invalid unsupported-mapping telemetry frame: '$gameStateLine'."
    }

    if (-not $SplitThreads.IsPresent) {
        # Only the configured Click-GUI bind is handled by the installed ImGui
        # WndProc bridge. INSERT and right Shift have no implicit behavior.
        if (-not [McOverlayOpenGlSmokeNative]::PostVirtualKey(
                $windowHandle, 0xDE, 0x28)) {
            throw 'Could not post single quote to close the private Click GUI.'
        }
        do {
            $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
                -LimitSeconds $TimeoutSeconds -Operation 'waiting for the quote close result' `
                -Received $received
        } while ($line -cne 'STATE_CHANGED 1 0')

        if (-not [McOverlayOpenGlSmokeNative]::PostVirtualKey(
                $windowHandle, 0xDE, 0x28)) {
            throw 'Could not post single quote to open the private Click GUI.'
        }
        do {
            $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
                -LimitSeconds $TimeoutSeconds -Operation 'waiting for the quote open result' `
                -Received $received
        } while ($line -cne 'STATE_CHANGED 1 1')
    }
    else {
        # PostMessage cannot drive a GetAsyncKeyState fallback because it does
        # not modify global keyboard state. Avoid SendInput in an automated
        # test (it would affect the user's active desktop); exercise equivalent
        # visibility/input state transitions over the authenticated pipe.
        $writer.WriteLine('STATE 0 0')
        do {
            $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
                -LimitSeconds $TimeoutSeconds -Operation 'hiding the split-thread renderer' `
                -Received $received
        } while ($line -cne 'STATE_APPLIED 0 0')
    }

    $writer.WriteLine('STATE 1 1')
    do {
        $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'restoring visible interactive state' `
            -Received $received
    } while ($line -cne 'STATE_APPLIED 1 1')

    # Keep SwapBuffers running while DETACH is in flight. The render callback
    # owns ImGui/OpenGL shutdown, WndProc restoration, callback rundown, and
    # only then allows DETACH_COMPLETE to cross the pipe.
    $writer.WriteLine('DETACH')
    $detachSent = $true
    while (-not $detachComplete) {
        $line = Read-ProtocolLine -Reader $reader -Stopwatch $protocolWatch `
            -LimitSeconds $TimeoutSeconds -Operation 'waiting for DETACH drain' `
            -Received $received
        if ($line -ceq 'DETACH_COMPLETE') {
            $detachComplete = $true
        }
    }

    if (-not [McOverlayOpenGlSmokeNative]::PostMessageW(
            $windowHandle, [McOverlayOpenGlSmokeNative]::WmClose,
            [UIntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Could not close the private WGL window after detach.'
    }

    $exitWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $remaining = Get-RemainingMilliseconds -Stopwatch $exitWatch `
        -LimitSeconds 15 -Operation 'waiting for the private harness to exit'
    if (-not $targetProcess.WaitForExit($remaining)) {
        throw 'Private OpenGL JVM harness did not exit after WM_CLOSE.'
    }
    $targetProcess.WaitForExit()
    $stderr = $targetErrorTask.GetAwaiter().GetResult()
    $stdoutTail = $targetProcess.StandardOutput.ReadToEnd()
    if ($targetProcess.ExitCode -ne 0) {
        throw "Private OpenGL JVM harness exited with code $($targetProcess.ExitCode). stdout: $($stdoutTail.Trim()) stderr: $($stderr.Trim())"
    }

    if ($SplitThreads.IsPresent) {
        Write-Host 'PASS: private split-thread JVM + WGL rendered ImGui and drained detach.'
    }
    else {
        Write-Host 'PASS: private JVM + WGL rendered ImGui, handled WndProc input, and drained detach.'
    }
    Write-Host "Target PID: $($targetProcess.Id) (owned smoke harness; no external PID accepted)"
    Write-Host "Threads: HWND=$windowThreadId HGLRC=$renderThreadId mode=$threadMode"
    Write-Host "Protocol: $([string]::Join(' | ', $received))"
}
catch {
    $failure = $_
}
finally {
    if ($null -ne $writer -and -not $detachSent) {
        try {
            $writer.WriteLine('DETACH')
            $detachSent = $true
        }
        catch { }
    }

    if ($windowHandle -ne [IntPtr]::Zero -and
        [McOverlayOpenGlSmokeNative]::IsWindow($windowHandle)) {
        try {
            [void] [McOverlayOpenGlSmokeNative]::PostMessageW(
                $windowHandle, [McOverlayOpenGlSmokeNative]::WmClose,
                [UIntPtr]::Zero, [IntPtr]::Zero)
        }
        catch { }
    }

    foreach ($resource in @($writer, $reader, $pipe)) {
        if ($null -ne $resource) {
            try { $resource.Dispose() } catch { }
        }
    }

    if ($null -ne $targetProcess) {
        try {
            if (-not $targetProcess.HasExited -and
                -not $targetProcess.WaitForExit(5000)) {
                # This can terminate only the Process object created above;
                # the script never accepts or discovers an external PID.
                $targetProcess.Kill()
                [void] $targetProcess.WaitForExit(5000)
            }
            if ($targetProcess.HasExited) {
                $diagnosticExitCode = $targetProcess.ExitCode
            }
            if ($null -ne $targetErrorTask -and $targetErrorTask.IsCompleted) {
                $diagnosticStderr = $targetErrorTask.GetAwaiter().GetResult().Trim()
            }
        }
        catch { }
        finally { $targetProcess.Dispose() }
    }

    [void] [McOverlayOpenGlSmokeNative]::SetErrorMode($previousErrorMode)
}

if ($null -ne $failure) {
    $diagnostic = if ($null -ne $diagnosticExitCode) {
        " Harness exit: $diagnosticExitCode."
    } else {
        ''
    }
    if (-not [string]::IsNullOrWhiteSpace($diagnosticStderr)) {
        $diagnostic += " Harness stderr: $diagnosticStderr"
    }
    [Console]::Error.WriteLine("FAIL: $($failure.Exception.Message)$diagnostic")
    exit 1
}

exit 0
