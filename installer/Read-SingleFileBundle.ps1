# Read the .NET bundle manifest without running the application or requiring a loose runtimeconfig.json.
# Format: https://github.com/dotnet/runtime/blob/v10.0.0/src/installer/managed/Microsoft.NET.HostModel/Bundle/Manifest.cs
function Read-SingleFileBundle {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Executable)

    $stream = [IO.File]::OpenRead($Executable)
    $reader = [IO.BinaryReader]::new($stream, [Text.Encoding]::UTF8)
    try {
        $prefix = $reader.ReadBytes([int][Math]::Min($stream.Length, 16MB))
        $encoding = [Text.Encoding]::GetEncoding(28591)
        $signature = [byte[]]@(0x8b,0x12,0x02,0xb9,0x6a,0x61,0x20,0x38,0x72,0x7b,0x93,0x02,0x14,0xd7,0xa0,0x32,0x13,0xf5,0xb9,0xe6,0xef,0xae,0x33,0x18,0xee,0x3b,0x2d,0xce,0x24,0xb3,0x6a,0xae)
        $marker = $encoding.GetString($prefix).IndexOf($encoding.GetString($signature), [StringComparison]::Ordinal)
        if ($marker -lt 8) { throw 'A .NET single-file publish is required. Rebuild with PublishSingleFile=true.' }
        $offset = [BitConverter]::ToInt64($prefix, $marker - 8)
        if ($offset -le 0 -or $offset -gt $stream.Length - 64) { throw 'The executable has no valid .NET bundle manifest.' }
        $stream.Position = $offset
        $major = $reader.ReadUInt32()
        $minor = $reader.ReadUInt32()
        $count = $reader.ReadInt32()
        if ($major -notin @(2,6) -or $minor -ne 0 -or $count -le 0 -or $count -gt 100000) { throw "Unsupported .NET bundle manifest: $major.$minor." }
        $bundleId = $reader.ReadString()
        [void]$reader.ReadInt64() # deps.json offset
        [void]$reader.ReadInt64() # deps.json length
        $configOffset = $reader.ReadInt64()
        $configLength = $reader.ReadInt64()
        [void]$reader.ReadUInt64() # compatibility flags
        $files = @{}
        for ($index = 0; $index -lt $count; $index++) {
            $fileOffset = $reader.ReadInt64()
            $size = $reader.ReadInt64()
            $compressed = if ($major -ge 6) { $reader.ReadInt64() } else { 0L }
            $type = $reader.ReadByte()
            $name = $reader.ReadString()
            $storedSize = if ($compressed -gt 0) { $compressed } else { $size }
            if ($fileOffset -lt 0 -or $storedSize -lt 0 -or $fileOffset -gt $stream.Length - $storedSize) { throw "Invalid bundled file: $name" }
            $files[$name] = [pscustomobject]@{ Type = $type; Size = $size; CompressedSize = $compressed }
        }
        if ($configOffset -le 0 -or $configLength -le 0 -or $configLength -gt 1MB -or $configOffset -gt $stream.Length - $configLength) { throw 'The bundle has no valid runtime configuration.' }
        $stream.Position = $configOffset
        $config = [Text.Encoding]::UTF8.GetString($reader.ReadBytes([int]$configLength)) | ConvertFrom-Json
        $options = $config.runtimeOptions
        $selfContained = $options.PSObject.Properties.Name -contains 'includedFrameworks'
        [pscustomobject]@{ BundleId = $bundleId; Framework = $options.tfm; SelfContained = $selfContained; Files = $files }
    } finally { $reader.Dispose(); $stream.Dispose() }
}
