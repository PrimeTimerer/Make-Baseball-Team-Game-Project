// Korean textures for YKTK3 (PCSX2 texture replacement PNGs: RGBA with A = 0x80 opaque, as the GS reads them).
//   TexTool sheet <dir with PNGs> <out.png> [columns] [cell]   thumbnails with numbers (+ <out>.txt: number, name, size)
//   TexTool render <spec.json> <dumps dir> <replacements dir>   draw the Korean text of each spec entry over its dump
// Build: csc -nologo -optimize+ -r:System.Drawing.dll -r:System.Web.Extensions.dll -out:TexTool.exe TexTool.cs
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Linq;
using System.Web.Script.Serialization;

static class TexTool
{
    static int Main(string[] args)
    {
        if (args.Length >= 3 && args[0] == "sheet")
            return Sheet(args[1], args[2], args.Length > 3 ? int.Parse(args[3]) : 8, args.Length > 4 ? int.Parse(args[4]) : 160);
        if (args.Length >= 4 && args[0] == "render")
            return Render(args[1], args[2], args[3]);
        if (args.Length >= 8 && args[0] == "crop")
        {
            // crop <png> <out.png> x y w h zoom: a part of a texture, enlarged, on grey (for looking at small glyphs)
            using (var raw = new Bitmap(args[1]))
            using (var img = Visible(raw))
            {
                int x = int.Parse(args[3]), y = int.Parse(args[4]), w = int.Parse(args[5]), h = int.Parse(args[6]), z = int.Parse(args[7]);
                using (var view = new Bitmap(w * z, h * z, PixelFormat.Format32bppArgb))
                using (var g = Graphics.FromImage(view))
                {
                    g.Clear(Color.FromArgb(60, 60, 70));
                    g.InterpolationMode = InterpolationMode.NearestNeighbor;
                    g.PixelOffsetMode = PixelOffsetMode.Half;
                    g.DrawImage(img, new Rectangle(0, 0, w * z, h * z), new Rectangle(x, y, w, h), GraphicsUnit.Pixel);
                    view.Save(args[2], ImageFormat.Png);
                }
            }
            return 0;
        }
        if (args.Length >= 3 && args[0] == "boxes")
            return Boxes(args[1], args[2], args.Length > 3 ? int.Parse(args[3]) : 3, args.Length > 4 ? int.Parse(args[4]) : 3,
                         args.Length > 8 ? new Rectangle(int.Parse(args[5]), int.Parse(args[6]), int.Parse(args[7]), int.Parse(args[8]))
                                         : (Rectangle?)null, args.Length > 9 ? int.Parse(args[9]) : 96);
        Console.Error.WriteLine("usage: TexTool sheet <dir> <out.png> [columns] [cell] | render <spec.json> <dumps> <out dir>");
        return 2;
    }

    // GS alpha (0x80 = opaque) to screen alpha, for looking at a dump.
    static Bitmap Visible(Bitmap src)
    {
        var dst = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb);
        for (int y = 0; y < src.Height; y++)
            for (int x = 0; x < src.Width; x++)
            {
                Color c = src.GetPixel(x, y);
                dst.SetPixel(x, y, Color.FromArgb(Math.Min(255, c.A * 2), c.R, c.G, c.B));
            }
        return dst;
    }

    static int Sheet(string dir, string outPath, int columns, int cell)
    {
        string[] files = Directory.GetFiles(dir, "*.png").OrderBy(f => Path.GetFileName(f), StringComparer.Ordinal).ToArray();
        int rows = (files.Length + columns - 1) / columns;
        int label = 14;
        using (var sheet = new Bitmap(columns * cell, Math.Max(1, rows) * (cell + label), PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(sheet))
        using (var font = new Font("Consolas", 9f))
        using (var index = new StreamWriter(outPath + ".txt"))
        {
            g.Clear(Color.FromArgb(70, 70, 80));
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            for (int i = 0; i < files.Length; i++)
            {
                using (var raw = new Bitmap(files[i]))
                using (var img = Visible(raw))
                {
                    int x = (i % columns) * cell, y = (i / columns) * (cell + label);
                    float scale = Math.Min((float)(cell - 4) / img.Width, (float)(cell - 4) / img.Height);
                    if (scale > 4f) scale = 4f;
                    int w = Math.Max(1, (int)(img.Width * scale)), h = Math.Max(1, (int)(img.Height * scale));
                    g.DrawImage(img, x + 2, y + label + 2, w, h);
                    g.DrawString(i + " " + img.Width + "x" + img.Height, font, Brushes.Yellow, x + 2, y);
                    index.WriteLine("{0}\t{1}\t{2}x{3}", i, Path.GetFileName(files[i]), img.Width, img.Height);
                }
            }
            sheet.Save(outPath, ImageFormat.Png);
        }
        Console.WriteLine("{0} textures -> {1}", files.Length, outPath);
        return 0;
    }

    // Spec: [{"file": "<dump name>.png", "same": ["<other dump>.png", ...] (variants with the same layout, e.g. another
    //   palette), "erase": [[x, y, w, h], ...] (default: each text box grown by 1), "fill": "#rrggbb" (optional, else each
    //   erase box is filled from its left and right edges), "text": [{"box": [x, y, w, h], "text": "...",
    //   "color": "#rrggbb" (default: the light ink of the box), "outline": "#rrggbb" (default: its dark ink when it has
    //   two), "outline_width": px, "bold": true, "align": "center"|"left"|"right", "spread": true (one syllable per equal
    //   cell), "font": "Malgun Gothic", "scale_x": 1.0, "height": 0.0-1.0 of the box}]}]
    static int Render(string specPath, string dumps, string outDir)
    {
        var json = new JavaScriptSerializer();
        var spec = json.Deserialize<List<Dictionary<string, object>>>(File.ReadAllText(specPath));
        Directory.CreateDirectory(outDir);
        int done = 0;
        var jobs = new List<KeyValuePair<string, Dictionary<string, object>>>();
        foreach (var entry in spec)
        {
            jobs.Add(new KeyValuePair<string, Dictionary<string, object>>((string)entry["file"], entry));
            if (entry.ContainsKey("same"))
                foreach (object other in (System.Collections.ArrayList)entry["same"])
                    jobs.Add(new KeyValuePair<string, Dictionary<string, object>>((string)other, entry));
        }
        foreach (var job in jobs)
        {
            string name = job.Key;
            var entry = job.Value;
            string src = Path.Combine(dumps, name);
            if (!File.Exists(src))
            {
                Console.Error.WriteLine("missing dump: " + name);
                continue;
            }
            using (var img = new Bitmap(src))
            using (var canvas = new Bitmap(img.Width, img.Height, PixelFormat.Format32bppArgb))
            {
                // Work in screen alpha (x2), back to GS alpha at the end.
                for (int y = 0; y < img.Height; y++)
                    for (int x = 0; x < img.Width; x++)
                    {
                        Color c = img.GetPixel(x, y);
                        canvas.SetPixel(x, y, Color.FromArgb(Math.Min(255, c.A * 2), c.R, c.G, c.B));
                    }
                var items = entry.ContainsKey("text")
                    ? ((System.Collections.ArrayList)entry["text"]).Cast<Dictionary<string, object>>().ToList()
                    : new List<Dictionary<string, object>>();
                var styles = items.Select(t => Style(canvas, Box(t["box"]), t)).ToList(); // from the original ink
                if (entry.ContainsKey("erase"))
                    foreach (Rectangle r in ((System.Collections.ArrayList)entry["erase"]).Cast<object>().Select(Box))
                        Erase(canvas, r, entry.ContainsKey("fill") ? Parse((string)entry["fill"]) : (Color?)null);
                else
                {
                    // each label: its own erase box (default: the text box grown by 1) and fill ("bg": the commonest
                    // colour of that box in the original, e.g. a tile or a circle behind the glyph)
                    var boxes = items.Select(t => { Rectangle r = t.ContainsKey("erase") ? Box(t["erase"]) : Box(t["box"]); if (!t.ContainsKey("erase")) r.Inflate(1, 1); return r; }).ToList();
                    var fills = items.Select((t, i) => !t.ContainsKey("fill") ? (Color?)null
                                                   : (string)t["fill"] == "bg" ? Common(canvas, boxes[i]) : Parse((string)t["fill"])).ToList();
                    for (int i = 0; i < items.Count; i++)
                        Erase(canvas, boxes[i], fills[i]);
                }
                for (int i = 0; i < items.Count; i++)
                    DrawText(canvas, items[i], styles[i]);
                using (var output = new Bitmap(img.Width, img.Height, PixelFormat.Format32bppArgb))
                {
                    for (int y = 0; y < img.Height; y++)
                        for (int x = 0; x < img.Width; x++)
                        {
                            Color c = canvas.GetPixel(x, y);
                            output.SetPixel(x, y, Color.FromArgb((c.A + 1) / 2, c.R, c.G, c.B));
                        }
                    output.Save(Path.Combine(outDir, name), ImageFormat.Png);
                }
            }
            done++;
        }
        Console.WriteLine("{0} textures -> {1}", done, outDir);
        return 0;
    }

    // Ink boxes: connected pixels with alpha, joined when closer than `gap` px side by side (a word, a label).
    // Writes <out>.png (the texture x`zoom` with numbered boxes) and prints "n x y w h". With a region (x y w h) only
    // pixels inside it count, and with a luminance limit (the 10th argument, below 96) dark ink on an opaque background
    // counts instead of alpha (paper).
    static int Boxes(string pngPath, string outPath, int gap, int zoom, Rectangle? region = null, int limit = 96)
    {
        using (var raw = new Bitmap(pngPath))
        using (var img = Visible(raw))
        {
            int w = img.Width, h = img.Height;
            var ink = new bool[w, h];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    Color c = img.GetPixel(x, y);
                    bool inside = !region.HasValue || region.Value.Contains(x, y);
                    ink[x, y] = inside && (limit >= 96 ? c.A >= 96 : c.A >= 96 && (c.R * 299 + c.G * 587 + c.B * 114) / 1000 < limit * 2);
                }
            var seen = new bool[w, h];
            var boxes = new List<Rectangle>();
            var stack = new Stack<Point>();
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    if (!ink[x, y] || seen[x, y]) continue;
                    int x0 = x, y0 = y, x1 = x, y1 = y;
                    stack.Push(new Point(x, y));
                    seen[x, y] = true;
                    while (stack.Count > 0)
                    {
                        Point p = stack.Pop();
                        x0 = Math.Min(x0, p.X); x1 = Math.Max(x1, p.X); y0 = Math.Min(y0, p.Y); y1 = Math.Max(y1, p.Y);
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++)
                            {
                                int nx = p.X + dx, ny = p.Y + dy;
                                if (nx >= 0 && ny >= 0 && nx < w && ny < h && ink[nx, ny] && !seen[nx, ny])
                                {
                                    seen[nx, ny] = true;
                                    stack.Push(new Point(nx, ny));
                                }
                            }
                    }
                    boxes.Add(Rectangle.FromLTRB(x0, y0, x1 + 1, y1 + 1));
                }
            // join boxes that touch after growing by `gap` horizontally and 1 px vertically, until nothing changes
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (int i = 0; i < boxes.Count && !merged; i++)
                    for (int j = i + 1; j < boxes.Count && !merged; j++)
                    {
                        Rectangle a = boxes[i], b = boxes[j];
                        var grown = Rectangle.FromLTRB(a.Left - gap, a.Top - 1, a.Right + gap, a.Bottom + 1);
                        bool overlapY = Math.Min(a.Bottom, b.Bottom) - Math.Max(a.Top, b.Top) > Math.Min(a.Height, b.Height) / 3;
                        if (grown.IntersectsWith(b) && (overlapY || a.IntersectsWith(b)))
                        {
                            boxes[i] = Rectangle.Union(a, b);
                            boxes.RemoveAt(j);
                            merged = true;
                        }
                    }
            }
            boxes = boxes.OrderBy(r => r.Top / 4).ThenBy(r => r.Left).ToList();
            using (var view = new Bitmap(w * zoom, h * zoom, PixelFormat.Format32bppArgb))
            using (var g = Graphics.FromImage(view))
            using (var font = new Font("Consolas", 8f, FontStyle.Bold))
            {
                g.Clear(Color.FromArgb(60, 60, 70));
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                g.DrawImage(img, 0, 0, w * zoom, h * zoom);
                for (int i = 0; i < boxes.Count; i++)
                {
                    Rectangle r = boxes[i];
                    g.DrawRectangle(Pens.Lime, r.X * zoom, r.Y * zoom, r.Width * zoom - 1, r.Height * zoom - 1);
                    g.DrawString(i.ToString(), font, Brushes.Magenta, r.X * zoom, Math.Max(0, r.Y * zoom - 11));
                    Console.WriteLine("{0} {1} {2} {3} {4}", i, r.X, r.Y, r.Width, r.Height);
                }
                view.Save(outPath, ImageFormat.Png);
            }
        }
        return 0;
    }

    static Rectangle Box(object o)
    {
        var a = (System.Collections.ArrayList)o;
        return new Rectangle(Convert.ToInt32(a[0]), Convert.ToInt32(a[1]), Convert.ToInt32(a[2]), Convert.ToInt32(a[3]));
    }

    static Color Parse(string hex)
    {
        hex = hex.TrimStart('#');
        int a = hex.Length == 8 ? Convert.ToInt32(hex.Substring(6, 2), 16) : 255;
        return Color.FromArgb(a, Convert.ToInt32(hex.Substring(0, 2), 16), Convert.ToInt32(hex.Substring(2, 2), 16),
                              Convert.ToInt32(hex.Substring(4, 2), 16));
    }

    // A flat colour, or each row blended from the pixel left of the box to the pixel right of it (gradients survive).
    static void Erase(Bitmap img, Rectangle r, Color? fill)
    {
        r.Intersect(new Rectangle(0, 0, img.Width, img.Height));
        for (int y = r.Top; y < r.Bottom; y++)
        {
            Color left = img.GetPixel(Math.Max(0, r.Left - 1), y), right = img.GetPixel(Math.Min(img.Width - 1, r.Right), y);
            for (int x = r.Left; x < r.Right; x++)
            {
                if (fill.HasValue)
                {
                    img.SetPixel(x, y, fill.Value);
                    continue;
                }
                float t = r.Width <= 1 ? 0f : (float)(x - r.Left) / (r.Width - 1);
                img.SetPixel(x, y, Color.FromArgb((int)(left.A + (right.A - left.A) * t), (int)(left.R + (right.R - left.R) * t),
                                                  (int)(left.G + (right.G - left.G) * t), (int)(left.B + (right.B - left.B) * t)));
            }
        }
    }

    static Color Common(Bitmap img, Rectangle r)
    {
        r.Intersect(new Rectangle(0, 0, img.Width, img.Height));
        var count = new Dictionary<int, int>();
        for (int y = r.Top; y < r.Bottom; y++)
            for (int x = r.Left; x < r.Right; x++)
            {
                int key = img.GetPixel(x, y).ToArgb();
                int n;
                count.TryGetValue(key, out n);
                count[key] = n + 1;
            }
        return count.Count == 0 ? Color.Transparent : Color.FromArgb(count.OrderByDescending(p => p.Value).First().Key);
    }

    // The colours of a label: given in the spec, or its light ink (fill) and dark ink (outline, when the two differ).
    static Color?[] Style(Bitmap img, Rectangle box, Dictionary<string, object> t)
    {
        Color? fill = t.ContainsKey("color") ? Parse((string)t["color"]) : (Color?)null;
        Color? outline = t.ContainsKey("outline") ? Parse((string)t["outline"]) : (Color?)null;
        if (fill.HasValue && (outline.HasValue || t.ContainsKey("color")))
            return new[] { fill, outline };
        var ink = new List<Color>();
        box.Intersect(new Rectangle(0, 0, img.Width, img.Height));
        for (int y = box.Top; y < box.Bottom; y++)
            for (int x = box.Left; x < box.Right; x++)
            {
                Color c = img.GetPixel(x, y);
                if (c.A >= 160) ink.Add(c);
            }
        if (ink.Count == 0)
            return new[] { fill ?? Color.White, outline };
        ink = ink.OrderBy(c => c.R * 299 + c.G * 587 + c.B * 114).ToList();
        int n = Math.Max(1, ink.Count / 4);
        Func<IEnumerable<Color>, Color> mean = cs =>
        {
            var l = cs.ToList();
            return Color.FromArgb(255, (int)l.Average(c => c.R), (int)l.Average(c => c.G), (int)l.Average(c => c.B));
        };
        Color light = mean(ink.Skip(ink.Count - n)), dark = mean(ink.Take(n));
        Func<Color, double> lum = c => c.R * 0.299 + c.G * 0.587 + c.B * 0.114;
        if (!fill.HasValue) fill = light;
        if (!outline.HasValue && !t.ContainsKey("no_outline") && lum(light) - lum(dark) > 60) outline = dark;
        return new[] { fill, outline };
    }

    static void DrawText(Bitmap img, Dictionary<string, object> t, Color?[] style)
    {
        Rectangle box = Box(t["box"]);
        string text = (string)t["text"];
        if (t.ContainsKey("spread") && (bool)t["spread"] && text.Length > 1)
        {
            // one syllable per equal cell across the box (labels like "人　事")
            string chars = text.Replace(" ", "");
            float cell = (float)box.Width / chars.Length;
            for (int i = 0; i < chars.Length; i++)
            {
                var one = new Dictionary<string, object>(t);
                one["spread"] = false;
                one["text"] = chars[i].ToString();
                one["box"] = new System.Collections.ArrayList { (int)(box.Left + i * cell), box.Top, (int)Math.Ceiling(cell), box.Height };
                DrawText(img, one, style);
            }
            return;
        }
        string family = t.ContainsKey("font") ? (string)t["font"] : "Malgun Gothic";
        bool bold = !t.ContainsKey("bold") || (bool)t["bold"];
        float scaleX = t.ContainsKey("scale_x") ? Convert.ToSingle(t["scale_x"]) : 1f;
        float height = t.ContainsKey("height") ? Convert.ToSingle(t["height"]) : 1f;
        string align = t.ContainsKey("align") ? (string)t["align"] : "center";
        using (var g = Graphics.FromImage(img))
        using (var path = new GraphicsPath())
        using (var family0 = new FontFamily(family))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            path.AddString(text, family0, (int)(bold ? FontStyle.Bold : FontStyle.Regular), 100f, new PointF(0, 0),
                           StringFormat.GenericTypographic);
            RectangleF b = path.GetBounds();
            if (b.Width <= 0)
                return;
            // Fit the ink to the box height (x height), scale_x, then shrink to the box width if needed.
            float pad = style[1].HasValue ? Convert.ToSingle(t.ContainsKey("outline_width") ? t["outline_width"] : 1.5) / 2f : 0f;
            float targetH = Math.Max(1f, box.Height * height - 2 * pad), targetW = Math.Max(1f, box.Width - 2 * pad);
            float sy = targetH / b.Height, sx = sy * scaleX;
            if (b.Width * sx > targetW) sx = targetW / b.Width;
            float w = b.Width * sx, h = b.Height * sy;
            float x0 = align == "left" ? box.Left + pad : align == "right" ? box.Right - pad - w : box.Left + (box.Width - w) / 2f;
            float y0 = box.Top + (box.Height - h) / 2f;
            using (var m = new Matrix())
            {
                m.Translate(x0, y0);
                m.Scale(sx, sy);
                m.Translate(-b.Left, -b.Top);
                path.Transform(m);
            }
            if (style[1].HasValue)
            {
                float width = Convert.ToSingle(t.ContainsKey("outline_width") ? t["outline_width"] : 1.5);
                using (var pen = new Pen(style[1].Value, width) { LineJoin = LineJoin.Round })
                    g.DrawPath(pen, path);
            }
            using (var brush = new SolidBrush(style[0] ?? Color.White))
                g.FillPath(brush, path);
        }
    }
}
