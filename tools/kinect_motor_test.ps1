<#
.SYNOPSIS
  Drives the Kinect v1 tilt motor through a list of angles and prints, for
  each command, what the runtime's own API answers and what the head's
  accelerometer measures while the motor is supposed to be moving.

.DESCRIPTION
  Standalone: talks to Kinect10.dll (the Kinect for Windows 1.8 runtime)
  directly, no xenia involved. Close xenia first; the runtime lets one
  process own the sensor.

  Public Windows API references and the per-sensor runtime checks are listed
  in docs/nui.md under "Requirements and backend selection" and "Where the
  angle comes from: the sensor's accelerometer". Microsoft's method reference
  documents the accelerometer signature and units, not the vtable slot numbers.

  For every commanded angle the script prints:
    * the HRESULT and the wall time of NuiCameraElevationSetAngle (the
      runtime blocks while the motor runs, so a healthy move costs ~1 s);
    * the angle NuiCameraElevationGetAngle reports afterwards;
    * the accelerometer's elevation sampled every 250 ms through the settle
      window, so even a twitch shows up as a change in the trace.

  The summary compares accepted targets with settled accelerometer readings
  within 3 degrees. It needs at least two accepted targets more than 6 degrees
  apart, three valid final samples per target, and no rejected commands or
  failed readings. Otherwise it reports insufficient evidence. A mismatch
  describes observed tracking, not a proven motor, gear or power-supply fault.

  Watch and listen while it runs: a healthy motor whines for about a second
  per command and the head visibly nods. Then compare with the printed trace.

.PARAMETER Angles
  The angles to command, in order, degrees (-27..27). Default sweeps
  0, +15, -15, +27, -27 and back to 0.
.PARAMETER SettleSeconds
  Seconds to sample the accelerometer after each command (default 5).
.PARAMETER Watch
  Seconds to only watch the accelerometer, commanding nothing. Use it to see
  whether the reading follows the head when you tilt it gently by hand.
.PARAMETER NoCommands
  Skip the command sweep (read status only, plus -Watch if given).
.PARAMETER LogDir
  Where the transcript goes (default: the TEMP directory); the path is
  printed first thing.

.EXAMPLE
  .\kinect_motor_test.ps1
  .\kinect_motor_test.ps1 -Angles 0,27,-27,0 -SettleSeconds 6
  .\kinect_motor_test.ps1 -NoCommands -Watch 20
#>
param(
  [int[]] $Angles = @(0, 15, -15, 27, -27, 0),
  [double] $SettleSeconds = 5,
  [int] $Watch = 0,
  [switch] $NoCommands,
  [string] $LogDir = $env:TEMP
)

$ErrorActionPreference = 'Stop'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$logPath = Join-Path $LogDir ("kinect_motor_test_" + $stamp + ".log")

$xenia = Get-Process xenia_canary -ErrorAction SilentlyContinue
if ($xenia) {
  Write-Host "xenia_canary is running (pid $($xenia.Id -join ', ')); close it first, the runtime lets one process own the sensor." -ForegroundColor Yellow
  exit 2
}

$csharp = @'
using System;
using System.Runtime.InteropServices;

public static class KinectMotor {
  [DllImport("Kinect10.dll")] public static extern int NuiGetSensorCount(out int count);
  [DllImport("Kinect10.dll")] public static extern int NuiInitialize(uint flags);
  [DllImport("Kinect10.dll")] public static extern void NuiShutdown();
  [DllImport("Kinect10.dll")] public static extern int NuiCameraElevationGetAngle(out int degrees);
  [DllImport("Kinect10.dll")] public static extern int NuiCameraElevationSetAngle(int degrees);
  [DllImport("Kinect10.dll")] public static extern int NuiCreateSensorByIndex(int index, out IntPtr sensor);

  // The per-sensor interface's table of functions. IUnknown holds slots 0-2;
  // the 1.8 runtime puts NuiInstanceIndex at 22, NuiStatus at 26,
  // NuiInitializationFlags at 27 and NuiAccelerometerGetCurrentReading at 33
  // (the same numbers xenia's host declaration uses; see docs/nui.md for
  // the public Windows API references and the runtime checks).
  const int kSlotRelease = 2;
  const int kSlotInstanceIndex = 22;
  const int kSlotStatus = 26;
  const int kSlotInitializationFlags = 27;
  const int kSlotAccelerometer = 33;

  [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int IntThis(IntPtr self);
  [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate uint UIntThis(IntPtr self);
  [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int HrThisPtr(IntPtr self, IntPtr arg);

  static IntPtr Slot(IntPtr obj, int slot) {
    IntPtr vtable = Marshal.ReadIntPtr(obj);
    return Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
  }

  public static int InstanceIndex(IntPtr sensor) {
    var f = (IntThis)Marshal.GetDelegateForFunctionPointer(Slot(sensor, kSlotInstanceIndex), typeof(IntThis));
    return f(sensor);
  }
  public static uint InitializationFlags(IntPtr sensor) {
    var f = (UIntThis)Marshal.GetDelegateForFunctionPointer(Slot(sensor, kSlotInitializationFlags), typeof(UIntThis));
    return f(sensor);
  }
  public static int Status(IntPtr sensor) {
    var f = (IntThis)Marshal.GetDelegateForFunctionPointer(Slot(sensor, kSlotStatus), typeof(IntThis));
    return f(sensor);
  }
  public static uint Release(IntPtr sensor) {
    var f = (UIntThis)Marshal.GetDelegateForFunctionPointer(Slot(sensor, kSlotRelease), typeof(UIntThis));
    return f(sensor);
  }
  // Returns the HRESULT; xyz receives the gravity vector in g on the sensor's
  // axes (x to its left, y up, z the way it looks); a level head reads (0,-1,0).
  public static int Accelerometer(IntPtr sensor, float[] xyz) {
    var f = (HrThisPtr)Marshal.GetDelegateForFunctionPointer(Slot(sensor, kSlotAccelerometer), typeof(HrThisPtr));
    IntPtr buf = Marshal.AllocHGlobal(16);
    try {
      int hr = f(sensor, buf);
      float[] v = new float[4];
      Marshal.Copy(buf, v, 0, 4);
      xyz[0] = v[0]; xyz[1] = v[1]; xyz[2] = v[2];
      return hr;
    } finally {
      Marshal.FreeHGlobal(buf);
    }
  }
  // The elevation the accelerometer implies, degrees, positive = head tilted
  // up, the convention NuiCameraElevationSetAngle takes (xenia's formula).
  public static double Elevation(float[] xyz) {
    return Math.Atan2(-xyz[2], -xyz[1]) * 180.0 / Math.PI;
  }
}
'@
Add-Type -TypeDefinition $csharp -Language CSharp

function Hr([int] $hr) { '0x{0:X8}' -f $hr }

function Read-Accel([IntPtr] $sensor) {
  $v = New-Object float[] 3
  $hr = [KinectMotor]::Accelerometer($sensor, $v)
  if ($hr -ne 0) { return @{ ok = $false; hr = $hr } }
  return @{ ok = $true; hr = 0; x = $v[0]; y = $v[1]; z = $v[2]; deg = [KinectMotor]::Elevation($v) }
}

function Format-Accel($a) {
  if (-not $a.ok) { return "accelerometer failed " + (Hr $a.hr) }
  return ('({0,6:F3}, {1,6:F3}, {2,6:F3}) g -> {3,6:F1} deg' -f $a.x, $a.y, $a.z, $a.deg)
}

function Get-MotorSweepAssessment($rows) {
  # Three degrees allows measurement noise; separated tolerance bands are
  # needed to distinguish following commands from a stationary head.
  $tolerance = 3.0
  if (@($rows).Count -lt 2) {
    return @{ Status = 'Inconclusive'; Message = 'insufficient sweep: need at least two usable targets.' }
  }
  foreach ($row in $rows) {
    if (-not $row.SetAccepted) {
      return @{ Status = 'Inconclusive'; Message = 'insufficient evidence: a command was rejected; inspect its HRESULT.' }
    }
    if ($null -eq $row.AccelAfter -or [double]::IsNaN($row.AccelAfter) -or
        [double]::IsInfinity($row.AccelAfter) -or $row.SampleCount -lt 3 -or
        $row.FailedSamples -gt 0 -or $null -eq $row.TailMin -or
        $null -eq $row.TailMax -or ($row.TailMax - $row.TailMin) -gt $tolerance -or
        [math]::Abs($row.AccelAfter - $row.TailMin) -gt $tolerance -or
        [math]::Abs($row.AccelAfter - $row.TailMax) -gt $tolerance) {
      return @{ Status = 'Inconclusive'; Message = 'insufficient evidence: missing, failed, too few or unsettled accelerometer readings.' }
    }
  }
  $targets = $rows | Measure-Object -Property Commanded -Minimum -Maximum
  if (($targets.Maximum - $targets.Minimum) -le (2 * $tolerance)) {
    return @{ Status = 'Inconclusive'; Message = 'insufficient sweep: accepted targets must be more than 6 degrees apart.' }
  }
  $misses = @($rows | Where-Object {
    [math]::Abs($_.AccelAfter - $_.Commanded) -gt $tolerance
  })
  if ($misses.Count -eq 0) {
    return @{ Status = 'Tracked'; Message = 'settled accelerometer readings followed every accepted target within 3 degrees.' }
  }
  return @{ Status = 'Mismatch'; Message = ('settled accelerometer readings differed from {0} of {1} accepted targets by more than 3 degrees; this does not establish a hardware cause.' -f $misses.Count, @($rows).Count) }
}

function Sample-Accel([IntPtr] $sensor, [double] $seconds, [string] $label) {
  # Samples every 250 ms and prints a compact trace of the implied elevation.
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $trace = @()
  $vals = @()
  $failed = 0
  while ($sw.Elapsed.TotalSeconds -lt $seconds) {
    $a = Read-Accel $sensor
    if ($a.ok -and -not [double]::IsNaN($a.deg) -and -not [double]::IsInfinity($a.deg)) {
      $trace += ('{0:F1}' -f $a.deg)
      $vals += $a.deg
    } else {
      $failed++
      $trace += $(if ($a.ok) { 'INVALID' } else { 'ERR' + (Hr $a.hr) })
    }
    Start-Sleep -Milliseconds 250
  }
  Write-Host ("    accel trace {0} (every 250 ms): {1}" -f $label, ($trace -join ' '))
  if ($vals.Count -gt 0) {
    $m = $vals | Measure-Object -Minimum -Maximum
    $tail = @($vals | Select-Object -Last 3) | Measure-Object -Minimum -Maximum
    return @{ min = $m.Minimum; max = $m.Maximum; last = $vals[-1]; first = $vals[0]
      count = $vals.Count; failed = $failed; tailMin = $tail.Minimum; tailMax = $tail.Maximum }
  }
  return $null
}

Start-Transcript -Path $logPath | Out-Null
try {
  Write-Host "Kinect tilt motor test, $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'); transcript: $logPath"
  Write-Host ""

  $count = 0
  $hr = [KinectMotor]::NuiGetSensorCount([ref] $count)
  Write-Host ("NuiGetSensorCount: {0} sensor(s), hr {1}" -f $count, (Hr $hr))
  if ($hr -ne 0 -or $count -lt 1) { Write-Host "No sensor; is it plugged in (USB and the 12 V adapter)?" -ForegroundColor Yellow; exit 3 }

  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $hr = [KinectMotor]::NuiInitialize(0x20)   # depth only, the flags xenia uses
  Write-Host ("NuiInitialize(0x20): hr {0} in {1} ms" -f (Hr $hr), $sw.ElapsedMilliseconds)
  if ($hr -ne 0) { Write-Host "The runtime refused to open the sensor (another process holding it, or not powered?)" -ForegroundColor Yellow; exit 4 }

  $sensor = [IntPtr]::Zero
  $hr = [KinectMotor]::NuiCreateSensorByIndex(0, [ref] $sensor)
  Write-Host ("NuiCreateSensorByIndex(0): hr {0}, object {1}" -f (Hr $hr), ('0x{0:X}' -f $sensor.ToInt64()))
  if ($hr -ne 0 -or $sensor -eq [IntPtr]::Zero) { Write-Host "No per-sensor interface; the accelerometer cannot be read." -ForegroundColor Yellow; [KinectMotor]::NuiShutdown(); exit 5 }

  $idx = [KinectMotor]::InstanceIndex($sensor)
  $flags = [KinectMotor]::InitializationFlags($sensor)
  $status = [KinectMotor]::Status($sensor)
  $selfCheck = if ($idx -eq 0 -and $flags -eq 0x20) { 'PASS' } else { 'FAIL, the slot numbers do not fit this runtime' }
  Write-Host ("  NuiInstanceIndex {0} (expect 0), NuiInitializationFlags 0x{1:X} (expect 0x20): self-check {2}" -f $idx, $flags, $selfCheck)
  $statusText = if ($status -eq 0) { 'S_OK, the runtime calls the sensor ready' } else { 'not ready; an E_NUI_NOTPOWERED here would mean the 12 V adapter is missing' }
  Write-Host ("  NuiStatus: {0} ({1})" -f (Hr $status), $statusText)

  $apiAngle = 0
  $hr = [KinectMotor]::NuiCameraElevationGetAngle([ref] $apiAngle)
  Write-Host ("  NuiCameraElevationGetAngle at start: {0} deg, hr {1}" -f $apiAngle, (Hr $hr))
  $a0 = Read-Accel $sensor
  Write-Host ("  accelerometer at start: {0}" -f (Format-Accel $a0))
  Write-Host ""

  if ($Watch -gt 0) {
    Write-Host ("WATCH: {0} s of accelerometer readings, no motor commands. Tilt the head gently by hand if you want to see whether the reading follows it." -f $Watch)
    $null = Sample-Accel $sensor $Watch 'watch'
    Write-Host ""
  }

  $rows = @()
  if (-not $NoCommands) {
    Write-Host ("SWEEP: {0}; {1} s of accelerometer samples after each command. Watch the head and listen for the motor." -f ($Angles -join ', '), $SettleSeconds)
    foreach ($target in $Angles) {
      $before = Read-Accel $sensor
      $beforeText = if ($before.ok) { ('{0:F1} deg' -f $before.deg) } else { '?' }
      Write-Host ("-> NuiCameraElevationSetAngle({0})  [head now reads {1}]" -f $target, $beforeText)
      $sw.Restart()
      $hrSet = [KinectMotor]::NuiCameraElevationSetAngle($target)
      $ms = $sw.ElapsedMilliseconds
      $verdict = if ($hrSet -eq 0) { '(accepted)' } elseif ($hrSet -eq -2147023659) { '(ERROR_RETRY: the driver rate limit; wait 20 s)' } else { '(refused)' }
      Write-Host ("    returned {0} after {1} ms {2}" -f (Hr $hrSet), $ms, $verdict)
      $s = Sample-Accel $sensor $SettleSeconds ("after {0}" -f $target)
      $after = Read-Accel $sensor
      $apiAfter = 0
      $hrGet = [KinectMotor]::NuiCameraElevationGetAngle([ref] $apiAfter)
      $moved = if ($before.ok -and $after.ok) { $after.deg - $before.deg } else { [double]::NaN }
      $afterText = if ($after.ok) { ('{0:F1}' -f $after.deg) } else { '?' }
      $movedText = if ([double]::IsNaN($moved)) { '?' } else { ('{0:+0.0;-0.0;0.0} deg' -f $moved) }
      Write-Host ("    API angle after: {0} deg (hr {1}); accelerometer {2} -> {3}, change {4}" -f $apiAfter, (Hr $hrGet), $beforeText, $afterText, $movedText)
      $rows += [pscustomobject]@{
        Commanded = $target; SetHr = (Hr $hrSet); SetMs = $ms; ApiAfter = $apiAfter
        SetAccepted = ($hrSet -eq 0)
        SampleCount = $(if ($s) { $s.count } else { 0 })
        FailedSamples = $(if ($s) { $s.failed } else { 0 })
        TailMin = $(if ($s) { $s.tailMin } else { $null })
        TailMax = $(if ($s) { $s.tailMax } else { $null })
        AccelBefore = $(if ($before.ok) { [math]::Round($before.deg, 1) } else { $null })
        AccelAfter = $(if ($after.ok) { $after.deg } else { $null })
        Change = $(if ([double]::IsNaN($moved)) { $null } else { [math]::Round($moved, 1) })
        TraceMin = $(if ($s) { [math]::Round($s.min, 1) } else { $null })
        TraceMax = $(if ($s) { [math]::Round($s.max, 1) } else { $null })
      }
    }
    Write-Host ""
    Write-Host "SUMMARY (degrees, + = head up; target agreement tolerance 3 degrees; SetMs is timing only):"
    $rows | Select-Object Commanded, SetHr, SetMs, ApiAfter, AccelBefore,
      AccelAfter, Change, TraceMin, TraceMax, SampleCount, FailedSamples |
      Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    $span = ($rows | Where-Object { $_.AccelAfter -ne $null -and
      -not [double]::IsNaN($_.AccelAfter) -and
      -not [double]::IsInfinity($_.AccelAfter) } |
      Measure-Object -Property AccelAfter -Minimum -Maximum)
    if ($span.Count -gt 0) {
      $range = $span.Maximum - $span.Minimum
      Write-Host ("Across all available final readings the elevation ranged {0:F1} .. {1:F1} deg ({2:F1} deg of travel)." -f $span.Minimum, $span.Maximum, $range)
    }
    $assessment = Get-MotorSweepAssessment $rows
    $color = if ($assessment.Status -eq 'Tracked') { 'Green' } else { 'Yellow' }
    Write-Host ("OBSERVATION: " + $assessment.Message) -ForegroundColor $color
  }

  [void][KinectMotor]::Release($sensor)
  [KinectMotor]::NuiShutdown()
  Write-Host "Closed the sensor. Transcript: $logPath"
} finally {
  Stop-Transcript | Out-Null
}
