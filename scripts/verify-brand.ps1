param([ValidateSet('Debug','Release')][string]$Configuration='Debug')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root "out/vs2026-qt611-$($Configuration.ToLower())_bin/SignalStudio.exe"
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BrandPe {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibraryEx(string p,IntPtr h,uint f);
 [DllImport("kernel32.dll")] public static extern IntPtr FindResource(IntPtr h,IntPtr n,IntPtr t);
 [DllImport("kernel32.dll")] public static extern uint SizeofResource(IntPtr h,IntPtr r);
 [DllImport("kernel32.dll")] public static extern IntPtr LoadResource(IntPtr h,IntPtr r);
 [DllImport("kernel32.dll")] public static extern IntPtr LockResource(IntPtr r);
 [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr h);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindResource(IntPtr h,string n,IntPtr t);
}
'@
$ico=[IO.File]::ReadAllBytes((Join-Path $root 'resources/branding/qt/windows/signal_studio.ico'))
$count=[BitConverter]::ToUInt16($ico,4)
$layers=@();for($i=0;$i -lt $count;$i++){$v=[int]$ico[6+16*$i];if($v -eq 0){$v=256};$layers+=$v}
$handle=[BrandPe]::LoadLibraryEx($exe,[IntPtr]::Zero,2)
if($handle -eq [IntPtr]::Zero){throw 'Cannot load PE resources'}
try {
 $resource=[BrandPe]::FindResource($handle,'IDI_APP_ICON',[IntPtr]14)
 if($resource -eq [IntPtr]::Zero){throw 'Embedded GROUP_ICON missing'}
 $size=[BrandPe]::SizeofResource($handle,$resource)
 $ptr=[BrandPe]::LockResource([BrandPe]::LoadResource($handle,$resource))
 $bytes=New-Object byte[] $size;[Runtime.InteropServices.Marshal]::Copy($ptr,$bytes,0,$size)
 $embedded=@();for($i=0;$i -lt [BitConverter]::ToUInt16($bytes,4);$i++){$v=[int]$bytes[6+14*$i];if($v -eq 0){$v=256};$embedded+=$v}
 foreach($n in @(16,24,32,48,64,128,256)){if($layers -notcontains $n -or $embedded -notcontains $n){throw "Missing ICO layer $n"}}
 foreach($file in Get-ChildItem -LiteralPath (Join-Path $root 'resources/branding/svg') -Filter '*.svg') {
  [xml]$svg=Get-Content -LiteralPath $file.FullName -Raw
  if($svg.SelectNodes("//*[local-name()='script' or local-name()='image' or local-name()='text']").Count){throw "External/unoutlined SVG $($file.Name)"}
 }
 [pscustomobject]@{pass=$true;exe=$exe;icoLayers=$layers;embeddedLayers=$embedded;svgCount=15;resourceLibrary='SignalStudioBrand';configuration=$Configuration}|ConvertTo-Json
} finally {[BrandPe]::FreeLibrary($handle)|Out-Null}
