$ErrorActionPreference = "Stop"
$clang = "C:\Program Files\Windhawk\Compiler\bin\clang++.exe"
$src = Join-Path $PSScriptRoot "PhotoWidget.cpp"
$exe = Join-Path $PSScriptRoot "PhotoWidget.exe"
$ico = Join-Path $PSScriptRoot "foto.ico"

& $clang `
    -std=c++23 -target x86_64-w64-mingw32 -O2 -s -Wall -Wextra `
    -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 `
    -mwindows -municode -fuse-ld=lld -static-libstdc++ -static-libgcc `
    $src -o $exe `
    -ld2d1 -lwindowscodecs -lcomdlg32 -lshlwapi -lshell32 -lole32 -luser32 -lgdi32 -ladvapi32 -luuid
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (Test-Path $ico) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class IconEmbedder {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr BeginUpdateResource(string pFileName, bool bDeleteExistingResources);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool UpdateResource(IntPtr hUpdate, IntPtr lpType, IntPtr lpName, ushort wLanguage, byte[] lpData, uint cbData);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool EndUpdateResource(IntPtr hUpdate, bool fDiscard);
    public static void Embed(string exePath, string icoPath) {
        byte[] ico = File.ReadAllBytes(icoPath);
        int count = BitConverter.ToUInt16(ico, 4);
        byte[] grp = new byte[6 + count * 14];
        Buffer.BlockCopy(ico, 0, grp, 0, 6);
        IntPtr h = BeginUpdateResource(exePath, false);
        try {
            for (int i = 0; i < count; i++) {
                int srcOff = 6 + i * 16, dstOff = 6 + i * 14;
                Buffer.BlockCopy(ico, srcOff, grp, dstOff, 12);
                ushort id = (ushort)(i + 1);
                grp[dstOff + 12] = (byte)(id & 0xFF); grp[dstOff + 13] = (byte)(id >> 8);
                uint imgSize = BitConverter.ToUInt32(ico, srcOff + 8), imgOff = BitConverter.ToUInt32(ico, srcOff + 12);
                byte[] img = new byte[imgSize];
                Buffer.BlockCopy(ico, (int)imgOff, img, 0, (int)imgSize);
                UpdateResource(h, (IntPtr)3, (IntPtr)id, 0, img, imgSize);
            }
            UpdateResource(h, (IntPtr)14, (IntPtr)1, 0, grp, (uint)grp.Length);
        } finally { EndUpdateResource(h, false); }
    }
}
"@
    [IconEmbedder]::Embed($exe, $ico)
}
Write-Host "Built: $exe"
