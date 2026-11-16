// A release folder on any PC: the disc image found wherever it is (and remembered for every copy of the launcher on this
// PC, so a new version unpacked elsewhere finds it at once), the Korean patch installed from its ZIP or switched off,
// and new releases on GitHub (how to update: unpack the new ZIP over the folder; it carries no user files).
//   YKTK3 Launcher.exe --diagnose <report.txt>      what the launcher finds (version, disc, Korean patch, updates)
//   YKTK3 Launcher.exe --install-korean <zip>        unpacks a Korean patch ZIP next to the game
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Web.Script.Serialization;

namespace Yktk3Launcher
{
    // %LOCALAPPDATA%\YKTK3: shared by every copy of the launcher on this PC.
    public static class SharedRecord
    {
        static string Dir
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "YKTK3"); }
        }

        public static string Read(string name)
        {
            try
            {
                string path = Path.Combine(Dir, name);
                return File.Exists(path) ? File.ReadAllText(path, Encoding.UTF8).Trim() : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static void Write(string name, string value)
        {
            try
            {
                Directory.CreateDirectory(Dir);
                File.WriteAllText(Path.Combine(Dir, name), value, new UTF8Encoding(false));
            }
            catch (Exception)
            {
            }
        }
    }

    public static class DiscSearch
    {
        // The disc image another copy of the launcher found or was given.
        public static string Remembered()
        {
            string path = SharedRecord.Read("disc.txt");
            return !string.IsNullOrEmpty(path) && File.Exists(path) && Disc.HasGame(path) ? path : null;
        }

        public static void Remember(string path)
        {
            if (!string.IsNullOrEmpty(path) && path != SharedRecord.Read("disc.txt"))
                SharedRecord.Write("disc.txt", path);
        }

        static readonly string[] Skip = { "Windows", "Program Files", "Program Files (x86)", "ProgramData", "$Recycle.Bin",
                                          "System Volume Information", "AppData", "Recovery", "PerfLogs", "MSOCache",
                                          "node_modules", "$WinREAgent", "OneDriveTemp" };

        // Wider than Disc.Find (the "game" folders): the record above, then the desktop, downloads and documents, then
        // every fixed drive, each two folders deep. The first image with the game, or null. progress: the folder looked at.
        public static string Search(Action<string> progress)
        {
            string found = Remembered();
            if (found != null)
                return found;
            var roots = new List<string>
            {
                Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"),
                Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)
            };
            foreach (DriveInfo drive in DriveInfo.GetDrives())
            {
                try
                {
                    if (drive.DriveType == DriveType.Fixed && drive.IsReady)
                        roots.Add(drive.RootDirectory.FullName);
                }
                catch (Exception)
                {
                }
            }
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string root in roots)
            {
                string hit = Walk(root, 2, seen, progress);
                if (hit != null)
                    return hit;
            }
            return null;
        }

        static string Walk(string dir, int depth, HashSet<string> seen, Action<string> progress)
        {
            if (string.IsNullOrEmpty(dir) || !seen.Add(dir.TrimEnd('\\')))
                return null;
            if (progress != null)
                progress(dir);
            try
            {
                foreach (string file in Directory.GetFiles(dir, "*.iso"))
                    if (string.Equals(Path.GetExtension(file), ".iso", StringComparison.OrdinalIgnoreCase) && Disc.HasGame(file))
                        return file;
                if (depth == 0)
                    return null;
                foreach (string sub in Directory.GetDirectories(dir))
                {
                    string name = Path.GetFileName(sub);
                    if (name.StartsWith(".") || Skip.Any(s => string.Equals(s, name, StringComparison.OrdinalIgnoreCase)))
                        continue;
                    if ((File.GetAttributes(sub) & (FileAttributes.System | FileAttributes.ReparsePoint)) != 0)
                        continue;
                    string hit = Walk(sub, depth - 1, seen, progress);
                    if (hit != null)
                        return hit;
                }
            }
            catch (Exception)
            {
            }
            return null;
        }
    }

    // The Korean patch: a "korean" folder next to yktk3.exe (the game reads it by itself); "korean.off" keeps it switched off.
    public static class KoreanPatch
    {
        public static string Folder(Launcher launcher) { return Path.Combine(launcher.Dir, "korean"); }
        public static string OffFolder(Launcher launcher) { return Path.Combine(launcher.Dir, "korean.off"); }

        static bool Has(string folder)
        {
            return File.Exists(Path.Combine(folder, "RAM_PATCH.TXT")) || Directory.Exists(Path.Combine(folder, "DATA.CVM"));
        }

        public static bool On(Launcher launcher) { return Has(Folder(launcher)); }
        public static bool Off(Launcher launcher) { return !On(launcher) && Has(OffFolder(launcher)); }

        // VERSION.txt of the installed patch (patches before 2026.11.16 have none).
        public static string Version(Launcher launcher)
        {
            string path = Path.Combine(On(launcher) ? Folder(launcher) : OffFolder(launcher), "VERSION.txt");
            try
            {
                return File.Exists(path) ? File.ReadAllText(path).Trim() : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        // A downloaded YKTK3-Korean-<version>.zip next to the launcher, one folder above, in Downloads or on the desktop:
        // the newest.
        public static string FindZip(Launcher launcher)
        {
            var places = new[]
            {
                launcher.Dir, Path.GetDirectoryName(launcher.Dir),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"),
                Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)
            };
            string best = null;
            foreach (string place in places)
            {
                try
                {
                    if (string.IsNullOrEmpty(place) || !Directory.Exists(place))
                        continue;
                    foreach (string file in Directory.GetFiles(place, "YKTK3-Korean-*.zip"))
                        if (best == null || string.CompareOrdinal(Path.GetFileName(file), Path.GetFileName(best)) > 0)
                            best = file;
                }
                catch (Exception)
                {
                }
            }
            return best;
        }

        public static string ZipVersion(string zip)
        {
            string name = Path.GetFileNameWithoutExtension(zip);
            return name.StartsWith("YKTK3-Korean-", StringComparison.OrdinalIgnoreCase) ? name.Substring(13) : null;
        }

        // Unpacks the ZIP's korean folder next to the game in place of the old one (switched on or off). Textures the
        // player collected (korean\textures\dumps) move to the new folder.
        public static void Install(Launcher launcher, string zip)
        {
            string staging = Path.Combine(launcher.Dir, "korean.new");
            if (Directory.Exists(staging))
                Directory.Delete(staging, true);
            Directory.CreateDirectory(staging);
            int count = 0;
            using (ZipArchive archive = ZipFile.OpenRead(zip))
            {
                foreach (ZipArchiveEntry entry in archive.Entries)
                {
                    string name = entry.FullName.Replace('\\', '/');
                    if (!name.StartsWith("korean/", StringComparison.OrdinalIgnoreCase) || name.EndsWith("/"))
                        continue;
                    string relative = name.Substring("korean/".Length);
                    if (relative.Split('/').Any(part => part == ".."))
                        continue;
                    string target = Path.Combine(staging, relative.Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    entry.ExtractToFile(target, true);
                    count++;
                }
            }
            if (count == 0 || !Has(staging))
            {
                Directory.Delete(staging, true);
                throw new InvalidDataException("이 ZIP에는 한글 패치(korean 폴더)가 없습니다: " + Path.GetFileName(zip));
            }
            string version = ZipVersion(zip);
            if (version != null && !File.Exists(Path.Combine(staging, "VERSION.txt")))
                File.WriteAllText(Path.Combine(staging, "VERSION.txt"), version);
            foreach (string old in new[] { Folder(launcher), OffFolder(launcher) })
            {
                if (!Directory.Exists(old))
                    continue;
                string dumps = Path.Combine(old, "textures", "dumps");
                string keep = Path.Combine(staging, "textures", "dumps");
                if (Directory.Exists(dumps) && !Directory.Exists(keep))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(keep));
                    Directory.Move(dumps, keep);
                }
                Directory.Delete(old, true);
            }
            Directory.Move(staging, Folder(launcher));
        }

        // Korean on (korean) or off (korean.off: the game plays in Japanese).
        public static void Switch(Launcher launcher, bool on)
        {
            if (on && Off(launcher))
                Directory.Move(OffFolder(launcher), Folder(launcher));
            else if (!on && On(launcher))
            {
                if (Directory.Exists(OffFolder(launcher)))
                    Directory.Delete(OffFolder(launcher), true);
                Directory.Move(Folder(launcher), OffFolder(launcher));
            }
        }
    }

    public class ReleaseUpdate
    {
        public string Local, Latest, Page, Error;
        public bool Newer { get { return Error == null && Latest != null && Local != null && string.CompareOrdinal(Latest, Local) > 0; } }
    }

    // Releases are tagged yktk3-<yyyy.mm.dd>; a release folder has VERSION.txt with that date (a build folder has none).
    public static class Releases
    {
        public static string Version(string dir)
        {
            string path = Path.Combine(dir, "VERSION.txt");
            try
            {
                return File.Exists(path) ? File.ReadAllText(path).Trim() : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static string Page { get { return "https://github.com/" + Updater.Repository + "/releases"; } }

        public static ReleaseUpdate Check(string dir)
        {
            var info = new ReleaseUpdate { Local = Version(dir) };
            try
            {
                var list = new JavaScriptSerializer().Deserialize<ArrayList>(Updater.Get("https://api.github.com/repos/" + Updater.Repository + "/releases?per_page=30"));
                foreach (Dictionary<string, object> release in list.Cast<Dictionary<string, object>>())
                {
                    string tag = release["tag_name"] as string;
                    bool skip = (release.ContainsKey("draft") && release["draft"] is bool && (bool)release["draft"]) ||
                                (release.ContainsKey("prerelease") && release["prerelease"] is bool && (bool)release["prerelease"]);
                    if (tag == null || !tag.StartsWith("yktk3-") || skip)
                        continue;
                    string version = tag.Substring("yktk3-".Length);
                    if (info.Latest == null || string.CompareOrdinal(version, info.Latest) > 0)
                    {
                        info.Latest = version;
                        info.Page = release["html_url"] as string;
                    }
                }
                if (info.Latest == null)
                    info.Error = "GitHub에 릴리스가 없습니다";
            }
            catch (Exception error)
            {
                info.Error = error.Message;
            }
            return info;
        }

        public static string HowToUpdate(Launcher launcher, ReleaseUpdate info)
        {
            string version = info != null && info.Latest != null ? info.Latest : "<새 버전>";
            return "1. 다운로드 페이지에서 YKTK3-PC-" + version + ".zip 을 받습니다. 한국어로 하면 YKTK3-Korean-" + version + ".zip 도 받습니다.\n" +
                   "2. 게임과 런처를 끄고, 받은 ZIP 안의 YKTK3-PC-" + version + " 폴더 내용을 지금 폴더에 그대로 덮어씁니다:\n   " + launcher.Dir + "\n" +
                   "   세이브(memcards), 빠른 저장(states), 설정(config.ini, controls.ini), 세이브별 치트 설정은 ZIP에 없어서 그대로 남습니다.\n" +
                   "3. 런처를 다시 열고, 홈의 '한글 패치'에서 받아 둔 한글 패치 ZIP을 설치합니다(옛 한글 패치를 바꿔 줍니다).\n" +
                   "다른 폴더에 새로 풀어도 됩니다: 디스크 이미지는 이 PC의 기록으로 바로 찾고, 세이브는 memcards 폴더를 옮기면 됩니다.";
        }
    }

    // --diagnose and --install-korean (no window).
    public static class CommandLine
    {
        public static int Run(Launcher launcher, string[] args)
        {
            int i = Array.FindIndex(args, a => a.Equals("--diagnose", StringComparison.OrdinalIgnoreCase));
            if (i >= 0)
            {
                var text = new StringBuilder();
                text.AppendLine("folder: " + launcher.Dir);
                text.AppendLine("version: " + (Releases.Version(launcher.Dir) ?? "(build folder)"));
                text.AppendLine("config Iso: " + launcher.Config("Game", "Iso", ""));
                text.AppendLine("game folder disc: " + (Disc.Find(launcher.Dir) ?? "-"));
                text.AppendLine("remembered disc: " + (DiscSearch.Remembered() ?? "-"));
                text.AppendLine("disc used: " + (launcher.DiscImage ?? "-"));
                if (args.Any(a => a.Equals("--search", StringComparison.OrdinalIgnoreCase)))
                {
                    var watch = System.Diagnostics.Stopwatch.StartNew();
                    int folders = 0;
                    string found = DiscSearch.Search(delegate { folders++; });
                    text.AppendLine("search: " + (found ?? "-") + " (" + folders + " folders, " + watch.ElapsedMilliseconds + " ms)");
                }
                text.AppendLine("korean: " + (KoreanPatch.On(launcher) ? "on" : KoreanPatch.Off(launcher) ? "off" : "not installed") +
                                ", version " + (KoreanPatch.Version(launcher) ?? "-") + ", zip found " + (KoreanPatch.FindZip(launcher) ?? "-"));
                if (args.Any(a => a.Equals("--check-update", StringComparison.OrdinalIgnoreCase)))
                {
                    ReleaseUpdate update = Releases.Check(launcher.Dir);
                    text.AppendLine("update: local " + (update.Local ?? "-") + ", latest " + (update.Latest ?? "-") + ", newer " + update.Newer +
                                    (update.Error != null ? ", error " + update.Error : "") + ", page " + (update.Page ?? "-"));
                }
                string report = i + 1 < args.Length && !args[i + 1].StartsWith("--") ? args[i + 1] : Path.Combine(launcher.Dir, "diagnose.txt");
                File.WriteAllText(report, text.ToString(), new UTF8Encoding(false));
                return 0;
            }
            i = Array.FindIndex(args, a => a.Equals("--install-korean", StringComparison.OrdinalIgnoreCase));
            if (i >= 0 && i + 1 < args.Length)
            {
                try
                {
                    KoreanPatch.Install(launcher, args[i + 1]);
                    return 0;
                }
                catch (Exception)
                {
                    return 1;
                }
            }
            return -1; // not a command: open the window
        }
    }
}
