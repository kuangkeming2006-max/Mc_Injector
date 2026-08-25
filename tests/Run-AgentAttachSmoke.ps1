[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $AgentDll,

    [Parameter(Mandatory = $true)]
    [string] $AttachHelperJar,

    [string] $JavaHome = $env:JAVA_HOME,

    [ValidateRange(5, 120)]
    [int] $TimeoutSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertTo-NativeArgument {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string] $Value
    )

    # ProcessStartInfo.Arguments is a Windows command line, not a shell. Apply
    # the CommandLineToArgvW quoting rules so paths with spaces and trailing
    # backslashes reach java.exe unchanged on Windows PowerShell 5.1 as well as
    # PowerShell 7.
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
            # Backslashes immediately before a quote are doubled, then one
            # additional slash escapes the quote itself.
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

    # A closing quote would otherwise consume trailing backslashes.
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
        # Flush redirected asynchronous stream notifications before reading the
        # task results on older .NET Framework builds.
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

function Get-JavaMajorVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string] $JavaExecutable
    )

    $version = Invoke-CapturedProcess -Executable $JavaExecutable -ArgumentList @('-version') `
        -TimeoutMilliseconds 10000
    if ($version.ExitCode -ne 0) {
        throw "java -version failed with exit code $($version.ExitCode): $($version.Stderr.Trim())"
    }

    $text = "$($version.Stdout)`n$($version.Stderr)"
    $match = [regex]::Match($text, 'version\s+"(?<value>[^"]+)"')
    if (-not $match.Success) {
        throw "Could not parse the Java version from: $($text.Trim())"
    }

    $value = $match.Groups['value'].Value
    if ($value.StartsWith('1.')) {
        $component = $value.Split('.')[1]
    }
    else {
        $component = $value.Split('.')[0]
    }

    $major = 0
    if (-not [int]::TryParse($component, [ref] $major) -or $major -lt 8) {
        throw "Unsupported Java version '$value'; JDK 8 or newer is required."
    }
    return $major
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
$helperPath = $null
$javaExecutable = $null
$javacExecutable = $null
$temporaryDirectory = $null
$pipe = $null
$reader = $null
$writer = $null
$targetProcess = $null
$targetErrorTask = $null
$helperProcess = $null
$helperOutputTask = $null
$helperErrorTask = $null
$detachSent = $false
$detachComplete = $false
$failure = $null

try {
    if ($env:OS -ne 'Windows_NT') {
        throw 'This smoke test is Windows-only.'
    }

    $agentPath = Resolve-FilePath -Path $AgentDll -Description 'Agent DLL'
    $helperPath = Resolve-FilePath -Path $AttachHelperJar -Description 'Attach helper JAR'
    if ([System.IO.Path]::GetExtension($agentPath) -ine '.dll') {
        throw "Agent path must end in .dll: '$agentPath'."
    }
    if ([System.IO.Path]::GetExtension($helperPath) -ine '.jar') {
        throw "Attach helper path must end in .jar: '$helperPath'."
    }

    $javaExecutable = Resolve-JdkTool -Name 'java' -ConfiguredJavaHome $JavaHome
    $javacExecutable = Resolve-JdkTool -Name 'javac' -ConfiguredJavaHome $JavaHome
    $jdkRoot = [System.IO.Directory]::GetParent(
        [System.IO.Path]::GetDirectoryName($javacExecutable)).FullName
    $javaMajor = Get-JavaMajorVersion -JavaExecutable $javaExecutable

    $temporaryDirectory = Join-Path -Path ([System.IO.Path]::GetTempPath()) `
        -ChildPath ("McOverlayAgentSmoke-{0}" -f [guid]::NewGuid().ToString('N'))
    $classesDirectory = Join-Path -Path $temporaryDirectory -ChildPath 'classes'
    [void] [System.IO.Directory]::CreateDirectory($classesDirectory)

    $targetSource = Join-Path -Path $PSScriptRoot -ChildPath 'AttachSmokeTarget.java'
    $compile = Invoke-CapturedProcess -Executable $javacExecutable -ArgumentList @(
        '-encoding', 'UTF-8', '-d', $classesDirectory, $targetSource
    ) -TimeoutMilliseconds 30000
    if ($compile.ExitCode -ne 0) {
        throw "javac failed with exit code $($compile.ExitCode): $($compile.Stderr.Trim())"
    }

    # The script never accepts an external target PID. This is the only JVM to
    # which the helper is allowed to attach, and the Process object is retained
    # for deterministic cleanup in finally.
    $targetProcess = Start-NativeProcess -Executable $javaExecutable -ArgumentList @(
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
    $pipeName = "McOverlayAgentSmoke-$PID-$([guid]::NewGuid().ToString('N'))"
    $fullPipeName = "\\.\pipe\$pipeName"
    $agentOptions = "pipe=$fullPipeName;token=$token;protocol=1"

    # The server is created before loadAgentPath so the agent can connect as
    # soon as Agent_OnAttach schedules its runtime thread.
    $pipe = [System.IO.Pipes.NamedPipeServerStream]::new(
        $pipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        1,
        [System.IO.Pipes.PipeTransmissionMode]::Byte,
        [System.IO.Pipes.PipeOptions]::Asynchronous,
        4096,
        4096)
    $connectionTask = $pipe.WaitForConnectionAsync()

    if ($javaMajor -ge 9) {
        $helperArguments = @('--add-modules', 'jdk.attach', '-jar', $helperPath)
    }
    else {
        $toolsJar = Join-Path -Path $jdkRoot -ChildPath 'lib\tools.jar'
        if (-not [System.IO.File]::Exists($toolsJar)) {
            throw "Java 8 Attach API library was not found: '$toolsJar'."
        }
        $helperClassPath = "$helperPath$([System.IO.Path]::PathSeparator)$toolsJar"
        $helperArguments = @(
            '-cp', $helperClassPath, 'com.mcoverlay.attach.AttachHelper'
        )
    }
    $helperArguments += @(
        $targetProcess.Id.ToString(), $agentPath, $agentOptions
    )

    $helperProcess = Start-NativeProcess -Executable $javaExecutable `
        -ArgumentList $helperArguments
    $helperOutputTask = $helperProcess.StandardOutput.ReadToEndAsync()
    $helperErrorTask = $helperProcess.StandardError.ReadToEndAsync()

    $protocolWatch = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $connectionTask.IsCompleted) {
        if ($helperProcess.HasExited -and $helperProcess.ExitCode -ne 0) {
            $helperProcess.WaitForExit()
            $earlyStdout = $helperOutputTask.GetAwaiter().GetResult().Trim()
            $earlyStderr = $helperErrorTask.GetAwaiter().GetResult().Trim()
            throw "AttachHelper exited with code $($helperProcess.ExitCode) before the agent connected. stdout: $earlyStdout stderr: $earlyStderr"
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
        # STATUS is informational. RENDERER_READY is deliberately not required:
        # this private target never creates an HWND, HGLRC, or presents a frame.
    }

    $remaining = Get-RemainingMilliseconds -Stopwatch $protocolWatch `
        -LimitSeconds $TimeoutSeconds -Operation 'waiting for AttachHelper to exit'
    if (-not $helperProcess.WaitForExit($remaining)) {
        throw 'AttachHelper did not exit after loading the agent.'
    }
    $helperProcess.WaitForExit()
    $helperStdout = $helperOutputTask.GetAwaiter().GetResult()
    $helperStderr = $helperErrorTask.GetAwaiter().GetResult()
    if ($helperProcess.ExitCode -ne 0) {
        throw "AttachHelper exited with code $($helperProcess.ExitCode). stdout: $($helperStdout.Trim()) stderr: $($helperStderr.Trim())"
    }

    # Exercise both controller commands. DETACH stops hooks and IPC but, by
    # design, does not attempt unsafe FreeLibrary from the target process.
    $writer.WriteLine('STATE 0 0')
    $writer.WriteLine('DETACH')
    $detachSent = $true

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

    Write-Host "PASS: AttachHelper exit 0; HELLO, HOOK_READY, and DETACH_COMPLETE received."
    Write-Host "Target PID: $($targetProcess.Id)"
    Write-Host "Protocol: $([string]::Join(' | ', $received))"
}
catch {
    $failure = $_
}
finally {
    if ($null -ne $writer -and -not $detachSent) {
        try {
            $writer.WriteLine('DETACH')
        }
        catch {
            # The pipe may already be broken; target-process cleanup below is
            # still deterministic.
        }
    }

    if ($null -ne $helperProcess) {
        try {
            if (-not $helperProcess.HasExited) {
                $helperProcess.Kill()
                [void] $helperProcess.WaitForExit(5000)
            }
        }
        catch { }
        finally { $helperProcess.Dispose() }
    }

    if ($null -ne $writer -and -not $detachComplete) {
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

    # Delete only the GUID-named directory created under the current temporary
    # root. This guard prevents a malformed variable from widening cleanup.
    if (-not [string]::IsNullOrWhiteSpace($temporaryDirectory)) {
        $expectedParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
        $actualParent = [System.IO.Path]::GetDirectoryName(
            [System.IO.Path]::GetFullPath($temporaryDirectory)).TrimEnd('\')
        $leaf = [System.IO.Path]::GetFileName($temporaryDirectory)
        if ($actualParent -ieq $expectedParent -and
            $leaf -cmatch '^McOverlayAgentSmoke-[0-9a-f]{32}$' -and
            [System.IO.Directory]::Exists($temporaryDirectory)) {
            Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
        }
    }
}

if ($null -ne $failure) {
    [Console]::Error.WriteLine("FAIL: $($failure.Exception.Message)")
    exit 1
}

exit 0
