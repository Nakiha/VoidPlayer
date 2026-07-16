[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,

    [string]$RequiredNames = "",

    [string]$InvokeName = "",

    [string]$InvokeNameBase64 = "",

    [ValidateRange(250, 30000)]
    [int]$TimeoutMs = 5000
)

$ErrorActionPreference = "Stop"
$utf8 = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName Accessibility
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class VoidPlayerWindowTree {
    private delegate bool EnumWindowProc(IntPtr hwnd, IntPtr lparam);

    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(
        IntPtr parent, EnumWindowProc callback, IntPtr lparam);

    public static IntPtr[] Descendants(IntPtr parent) {
        var handles = new List<IntPtr>();
        EnumChildWindows(parent, (hwnd, lparam) => {
            handles.Add(hwnd);
            return true;
        }, IntPtr.Zero);
        return handles.ToArray();
    }
}

public static class VoidPlayerAccessibleObject {
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(
        IntPtr hwnd,
        int objectId,
        ref Guid interfaceId,
        [MarshalAs(UnmanagedType.Interface)] out object accessible);
}
"@

function Add-AccessibleNames {
    param(
        [object]$Accessible,
        [System.Collections.Generic.List[string]]$Names,
        [ref]$NodeCount,
        [int]$Depth = 0
    )

    if ($null -eq $Accessible -or $Depth -gt 64 -or $NodeCount.Value -gt 5000 -or
        -not [string]::IsNullOrEmpty($script:invokedName)) {
        return
    }
    $NodeCount.Value++
    try {
        $name = $Accessible.accName(0)
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            $Names.Add($name)
            if ($script:invokeNames.Count -gt 0 -and $script:invokeNames -contains $name) {
                $Accessible.accDoDefaultAction(0)
                $script:invokedName = $name
                $script:invokePattern = "IAccessible.accDoDefaultAction"
                return
            }
        }
    } catch {
    }

    try {
        $childCount = $Accessible.accChildCount
    } catch {
        return
    }
    for ($childId = 1; $childId -le $childCount; $childId++) {
        try {
            $child = $Accessible.accChild($childId)
            if ($null -ne $child -and $child -isnot [int]) {
                Add-AccessibleNames -Accessible $child -Names $Names -NodeCount $NodeCount -Depth ($Depth + 1)
                continue
            }
            $name = $Accessible.accName($childId)
            if (-not [string]::IsNullOrWhiteSpace($name)) {
                $Names.Add($name)
                if ($script:invokeNames.Count -gt 0 -and $script:invokeNames -contains $name) {
                    $Accessible.accDoDefaultAction($childId)
                    $script:invokedName = $name
                    $script:invokePattern = "IAccessible.accDoDefaultAction"
                    return
                }
            }
            $NodeCount.Value++
        } catch {
            # Nodes can be replaced while hover overlays are changing.
        }
    }
}

$requiredGroups = @(
    $RequiredNames.Split('|', [System.StringSplitOptions]::RemoveEmptyEntries) |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_.Length -gt 0 }
)
$decodedInvokeName = if ([string]::IsNullOrWhiteSpace($InvokeNameBase64)) {
    $InvokeName
} else {
    [System.Text.Encoding]::UTF8.GetString(
        [System.Convert]::FromBase64String($InvokeNameBase64)
    )
}
$invokeNames = @(
    $decodedInvokeName.Split(';', [System.StringSplitOptions]::RemoveEmptyEntries) |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_.Length -gt 0 }
)
$script:invokeNames = $invokeNames
$script:invokedName = ""
$script:invokePattern = ""
$deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
$lastNames = @()
$lastError = $null
$lastDiagnostics = @()

while ([DateTime]::UtcNow -lt $deadline) {
    try {
        $process = Get-Process -Id $TargetProcessId -ErrorAction Stop
        $process.Refresh()
        $handle = $process.MainWindowHandle
        if ($handle -eq [IntPtr]::Zero) {
            throw "Process $TargetProcessId does not have a main window yet"
        }

        $names = [System.Collections.Generic.List[string]]::new()
        $diagnostics = [System.Collections.Generic.List[string]]::new()
        $nodeCount = 0
        $handles = @([IntPtr]$handle) + @(
            [VoidPlayerWindowTree]::Descendants([IntPtr]$handle)
        )
        foreach ($candidateHandle in $handles) {
            try {
                $accessibleObject = $null
                $iidAccessible = [Guid]"618736e0-3c3d-11cf-810c-00aa00389b71"
                $hr = [VoidPlayerAccessibleObject]::AccessibleObjectFromWindow(
                    $candidateHandle,
                    -4,
                    [ref]$iidAccessible,
                    [ref]$accessibleObject
                )
                $diagnostics.Add("MSAA hwnd=$candidateHandle hr=$hr object=$($null -ne $accessibleObject)")
                if ($hr -ge 0 -and $null -ne $accessibleObject) {
                    $unknown = [Runtime.InteropServices.Marshal]::GetIUnknownForObject(
                        $accessibleObject
                    )
                    try {
                        $typedAccessible = [Runtime.InteropServices.Marshal]::GetTypedObjectForIUnknown(
                            $unknown,
                            [Accessibility.IAccessible]
                        )
                        Add-AccessibleNames -Accessible $typedAccessible -Names $names -NodeCount ([ref]$nodeCount)
                        if (-not [string]::IsNullOrEmpty($script:invokedName)) {
                            [pscustomobject]@{
                                processId = $TargetProcessId
                                invoked = $script:invokedName
                                pattern = $script:invokePattern
                            } | ConvertTo-Json -Compress
                            exit 0
                        }
                    } finally {
                        [void][Runtime.InteropServices.Marshal]::Release($unknown)
                    }
                }
            } catch {
                $diagnostics.Add("MSAA hwnd=$candidateHandle error=$($_.Exception.Message)")
                # Non-Flutter child windows do not need an MSAA provider.
            }
            try {
                $root = [System.Windows.Automation.AutomationElement]::FromHandle(
                    $candidateHandle
                )
                $elements = $root.FindAll(
                    [System.Windows.Automation.TreeScope]::Subtree,
                    [System.Windows.Automation.Condition]::TrueCondition
                )
                $nodeCount += $elements.Count
                for ($i = 0; $i -lt $elements.Count; $i++) {
                    try {
                        $element = $elements.Item($i)
                        $name = $element.Current.Name
                        if (-not [string]::IsNullOrWhiteSpace($name)) {
                            $names.Add($name)
                        }
                        if ($invokeNames.Count -gt 0 -and $invokeNames -contains $name) {
                            $pattern = $null
                            if ($element.TryGetCurrentPattern(
                                [System.Windows.Automation.InvokePattern]::Pattern,
                                [ref]$pattern
                            )) {
                                ([System.Windows.Automation.InvokePattern]$pattern).Invoke()
                                [pscustomobject]@{
                                    processId = $TargetProcessId
                                    invoked = $name
                                    pattern = "InvokePattern"
                                } | ConvertTo-Json -Compress
                                exit 0
                            }
                            if ($element.TryGetCurrentPattern(
                                [System.Windows.Automation.LegacyIAccessiblePattern]::Pattern,
                                [ref]$pattern
                            )) {
                                ([System.Windows.Automation.LegacyIAccessiblePattern]$pattern).DoDefaultAction()
                                [pscustomobject]@{
                                    processId = $TargetProcessId
                                    invoked = $name
                                    pattern = "LegacyIAccessiblePattern"
                                } | ConvertTo-Json -Compress
                                exit 0
                            }
                            $diagnostics.Add("UIA name=$name has no invokable pattern")
                        }
                    } catch {
                        # A hover popup can disappear while UIA is reading it.
                    }
                }
            } catch {
                $diagnostics.Add("UIA hwnd=$candidateHandle error=$($_.Exception.Message)")
                # Some implementation-only child HWNDs do not expose UIA.
            }
        }

        $lastNames = @($names | Sort-Object -Unique)
        $lastDiagnostics = @($diagnostics)
        $missing = @(
            $requiredGroups | Where-Object {
                $alternatives = $_.Split(
                    ';',
                    [System.StringSplitOptions]::RemoveEmptyEntries
                )
                -not @(
                    $alternatives | Where-Object { $lastNames -contains $_ }
                )
            }
        )
        if ($missing.Count -eq 0 -and $invokeNames.Count -eq 0) {
            [pscustomobject]@{
                processId = $TargetProcessId
                nodeCount = $nodeCount
                names = $lastNames
            } | ConvertTo-Json -Compress
            exit 0
        }
        $lastError = if ($invokeNames.Count -gt 0) {
            "unable to invoke AXTree name: $($invokeNames -join '; ')"
        } else {
            "missing AXTree names: $($missing -join ', ')"
        }
    } catch {
        $lastError = $_.Exception.Message
    }

    Start-Sleep -Milliseconds 100
}

$sample = @($lastNames | Select-Object -First 80) -join ', '
$diagnosticText = $lastDiagnostics -join '; '
Write-Error "$lastError. Observed names: $sample. Diagnostics: $diagnosticText"
exit 2
