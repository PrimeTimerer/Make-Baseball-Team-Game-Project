// Cheats: PCSX2 .pnach files in the game's "mods" folder, on when config.ini [Mods] Enabled lists them. yktk3.exe
// watches that setting, so switching one on or off also takes effect while the game runs.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;

namespace Yktk3Launcher
{
    public class CheatFile
    {
        public string Name; // the file name, as [Mods] Enabled lists it
        public string Path;
        public string Description;
        public int Patches;
        public bool Code;     // rewrites game code (works once compiled in: tools/cheat_code.py)
        public bool Compiled; // in cheats_native.txt with this content: compiled into yktk3.exe
    }

    public class CheatFunction : CheatFile
    {
        public string Category, Title, Japanese, Note;
    }

    public static class Cheats
    {
        public static List<string> EnabledNames(Launcher launcher)
        {
            return launcher.Config("Mods", "Enabled", "").Split(';').Select(n => n.Trim()).Where(n => n.Length > 0).ToList();
        }

        public static void SetEnabled(Launcher launcher, IEnumerable<string> names)
        {
            launcher.SetConfig("Mods", "Enabled", string.Join(";", names.Distinct(StringComparer.OrdinalIgnoreCase)));
        }

        // Whether a .pnach writes into the game code (0x100000-0x56F1B8), with PCSX2 1.6.0's address rules as
        // tools/cheat_code.py: plain types through the EE memory map (RAM and its mirrors), extended 0/1/2 writes.
        static bool ChangesCode(string path)
        {
            foreach (string raw in File.ReadLines(path, Encoding.UTF8))
            {
                string line = raw.Split(new[] { "//" }, StringSplitOptions.None)[0].Trim().TrimStart('﻿');
                if (!line.StartsWith("patch", StringComparison.OrdinalIgnoreCase) || line.IndexOf('=') < 0)
                    continue;
                string[] fields = line.Substring(line.IndexOf('=') + 1).Split(',');
                uint address;
                if (fields.Length < 5 || !fields[1].Trim().Equals("EE", StringComparison.OrdinalIgnoreCase) ||
                    !uint.TryParse(fields[2].Trim(), System.Globalization.NumberStyles.HexNumber, null, out address))
                    continue;
                if (fields[3].Trim().Equals("extended", StringComparison.OrdinalIgnoreCase))
                {
                    if (address >> 28 > 2)
                        continue;
                }
                else
                {
                    uint segment = address & 0xFE000000u;
                    if (segment != 0 && segment != 0x20000000u && segment != 0x30000000u && segment != 0x80000000u && segment != 0xA0000000u)
                        continue;
                }
                uint physical = address & 0x01FFFFFFu;
                if (physical >= 0x100000u && physical < 0x56F1B8u)
                    return true;
            }
            return false;
        }

        // cheats_native.txt (tools/cheat_code.py at the last generate): name -> sha256 of the cheat files compiled in.
        static Dictionary<string, string> CompiledFiles(Launcher launcher)
        {
            var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            string path = System.IO.Path.Combine(launcher.Dir, "cheats_native.txt");
            if (File.Exists(path))
                foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
                {
                    string[] parts = line.Split('\t');
                    if (parts.Length == 3 && !line.StartsWith("#"))
                        files[parts[2]] = parts[0];
                }
            return files;
        }

        static string Sha256(string path)
        {
            using (var sha = System.Security.Cryptography.SHA256.Create())
            using (var file = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "").ToLowerInvariant();
        }

        public static List<CheatFile> List(Launcher launcher)
        {
            var list = new List<CheatFile>();
            if (!Directory.Exists(launcher.ModsDir))
                return list;
            Dictionary<string, string> compiled = CompiledFiles(launcher);
            foreach (string file in Directory.GetFiles(launcher.ModsDir, "*.pnach").OrderBy(f => f, StringComparer.CurrentCultureIgnoreCase))
            {
                var cheat = new CheatFile { Name = Path.GetFileName(file), Path = file };
                var notes = new List<string>();
                try
                {
                    foreach (string raw in File.ReadLines(file, Encoding.UTF8))
                    {
                        string line = raw.Trim().TrimStart('﻿');
                        if (line.StartsWith("patch=", StringComparison.OrdinalIgnoreCase))
                            cheat.Patches++;
                        else if (notes.Count < 2 && line.StartsWith("//") && line.Trim('/', ' ').Length > 0)
                            notes.Add(line.Trim('/', ' '));
                        else if (notes.Count < 2 && (line.StartsWith("comment=") || line.StartsWith("description=")))
                            notes.Add(line.Substring(line.IndexOf('=') + 1).Trim());
                    }
                }
                catch (Exception)
                {
                }
                cheat.Description = string.Join(" · ", notes);
                try
                {
                    cheat.Code = ChangesCode(file);
                    string digest;
                    cheat.Compiled = cheat.Code && compiled.TryGetValue(cheat.Name, out digest) && digest == Sha256(file);
                }
                catch (Exception)
                {
                }
                list.Add(cheat);
            }
            return list;
        }

        // The cheats one by one: mods/기능별 (tools/cheat_groups.py splits the cheat files by function), listed with their
        // category and Korean name in mods/기능별/목록.tsv. Name is the path [Mods] Enabled lists ("기능별/<file>", or a
        // whole file of the mods folder such as a team S+ file).
        public static List<CheatFunction> Functions(Launcher launcher)
        {
            var list = new List<CheatFunction>();
            string index = System.IO.Path.Combine(launcher.ModsDir, "기능별", "목록.tsv");
            if (!File.Exists(index))
                return list;
            Dictionary<string, string> compiled = CompiledFiles(launcher);
            foreach (string line in File.ReadAllLines(index, Encoding.UTF8))
            {
                if (line.StartsWith("#") || line.Trim().Length == 0)
                    continue;
                string[] f = line.Split('\t');
                if (f.Length < 3)
                    continue;
                string path = System.IO.Path.Combine(launcher.ModsDir, f[0].Replace('/', System.IO.Path.DirectorySeparatorChar));
                var c = new CheatFunction { Name = f[0], Path = path, Category = f[1], Title = f[2],
                                            Japanese = f.Length > 3 ? f[3] : "", Note = f.Length > 5 ? f[5] : "", Description = "" };
                try
                {
                    c.Code = File.Exists(path) && ChangesCode(path);
                    string digest;
                    c.Compiled = c.Code && compiled.TryGetValue(c.Name, out digest) && digest == Sha256(path);
                }
                catch (Exception)
                {
                }
                list.Add(c);
            }
            return list;
        }

        // .pnach files, or .zip files with .pnach inside (copied flat into the mods folder). Returns the names added.
        public static List<string> Import(Launcher launcher, IEnumerable<string> files)
        {
            Directory.CreateDirectory(launcher.ModsDir);
            var added = new List<string>();
            foreach (string file in files)
            {
                if (file.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                {
                    using (ZipArchive zip = ZipFile.OpenRead(file))
                        foreach (ZipArchiveEntry entry in zip.Entries.Where(e => e.Name.EndsWith(".pnach", StringComparison.OrdinalIgnoreCase)))
                        {
                            entry.ExtractToFile(Path.Combine(launcher.ModsDir, entry.Name), true);
                            added.Add(entry.Name);
                        }
                }
                else if (file.EndsWith(".pnach", StringComparison.OrdinalIgnoreCase))
                {
                    string target = Path.Combine(launcher.ModsDir, Path.GetFileName(file));
                    if (!string.Equals(Path.GetFullPath(file), Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase))
                        File.Copy(file, target, true);
                    added.Add(Path.GetFileName(file));
                }
            }
            return added;
        }
    }

    public class CheatPage : PageBase
    {
        string filter = "";

        public CheatPage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            List<CheatFunction> functions = Cheats.Functions(Launcher);
            var wholeFunctionFiles = new HashSet<string>(functions.Select(f => f.Name), StringComparer.OrdinalIgnoreCase);
            List<CheatFile> cheats = Cheats.List(Launcher).Where(c => !wholeFunctionFiles.Contains(c.Name)).ToList();
            var enabled = new HashSet<string>(Cheats.EnabledNames(Launcher), StringComparer.OrdinalIgnoreCase);
            var functionList = new StackPanel();
            var list = new StackPanel();
            Func<CheatFile, string, string, bool, Border> makeRow = null;
            Action fill = null;
            fill = delegate
            {
                functionList.Children.Clear();
                string shownCategory = null;
                StackPanel categoryRows = null;
                foreach (CheatFunction function in functions.Where(f => filter.Length == 0 ||
                         (f.Title + " " + f.Japanese + " " + f.Note + " " + f.Category).IndexOf(filter, StringComparison.CurrentCultureIgnoreCase) >= 0))
                {
                    if (function.Category != shownCategory)
                    {
                        shownCategory = function.Category;
                        categoryRows = new StackPanel();
                        string category = function.Category;
                        int count = functions.Count(f => f.Category == category);
                        int on = functions.Count(f => f.Category == category && enabled.Contains(f.Name));
                        var header = Ui.Text(category + "  ·  " + on + " / " + count + " 켜짐", "H2");
                        header.Margin = new Thickness(0, functionList.Children.Count == 0 ? 0 : 12, 0, 6);
                        functionList.Children.Add(header);
                        functionList.Children.Add(categoryRows);
                    }
                    string caption = string.Join(" · ", new[] { function.Japanese, function.Note }.Where(s => !string.IsNullOrEmpty(s)));
                    categoryRows.Children.Add(makeRow(function, function.Title, caption, false));
                }
                if (functions.Count == 0)
                    functionList.Children.Add(Ui.Text("기능별 목록이 없습니다 (mods/기능별/목록.tsv). 다시 빌드하면 tools/cheat_groups.py가 만듭니다.", "Caption"));

                list.Children.Clear();
                IEnumerable<CheatFile> shown = cheats.Where(c => filter.Length == 0 || c.Name.IndexOf(filter, StringComparison.CurrentCultureIgnoreCase) >= 0
                                                                 || c.Description.IndexOf(filter, StringComparison.CurrentCultureIgnoreCase) >= 0);
                if (cheats.Count == 0)
                    list.Children.Add(Ui.Text("치트가 없습니다. \"치트 추가\"로 .pnach 파일이나 그것이 든 .zip을 넣으세요.", "Caption"));
                foreach (CheatFile cheat in shown)
                    list.Children.Add(makeRow(cheat, Path.GetFileNameWithoutExtension(cheat.Name),
                                              (cheat.Description.Length > 0 ? cheat.Description + " · " : "") + "patch " + cheat.Patches + "줄", true));
            };
            makeRow = delegate(CheatFile cheat, string title, string caption, bool removable)
            {
                {
                    CheatFile c = cheat;
                    var row = new Grid();
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    row.ColumnDefinitions.Add(new ColumnDefinition());
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    var toggle = Ui.Switch(enabled.Contains(c.Name), delegate(bool on)
                    {
                        List<string> names = Cheats.EnabledNames(Launcher);
                        names.RemoveAll(n => string.Equals(n, c.Name, StringComparison.OrdinalIgnoreCase));
                        if (on)
                            names.Add(c.Name);
                        Cheats.SetEnabled(Launcher, names);
                        if (on)
                            enabled.Add(c.Name);
                        else
                            enabled.Remove(c.Name);
                        Window.Toast((on ? "켬: " : "끔: ") + title + (Game.IsRunning ? " (게임에 바로 적용)" : ""));
                    });
                    toggle.Margin = new Thickness(0, 0, 14, 0);
                    row.Children.Add(toggle);
                    var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                    var name = Ui.Text(title, "Body");
                    name.FontWeight = FontWeights.SemiBold;
                    Border kind = !c.Code ? Ui.Chip("", "데이터", Ui.Brush("SubText"))
                                  : c.Compiled ? Ui.Chip("", "코드 · 네이티브", Ui.Brush("Good"))
                                  : Ui.Chip("", "코드 · 다시 빌드 필요", Ui.Brush("Warn"));
                    kind.Margin = new Thickness(10, 0, 0, 0);
                    kind.Padding = new Thickness(8, 1, 8, 1);
                    text.Children.Add(Ui.Row(name, kind));
                    if (caption.Length > 0)
                        text.Children.Add(Ui.Text(caption, "Caption"));
                    Grid.SetColumn(text, 1);
                    row.Children.Add(text);
                    var remove = Ui.Button("휴지통으로", delegate
                    {
                        Window.Confirm("치트 지우기", c.Name + "을(를) 휴지통으로 옮깁니다.", "옮기기", delegate
                        {
                            try
                            {
                                Microsoft.VisualBasic.FileIO.FileSystem.DeleteFile(c.Path, Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs,
                                                                                  Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
                                List<string> names = Cheats.EnabledNames(Launcher);
                                if (names.RemoveAll(n => string.Equals(n, c.Name, StringComparison.OrdinalIgnoreCase)) > 0)
                                    Cheats.SetEnabled(Launcher, names);
                                Window.Refresh();
                            }
                            catch (Exception error)
                            {
                                Window.Error("옮기지 못했습니다", error);
                            }
                        });
                    }, null, "");
                    remove.Padding = new Thickness(10, 5, 10, 5);
                    remove.Opacity = 0.8;
                    Grid.SetColumn(remove, 2);
                    if (removable)
                        row.Children.Add(remove);
                    return new Border { Background = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(0x14, 0x1C, 0x2E)), CornerRadius = new CornerRadius(10), Padding = new Thickness(14, 10, 14, 10), Margin = new Thickness(0, 0, 0, 6), Child = row };
                }
            };
            fill();

            var search = new TextBox { Width = 260, Text = filter, ToolTip = "이름이나 설명으로 찾기" };
            search.TextChanged += delegate
            {
                filter = search.Text.Trim();
                fill();
            };
            var searchIcon = Ui.Icon("", 14, Ui.Brush("SubText"));
            searchIcon.Margin = new Thickness(0, 0, 8, 0);
            var toolbar = Ui.Row(searchIcon, search,
                Ui.Button("치트 추가", delegate
                {
                    var dialog = new Microsoft.Win32.OpenFileDialog { Title = "치트 파일", Filter = "PCSX2 치트 (*.pnach;*.zip)|*.pnach;*.zip", Multiselect = true };
                    if (dialog.ShowDialog(Window) != true)
                        return;
                    try
                    {
                        List<string> added = Cheats.Import(Launcher, dialog.FileNames);
                        Window.Toast(added.Count + "개를 추가했습니다 (꺼진 상태)");
                        Window.Refresh();
                    }
                    catch (Exception error)
                    {
                        Window.Error("치트를 추가하지 못했습니다", error);
                    }
                }, "AccentButton", "", new Thickness(10, 0, 0, 0)),
                Ui.Button("모두 끄기", delegate
                {
                    Cheats.SetEnabled(Launcher, new string[0]);
                    Window.Toast("치트를 모두 껐습니다");
                    Window.Refresh();
                }, null, "", new Thickness(8, 0, 0, 0)),
                Ui.Button("폴더 열기", delegate { Ui.OpenFolder(Launcher.ModsDir); }, null, "", new Thickness(8, 0, 0, 0)));
            toolbar.Margin = new Thickness(0, 0, 0, 14);

            var page = new List<UIElement> { toolbar };
            int pending = cheats.Count(c => c.Code && !c.Compiled) + functions.Count(c => c.Code && !c.Compiled);
            if (pending > 0)
            {
                string root = Updater.RepoRoot(Launcher.Dir);
                var banner = Ui.Card(null,
                    Ui.Text("코드 치트 " + pending + "개가 아직 빌드에 들어 있지 않습니다", "H2"),
                    Ui.Text("게임 코드를 바꾸는 치트는 게임과 함께 C++로 번역해야 작동합니다. 다시 빌드하면(바뀐 부분만, 몇 분) 켜고 끌 때 바로 적용됩니다.", "Body"),
                    root == null ? (UIElement)Ui.Text("이 폴더는 소스 저장소 안에 있지 않아 여기서 빌드할 수 없습니다.", "Caption")
                                 : Ui.Button("다시 빌드", delegate
                                 {
                                     Window.Confirm("다시 빌드", "런처를 닫고, 명령 창에서 치트를 포함해 게임 코드를 다시 만들고 빌드합니다(바뀐 부분만, 몇 분). 끝나면 런처가 다시 열립니다.", "빌드", delegate
                                     {
                                         try
                                         {
                                             Updater.Install(Launcher, root, false);
                                             Application.Current.Shutdown();
                                         }
                                         catch (Exception error)
                                         {
                                             Window.Error("빌드를 시작하지 못했습니다", error);
                                         }
                                     });
                                 }, "AccentButton", "", new Thickness(0, 12, 0, 0)));
                banner.BorderBrush = Ui.Brush("Warn");
                page.Add(banner);
            }
            page.Add(Ui.Card("기능별 치트 " + functions.Count + "개 · 켜짐 " + functions.Count(c => enabled.Contains(c.Name)) + "개", functionList));
            page.Add(Ui.Card("원본 파일 (파일째 켜기) " + cheats.Count + "개 · 켜짐 " + cheats.Count(c => enabled.Contains(c.Name)) + "개", list));
            return Ui.Page("치트", "치트 파일을 기능별로 나눠(mods/기능별) 하나씩 켜고 끕니다. 게임 실행 중에도 바로 적용되고, 게임 안 F2 > 치트 탭에서도 같습니다. 원본 파일을 통째로 켜면 같은 기능이 두 번 돌 수 있으니 한쪽만 쓰세요.",
                page.ToArray());
        }
    }
}
