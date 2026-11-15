# Generates demo_emblem.png (64x64 RGBA shield) used by the uidemo scene to exercise PNG loading.
# Original art drawn with System.Drawing primitives; re-run to regenerate:
#   powershell -NoProfile -File assets_src/ui/make_demo_emblem.ps1
Add-Type -AssemblyName System.Drawing

$size = 64
$bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::Transparent)

# Shield outline: flat top, curved sides meeting in a point at the bottom.
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$path.AddLine(8, 6, 56, 6)
$path.AddBezier(56, 6, 56, 34, 50, 48, 32, 60)
$path.AddBezier(32, 60, 14, 48, 8, 34, 8, 6)
$path.CloseFigure()

$fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Point 0, 6), (New-Object System.Drawing.Point 0, 60), ([System.Drawing.Color]::FromArgb(255, 40, 70, 120)), ([System.Drawing.Color]::FromArgb(255, 14, 24, 48))
$g.FillPath($fill, $path)
$gold = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 222, 180, 80)), 3
$g.DrawPath($gold, $path)

# Chevron and a small diamond in gold.
$chev = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 240, 205, 110)), 5
$chev.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$chev.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$chev.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
$g.DrawLines($chev, [System.Drawing.Point[]]@((New-Object System.Drawing.Point 19, 22), (New-Object System.Drawing.Point 32, 38), (New-Object System.Drawing.Point 45, 22)))
$diamond = [System.Drawing.Point[]]@((New-Object System.Drawing.Point 32, 42), (New-Object System.Drawing.Point 36, 47), (New-Object System.Drawing.Point 32, 52), (New-Object System.Drawing.Point 28, 47))
$g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 240, 205, 110))), $diamond)

$g.Dispose()
$out = Join-Path $PSScriptRoot 'demo_emblem.png'
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "wrote $out"
