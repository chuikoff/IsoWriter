Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

public class IconGen {
    public static void CreateIsoWriterIcon(string outputPath) {
        int[] sizes = new int[] { 16, 32, 48, 64 };
        using (MemoryStream ms = new MemoryStream()) {
            BinaryWriter bw = new BinaryWriter(ms);
            bw.Write((short)0);
            bw.Write((short)1);
            bw.Write((short)sizes.Length);
            byte[][] pngBytes = new byte[sizes.Length][];

            for (int i = 0; i < sizes.Length; i++) {
                int s = sizes[i];
                using (Bitmap bmp = new Bitmap(s, s, PixelFormat.Format32bppArgb))
                using (Graphics g = Graphics.FromImage(bmp)) {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.PixelOffsetMode = PixelOffsetMode.HighQuality;

                    float pad = s * 0.05f;
                    float d = s - pad * 2;
                    RectangleF cdRect = new RectangleF(pad, pad, d, d);
                    using (LinearGradientBrush cdBrush = new LinearGradientBrush(
                        cdRect, Color.FromArgb(70, 130, 220), Color.FromArgb(20, 60, 140), 45f)) {
                        g.FillEllipse(cdBrush, cdRect);
                    }
                    using (Pen cdPen = new Pen(Color.FromArgb(200, 230, 255), s * 0.04f)) {
                        g.DrawEllipse(cdPen, cdRect);
                    }

                    float innerD = d * 0.55f;
                    float innerPad = (s - innerD) / 2f;
                    using (Pen ringPen = new Pen(Color.FromArgb(100, 255, 255, 255), s * 0.03f)) {
                        g.DrawEllipse(ringPen, innerPad, innerPad, innerD, innerD);
                    }

                    float holeD = d * 0.22f;
                    float holePad = (s - holeD) / 2f;
                    using (SolidBrush holeBrush = new SolidBrush(Color.FromArgb(240, 245, 255))) {
                        g.FillEllipse(holeBrush, holePad, holePad, holeD, holeD);
                    }
                    using (Pen holePen = new Pen(Color.FromArgb(50, 100, 180), s * 0.03f)) {
                        g.DrawEllipse(holePen, holePad, holePad, holeD, holeD);
                    }

                    float usbW = s * 0.40f;
                    float usbH = s * 0.65f;
                    float usbX = s * 0.55f;
                    float usbY = s * 0.30f;
                    RectangleF bodyRect = new RectangleF(usbX, usbY + usbH * 0.35f, usbW, usbH * 0.65f);
                    using (GraphicsPath path = new GraphicsPath()) {
                        float r = s * 0.08f;
                        path.AddArc(bodyRect.X, bodyRect.Y, r, r, 180, 90);
                        path.AddArc(bodyRect.Right - r, bodyRect.Y, r, r, 270, 90);
                        path.AddArc(bodyRect.Right - r, bodyRect.Bottom - r, r, r, 0, 90);
                        path.AddArc(bodyRect.X, bodyRect.Bottom - r, r, r, 90, 90);
                        path.CloseFigure();
                        using (LinearGradientBrush bBrush = new LinearGradientBrush(
                            bodyRect, Color.FromArgb(250, 150, 30), Color.FromArgb(200, 70, 10), 90f)) {
                            g.FillPath(bBrush, path);
                        }
                        using (Pen bPen = new Pen(Color.White, s * 0.03f)) {
                            g.DrawPath(bPen, path);
                        }
                    }

                    float plugW = usbW * 0.65f;
                    float plugH = usbH * 0.35f;
                    float plugX = usbX + (usbW - plugW) / 2f;
                    float plugY = usbY;
                    RectangleF plugRect = new RectangleF(plugX, plugY, plugW, plugH);
                    using (LinearGradientBrush plugBrush = new LinearGradientBrush(
                        plugRect, Color.FromArgb(240, 240, 245), Color.FromArgb(180, 180, 190), 90f)) {
                        g.FillRectangle(plugBrush, plugRect);
                    }
                    using (Pen plugPen = new Pen(Color.FromArgb(100, 100, 110), s * 0.02f)) {
                        g.DrawRectangle(plugPen, plugRect.X, plugRect.Y, plugRect.Width, plugRect.Height);
                    }

                    if (s >= 32) {
                        float dotW = plugW * 0.22f;
                        float dotH = plugH * 0.30f;
                        float dotY = plugY + plugH * 0.30f;
                        using (SolidBrush dotBrush = new SolidBrush(Color.FromArgb(80, 80, 90))) {
                            g.FillRectangle(dotBrush, plugX + plugW * 0.20f, dotY, dotW, dotH);
                            g.FillRectangle(dotBrush, plugX + plugW * 0.58f, dotY, dotW, dotH);
                        }
                    }

                    using (MemoryStream pngMs = new MemoryStream()) {
                        bmp.Save(pngMs, ImageFormat.Png);
                        pngBytes[i] = pngMs.ToArray();
                    }
                }
            }

            int imageOffset = 6 + sizes.Length * 16;
            for (int i = 0; i < sizes.Length; i++) {
                int s = sizes[i];
                bw.Write((byte)(s >= 256 ? 0 : s));
                bw.Write((byte)(s >= 256 ? 0 : s));
                bw.Write((byte)0);
                bw.Write((byte)0);
                bw.Write((short)1);
                bw.Write((short)32);
                bw.Write((int)pngBytes[i].Length);
                bw.Write((int)imageOffset);
                imageOffset += pngBytes[i].Length;
            }

            for (int i = 0; i < sizes.Length; i++) {
                bw.Write(pngBytes[i]);
            }

            File.WriteAllBytes(outputPath, ms.ToArray());
        }
    }
}
'@

[IconGen]::CreateIsoWriterIcon((Join-Path $PSScriptRoot "res\IsoWriter.ico"))
