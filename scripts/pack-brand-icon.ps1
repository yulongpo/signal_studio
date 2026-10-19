$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$destination=Join-Path $root 'resources/branding/qt/windows/signal_studio.ico'
$source=Join-Path $root 'resources/branding/png/icon'
# Package original raster layers without resampling or changing the artwork.
$sizes=@(16,24,32,48,64,128,256)
$stream=[IO.MemoryStream]::new();$writer=[IO.BinaryWriter]::new($stream)
try {
 $writer.Write([UInt16]0);$writer.Write([UInt16]1);$writer.Write([UInt16]$sizes.Count)
 $offset=[UInt32](6+16*$sizes.Count);$images=@()
 foreach($n in $sizes){
  $image=[IO.File]::ReadAllBytes((Join-Path $source "signal_studio_icon_$n.png"));$images+=,$image
  $dimension=if($n -eq 256){0}else{$n}
  $writer.Write([byte]$dimension);$writer.Write([byte]$dimension);$writer.Write([byte]0);$writer.Write([byte]0)
  $writer.Write([UInt16]1);$writer.Write([UInt16]32);$writer.Write([UInt32]$image.Length);$writer.Write($offset)
  $offset+=[UInt32]$image.Length
 }
 foreach($image in $images){$writer.Write([byte[]]$image)}
 $writer.Flush();[IO.File]::WriteAllBytes($destination,$stream.ToArray())
} finally {$writer.Dispose();$stream.Dispose()}
