using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Text;

// Renders characters into 22x20 cells with 4 alpha levels (0..3), one byte per pixel.
// Like the game's kanji, the ink sits in columns 1..20 and rows 1..18 (1-pixel margins, so lines do not touch):
// the union of all outlines is scaled into that box, so every glyph keeps the same baseline and size.
// usage: HangulGlyphs <chars.txt (UTF-8)> <out.bin> [font] [ignored] [preview.png]
public static class HangulGlyphs
{
    const int CW = 22, CH = 20, S = 4;

    public static int Main(string[] args)
    {
        string text = File.ReadAllText(args[0], Encoding.UTF8).Trim();
        string fontName = args.Length > 2 ? args[2] : "Malgun Gothic";
        var family = new FontFamily(fontName);
        const float em = 100f;
        var paths = new GraphicsPath[text.Length];
        RectangleF union = RectangleF.Empty;
        for (int i = 0; i < text.Length; i++)
        {
            var p = new GraphicsPath();
            p.AddString(text[i].ToString(), family, (int)FontStyle.Bold, em, new PointF(0, 0), StringFormat.GenericTypographic);
            paths[i] = p;
            RectangleF b = p.GetBounds();
            if (b.Width <= 0 || b.Height <= 0)
                continue;
            union = union.IsEmpty ? b : RectangleF.Union(union, b);
        }
        // target ink box in the supersampled cell: x 1..21, y 1..19 (pixels), i.e. 20 x 18
        float sx = (20f * S) / union.Width, sy = (18f * S) / union.Height;
        var outb = new byte[text.Length * CW * CH];
        Bitmap preview = args.Length > 4 ? new Bitmap(CW * 32 * 2, CH * ((text.Length + 31) / 32) * 2) : null;
        for (int i = 0; i < text.Length; i++)
        {
            var bmp = new Bitmap(CW * S, CH * S);
            using (var g = Graphics.FromImage(bmp))
            {
                g.Clear(Color.Black);
                g.SmoothingMode = SmoothingMode.AntiAlias;
                var m = new Matrix();
                m.Translate(1f * S, 1f * S);
                m.Scale(sx, sy);
                m.Translate(-union.X, -union.Y);
                paths[i].Transform(m);
                g.FillPath(Brushes.White, paths[i]);
            }
            for (int y = 0; y < CH; y++)
                for (int x = 0; x < CW; x++)
                {
                    int sum = 0;
                    for (int dy = 0; dy < S; dy++)
                        for (int dx = 0; dx < S; dx++)
                            sum += bmp.GetPixel(x * S + dx, y * S + dy).R;
                    double v = sum / (S * S * 255.0);
                    byte lvl = (byte)(v < 0.12 ? 0 : v < 0.40 ? 1 : v < 0.70 ? 2 : 3);
                    outb[i * CW * CH + y * CW + x] = lvl;
                    if (preview != null)
                    {
                        int c = lvl * 85;
                        int px = (i % 32) * CW * 2 + x * 2, py = (i / 32) * CH * 2 + y * 2;
                        for (int k = 0; k < 4; k++) preview.SetPixel(px + (k & 1), py + (k >> 1), Color.FromArgb(c, c, c));
                    }
                }
            bmp.Dispose();
        }
        File.WriteAllBytes(args[1], outb);
        if (preview != null) preview.Save(args[4]);
        Console.WriteLine("glyphs " + text.Length + " font " + family.Name);
        return 0;
    }
}
