// YKTK3 PC launcher: WPF on the .NET Framework 4 that is part of Windows, built by build.py with its C# compiler (C# 5:
// no string interpolation, no ?.). It edits the game's settings files next to it (config.ini, controls.ini), manages the
// memory card, cheats, logs and shortcuts, and starts yktk3.exe.
//   YKTK3 Launcher.exe          the launcher window
//   YKTK3 Launcher.exe --play   starts the game at once (desktop and Steam shortcuts); a window only when it fails
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Markup;
using System.Windows.Threading;
using Microsoft.Win32;

namespace Yktk3Launcher
{
    static class Program
    {
        [DllImport("user32.dll")]
        static extern bool SetProcessDPIAware();

        [STAThread]
        static int Main(string[] args)
        {
            try
            {
                SetProcessDPIAware();
            }
            catch (Exception)
            {
            }
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            using (Stream theme = typeof(Program).Assembly.GetManifestResourceStream("Theme.xaml"))
                app.Resources = (ResourceDictionary)XamlReader.Load(theme);
            var launcher = new Launcher(AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/'));
            bool play = args.Any(a => a.Equals("--play", StringComparison.OrdinalIgnoreCase));
            MainWindow window = null;
            Func<MainWindow> showWindow = delegate
            {
                if (window == null)
                {
                    window = new MainWindow(launcher);
                    window.Closed += delegate { app.Shutdown(); };
                }
                window.Show();
                window.Activate();
                return window;
            };
            launcher.Runner.Finished += delegate(RunResult result)
            {
                if (window == null && result.Diagnosis == null)
                    app.Shutdown(); // --play: the game ended normally
                else
                    showWindow().GameFinished(result);
            };
            if (play)
            {
                string problem = launcher.Runner.Start(false);
                if (problem != null)
                    showWindow().ShowProblem(problem);
            }
            else
                showWindow();
            return app.Run();
        }
    }

    // The folder of yktk3.exe and the files the launcher reads and writes there. Each change reads the file again, so
    // what the game adds to it (settings of newer versions) is kept.
    public class Launcher
    {
        public readonly string Dir;
        public readonly GameRunner Runner;

        public Launcher(string dir)
        {
            Dir = dir;
            Game.WriteDefaultSettings(dir);
            Runner = new GameRunner(this);
        }

        public string ConfigPath { get { return Path.Combine(Dir, "config.ini"); } }
        public string ControlsPath { get { return Path.Combine(Dir, "controls.ini"); } }
        public string StatePath { get { return Path.Combine(Dir, "launcher.ini"); } }
        public string ModsDir { get { return Path.Combine(Dir, "mods"); } }
        public string LogsDir { get { return Path.Combine(Dir, "logs"); } }
        public string CardsDir { get { return Path.Combine(Dir, "memcards"); } }
        public string Exe { get { return Path.Combine(Dir, Game.Exe); } }

        public string Config(string section, string key, string fallback)
        {
            return new IniFile(ConfigPath).Get(section, key, fallback);
        }

        public void SetConfig(string section, string key, string value)
        {
            var ini = new IniFile(ConfigPath);
            ini.Set(section, key, value);
            ini.Save();
        }

        // launcher.ini: the launcher's own settings and play record.
        public string State(string key, string fallback)
        {
            return new IniFile(StatePath).Get("Launcher", key, fallback);
        }

        public void SetState(string key, string value)
        {
            var ini = new IniFile(StatePath);
            ini.Set("Launcher", key, value);
            ini.Save();
        }

        // The disc image the game will use: config.ini [Game] Iso, or the one it finds in a "game" folder.
        public string DiscImage
        {
            get
            {
                string iso = Config("Game", "Iso", "");
                return iso.Length > 0 ? iso : Disc.Find(Dir);
            }
        }

        public void AddPlayTime(TimeSpan time)
        {
            long seconds;
            long.TryParse(State("PlayTimeSeconds", "0"), out seconds);
            SetState("PlayTimeSeconds", (seconds + (long)time.TotalSeconds).ToString());
            SetState("LastPlayed", DateTime.Now.ToString("yyyy-MM-dd HH:mm"));
        }
    }

    // A settings file edited in place: comments, order and every other setting stay as they are.
    public class IniFile
    {
        // yktk3.exe reads its files as narrow strings (the ANSI code page): paths with Korean letters are written so too.
        public static readonly Encoding FileEncoding = Encoding.Default.CodePage == 65001 ? new UTF8Encoding(false) : Encoding.Default;
        readonly string path;
        public readonly List<string> Lines = new List<string>();
        string newline = "\r\n";

        public IniFile(string path)
        {
            this.path = path;
            if (!File.Exists(path))
                return;
            string text = File.ReadAllText(path, FileEncoding);
            newline = text.Contains("\r\n") || !text.Contains("\n") ? "\r\n" : "\n";
            Lines.AddRange(text.Split('\n').Select(line => line.TrimEnd('\r')));
            if (Lines.Count > 0 && Lines[Lines.Count - 1].Length == 0)
                Lines.RemoveAt(Lines.Count - 1); // the final newline
        }

        public static string SectionName(string line)
        {
            string t = line.Trim();
            return t.StartsWith("[") && t.EndsWith("]") ? t.Substring(1, t.Length - 2).Trim() : null;
        }

        // [start, end) of a section's lines after its header; start = -1 when there is no such section.
        public void SectionRange(string section, out int start, out int end)
        {
            start = end = -1;
            for (int i = 0; i < Lines.Count; i++)
            {
                string name = SectionName(Lines[i]);
                if (name == null)
                    continue;
                if (start >= 0)
                {
                    end = i;
                    return;
                }
                if (string.Equals(name, section, StringComparison.OrdinalIgnoreCase))
                    start = i + 1;
            }
            if (start >= 0)
                end = Lines.Count;
        }

        public static bool KeyValue(string line, out string key, out string value)
        {
            key = value = null;
            string t = line.Trim();
            int equals = t.IndexOf('=');
            if (t.Length == 0 || t.StartsWith("#") || t.StartsWith(";") || equals <= 0)
                return false;
            key = t.Substring(0, equals).Trim();
            value = t.Substring(equals + 1).Trim();
            return true;
        }

        int Find(string section, string key, out int insertAt)
        {
            int start, end;
            SectionRange(section, out start, out end);
            insertAt = start < 0 ? -1 : start;
            for (int i = start; i >= 0 && i < end; i++)
            {
                string k, v;
                if (!KeyValue(Lines[i], out k, out v))
                    continue;
                insertAt = i + 1;
                if (string.Equals(k, key, StringComparison.OrdinalIgnoreCase))
                    return i;
            }
            return -1;
        }

        public string Get(string section, string key, string fallback)
        {
            int insertAt;
            int i = Find(section, key, out insertAt);
            string k, v;
            return i >= 0 && KeyValue(Lines[i], out k, out v) ? v : fallback;
        }

        public void Set(string section, string key, string value)
        {
            int insertAt;
            int i = Find(section, key, out insertAt);
            if (i >= 0)
            {
                int equals = Lines[i].IndexOf('=');
                if (Lines[i].Substring(equals + 1).Trim() != value)
                    Lines[i] = Lines[i].Substring(0, equals + 1) + (Lines[i].Substring(equals - 1, 1) == " " ? " " : "") + value;
                return;
            }
            if (insertAt < 0)
            {
                if (Lines.Count > 0 && Lines[Lines.Count - 1].Trim().Length > 0)
                    Lines.Add("");
                Lines.Add("[" + section + "]");
                insertAt = Lines.Count;
            }
            Lines.Insert(insertAt, key + "=" + value);
        }

        public void Save()
        {
            File.WriteAllText(path, string.Join(newline, Lines) + newline, FileEncoding);
        }
    }

    public static class Disc
    {
        const string BootElf = "SLPM_658.53";

        // An ISO9660 image with SLPM_658.53 in its root directory (yktk3.exe then checks the file's hash).
        public static bool HasGame(string path)
        {
            try
            {
                using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                {
                    var pvd = new byte[2048];
                    file.Position = 16 * 2048;
                    if (file.Read(pvd, 0, pvd.Length) != pvd.Length || Encoding.ASCII.GetString(pvd, 1, 5) != "CD001")
                        return false;
                    long lba = BitConverter.ToUInt32(pvd, 156 + 2);
                    var dir = new byte[Math.Min(BitConverter.ToUInt32(pvd, 156 + 10), 1u << 20)];
                    file.Position = lba * 2048;
                    int size = file.Read(dir, 0, dir.Length);
                    for (int pos = 0; pos + 33 < size; )
                    {
                        int length = dir[pos];
                        if (length == 0)
                        {
                            pos = (pos / 2048 + 1) * 2048; // records do not cross sectors
                            continue;
                        }
                        string name = Encoding.ASCII.GetString(dir, pos + 33, Math.Min(dir[pos + 32], size - pos - 33));
                        if (string.Equals(name.Split(';')[0], BootElf, StringComparison.OrdinalIgnoreCase))
                            return true;
                        pos += length;
                    }
                }
            }
            catch (Exception)
            {
            }
            return false;
        }

        // The image yktk3.exe takes when config.ini [Game] Iso is empty: the first .iso with the game in the "game"
        // folder next to it or up to three folders above (src/urban/game_disc.cpp findDiscImage).
        public static string Find(string dir)
        {
            for (int up = 0; up <= 3 && dir != null; up++, dir = Path.GetDirectoryName(dir))
            {
                string folder = Path.Combine(dir, "game");
                if (!Directory.Exists(folder))
                    continue;
                foreach (string file in Directory.GetFiles(folder).OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
                    if (string.Equals(Path.GetExtension(file), ".iso", StringComparison.OrdinalIgnoreCase) && HasGame(file))
                        return file;
            }
            return null;
        }
    }

    public static class Game
    {
        public const string Exe = "yktk3.exe";

        public static bool IsRunning
        {
            get { return Process.GetProcessesByName(Path.GetFileNameWithoutExtension(Exe)).Length > 0; }
        }

        // Why the game cannot start, or null.
        public static string Problem(Launcher launcher)
        {
            if (!File.Exists(launcher.Exe))
                return "이 폴더에 " + Exe + "가 없습니다. 런처는 " + Exe + "와 같은 폴더에 있어야 합니다.";
            if (IsRunning)
                return "게임이 이미 실행 중입니다.";
            string iso = launcher.Config("Game", "Iso", "");
            if (iso.Length == 0)
                return Disc.Find(launcher.Dir) == null ? "디스크 이미지가 없습니다. game 폴더에 본인 SLPM-65853 디스크의 이미지(.iso)를 넣거나 홈에서 직접 고르세요." : null;
            if (!File.Exists(iso))
                return "디스크 이미지 파일이 없습니다: " + iso;
            return Disc.HasGame(iso) ? null : "SLPM-65853 디스크 이미지가 아닙니다: " + iso;
        }

        // A new config.ini/controls.ini with the game's own template and defaults (yktk3.exe --write-settings writes the
        // missing ones and exits). userDir: where to write them (URBAN_USER_DIR), null = the game's folder.
        public static void WriteDefaultSettings(string dir, string userDir = null)
        {
            if (userDir == null && File.Exists(Path.Combine(dir, "config.ini")) && File.Exists(Path.Combine(dir, "controls.ini")))
                return;
            if (!File.Exists(Path.Combine(dir, Exe)))
                return;
            try
            {
                var info = new ProcessStartInfo(Path.Combine(dir, Exe), "--write-settings") { WorkingDirectory = dir, UseShellExecute = false, CreateNoWindow = true };
                if (userDir != null)
                    info.EnvironmentVariables["URBAN_USER_DIR"] = userDir;
                using (Process process = Process.Start(info))
                    process.WaitForExit(15000);
            }
            catch (Exception)
            {
            }
        }
    }

    public class RunResult
    {
        public int ExitCode;
        public TimeSpan Ran;
        public string LogPath;
        public bool SafeMode;
        public Diagnosis Diagnosis;
    }

    // Starts yktk3.exe with its console hidden, writes what it prints to logs\yktk3-<time>.log and explains a failure.
    public class GameRunner
    {
        // Safe settings for one run (the game takes an environment variable over config.ini; nothing is written).
        public static readonly string[,] SafeSettings =
        {
            { "PS2X_GS_SCALE", "1" }, { "PS2X_PCSX2_TEXTURES", "0" }, { "PS2X_PCSX2_CAS", "0" }, { "PS2X_PCSX2_SHADEBOOST", "0" },
            { "PS2X_PCSX2_TV_SHADER", "0" }, { "PS2X_PCSX2_ANISOTROPY", "0" }, { "PS2X_PCSX2_BLENDING", "1" },
            { "PS2X_PCSX2_FXAA", "0" }, { "URBAN_START_FULLSCREEN", "0" }, { "URBAN_MODS", ";" }, { "URBAN_MOD_DLLS", ";" },
        };
        readonly Launcher launcher;
        Process process;
        StreamWriter log;
        string logPath;
        DateTime started;
        bool safeMode;
        DispatcherTimer flush;
        public event Action<RunResult> Finished;

        public GameRunner(Launcher launcher)
        {
            this.launcher = launcher;
        }

        public bool Running { get { return process != null; } }

        public string Start(bool safe)
        {
            if (process != null)
                return "게임이 이미 실행 중입니다.";
            string problem = Game.Problem(launcher);
            if (problem != null)
                return problem;
            if (launcher.State("AutoBackup", "1") == "1")
            {
                try
                {
                    CardStore.Backup(launcher, "auto");
                }
                catch (Exception)
                {
                }
            }
            Directory.CreateDirectory(launcher.LogsDir);
            Logs.Trim(launcher.LogsDir, 20);
            logPath = Path.Combine(launcher.LogsDir, "yktk3-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
            var info = new ProcessStartInfo(launcher.Exe)
            {
                WorkingDirectory = launcher.Dir, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.Default, StandardErrorEncoding = Encoding.Default,
            };
            if (safe)
                for (int i = 0; i < SafeSettings.GetLength(0); i++)
                    info.EnvironmentVariables[SafeSettings[i, 0]] = SafeSettings[i, 1];
            var writer = new StreamWriter(logPath, false, new UTF8Encoding(false));
            writer.WriteLine("# YKTK3 launcher log");
            writer.WriteLine("# started: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + (safe ? " (safe settings)" : ""));
            var p = new Process { StartInfo = info, EnableRaisingEvents = true };
            DataReceivedEventHandler write = delegate(object sender, DataReceivedEventArgs e)
            {
                if (e.Data != null)
                    lock (writer)
                        writer.WriteLine(e.Data);
            };
            p.OutputDataReceived += write;
            p.ErrorDataReceived += write;
            p.Exited += delegate { OnExited(p, writer); };
            try
            {
                p.Start();
            }
            catch (Exception error)
            {
                writer.Dispose();
                return "게임을 시작하지 못했습니다: " + error.Message;
            }
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            process = p;
            log = writer;
            started = DateTime.Now;
            safeMode = safe;
            flush = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            flush.Tick += delegate
            {
                lock (writer)
                    writer.Flush();
            };
            flush.Start();
            return null;
        }

        void OnExited(Process p, StreamWriter writer)
        {
            p.WaitForExit(); // the rest of the output
            int code = p.ExitCode;
            TimeSpan ran = DateTime.Now - started;
            lock (writer)
            {
                writer.WriteLine("# exit code: " + code + " (0x" + code.ToString("X8") + ")");
                writer.WriteLine("# play time: " + ran.ToString(@"hh\:mm\:ss"));
                writer.Dispose();
            }
            var result = new RunResult { ExitCode = code, Ran = ran, LogPath = logPath, SafeMode = safeMode };
            result.Diagnosis = Diagnosis.Of(code, Logs.Tail(logPath, 300));
            Application.Current.Dispatcher.BeginInvoke((Action)delegate
            {
                flush.Stop();
                log = null;
                process = null;
                p.Dispose();
                launcher.AddPlayTime(ran);
                if (Finished != null)
                    Finished(result);
            });
        }
    }

    // A failed run in plain words, from the end of its log and the exit code.
    public class Diagnosis
    {
        public string Title, Detail, Advice;
        public bool SafeModeHelps;

        static Diagnosis Make(string title, string detail, string advice, bool safeModeHelps)
        {
            return new Diagnosis { Title = title, Detail = detail, Advice = advice, SafeModeHelps = safeModeHelps };
        }

        public static Diagnosis Of(int code, string log)
        {
            string text = log ?? "";
            uint status = unchecked((uint)code);
            if (text.Contains("no disc image found"))
                return Make("디스크 이미지를 찾지 못했습니다", "게임이 game 폴더에서 SLPM-65853 디스크 이미지를 찾지 못했습니다.",
                            "홈에서 디스크 이미지를 직접 고르거나, game 폴더에 .iso 파일을 넣어 주세요.", false);
            if (text.Contains("boot executable hash mismatch"))
                return Make("다른 판의 디스크 이미지입니다", "디스크 안의 SLPM_658.53이 이 포트를 만든 디스크와 다릅니다.",
                            "SLPM-65853(일본판) 디스크에서 이미지를 다시 만들어 보세요.", false);
            if (text.Contains("cannot mount") || text.Contains("SYSTEM.CNF") || text.Contains("boot executable"))
                return Make("디스크 이미지를 읽지 못했습니다", LastError(text, "디스크 이미지가 손상되었거나 PS2 디스크가 아닙니다."),
                            "디스크에서 이미지를 다시 만들어 보세요.", false);
            if (Regex.IsMatch(text, "DXGI|D3D11|device removed|DEVICE_REMOVED|DEVICE_HUNG", RegexOptions.IgnoreCase))
                return Make("그래픽카드 오류로 끝났습니다", LastError(text, "그래픽 드라이버가 응답하지 않았습니다."),
                            "그래픽 드라이버를 최신으로 업데이트하고, 안전 설정(1배 해상도, HD 텍스처·필터 끔)으로 실행해 보세요.", true);
            if (text.Contains("bad_alloc") || status == 0xC0000017u)
                return Make("메모리가 부족했습니다", "게임이 메모리를 더 받지 못했습니다.",
                            "다른 프로그램을 닫고, 내부 해상도를 낮추거나 HD 텍스처를 꺼 보세요.", true);
            if (status == 0xC0000135u || status == 0xC0000139u || status == 0xC000007Bu)
                return Make("필요한 DLL이 없습니다", "게임 폴더의 DLL 또는 Visual C++ 런타임을 찾지 못했습니다 (0x" + status.ToString("X8") + ").",
                            "Microsoft Visual C++ 재배포 가능 패키지(x64)를 설치하고, 게임을 다시 빌드해 보세요.", false);
            if (text.Contains("fatal:") || text.Contains("runtime initialization failed") || text.Contains("failed to load"))
                return Make("게임을 시작하지 못했습니다", LastError(text, "종료 코드 " + code),
                            "로그의 마지막 오류를 확인해 주세요. 안전 설정으로 다시 실행해 볼 수 있습니다.", true);
            Match crash = Regex.Match(text, @"\[crash\] exception (0x[0-9a-fA-F]+) at ([^\r\n,]*)");
            if (!crash.Success && (status == 0xFFFFFFFFu || code == 1))
                return Make("게임이 강제로 종료되었습니다", "작업 관리자 등 밖에서 게임을 끝냈습니다 (종료 코드 " + code + ").",
                            "직접 끄신 것이 아니라면 로그를 확인해 주세요.", false);
            if (crash.Success || status >= 0xC0000000u)
                return Make("게임이 오류로 멈췄습니다",
                            crash.Success ? "예외 " + crash.Groups[1].Value + " (" + crash.Groups[2].Value.Trim() + ")" : "종료 코드 0x" + status.ToString("X8"),
                            "안전 설정으로 다시 실행해 보세요. 치트를 켰다면 끄고 해 보세요. 계속되면 로그를 함께 알려 주세요.", true);
            if (code != 0)
                return Make("게임이 오류로 끝났습니다", LastError(text, "종료 코드 " + code),
                            "로그에서 마지막 오류를 확인해 주세요. 안전 설정으로 다시 실행해 볼 수 있습니다.", true);
            return null;
        }

        static string LastError(string text, string fallback)
        {
            string line = text.Split('\n').Select(l => l.Trim()).LastOrDefault(l => Regex.IsMatch(l, "error|fatal|fail", RegexOptions.IgnoreCase) && !l.StartsWith("#"));
            return line ?? fallback;
        }
    }

    public class LogInfo
    {
        public string Path;
        public DateTime Started;
        public string ExitCode; // null while running or when the launcher was closed during the run
        public string PlayTime;
        public bool Safe;
    }

    public static class Logs
    {
        public static List<LogInfo> List(string dir)
        {
            var logs = new List<LogInfo>();
            if (!Directory.Exists(dir))
                return logs;
            foreach (string file in Directory.GetFiles(dir, "yktk3-*.log").OrderByDescending(f => f, StringComparer.Ordinal))
            {
                var info = new LogInfo { Path = file, Started = File.GetCreationTime(file) };
                try
                {
                    string head = Head(file);
                    Match started = Regex.Match(head, @"# started: ([0-9-]+ [0-9:]+)");
                    if (started.Success)
                        DateTime.TryParse(started.Groups[1].Value, out info.Started);
                    info.Safe = head.Contains("(safe settings)");
                    string tail = Tail(file, 4);
                    Match exit = Regex.Match(tail, @"# exit code: (-?[0-9]+)");
                    if (exit.Success)
                        info.ExitCode = exit.Groups[1].Value;
                    Match time = Regex.Match(tail, @"# play time: ([0-9:]+)");
                    if (time.Success)
                        info.PlayTime = time.Groups[1].Value;
                }
                catch (Exception)
                {
                }
                logs.Add(info);
            }
            return logs;
        }

        static string Head(string path)
        {
            using (var reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)))
            {
                var buffer = new char[400];
                int n = reader.Read(buffer, 0, buffer.Length);
                return new string(buffer, 0, n);
            }
        }

        // The last lines of a log (it may be large: only its end is read).
        public static string Tail(string path, int lines)
        {
            try
            {
                using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    long start = Math.Max(0, file.Length - 256 * 1024);
                    file.Position = start;
                    string text = new StreamReader(file, Encoding.UTF8).ReadToEnd();
                    string[] all = text.Split('\n');
                    return string.Join("\n", all.Skip(Math.Max(start > 0 ? 1 : 0, all.Length - lines)));
                }
            }
            catch (Exception)
            {
                return "";
            }
        }

        public static void Trim(string dir, int keep)
        {
            foreach (string old in Directory.GetFiles(dir, "yktk3-*.log").OrderByDescending(f => f, StringComparer.Ordinal).Skip(keep))
            {
                try
                {
                    File.Delete(old);
                }
                catch (Exception)
                {
                }
            }
        }
    }

    // The memory card file the game uses (memcards\Mcd001.ps2) and its backups (memcards\backups).
    public static class CardStore
    {
        public static string CardPath(Launcher launcher)
        {
            return Path.Combine(launcher.CardsDir, "Mcd001.ps2");
        }

        public static string BackupsDir(Launcher launcher)
        {
            return Path.Combine(launcher.CardsDir, "backups");
        }

        // A copy of the card as backups\Mcd001-<time>-<why>.ps2; the oldest beyond the kept number are removed.
        public static string Backup(Launcher launcher, string why)
        {
            string card = CardPath(launcher);
            if (!File.Exists(card))
                return null;
            string dir = BackupsDir(launcher);
            Directory.CreateDirectory(dir);
            string target = Path.Combine(dir, "Mcd001-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + why + ".ps2");
            File.Copy(card, target, true);
            int keep;
            if (!int.TryParse(launcher.State("BackupsToKeep", "10"), out keep) || keep < 1)
                keep = 10;
            foreach (string old in Directory.GetFiles(dir, "Mcd001-*.ps2").OrderByDescending(f => f, StringComparer.Ordinal).Skip(keep))
                File.Delete(old);
            return target;
        }
    }

    // What the PC has, for the recommended settings (registry and Win32 only: quick, nothing installed).
    public class PcInfo
    {
        public string Cpu = "알 수 없음";
        public int Threads = Environment.ProcessorCount;
        public long Ram;
        public string Gpu = "알 수 없음";
        public long Vram;
        public int ScreenWidth, ScreenHeight;

        [StructLayout(LayoutKind.Sequential)]
        class MemoryStatus
        {
            public uint Length = 64;
            public uint Load;
            public ulong TotalPhys, AvailPhys, TotalPageFile, AvailPageFile, TotalVirtual, AvailVirtual, AvailExtendedVirtual;
        }

        [DllImport("kernel32.dll")]
        static extern bool GlobalMemoryStatusEx([In, Out] MemoryStatus status);

        [DllImport("user32.dll")]
        static extern int GetSystemMetrics(int index);

        public static PcInfo Read()
        {
            var pc = new PcInfo();
            try
            {
                using (RegistryKey cpu = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0"))
                    if (cpu != null && cpu.GetValue("ProcessorNameString") != null)
                        pc.Cpu = Regex.Replace(cpu.GetValue("ProcessorNameString").ToString(), @"\s+", " ").Trim();
            }
            catch (Exception)
            {
            }
            var memory = new MemoryStatus();
            if (GlobalMemoryStatusEx(memory))
                pc.Ram = (long)memory.TotalPhys;
            pc.ScreenWidth = GetSystemMetrics(0);
            pc.ScreenHeight = GetSystemMetrics(1);
            // Display adapters: the one with the most video memory (the dedicated card on laptops with two).
            try
            {
                using (RegistryKey adapters = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"))
                {
                    foreach (string name in adapters.GetSubKeyNames())
                    {
                        try
                        {
                            using (RegistryKey adapter = adapters.OpenSubKey(name))
                            {
                                object desc = adapter == null ? null : adapter.GetValue("DriverDesc");
                                if (desc == null || desc.ToString().Contains("Basic") || desc.ToString().Contains("Remote"))
                                    continue;
                                long vram = 0;
                                object q = adapter.GetValue("HardwareInformation.qwMemorySize");
                                object d = adapter.GetValue("HardwareInformation.MemorySize");
                                if (q is long)
                                    vram = (long)q;
                                else if (d is int)
                                    vram = (uint)(int)d;
                                else if (d is byte[] && ((byte[])d).Length >= 4)
                                    vram = BitConverter.ToUInt32((byte[])d, 0);
                                if (vram > pc.Vram || pc.Gpu == "알 수 없음")
                                {
                                    pc.Gpu = desc.ToString();
                                    pc.Vram = vram;
                                }
                            }
                        }
                        catch (Exception)
                        {
                        }
                    }
                }
            }
            catch (Exception)
            {
            }
            return pc;
        }

        public bool WeakGpu
        {
            get { return Vram < 2560L << 20 || (Gpu.Contains("Intel") && !Gpu.Contains("Arc")); }
        }

        // The internal resolution for this monitor (448 lines per step), capped by the graphics card.
        public int RecommendedScale(out string why)
        {
            int byScreen = Math.Max(1, (int)Math.Ceiling(ScreenHeight / 448.0));
            int cap = WeakGpu ? 2 : Vram < 4608L << 20 ? 3 : Vram < 7168L << 20 ? 4 : 6;
            int scale = Math.Min(8, Math.Min(byScreen, cap));
            why = "모니터 " + ScreenWidth + "×" + ScreenHeight + "에는 " + byScreen + "배" +
                  (byScreen > cap ? ", 그래픽카드(" + Size(Vram) + ")에 맞춰 " + scale + "배" : "") + "가 알맞습니다.";
            return scale;
        }

        public static string Size(long bytes)
        {
            if (bytes <= 0)
                return "?";
            return bytes >= 1L << 30 ? (bytes / (double)(1L << 30)).ToString("0.#") + " GB" : (bytes >> 20) + " MB";
        }
    }
}
