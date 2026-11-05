// The memory card: Mcd001.ps2 in PCSX2's format (528-byte pages with ECC, or 512 without), its saves, backups, and new
// cards made from folders of saves (the layout of tools/make_memcard.py, which is checked against the game).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace Yktk3Launcher
{
    public class SaveInfo
    {
        public string Folder;
        public string Title; // icon.sys (Shift-JIS), null without one
        public DateTime Modified;
        public long Bytes;
        public int Files;
    }

    public class Ps2Card
    {
        const int Page = 512;
        readonly byte[] raw;
        readonly int stride, pagesPerCluster, clustersPerCard, allocOffset, allocEnd, rootCluster;
        readonly int[] ifc = new int[32];
        public readonly bool Valid;

        public static Ps2Card Load(string path)
        {
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                var data = new byte[file.Length];
                int read = 0;
                while (read < data.Length)
                {
                    int n = file.Read(data, read, data.Length - read);
                    if (n <= 0)
                        break;
                    read += n;
                }
                return new Ps2Card(data);
            }
        }

        public Ps2Card(byte[] raw)
        {
            this.raw = raw;
            if (raw.Length < 0x200 || !Encoding.ASCII.GetString(raw, 0, 27).StartsWith("Sony PS2 Memory Card Format"))
                return;
            pagesPerCluster = BitConverter.ToUInt16(raw, 0x2A);
            clustersPerCard = BitConverter.ToInt32(raw, 0x30);
            allocOffset = BitConverter.ToInt32(raw, 0x34);
            allocEnd = BitConverter.ToInt32(raw, 0x38);
            rootCluster = BitConverter.ToInt32(raw, 0x3C);
            for (int i = 0; i < 32; i++)
                ifc[i] = BitConverter.ToInt32(raw, 0x50 + 4 * i);
            long pages = (long)clustersPerCard * pagesPerCluster;
            if (pages <= 0 || pagesPerCluster <= 0)
                return;
            stride = (int)(raw.Length / pages);
            Valid = (stride == 512 || stride == 528) && raw.Length == pages * stride;
        }

        byte[] Cluster(int absolute)
        {
            var data = new byte[Page * pagesPerCluster];
            for (int p = 0; p < pagesPerCluster; p++)
                Buffer.BlockCopy(raw, (absolute * pagesPerCluster + p) * stride, data, p * Page, Page);
            return data;
        }

        uint Fat(int relative)
        {
            int perCluster = Page * pagesPerCluster / 4;
            int indirectIndex = relative / perCluster;
            byte[] indirect = Cluster(ifc[indirectIndex / perCluster]);
            int fatCluster = BitConverter.ToInt32(indirect, indirectIndex % perCluster * 4);
            return BitConverter.ToUInt32(Cluster(fatCluster), relative % perCluster * 4);
        }

        byte[] Read(int start, long length)
        {
            var data = new byte[length];
            int pos = 0, cluster = start, guard = 0;
            while (pos < length && cluster >= 0 && guard++ < clustersPerCard)
            {
                byte[] block = Cluster(allocOffset + cluster);
                int n = (int)Math.Min(block.Length, length - pos);
                Buffer.BlockCopy(block, 0, data, pos, n);
                pos += n;
                uint next = Fat(cluster);
                if (next == 0xFFFFFFFFu || (next & 0x80000000u) == 0)
                    break;
                cluster = (int)(next & 0x7FFFFFFFu);
            }
            return data;
        }

        class Entry
        {
            public ushort Mode;
            public int Length, Cluster;
            public DateTime Modified;
            public string Name;
            public bool Exists { get { return (Mode & 0x8000) != 0; } }
            public bool IsDirectory { get { return (Mode & 0x0020) != 0; } }
        }

        List<Entry> Directory(int start, int count)
        {
            var entries = new List<Entry>();
            byte[] data = Read(start, (long)count * Page);
            for (int i = 0; i < count; i++)
            {
                int o = i * Page;
                int nameLength = 0;
                while (nameLength < 32 && data[o + 0x40 + nameLength] != 0)
                    nameLength++;
                entries.Add(new Entry
                {
                    Mode = BitConverter.ToUInt16(data, o), Length = BitConverter.ToInt32(data, o + 4), Cluster = BitConverter.ToInt32(data, o + 0x10),
                    Modified = Time(data, o + 0x18), Name = Encoding.ASCII.GetString(data, o + 0x40, nameLength),
                });
            }
            return entries;
        }

        // sceMcStDateTime: unused, sec, min, hour, day, month, year (u16), Japan time.
        static DateTime Time(byte[] d, int o)
        {
            try
            {
                return new DateTime(BitConverter.ToUInt16(d, o + 6), d[o + 5], d[o + 4], d[o + 3], d[o + 2], d[o + 1], DateTimeKind.Utc).AddHours(-9).ToLocalTime();
            }
            catch (Exception)
            {
                return DateTime.MinValue;
            }
        }

        public List<SaveInfo> Saves()
        {
            var saves = new List<SaveInfo>();
            if (!Valid)
                return saves;
            int rootCount = Directory(rootCluster, 1)[0].Length;
            foreach (Entry dir in Directory(rootCluster, rootCount).Skip(2).Where(e => e.Exists && e.IsDirectory))
            {
                var save = new SaveInfo { Folder = dir.Name, Modified = dir.Modified };
                foreach (Entry file in Directory(dir.Cluster, dir.Length).Skip(2).Where(e => e.Exists && !e.IsDirectory))
                {
                    save.Files++;
                    save.Bytes += file.Length;
                    if (file.Name.Equals("icon.sys", StringComparison.OrdinalIgnoreCase) && file.Length >= 0xC0 + 68)
                        save.Title = IconTitle(Read(file.Cluster, file.Length));
                }
                saves.Add(save);
            }
            return saves;
        }

        // icon.sys: "PS2D", the title's line break at 0x06, the title at 0xC0 (68 bytes of Shift-JIS).
        static string IconTitle(byte[] icon)
        {
            if (Encoding.ASCII.GetString(icon, 0, 4) != "PS2D")
                return null;
            int length = 0;
            while (length < 68 && icon[0xC0 + length] != 0)
                length++;
            int lineBreak = Math.Min(BitConverter.ToUInt16(icon, 6), length);
            Encoding sjis = Encoding.GetEncoding(932);
            string first = sjis.GetString(icon, 0xC0, lineBreak).Trim(), second = sjis.GetString(icon, 0xC0 + lineBreak, length - lineBreak).Trim();
            return (first + " " + second).Trim().Normalize(NormalizationForm.FormKC);
        }

        public int FreeClusters()
        {
            if (!Valid)
                return 0;
            int free = 0;
            for (int c = 0; c < allocEnd; c++)
                if (Fat(c) == 0x7FFFFFFFu)
                    free++;
            return free;
        }

        public int ClusterBytes { get { return Page * pagesPerCluster; } }

        // ---- a new 8 MB card (tools/make_memcard.py): formatted, with the subfolders of importFolder as saves ----
        const int Pages = 16384, PagesPerCluster = 2, PagesPerBlock = 16, Clusters = 8192, Blocks = 1024, EntriesPerCluster = 256;
        const int Ifc = 8, FatClusters = 32, AllocOffset = 41, AllocEnd = 8135, ClusterSize = 1024;
        static readonly byte[] Parity = new byte[256], ColumnMasks = new byte[256];

        static Ps2Card()
        {
            for (int b = 0; b < 256; b++)
            {
                int p = b ^ (b >> 4);
                p ^= p >> 2;
                p ^= p >> 1;
                Parity[b] = (byte)(p & 1);
            }
            int[] masks = { 0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0 };
            for (int b = 0; b < 256; b++)
            {
                int mask = 0;
                for (int i = 0; i < masks.Length; i++)
                    mask |= Parity[b & masks[i]] << i;
                ColumnMasks[b] = (byte)mask;
            }
        }

        static void Ecc128(byte[] data, int offset, byte[] output, int at)
        {
            int column = 0x77, line0 = 0x7F, line1 = 0x7F;
            for (int i = 0; i < 128; i++)
            {
                byte b = data[offset + i];
                column ^= ColumnMasks[b];
                if (Parity[b] != 0)
                {
                    line0 ^= ~i;
                    line1 ^= i;
                }
            }
            output[at] = (byte)(column & 0xFF);
            output[at + 1] = (byte)(line0 & 0x7F);
            output[at + 2] = (byte)(line1 & 0xFF);
        }

        static byte[] Stamp(DateTime local)
        {
            DateTime jst = local.ToUniversalTime().AddHours(9);
            var s = new byte[8];
            s[1] = (byte)jst.Second;
            s[2] = (byte)jst.Minute;
            s[3] = (byte)jst.Hour;
            s[4] = (byte)jst.Day;
            s[5] = (byte)jst.Month;
            BitConverter.GetBytes((ushort)jst.Year).CopyTo(s, 6);
            return s;
        }

        static byte[] DirEntry(ushort mode, int length, int cluster, int index, string name, byte[] stamp)
        {
            var e = new byte[Page];
            BitConverter.GetBytes(mode).CopyTo(e, 0);
            BitConverter.GetBytes(length).CopyTo(e, 4);
            stamp.CopyTo(e, 8);
            BitConverter.GetBytes(cluster).CopyTo(e, 0x10);
            BitConverter.GetBytes(index).CopyTo(e, 0x14);
            stamp.CopyTo(e, 0x18);
            byte[] n = Encoding.ASCII.GetBytes(name);
            Buffer.BlockCopy(n, 0, e, 0x40, Math.Min(n.Length, 31));
            return e;
        }

        public static byte[] Build(string importFolder, out int freeClusters)
        {
            var clusters = new byte[Clusters][];
            for (int i = 0; i < Clusters; i++)
            {
                clusters[i] = new byte[ClusterSize];
                for (int j = 0; j < ClusterSize; j++)
                    clusters[i][j] = 0xFF;
            }
            var fat = new uint[FatClusters * EntriesPerCluster];
            for (int i = 0; i < fat.Length; i++)
                fat[i] = i < AllocEnd ? 0x7FFFFFFFu : 0xFFFFFFFFu;
            int nextFree = 0;
            Func<int, List<int>> alloc = delegate(int count)
            {
                var chain = Enumerable.Range(nextFree, count).ToList();
                if (chain.Count == 0 || chain[chain.Count - 1] >= AllocEnd)
                    throw new InvalidOperationException("세이브가 8 MB 카드에 다 들어가지 않습니다.");
                nextFree += count;
                for (int i = 0; i + 1 < chain.Count; i++)
                    fat[chain[i]] = 0x80000000u | (uint)chain[i + 1];
                fat[chain[chain.Count - 1]] = 0xFFFFFFFFu;
                return chain;
            };
            Action<List<int>, byte[], byte> write = delegate(List<int> chain, byte[] data, byte fill)
            {
                for (int i = 0; i < chain.Count; i++)
                {
                    byte[] target = clusters[AllocOffset + chain[i]];
                    for (int j = 0; j < ClusterSize; j++)
                    {
                        int k = i * ClusterSize + j;
                        target[j] = k < data.Length ? data[k] : fill;
                    }
                }
            };

            var saves = new List<string>();
            if (importFolder != null)
                saves.AddRange(System.IO.Directory.GetDirectories(importFolder).OrderBy(d => System.IO.Path.GetFileName(d), StringComparer.Ordinal));
            byte[] now = Stamp(DateTime.Now);
            int rootCount = 2 + saves.Count;
            List<int> rootChain = alloc((rootCount + 1) / 2);
            var root = new List<byte[]> { DirEntry(0x8427, rootCount, 0, 0, ".", now), DirEntry(0xA426, 0, 0, 0, "..", now) };
            for (int index = 0; index < saves.Count; index++)
            {
                string dir = saves[index];
                byte[] dirStamp = Stamp(System.IO.Directory.GetLastWriteTime(dir));
                string[] files = System.IO.Directory.GetFiles(dir).OrderBy(f => System.IO.Path.GetFileName(f), StringComparer.Ordinal).ToArray();
                int count = 2 + files.Length;
                List<int> dirChain = alloc((count + 1) / 2);
                var entries = new List<byte[]> { DirEntry(0x8427, 0, rootChain[0], index + 2, ".", dirStamp), DirEntry(0x8427, 0, 0, 0, "..", dirStamp) };
                foreach (string file in files)
                {
                    byte[] data = File.ReadAllBytes(file);
                    List<int> chain = data.Length > 0 ? alloc((data.Length + ClusterSize - 1) / ClusterSize) : null;
                    if (chain != null)
                        write(chain, data, 0);
                    entries.Add(DirEntry(0x8497, data.Length, chain != null ? chain[0] : 0, 0, System.IO.Path.GetFileName(file), Stamp(File.GetLastWriteTime(file))));
                }
                write(dirChain, entries.SelectMany(e => e).ToArray(), 0xFF);
                root.Add(DirEntry(0x842F, count, dirChain[0], 0, System.IO.Path.GetFileName(dir), dirStamp));
            }
            write(rootChain, root.SelectMany(e => e).ToArray(), 0xFF);

            byte[] super = clusters[0];
            for (int j = 0; j < ClusterSize; j++)
                super[j] = 0xFF;
            var header = new byte[0x152];
            Encoding.ASCII.GetBytes("Sony PS2 Memory Card Format ").CopyTo(header, 0);
            Encoding.ASCII.GetBytes("1.2.0.0").CopyTo(header, 28);
            BitConverter.GetBytes((ushort)Page).CopyTo(header, 0x28);
            BitConverter.GetBytes((ushort)PagesPerCluster).CopyTo(header, 0x2A);
            BitConverter.GetBytes((ushort)PagesPerBlock).CopyTo(header, 0x2C);
            BitConverter.GetBytes((ushort)0xFF00).CopyTo(header, 0x2E);
            int[] fields = { Clusters, AllocOffset, AllocEnd, 0, Blocks - 1, Blocks - 2 };
            for (int i = 0; i < fields.Length; i++)
                BitConverter.GetBytes(fields[i]).CopyTo(header, 0x30 + 4 * i);
            BitConverter.GetBytes(Ifc).CopyTo(header, 0x50);
            for (int i = 0; i < 32; i++)
                BitConverter.GetBytes(-1).CopyTo(header, 0xD0 + 4 * i);
            header[0x150] = 2;    // PS2 card
            header[0x151] = 0x52; // ECC, bad blocks, erased bits are zeros (as the console formats)
            header.CopyTo(super, 0);
            for (int i = 0; i < EntriesPerCluster; i++)
                BitConverter.GetBytes(i < FatClusters ? Ifc + 1 + i : 0).CopyTo(clusters[Ifc], 4 * i);
            for (int f = 0; f < FatClusters; f++)
                for (int i = 0; i < EntriesPerCluster; i++)
                    BitConverter.GetBytes(fat[f * EntriesPerCluster + i]).CopyTo(clusters[Ifc + 1 + f], 4 * i);

            var image = new byte[Pages * (Page + 16)];
            for (int c = 0; c < Clusters; c++)
                for (int p = 0; p < PagesPerCluster; p++)
                {
                    int at = (c * PagesPerCluster + p) * (Page + 16);
                    Buffer.BlockCopy(clusters[c], p * Page, image, at, Page);
                    for (int chunk = 0; chunk < 4; chunk++)
                        Ecc128(clusters[c], p * Page + chunk * 128, image, at + Page + chunk * 3);
                }
            freeClusters = AllocEnd - nextFree;
            return image;
        }
    }

    public class CardPage : PageBase
    {
        public CardPage(MainWindow window) : base(window)
        {
        }

        static string Why(string file)
        {
            if (file.EndsWith("-auto.ps2"))
                return "실행 전 자동";
            if (file.EndsWith("-manual.ps2"))
                return "직접 백업";
            if (file.EndsWith("-before-restore.ps2"))
                return "복원하기 전";
            if (file.EndsWith("-before-import.ps2"))
                return "가져오기 전";
            return "";
        }

        // Replaces the card with new contents after a backup of the old one.
        void Replace(string why, Func<byte[]> contents, string done)
        {
            if (Game.IsRunning)
            {
                Window.ShowProblem("게임이 실행 중입니다. 게임을 끈 다음에 바꿔 주세요.");
                return;
            }
            try
            {
                byte[] data = contents();
                if (data == null)
                    return;
                CardStore.Backup(Launcher, why);
                Directory.CreateDirectory(Launcher.CardsDir);
                File.WriteAllBytes(CardStore.CardPath(Launcher), data);
                Window.Toast(done);
                Window.Refresh();
            }
            catch (Exception error)
            {
                Window.Error("메모리카드를 바꾸지 못했습니다", error);
            }
        }

        protected override FrameworkElement Build()
        {
            string path = CardStore.CardPath(Launcher);
            var cardStack = new StackPanel();
            if (!File.Exists(path))
                cardStack.Children.Add(Ui.Text("아직 카드가 없습니다. 게임을 처음 시작하면 만들어집니다.", "Caption"));
            else
            {
                Ps2Card card = null;
                try
                {
                    card = Ps2Card.Load(path);
                }
                catch (Exception)
                {
                }
                if (card == null || !card.Valid)
                    cardStack.Children.Add(Ui.Chip("", "카드 형식을 읽지 못했습니다", Ui.Brush("Warn")));
                else
                {
                    List<SaveInfo> saves = card.Saves();
                    int free = card.FreeClusters();
                    var info = new WrapPanel();
                    info.Children.Add(Ui.Chip("", "PCSX2 형식 · 8 MB", Ui.Brush("Good")));
                    info.Children.Add(Ui.Chip("", "세이브 " + saves.Count + "개", Ui.Brush("Accent")));
                    info.Children.Add(Ui.Chip("", "남은 공간 " + (long)free * card.ClusterBytes / 1024 + " KB", Ui.Brush("SubText")));
                    cardStack.Children.Add(info);
                    foreach (SaveInfo save in saves.OrderByDescending(s => s.Modified))
                    {
                        var row = new Grid { Margin = new Thickness(0, 0, 0, 6) };
                        row.ColumnDefinitions.Add(new ColumnDefinition());
                        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                        var left = new StackPanel();
                        var title = new TextBlock { Text = save.Title ?? save.Folder, FontFamily = Ui.Res<FontFamily>("TitleFont"), FontSize = 14, Foreground = Ui.Brush("Text") };
                        left.Children.Add(title);
                        left.Children.Add(Ui.Text(save.Folder + " · 파일 " + save.Files + "개", "Caption"));
                        row.Children.Add(left);
                        var right = Ui.Text((save.Modified == DateTime.MinValue ? "" : save.Modified.ToString("yyyy-MM-dd HH:mm")) + "\n" + (save.Bytes + 1023) / 1024 + " KB", "Caption");
                        right.TextAlignment = TextAlignment.Right;
                        Grid.SetColumn(right, 1);
                        row.Children.Add(right);
                        cardStack.Children.Add(new Border { Background = new SolidColorBrush(Color.FromRgb(0x14, 0x1C, 0x2E)), CornerRadius = new CornerRadius(10), Padding = new Thickness(14, 10, 14, 10), Margin = new Thickness(0, 0, 0, 6), Child = row });
                    }
                }
            }
            cardStack.Children.Add(Ui.Row(
                Ui.Button("지금 백업", delegate
                {
                    try
                    {
                        CardStore.Backup(Launcher, "manual");
                        Window.Toast("백업했습니다");
                        Window.Refresh();
                    }
                    catch (Exception error)
                    {
                        Window.Error("백업하지 못했습니다", error);
                    }
                }, "AccentButton", "", new Thickness(0, 10, 8, 0)),
                Ui.Button("폴더 열기", delegate { Ui.OpenFolder(Launcher.CardsDir); }, null, "", new Thickness(0, 10, 0, 0))));

            var backups = new StackPanel();
            string backupDir = CardStore.BackupsDir(Launcher);
            string[] files = Directory.Exists(backupDir) ? Directory.GetFiles(backupDir, "Mcd001-*.ps2").OrderByDescending(f => f, StringComparer.Ordinal).ToArray() : new string[0];
            if (files.Length == 0)
                backups.Children.Add(Ui.Text("아직 백업이 없습니다.", "Caption"));
            foreach (string file in files)
            {
                string f = file;
                DateTime when = File.GetLastWriteTime(f);
                var row = new Grid { Margin = new Thickness(0, 0, 0, 6) };
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                var text = Ui.Text(when.ToString("yyyy-MM-dd HH:mm:ss") + "   " + Why(f), "Body");
                text.VerticalAlignment = VerticalAlignment.Center;
                row.Children.Add(text);
                var restore = Ui.Button("이 백업으로 복원", delegate
                {
                    Window.Confirm("백업으로 복원", when.ToString("yyyy-MM-dd HH:mm:ss") + " 백업으로 카드를 되돌립니다. 지금 카드는 먼저 백업해 둡니다.", "복원", delegate
                    {
                        Replace("before-restore", delegate { return File.ReadAllBytes(f); }, "복원했습니다");
                    });
                }, null, "");
                restore.Padding = new Thickness(12, 5, 12, 5);
                Grid.SetColumn(restore, 1);
                row.Children.Add(restore);
                backups.Children.Add(row);
            }

            return Ui.Page("메모리카드", "memcards\\Mcd001.ps2 (PCSX2와 같은 형식). 게임을 시작하기 전에 자동으로 백업할 수 있습니다.",
                Ui.Card("카드 1 (Mcd001.ps2)", cardStack),
                Ui.Card("백업",
                    Ui.Setting("게임 시작 전 자동 백업", "시작할 때마다 카드를 backups 폴더에 복사합니다.", Ui.Switch(Launcher.State("AutoBackup", "1") == "1", delegate(bool on)
                    {
                        Launcher.SetState("AutoBackup", on ? "1" : "0");
                        Window.Toast(on ? "자동 백업 켬" : "자동 백업 끔");
                    })),
                    Ui.Setting("보관할 백업 수", "넘으면 오래된 것부터 지웁니다.", Ui.Choices(Launcher.State("BackupsToKeep", "10"), delegate(string v)
                    {
                        Launcher.SetState("BackupsToKeep", v);
                        Window.Toast("저장했습니다");
                    }, "5", "5개", "10", "10개", "20", "20개", "50", "50개")),
                    backups),
                Ui.Card("가져오기",
                    Ui.Text("가져오기 전에 지금 카드를 자동으로 백업합니다.", "Caption"),
                    Ui.Row(
                        Ui.Button("PCSX2 카드 파일 가져오기", delegate
                        {
                            var dialog = new Microsoft.Win32.OpenFileDialog { Title = "PCSX2 메모리카드 (.ps2)", Filter = "PS2 메모리카드 (*.ps2;*.mcd;*.bin)|*.ps2;*.mcd;*.bin|모든 파일 (*.*)|*.*" };
                            if (dialog.ShowDialog(Window) != true)
                                return;
                            Replace("before-import", delegate
                            {
                                byte[] data = File.ReadAllBytes(dialog.FileName);
                                if (!new Ps2Card(data).Valid)
                                    throw new InvalidDataException("PS2 메모리카드 형식이 아닙니다: " + dialog.FileName);
                                return data;
                            }, "카드를 가져왔습니다");
                        }, null, "", new Thickness(0, 12, 8, 0)),
                        Ui.Button("폴더 세이브로 새 카드 만들기", delegate
                        {
                            string folder = Ui.FolderDialog("세이브 폴더들이 들어 있는 폴더 (PCSX2 폴더형 카드, 옛 빌드의 mc0 등)", null);
                            if (folder == null)
                                return;
                            Window.Confirm("새 카드 만들기", folder + " 안의 폴더들을 세이브로 넣은 새 카드로 지금 카드를 바꿉니다.", "만들기", delegate
                            {
                                Replace("before-import", delegate
                                {
                                    int free;
                                    return Ps2Card.Build(folder, out free);
                                }, "새 카드를 만들었습니다");
                            });
                        }, null, "", new Thickness(0, 12, 8, 0)),
                        Ui.Button("빈 카드로 초기화", delegate
                        {
                            Window.Confirm("빈 카드로 초기화", "카드의 모든 세이브를 지우고 포맷된 빈 카드로 바꿉니다. 지금 카드는 먼저 백업해 둡니다.", "초기화", delegate
                            {
                                Replace("before-import", delegate
                                {
                                    int free;
                                    return Ps2Card.Build(null, out free);
                                }, "빈 카드로 초기화했습니다");
                            });
                        }, null, "", new Thickness(0, 12, 0, 0)))));
        }
    }
}
