// Font page renderer for the XXL font upscale.
//   render.exe ink SPEC                       -> per entry: outline bounds (page px) of Arial Bold at the node's place
//   render.exe page SPEC CELLS PAGE.png S OUT.png
//       PAGE.png: the original 256 page; CELLS: "code x0 y0 x1 y1 text(0/1)" per glyph (page px, cell rects)
//       Output: page S times larger: icon cells upscaled from the original, text cells cleared and redrawn
//       from the Arial Bold outlines, green borders gone.
// SPEC lines: code<TAB>text<TAB>x<TAB>y<TAB>size<TAB>letterSpacingPercent (Figma text node: top-left, px)
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Text;

static class R {
	static FontFamily fam = new FontFamily("Arial");
	static CultureInfo ci = CultureInfo.InvariantCulture;
	public static double K = 0.9167;   // Figma: baseline = node y + K * size (Arial, auto line height)

	struct Entry { public int code; public string text; public float x, y, size, ls; }

	static List<Entry> ReadSpec(string path) {
		var l = new List<Entry>();
		foreach (var line in File.ReadAllLines(path, Encoding.UTF8)) {
			var p = line.Split('\t');
			if (p.Length < 6) continue;
			l.Add(new Entry { code = int.Parse(p[0]), text = p[1], x = float.Parse(p[2], ci), y = float.Parse(p[3], ci),
				size = float.Parse(p[4], ci), ls = float.Parse(p[5], ci) });
		}
		return l;
	}

	static float Advance(Graphics g, string ch, float size) {
		using (var f = new Font(fam, size, FontStyle.Bold, GraphicsUnit.Pixel))
			return g.MeasureString(ch, f, PointF.Empty, StringFormat.GenericTypographic).Width;
	}

	// The node's outline in page px.
	static GraphicsPath PathOf(Graphics g, Entry e) {
		var path = new GraphicsPath();
		float asc = e.size * fam.GetCellAscent(FontStyle.Bold) / fam.GetEmHeight(FontStyle.Bold);
		float baseY = (float)(e.y + K * e.size);
		float pen = e.x;
		var sf = StringFormat.GenericTypographic;
		for (int i = 0; i < e.text.Length; i++) {
			string ch = e.text.Substring(i, 1);
			path.AddString(ch, fam, (int)FontStyle.Bold, e.size, new PointF(pen, baseY - asc), sf);
			pen += Advance(g, ch, e.size) + e.ls / 100f * e.size;
		}
		return path;
	}

	static int Main(string[] a) {
		using (var scratch = new Bitmap(4, 4))
		using (var g = Graphics.FromImage(scratch)) {
			if (a[0] == "ink") {
				foreach (var e in ReadSpec(a[1])) {
					var b = PathOf(g, e).GetBounds();
					Console.WriteLine(string.Format(ci, "{0}\t{1:0.00}\t{2:0.00}\t{3:0.00}\t{4:0.00}", e.code, b.Left, b.Top, b.Right, b.Bottom));
				}
				return 0;
			}
			if (a[0] == "check") {
				// check SPEC PAGE.png S OUT.png: old page upscaled on grey, the new outlines dark on top;
				// prints per entry the share of the old glyph's ink (alpha >= 0.5) the new one does not cover.
				var spec = ReadSpec(a[1]);
				int S = int.Parse(a[3]);
				var src = new Bitmap(a[2]);
				int W = src.Width * S, H = src.Height * S;
				var old = new Bitmap(W, H, PixelFormat.Format32bppArgb);
				using (var go = Graphics.FromImage(old)) {
					go.InterpolationMode = InterpolationMode.HighQualityBilinear;
					go.PixelOffsetMode = PixelOffsetMode.Half;
					go.DrawImage(src, new Rectangle(0, 0, W, H));
				}
				var mask = new Bitmap(W, H, PixelFormat.Format32bppArgb);
				var outImg = new Bitmap(W, H, PixelFormat.Format32bppArgb);
				using (var gm = Graphics.FromImage(mask))
				using (var gv = Graphics.FromImage(outImg)) {
					gv.Clear(Color.FromArgb(255, 128, 128, 136));
					gv.DrawImage(old, 0, 0);
					foreach (var g2 in new[] { gm, gv }) { g2.SmoothingMode = SmoothingMode.AntiAlias; g2.ScaleTransform(S, S); }
					using (var dark = new SolidBrush(Color.FromArgb(235, 10, 10, 20)))
						foreach (var e in spec) { gm.FillPath(Brushes.White, PathOf(g, e)); gv.FillPath(dark, PathOf(g, e)); }
				}
				outImg.Save(a[4], ImageFormat.Png);
				foreach (var e in spec) {
					var b = PathOf(g, e).GetBounds();
					int x0 = Math.Max(0, (int)((b.Left - 2) * S)), x1 = Math.Min(W, (int)((b.Right + 2) * S));
					int y0 = Math.Max(0, (int)((b.Top - 2) * S)), y1 = Math.Min(H, (int)((b.Bottom + 2) * S));
					int ink = 0, bare = 0;
					for (int y = y0; y < y1; y++)
						for (int x = x0; x < x1; x++) {
							Color c = old.GetPixel(x, y);
							if (c.A < 128 || (c.G > 200 && c.R < 60)) continue;
							ink++;
							if (mask.GetPixel(x, y).A < 128) bare++;
						}
					Console.WriteLine(string.Format(ci, "{0}\t{1}\t{2:0.000}", e.code, ink, ink > 0 ? (double)bare / ink : 0));
				}
				return 0;
			}
			if (a[0] == "page") {
				var spec = ReadSpec(a[1]);
				int S = int.Parse(a[4]);
				var src = new Bitmap(a[3]);
				int W = src.Width * S, H = src.Height * S;
				var dst = new Bitmap(W, H, PixelFormat.Format32bppArgb);
				// icons: the original, upscaled (premultiplied-correct: white text never overlaps icons)
				var clean = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb);
				for (int y = 0; y < src.Height; y++)
					for (int x = 0; x < src.Width; x++) {
						Color c = src.GetPixel(x, y);
						bool green = c.R == 0 && c.G == 255 && c.B == 0 && c.A == 255;
						clean.SetPixel(x, y, green ? Color.FromArgb(0, 0, 0, 0) : c);
					}
				// only the icon cells kept: text cells, borders and corner marks cleared
				var keep = new bool[src.Width, src.Height];
				foreach (var line in File.ReadAllLines(a[2])) {
					var p = line.Split(' ');
					if (p.Length < 6 || p[5] != "0") continue;
					int x0 = (int)Math.Floor(float.Parse(p[1], ci)), y0 = (int)Math.Floor(float.Parse(p[2], ci));
					int x1 = (int)Math.Ceiling(float.Parse(p[3], ci)), y1 = (int)Math.Ceiling(float.Parse(p[4], ci));
					for (int y = Math.Max(0, y0); y < Math.Min(src.Height, y1); y++)
						for (int x = Math.Max(0, x0); x < Math.Min(src.Width, x1); x++)
							keep[x, y] = true;
				}
				for (int y = 0; y < src.Height; y++)
					for (int x = 0; x < src.Width; x++)
						if (!keep[x, y]) clean.SetPixel(x, y, Color.FromArgb(0, 0, 0, 0));
				using (var gd = Graphics.FromImage(dst)) {
					gd.CompositingMode = CompositingMode.SourceCopy;
					gd.InterpolationMode = InterpolationMode.HighQualityBicubic;
					gd.PixelOffsetMode = PixelOffsetMode.Half;
					var at = new ImageAttributes();
					at.SetWrapMode(WrapMode.TileFlipXY);
					gd.DrawImage(clean, new Rectangle(0, 0, W, H), 0, 0, src.Width, src.Height, GraphicsUnit.Pixel, at);
					gd.CompositingMode = CompositingMode.SourceOver;
					gd.SmoothingMode = SmoothingMode.AntiAlias;
					gd.PixelOffsetMode = PixelOffsetMode.HighQuality;
					gd.ScaleTransform(S, S);
					foreach (var e in spec)
						gd.FillPath(Brushes.White, PathOf(g, e));
				}
				// transparent pixels white (no dark fringe under filtering)
				var bd = dst.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
				var px = new int[W * H];
				System.Runtime.InteropServices.Marshal.Copy(bd.Scan0, px, 0, px.Length);
				for (int i = 0; i < px.Length; i++) if ((px[i] >> 24) == 0) px[i] = 0x00FFFFFF;
				System.Runtime.InteropServices.Marshal.Copy(px, 0, bd.Scan0, px.Length);
				dst.UnlockBits(bd);
				dst.Save(a[5], ImageFormat.Png);
				return 0;
			}
		}
		return 1;
	}
}
