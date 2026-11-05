// Logs page; tools page: desktop and Steam shortcuts, updates from GitHub, folders.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.Win32;

namespace Yktk3Launcher
{
    public class LogPage : PageBase
    {
        string selected;

        public LogPage(MainWindow window) : base(window)
        {
        }

        static string Status(LogInfo log)
        {
            if (log.ExitCode == null)
                return "끝을 모름 (실행 중이었거나 런처가 먼저 닫힘)";
            return (log.ExitCode == "0" ? "정상 종료" : "오류로 끝남 (코드 " + log.ExitCode + ")") + (log.PlayTime != null ? " · " + log.PlayTime : "") + (log.Safe ? " · 안전 설정" : "");
        }

        protected override FrameworkElement Build()
        {
            List<LogInfo> logs = Logs.List(Launcher.LogsDir);
            if (selected == null || !logs.Any(l => l.Path == selected))
                selected = logs.Count > 0 ? logs[0].Path : null;
            var list = new ListBox();
            if (logs.Count == 0)
                list.Items.Add(new ListBoxItem { Content = Ui.Text("아직 실행 기록이 없습니다.", "Caption"), IsEnabled = false });
            foreach (LogInfo log in logs)
            {
                var item = new StackPanel();
                var when = Ui.Text(log.Started.ToString("yyyy-MM-dd HH:mm:ss"), "Body");
                when.FontWeight = FontWeights.SemiBold;
                item.Children.Add(when);
                var status = Ui.Text(Status(log), "Caption");
                if (log.ExitCode != null && log.ExitCode != "0")
                    status.Foreground = Ui.Brush("Bad");
                item.Children.Add(status);
                var listItem = new ListBoxItem { Content = item, Tag = log.Path };
                list.Items.Add(listItem);
                if (log.Path == selected)
                    list.SelectedItem = listItem;
            }
            list.SelectionChanged += delegate
            {
                var item = list.SelectedItem as ListBoxItem;
                if (item != null && (string)item.Tag != selected)
                {
                    selected = (string)item.Tag;
                    Window.Refresh();
                }
            };

            var right = new DockPanel();
            LogInfo current = logs.FirstOrDefault(l => l.Path == selected);
            string text = selected != null ? Logs.Tail(selected, 4000) : "아직 로그가 없습니다. 런처로 게임을 실행하면 여기에 기록됩니다.";
            if (current != null && current.ExitCode != null && current.ExitCode != "0")
            {
                Diagnosis d = Diagnosis.Of(int.Parse(current.ExitCode), Logs.Tail(selected, 300));
                if (d != null)
                {
                    var card = Ui.Card(null, Ui.Row(Ui.Icon("", 20, Ui.Brush("Bad")), new TextBlock { Text = "  " + d.Title, FontSize = 15, FontWeight = FontWeights.SemiBold, Foreground = Ui.Brush("Text") }),
                                       Ui.Text(d.Detail, "Caption"), Ui.Text(d.Advice, "Body"));
                    card.BorderBrush = Ui.Brush("Bad");
                    DockPanel.SetDock(card, Dock.Top);
                    right.Children.Add(card);
                }
            }
            var viewer = new TextBox
            {
                Text = text, IsReadOnly = true, FontFamily = Ui.Res<FontFamily>("MonoFont"), FontSize = 12, TextWrapping = TextWrapping.NoWrap,
                AcceptsReturn = true, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
                VerticalContentAlignment = VerticalAlignment.Top,
            };
            viewer.Loaded += delegate { viewer.ScrollToEnd(); };
            var buttons = Ui.Row(
                Ui.Button("복사", delegate
                {
                    Clipboard.SetText(viewer.Text);
                    Window.Toast("로그를 복사했습니다");
                }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button("메모장으로 열기", delegate
                {
                    if (selected != null)
                        Process.Start("notepad.exe", "\"" + selected + "\"");
                }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button("폴더 열기", delegate { Ui.OpenFolder(Launcher.LogsDir); }, null, ""));
            buttons.Margin = new Thickness(0, 12, 0, 0);
            DockPanel.SetDock(buttons, Dock.Bottom);
            right.Children.Add(buttons);
            right.Children.Add(viewer);

            var body = new Grid { Margin = new Thickness(16, 8, 16, 24) };
            body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            body.RowDefinitions.Add(new RowDefinition());
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(300) });
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
            body.ColumnDefinitions.Add(new ColumnDefinition());
            var head = new StackPanel { Margin = new Thickness(0, 0, 0, 20) };
            head.Children.Add(Ui.Text("로그", "H1"));
            var sub = Ui.Text("런처로 실행한 게임이 남긴 기록 (최근 20번, logs 폴더). 오류로 끝나면 원인을 쉬운 말로 알려 줍니다.", "Caption");
            sub.Margin = new Thickness(0, 4, 0, 0);
            head.Children.Add(sub);
            Grid.SetColumnSpan(head, 3);
            body.Children.Add(head);
            Grid.SetRow(list, 1);
            body.Children.Add(list);
            Grid.SetRow(right, 1);
            Grid.SetColumn(right, 2);
            body.Children.Add(right);
            return body;
        }
    }

    public class ToolsPage : PageBase
    {
        UpdateInfo update;
        bool checking;

        public ToolsPage(MainWindow window) : base(window)
        {
        }

        public void SetUpdate(UpdateInfo info)
        {
            update = info;
        }

        protected override FrameworkElement Build()
        {
            string root = Updater.RepoRoot(Launcher.Dir);
            var updateStack = new StackPanel();
            if (root == null)
                updateStack.Children.Add(Ui.Text("이 폴더는 git으로 받은 저장소 안에 있지 않아 자동 업데이트를 할 수 없습니다. GitHub에서 새 버전을 받아 다시 빌드하세요.", "Caption"));
            else
            {
                string local = Updater.LocalCommit(root);
                updateStack.Children.Add(Ui.Text("지금 버전: " + (local != null ? local.Substring(0, Math.Min(7, local.Length)) : "?") + " (" + root + ")", "Body"));
                if (update == null && Window.LastUpdate != null)
                    update = Window.LastUpdate;
                if (checking)
                    updateStack.Children.Add(Ui.Text("확인하는 중...", "Caption"));
                else if (update != null)
                {
                    if (update.Error != null)
                        updateStack.Children.Add(Ui.Text("확인하지 못했습니다: " + update.Error, "Caption"));
                    else if (update.Behind == 0)
                        updateStack.Children.Add(Ui.Chip("", "최신입니다", Ui.Brush("Good")));
                    else
                    {
                        updateStack.Children.Add(Ui.Chip("", "새 커밋 " + update.Behind + "개", Ui.Brush("Accent")));
                        foreach (string message in update.Messages.Take(8))
                            updateStack.Children.Add(Ui.Text("· " + message, "Caption"));
                    }
                }
                var buttons = Ui.Row(Ui.Button("업데이트 확인", delegate
                {
                    checking = true;
                    Window.Refresh();
                    Updater.CheckAsync(Launcher, delegate(UpdateInfo info)
                    {
                        checking = false;
                        update = info;
                        Window.LastUpdate = info;
                        Window.ShowUpdateBadge(info);
                        Window.Refresh();
                    });
                }, null, "", new Thickness(0, 12, 8, 0)));
                if (update != null && update.Behind > 0)
                    buttons.Children.Add(Ui.Button("업데이트 설치", delegate
                    {
                        Window.Confirm("업데이트 설치", "런처를 닫고, 명령 창에서 GitHub의 새 코드를 받아(git pull) 게임 코드와 실행 파일을 다시 빌드합니다. 바뀐 만큼 몇 분에서 수십 분 걸리고, 끝나면 런처가 다시 열립니다.", "설치", delegate
                        {
                            try
                            {
                                Updater.Install(Launcher, root);
                                Application.Current.Shutdown();
                            }
                            catch (Exception error)
                            {
                                Window.Error("업데이트를 시작하지 못했습니다", error);
                            }
                        });
                    }, "AccentButton", "", new Thickness(0, 12, 8, 0)));
                buttons.Children.Add(Ui.Button("GitHub 열기", delegate { Process.Start("https://github.com/" + Updater.Repository); }, null, "", new Thickness(0, 12, 0, 0)));
                updateStack.Children.Add(buttons);
            }

            string steam = SteamShortcut.SteamPath();
            return Ui.Page("도구", "바로가기, 업데이트, 폴더.",
                Ui.Card("바로가기",
                    Ui.Setting("바탕화면 바로가기", "더블클릭하면 런처 창 없이 바로 게임이 시작됩니다 (--play).", Ui.Button("만들기", delegate
                    {
                        try
                        {
                            string path = Shortcuts.Desktop(Launcher);
                            Window.Toast("바탕화면에 만들었습니다: " + Path.GetFileName(path));
                        }
                        catch (Exception error)
                        {
                            Window.Error("바로가기를 만들지 못했습니다", error);
                        }
                    }, null, "")),
                    Ui.Setting("Steam에 추가", steam == null ? "Steam이 설치되어 있지 않습니다." : "비Steam 게임으로 추가합니다 (--play, 홈의 표지 이미지를 Steam 그림으로). Steam을 끈 상태에서 눌러 주세요.",
                               Ui.Button("추가", delegate
                               {
                                   if (SteamShortcut.SteamRunning)
                                   {
                                       Window.ShowProblem("Steam이 켜져 있습니다. Steam을 완전히 종료한 다음 다시 눌러 주세요 (켜져 있으면 Steam이 바로가기 파일을 덮어씁니다).");
                                       return;
                                   }
                                   try
                                   {
                                       int users = SteamShortcut.Add(Launcher);
                                       Window.Dialog("", Ui.Brush("Good"), "Steam에 추가했습니다", "Steam 사용자 " + users + "명의 라이브러리에 추가했습니다. Steam을 켜면 \"" + SteamShortcut.AppName + "\"이 보입니다.", new DialogAction("확인", null, true));
                                   }
                                   catch (Exception error)
                                   {
                                       Window.Error("Steam에 추가하지 못했습니다", error);
                                   }
                               }, null, ""))),
                Ui.Card("업데이트", updateStack),
                Ui.Card("폴더",
                    Ui.Row(Ui.Button("게임 폴더", delegate { Ui.OpenFolder(Launcher.Dir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("메모리카드", delegate { Ui.OpenFolder(Launcher.CardsDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("치트 (mods)", delegate { Ui.OpenFolder(Launcher.ModsDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("로그", delegate { Ui.OpenFolder(Launcher.LogsDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("config.ini 열기", delegate { Process.Start("notepad.exe", "\"" + Launcher.ConfigPath + "\""); }, null, ""))),
                Ui.Card("정보",
                    Ui.Text("Make Baseball Team Game Project: プロ野球チームをつくろう！3 (SLPM-65853)의 PC 포트입니다. 비영리 팬 프로젝트이며 SEGA와 관계없습니다.", "Body"),
                    Ui.Text("게임과 그 안의 모든 데이터·코드의 권리는 SEGA에 있습니다. 이 프로그램에는 게임 데이터가 들어 있지 않고, 본인이 가진 디스크의 이미지로만 동작합니다.", "Caption"),
                    Ui.Text("기반: Urban Reign PC port (thiagordsfx), PS2Recomp, PCSX2의 GS. 라이선스 GPL-3.0.", "Caption")));
        }
    }

    public static class Shortcuts
    {
        // A .lnk on the desktop: the launcher with --play.
        public static string Desktop(Launcher launcher)
        {
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "YKTK3.lnk");
            Create(path, Path.Combine(launcher.Dir, "YKTK3 Launcher.exe"), "--play", launcher.Dir, "プロ野球チームをつくろう！3 (PC)");
            return path;
        }

        public static void Create(string path, string target, string arguments, string workingDirectory, string description)
        {
            Type shellType = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(shellType);
            object link = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { path });
            Type linkType = link.GetType();
            linkType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, link, new object[] { target });
            linkType.InvokeMember("Arguments", BindingFlags.SetProperty, null, link, new object[] { arguments });
            linkType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, link, new object[] { workingDirectory });
            linkType.InvokeMember("Description", BindingFlags.SetProperty, null, link, new object[] { description });
            string icon = Path.Combine(workingDirectory, Game.Exe);
            linkType.InvokeMember("IconLocation", BindingFlags.SetProperty, null, link, new object[] { (File.Exists(icon) ? icon : target) + ",0" });
            linkType.InvokeMember("Save", BindingFlags.InvokeMethod, null, link, null);
        }
    }

    // Steam's binary shortcuts.vdf (userdata\<id>\config): a map of maps; types 0 map, 1 string, 2 int32, 8 end.
    public static class SteamShortcut
    {
        public const string AppName = "プロ野球チームをつくろう！3";

        class Raw
        {
            public byte Type;
            public byte[] Bytes;
        }

        public static string SteamPath()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
                {
                    object path = key == null ? null : key.GetValue("SteamPath");
                    return path != null && Directory.Exists(path.ToString()) ? Path.GetFullPath(path.ToString()) : null;
                }
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static bool SteamRunning
        {
            get { return Process.GetProcessesByName("steam").Length > 0; }
        }

        static string ReadString(BinaryReader reader)
        {
            var bytes = new List<byte>();
            byte b;
            while ((b = reader.ReadByte()) != 0)
                bytes.Add(b);
            return Encoding.UTF8.GetString(bytes.ToArray());
        }

        static void WriteString(BinaryWriter writer, string text)
        {
            writer.Write(Encoding.UTF8.GetBytes(text));
            writer.Write((byte)0);
        }

        public static List<KeyValuePair<string, object>> ReadMap(BinaryReader reader)
        {
            var map = new List<KeyValuePair<string, object>>();
            while (true)
            {
                byte type = reader.ReadByte();
                if (type == 8)
                    return map;
                string name = ReadString(reader);
                object value;
                switch (type)
                {
                    case 0: value = ReadMap(reader); break;
                    case 1: value = ReadString(reader); break;
                    case 2: value = reader.ReadInt32(); break;
                    case 3: value = new Raw { Type = type, Bytes = reader.ReadBytes(4) }; break;
                    case 7:
                    case 10: value = new Raw { Type = type, Bytes = reader.ReadBytes(8) }; break;
                    default: throw new InvalidDataException("shortcuts.vdf: 모르는 형식 " + type);
                }
                map.Add(new KeyValuePair<string, object>(name, value));
            }
        }

        public static void WriteMap(BinaryWriter writer, List<KeyValuePair<string, object>> map)
        {
            foreach (KeyValuePair<string, object> item in map)
            {
                var raw = item.Value as Raw;
                if (item.Value is List<KeyValuePair<string, object>>)
                {
                    writer.Write((byte)0);
                    WriteString(writer, item.Key);
                    WriteMap(writer, (List<KeyValuePair<string, object>>)item.Value);
                }
                else if (item.Value is string)
                {
                    writer.Write((byte)1);
                    WriteString(writer, item.Key);
                    WriteString(writer, (string)item.Value);
                }
                else if (item.Value is int)
                {
                    writer.Write((byte)2);
                    WriteString(writer, item.Key);
                    writer.Write((int)item.Value);
                }
                else if (raw != null)
                {
                    writer.Write(raw.Type);
                    WriteString(writer, item.Key);
                    writer.Write(raw.Bytes);
                }
            }
            writer.Write((byte)8);
        }

        static object Get(List<KeyValuePair<string, object>> map, string key)
        {
            foreach (KeyValuePair<string, object> item in map)
                if (string.Equals(item.Key, key, StringComparison.OrdinalIgnoreCase))
                    return item.Value;
            return null;
        }

        static uint Crc32(byte[] data)
        {
            uint crc = 0xFFFFFFFFu;
            foreach (byte b in data)
            {
                crc ^= b;
                for (int k = 0; k < 8; k++)
                    crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            }
            return ~crc;
        }

        // Adds (or updates) the launcher with --play in every Steam user's shortcuts; the cover image as its artwork.
        public static int Add(Launcher launcher)
        {
            string steam = SteamPath();
            if (steam == null)
                throw new InvalidOperationException("Steam을 찾지 못했습니다.");
            string exe = "\"" + Path.Combine(launcher.Dir, "YKTK3 Launcher.exe") + "\"";
            uint appId = Crc32(Encoding.UTF8.GetBytes(exe + AppName)) | 0x80000000u;
            string cover = launcher.State("CoverImage", "");
            int users = 0;
            string userdata = Path.Combine(steam, "userdata");
            if (!Directory.Exists(userdata))
                throw new InvalidOperationException("Steam에 로그인한 사용자가 없습니다 (userdata 폴더 없음).");
            foreach (string user in Directory.GetDirectories(userdata).Where(d => Path.GetFileName(d).All(char.IsDigit) && Path.GetFileName(d) != "0"))
            {
                string config = Path.Combine(user, "config");
                Directory.CreateDirectory(config);
                string file = Path.Combine(config, "shortcuts.vdf");
                List<KeyValuePair<string, object>> root;
                if (File.Exists(file))
                {
                    File.Copy(file, file + ".yktk3-backup", true);
                    using (var reader = new BinaryReader(File.OpenRead(file)))
                        root = ReadMap(reader);
                }
                else
                    root = new List<KeyValuePair<string, object>>();
                var shortcuts = Get(root, "shortcuts") as List<KeyValuePair<string, object>>;
                if (shortcuts == null)
                {
                    shortcuts = new List<KeyValuePair<string, object>>();
                    root.Add(new KeyValuePair<string, object>("shortcuts", shortcuts));
                }
                var entry = new List<KeyValuePair<string, object>>
                {
                    new KeyValuePair<string, object>("appid", unchecked((int)appId)),
                    new KeyValuePair<string, object>("AppName", AppName),
                    new KeyValuePair<string, object>("Exe", exe),
                    new KeyValuePair<string, object>("StartDir", "\"" + launcher.Dir + "\\\""),
                    new KeyValuePair<string, object>("icon", Path.Combine(launcher.Dir, Game.Exe)),
                    new KeyValuePair<string, object>("ShortcutPath", ""),
                    new KeyValuePair<string, object>("LaunchOptions", "--play"),
                    new KeyValuePair<string, object>("IsHidden", 0),
                    new KeyValuePair<string, object>("AllowDesktopConfig", 1),
                    new KeyValuePair<string, object>("AllowOverlay", 1),
                    new KeyValuePair<string, object>("OpenVR", 0),
                    new KeyValuePair<string, object>("Devkit", 0),
                    new KeyValuePair<string, object>("DevkitGameID", ""),
                    new KeyValuePair<string, object>("DevkitOverrideAppID", 0),
                    new KeyValuePair<string, object>("LastPlayTime", 0),
                    new KeyValuePair<string, object>("FlatpakAppID", ""),
                    new KeyValuePair<string, object>("tags", new List<KeyValuePair<string, object>>()),
                };
                int index = shortcuts.FindIndex(s => s.Value is List<KeyValuePair<string, object>> &&
                                                     string.Equals(Get((List<KeyValuePair<string, object>>)s.Value, "Exe") as string, exe, StringComparison.OrdinalIgnoreCase));
                if (index >= 0)
                    shortcuts[index] = new KeyValuePair<string, object>(shortcuts[index].Key, entry);
                else
                    shortcuts.Add(new KeyValuePair<string, object>(shortcuts.Count.ToString(), entry));
                using (var writer = new BinaryWriter(File.Create(file)))
                {
                    WriteMap(writer, root);
                }
                if (cover.Length > 0 && File.Exists(cover))
                {
                    string grid = Path.Combine(config, "grid");
                    Directory.CreateDirectory(grid);
                    string ext = Path.GetExtension(cover);
                    foreach (string suffix in new[] { "p", "", "_hero" })
                        File.Copy(cover, Path.Combine(grid, appId + suffix + ext), true);
                }
                users++;
            }
            return users;
        }
    }

    public class UpdateInfo
    {
        public string Local, Remote, Error;
        public int Behind;
        public List<string> Messages = new List<string>();
    }

    public static class Updater
    {
        public const string Repository = "PrimeTimerer/Make-Baseball-Team-Game-Project";

        // The git checkout the game folder is in (build\port\bin is three folders below it), or null.
        public static string RepoRoot(string dir)
        {
            for (int up = 0; up <= 5 && dir != null; up++, dir = Path.GetDirectoryName(dir))
                if (Directory.Exists(Path.Combine(dir, ".git")))
                    return dir;
            return null;
        }

        public static string LocalCommit(string root)
        {
            try
            {
                string git = Path.Combine(root, ".git");
                string head = File.ReadAllText(Path.Combine(git, "HEAD")).Trim();
                if (!head.StartsWith("ref:"))
                    return head;
                string reference = head.Substring(4).Trim();
                string loose = Path.Combine(git, reference.Replace('/', '\\'));
                if (File.Exists(loose))
                    return File.ReadAllText(loose).Trim();
                string packed = Path.Combine(git, "packed-refs");
                if (File.Exists(packed))
                    foreach (string line in File.ReadAllLines(packed))
                        if (line.EndsWith(" " + reference))
                            return line.Substring(0, 40);
            }
            catch (Exception)
            {
            }
            return null;
        }

        static string Get(string url)
        {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072; // TLS 1.2
            using (var web = new WebClient())
            {
                web.Headers[HttpRequestHeader.UserAgent] = "YKTK3-Launcher";
                web.Headers[HttpRequestHeader.Accept] = "application/vnd.github+json";
                web.Encoding = Encoding.UTF8;
                return web.DownloadString(url);
            }
        }

        public static UpdateInfo Check(Launcher launcher)
        {
            var info = new UpdateInfo();
            try
            {
                string root = RepoRoot(launcher.Dir);
                info.Local = root == null ? null : LocalCommit(root);
                if (info.Local == null)
                {
                    info.Error = "git 저장소가 아닙니다";
                    return info;
                }
                var json = new JavaScriptSerializer();
                try
                {
                    var compare = json.Deserialize<Dictionary<string, object>>(Get("https://api.github.com/repos/" + Repository + "/compare/" + info.Local + "...main"));
                    info.Behind = Convert.ToInt32(compare["ahead_by"]);
                    var commits = compare["commits"] as ArrayList;
                    if (commits != null)
                        foreach (Dictionary<string, object> c in commits.Cast<Dictionary<string, object>>().Reverse())
                            info.Messages.Add(((string)((Dictionary<string, object>)c["commit"])["message"]).Split('\n')[0]);
                }
                catch (WebException)
                {
                    // the local commit is not on GitHub (local work): compare with the newest commit only
                    var head = json.Deserialize<Dictionary<string, object>>(Get("https://api.github.com/repos/" + Repository + "/commits/main"));
                    info.Remote = (string)head["sha"];
                    info.Behind = info.Remote == info.Local ? 0 : 1;
                    if (info.Behind > 0)
                        info.Messages.Add(((string)((Dictionary<string, object>)head["commit"])["message"]).Split('\n')[0]);
                }
            }
            catch (Exception error)
            {
                info.Error = error.Message;
            }
            return info;
        }

        public static void CheckAsync(Launcher launcher, Action<UpdateInfo> done)
        {
            var dispatcher = Application.Current.Dispatcher;
            new Thread(delegate()
            {
                UpdateInfo info = Check(launcher);
                dispatcher.BeginInvoke(done, info);
            }) { IsBackground = true }.Start();
        }

        // A console window that pulls (pull) and rebuilds, then opens the launcher again (the launcher closes first,
        // so its file can be rebuilt).
        public static void Install(Launcher launcher, string root, bool pull = true)
        {
            string script = Path.Combine(Path.GetTempPath(), "yktk3-update.cmd");
            string launcherExe = Path.Combine(launcher.Dir, "YKTK3 Launcher.exe");
            var text = new StringBuilder();
            text.AppendLine("@echo off");
            text.AppendLine("chcp 65001 > nul");
            text.AppendLine("title YKTK3 update");
            text.AppendLine(pull ? "echo YKTK3: GitHub에서 새 코드를 받고 다시 빌드합니다." : "echo YKTK3: 치트를 포함해 게임 코드를 다시 만들고 빌드합니다.");
            text.AppendLine("timeout /t 2 /nobreak > nul");
            text.AppendLine("cd /d \"" + root + "\"");
            if (pull)
                text.AppendLine("git pull --ff-only || goto failed");
            text.AppendLine("py -3 build.py generate || goto failed");
            text.AppendLine("py -3 build.py build || goto failed");
            text.AppendLine("echo.");
            text.AppendLine("echo 업데이트를 마쳤습니다. 런처를 다시 엽니다.");
            text.AppendLine("start \"\" \"" + launcherExe + "\"");
            text.AppendLine("exit /b 0");
            text.AppendLine(":failed");
            text.AppendLine("echo.");
            text.AppendLine("echo 업데이트 중 오류가 났습니다. 위 메시지를 확인해 주세요.");
            text.AppendLine("pause");
            File.WriteAllText(script, text.ToString(), new UTF8Encoding(false));
            Process.Start(new ProcessStartInfo("cmd.exe", "/c \"" + script + "\"") { UseShellExecute = true, WorkingDirectory = root });
        }
    }
}
