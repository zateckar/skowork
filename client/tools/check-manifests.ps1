param([Parameter(Mandatory)] [string] $BuildDir)

$ErrorActionPreference = 'Stop'
$objdump = Get-Command objdump -CommandType Application -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SkoworkManifestResource {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool FreeLibrary(IntPtr module);
}
'@

foreach ($name in 'Skowork.exe', 'skowork-worker.exe') {
    $path = (Resolve-Path -LiteralPath (Join-Path $BuildDir "bin/$name")).Path
    $details = (& $objdump -x $path) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect resources in $name" }
    $table = [regex]::Match($details, '(?s)Entry: ID: 0x000018,.*?Name Table: [^\n]*IDs: (\d+)')
    if (-not $table.Success -or $table.Groups[1].Value -ne '1') { throw "$name must have exactly one manifest resource" }

    # LOAD_LIBRARY_AS_DATAFILE reads resources without executing the program.
    $module = [SkoworkManifestResource]::LoadLibraryEx($path, [IntPtr]::Zero, 2)
    if ($module -eq [IntPtr]::Zero) { throw "Could not load resources from $name" }
    try {
        $resource = [SkoworkManifestResource]::FindResource($module, [IntPtr]1, [IntPtr]24)
        if ($resource -eq [IntPtr]::Zero) { throw "Missing manifest 1 in $name" }
        $size = [SkoworkManifestResource]::SizeofResource($module, $resource)
        $loaded = [SkoworkManifestResource]::LoadResource($module, $resource)
        $data = [SkoworkManifestResource]::LockResource($loaded)
        if ($size -eq 0 -or $data -eq [IntPtr]::Zero) { throw "Could not read manifest 1 in $name" }
        $bytes = New-Object byte[] $size
        [Runtime.InteropServices.Marshal]::Copy($data, $bytes, 0, $size)
        $manifest = [Text.Encoding]::UTF8.GetString($bytes)
        if ($manifest -notmatch '<activeCodePage[^>]*>UTF-8</activeCodePage>' -or
            $manifest -notmatch '<longPathAware[^>]*>true</longPathAware>') {
            throw "$name is missing its UTF-8 or long-path manifest setting"
        }
        if ($name -eq 'Skowork.exe' -and $manifest -notmatch '<dpiAwareness[^>]*>PerMonitorV2</dpiAwareness>') {
            throw "$name is missing its DPI manifest setting"
        }
        Write-Host "$name has one manifest with the expected Windows settings."
    } finally {
        [void][SkoworkManifestResource]::FreeLibrary($module)
    }
}
