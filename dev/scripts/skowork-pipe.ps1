# Calls a Skowork local API method over the named pipe (JSON-RPC 2.0, 4-byte little-endian length-prefixed).
#   ./skowork-pipe.ps1 -Method app.status [-Params '{"minutes":15}'] [-PipeName Skowork-<SID>]
param(
    [Parameter(Mandatory)] [string] $Method,
    [string] $Params = '{}',
    [string] $PipeName = "Skowork-$([System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value)",
    [int] $TimeoutMs = 5000
)
$ErrorActionPreference = 'Stop'
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect($TimeoutMs)
try {
    $req = '{"jsonrpc":"2.0","id":1,"method":"' + $Method + '","params":' + $Params + '}'
    $body = [System.Text.Encoding]::UTF8.GetBytes($req)
    $pipe.Write([BitConverter]::GetBytes([uint32]$body.Length), 0, 4)
    $pipe.Write($body, 0, $body.Length)
    $pipe.Flush()
    $len = New-Object byte[] 4
    $read = 0
    while ($read -lt 4) { $n = $pipe.Read($len, $read, 4 - $read); if ($n -le 0) { throw 'pipe closed' }; $read += $n }
    $size = [BitConverter]::ToUInt32($len, 0)
    $buf = New-Object byte[] $size
    $read = 0
    while ($read -lt $size) { $n = $pipe.Read($buf, $read, $size - $read); if ($n -le 0) { throw 'pipe closed' }; $read += $n }
    [System.Text.Encoding]::UTF8.GetString($buf)
} finally {
    $pipe.Dispose()
}
