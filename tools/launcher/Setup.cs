// A release folder on any PC: the disc image found wherever it is (and remembered for every copy of the launcher on this
// PC, so a new version unpacked elsewhere finds it at once), the Korean patch installed from its ZIP or switched off,
// and new releases on GitHub (how to update: unpack the new ZIP over the folder; it carries no user files).
//   YKTK3 Launcher.exe --diagnose <report.txt>      what the launcher finds (version, disc, Korean patch, updates)
//   YKTK3 Launcher.exe --install-korean <zip>        unpacks a Korean patch ZIP next to the game
//   YKTK3 Launcher.exe --update <report.txt>         downloads the newest release and swaps it in (ReleaseUpdater)
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
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
                    if (relative.Split('/').Any(part => part == ".." || part.Contains(":")))
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
        public ReleaseAsset Program, Korean; // the newest release's YKTK3-PC and YKTK3-Korean ZIPs (not in the daily cache)
        public bool Newer { get { return Error == null && Latest != null && Local != null && string.CompareOrdinal(Latest, Local) > 0; } }
    }

    public class ReleaseAsset
    {
        public string Name, Url, Sha256; // Sha256: GitHub's digest of the upload, lower case (null when GitHub gives none)
        public long Size;
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
                        info.Program = Asset(release, "YKTK3-PC-");
                        info.Korean = Asset(release, "YKTK3-Korean-");
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

        static ReleaseAsset Asset(Dictionary<string, object> release, string prefix)
        {
            var assets = release.ContainsKey("assets") ? release["assets"] as IEnumerable : null;
            if (assets == null)
                return null;
            foreach (object item in assets)
            {
                var asset = item as Dictionary<string, object>;
                string name = asset != null && asset.ContainsKey("name") ? asset["name"] as string : null;
                if (name == null || !name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) || !name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                    continue;
                string digest = asset.ContainsKey("digest") ? asset["digest"] as string : null;
                return new ReleaseAsset
                {
                    Name = name,
                    Url = asset.ContainsKey("browser_download_url") ? asset["browser_download_url"] as string : null,
                    Size = asset.ContainsKey("size") && asset["size"] != null ? Convert.ToInt64(asset["size"]) : 0,
                    Sha256 = digest != null && digest.StartsWith("sha256:") ? digest.Substring(7).ToLowerInvariant() : null
                };
            }
            return null;
        }

        public static string HowToUpdate(Launcher launcher, ReleaseUpdate info)
        {
            string version = info != null && info.Latest != null ? info.Latest : "<새 버전>";
            return "홈의 '지금 업데이트'를 누르면 런처가 아래 일을 대신합니다(한글 패치를 쓰고 있으면 그것도 바꿈). 직접 하려면:\n" +
                   "1. 다운로드 페이지에서 YKTK3-PC-" + version + ".zip 을 받습니다. 한국어로 하면 YKTK3-Korean-" + version + ".zip 도 받습니다.\n" +
                   "2. 게임과 런처를 끄고, 받은 ZIP 안의 YKTK3-PC-" + version + " 폴더 내용을 지금 폴더에 그대로 덮어씁니다:\n   " + launcher.Dir + "\n" +
                   "   세이브(memcards), 빠른 저장(states), 설정(config.ini, controls.ini), 세이브별 치트 설정은 ZIP에 없어서 그대로 남습니다.\n" +
                   "3. 런처를 다시 열고, 홈의 '한글 패치'에서 받아 둔 한글 패치 ZIP을 설치합니다(옛 한글 패치를 바꿔 줍니다).\n" +
                   "다른 폴더에 새로 풀어도 됩니다: 디스크 이미지는 이 PC의 기록으로 바로 찾고, 세이브는 memcards 폴더를 옮기면 됩니다.";
        }
    }

    public class UpdateResult
    {
        public string Version; // null: this folder already has the newest release
        public string Note = "";
    }

    // One-click update of a release folder. The newest release's YKTK3-PC ZIP (and the Korean ZIP when an older Korean patch
    // is installed) is downloaded into update.new next to the game, checked against GitHub's size and SHA-256 and unpacked
    // there. Then each file is swapped in, the old one renamed to *.old first: Windows lets a file in use (the running
    // launcher, a DLL it loaded) be renamed, and any failure puts every old file back. User files (settings, saves, cheats per
    // save, the Korean patch) are not in the ZIP; their names are skipped anyway.
    public static class ReleaseUpdater
    {
        const string Leftovers = "update.old.txt"; // *.old files still in use when an update ended: deleted at the next start

        static readonly string[] UserFiles = { "config.ini", "controls.ini", "launcher.ini", Leftovers };
        static readonly string[] UserFolders = { "memcards", "states", "cheat_saves", "logs", "game", "korean", "korean.off", "korean.new", "update.new" };

        static string Staging(string dir) { return Path.Combine(dir, "update.new"); }

        public static bool GameRunning(Launcher launcher)
        {
            if (launcher.Runner.Running)
                return true;
            string exe = Path.Combine(launcher.Dir, "yktk3.exe");
            foreach (Process process in Process.GetProcessesByName("yktk3"))
            {
                try
                {
                    if (string.Equals(process.MainModule.FileName, exe, StringComparison.OrdinalIgnoreCase))
                        return true;
                }
                catch (Exception)
                {
                }
            }
            return false;
        }

        // progress: a line for the window (from this thread). Throws with a message for the player; the folder is then as it was.
        public static UpdateResult Run(Launcher launcher, Action<string> progress)
        {
            if (Releases.Version(launcher.Dir) == null || Updater.RepoRoot(launcher.Dir) != null)
                throw new InvalidOperationException("배포판 폴더(VERSION.txt 있음)가 아니라서 이 방법으로 업데이트하지 않습니다. 개발 폴더는 도구 탭의 git 업데이트를 씁니다.");
            if (GameRunning(launcher))
                throw new InvalidOperationException("게임이 켜져 있습니다. 게임을 끈 뒤 다시 누르세요.");
            Report(progress, "새 버전을 확인하는 중...");
            ReleaseUpdate info = Releases.Check(launcher.Dir);
            if (info.Error != null)
                throw new IOException("GitHub에서 새 버전을 확인하지 못했습니다: " + info.Error);
            if (!info.Newer)
                return new UpdateResult();
            if (info.Program == null || info.Program.Url == null)
                throw new IOException("새 버전 " + info.Latest + "에 YKTK3-PC ZIP이 없습니다. 다운로드 페이지에서 직접 받으세요.");
            string staging = Staging(launcher.Dir);
            try
            {
                TryDelete(staging);
                Directory.CreateDirectory(staging);
                string zip = Path.Combine(staging, info.Program.Name);
                Download(info.Program, zip, progress, "새 버전 " + info.Latest + " 받는 중");
                string koreanZip = null;
                if ((KoreanPatch.On(launcher) || KoreanPatch.Off(launcher)) && info.Korean != null && info.Korean.Url != null)
                {
                    string have = KoreanPatch.Version(launcher), offered = KoreanPatch.ZipVersion(info.Korean.Name);
                    if (have == null || (offered != null && string.CompareOrdinal(offered, have) > 0))
                    {
                        koreanZip = Path.Combine(staging, info.Korean.Name);
                        Download(info.Korean, koreanZip, progress, "새 한글 패치 받는 중");
                    }
                }
                Report(progress, "푸는 중...");
                string files = Path.Combine(staging, "files");
                Unpack(zip, files);
                File.Delete(zip);
                Report(progress, "파일을 바꾸는 중...");
                Swap(launcher.Dir, files);
                var result = new UpdateResult { Version = Releases.Version(launcher.Dir) ?? info.Latest };
                if (koreanZip != null)
                {
                    try
                    {
                        bool off = KoreanPatch.Off(launcher);
                        KoreanPatch.Install(launcher, koreanZip);
                        if (off)
                            KoreanPatch.Switch(launcher, false);
                        result.Note = "한글 패치도 바꿨습니다: " + (KoreanPatch.Version(launcher) ?? KoreanPatch.ZipVersion(koreanZip));
                    }
                    catch (Exception error)
                    {
                        result.Note = "한글 패치는 바꾸지 못했습니다(" + error.Message + "). 홈의 '한글 패치'에서 다시 설치하세요.";
                    }
                }
                return result;
            }
            finally
            {
                TryDelete(staging);
            }
        }

        static void Download(ReleaseAsset asset, string path, Action<string> progress, string label)
        {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072; // TLS 1.2
            var request = (HttpWebRequest)WebRequest.Create(asset.Url);
            request.UserAgent = "YKTK3-Launcher";
            request.Timeout = 30000;
            request.ReadWriteTimeout = 60000;
            long done = 0;
            string hash;
            using (var response = (HttpWebResponse)request.GetResponse())
            using (Stream input = response.GetResponseStream())
            using (FileStream output = File.Create(path))
            using (SHA256 sha = SHA256.Create())
            {
                long total = asset.Size > 0 ? asset.Size : response.ContentLength;
                var buffer = new byte[1 << 16];
                int read, shown = -1;
                while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
                {
                    output.Write(buffer, 0, read);
                    sha.TransformBlock(buffer, 0, read, null, 0);
                    done += read;
                    int percent = total > 0 ? (int)(done * 100 / total) : 0;
                    if (percent != shown)
                    {
                        shown = percent;
                        Report(progress, label + " " + percent + "% (" + done / 1048576 + " / " + Math.Max(total, 0) / 1048576 + " MB)");
                    }
                }
                sha.TransformFinalBlock(buffer, 0, 0);
                hash = BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
            }
            if (asset.Size > 0 && done != asset.Size)
                throw new IOException(asset.Name + "을 끝까지 받지 못했습니다(" + done + " / " + asset.Size + " 바이트). 다시 시도하세요.");
            if (asset.Sha256 != null && hash != asset.Sha256)
                throw new IOException(asset.Name + "의 SHA-256이 GitHub 값과 다릅니다. 다시 시도하세요.");
        }

        // The ZIP holds one folder, YKTK3-PC-<version>; its contents go to target.
        static void Unpack(string zip, string target)
        {
            string root = Path.GetFullPath(target) + Path.DirectorySeparatorChar;
            using (ZipArchive archive = ZipFile.OpenRead(zip))
            {
                foreach (ZipArchiveEntry entry in archive.Entries)
                {
                    string name = entry.FullName.Replace('\\', '/');
                    if (name.EndsWith("/"))
                        continue;
                    string[] parts = name.Split('/');
                    if (parts.Length > 1 && parts[0].StartsWith("YKTK3-PC-", StringComparison.OrdinalIgnoreCase))
                        parts = parts.Skip(1).ToArray();
                    if (parts.Length == 0 || parts.Any(part => part.Length == 0 || part == ".." || part.Contains(":")))
                        continue;
                    if (parts.Length == 1 ? UserFiles.Contains(parts[0], StringComparer.OrdinalIgnoreCase)
                                          : UserFolders.Contains(parts[0], StringComparer.OrdinalIgnoreCase))
                        continue;
                    string path = Path.GetFullPath(Path.Combine(target, string.Join(Path.DirectorySeparatorChar.ToString(), parts)));
                    if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                        continue;
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    entry.ExtractToFile(path, true);
                }
            }
            if (!File.Exists(Path.Combine(target, "yktk3.exe")) || !File.Exists(Path.Combine(target, "YKTK3 Launcher.exe")))
                throw new InvalidDataException("받은 ZIP에 yktk3.exe와 런처가 없습니다. 다운로드 페이지에서 직접 받으세요.");
        }

        // Every file of source into dir. Old files are renamed *.old first; on any failure the new files are removed and the old
        // ones put back. Then the *.old files are deleted; those still in use are listed for the next start.
        static void Swap(string dir, string source)
        {
            var renamed = new List<KeyValuePair<string, string>>(); // target, its old file
            var placed = new List<string>();
            try
            {
                foreach (string file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
                {
                    string target = Path.Combine(dir, file.Substring(source.Length + 1));
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    if (File.Exists(target))
                    {
                        string old = target + ".old";
                        if (!TryDeleteFile(old))
                            old = target + "." + Guid.NewGuid().ToString("N").Substring(0, 8) + ".old";
                        File.Move(target, old);
                        renamed.Add(new KeyValuePair<string, string>(target, old));
                    }
                    File.Move(file, target);
                    placed.Add(target);
                }
            }
            catch (Exception)
            {
                foreach (string target in placed)
                    TryDeleteFile(target);
                foreach (KeyValuePair<string, string> pair in renamed)
                {
                    try
                    {
                        File.Move(pair.Value, pair.Key);
                    }
                    catch (Exception)
                    {
                    }
                }
                throw;
            }
            List<string> busy = renamed.Select(pair => pair.Value).Where(old => !TryDeleteFile(old)).ToList();
            if (busy.Count > 0)
                File.AppendAllLines(Path.Combine(dir, Leftovers), busy.Select(old => old.Substring(dir.Length + 1)), new UTF8Encoding(false));
        }

        // At start: *.old files an update could not delete, and the download folder of an update that did not finish.
        public static void Cleanup(string dir)
        {
            string list = Path.Combine(dir, Leftovers);
            try
            {
                if (File.Exists(list))
                {
                    List<string> left = File.ReadAllLines(list).Select(line => line.Trim())
                        .Where(line => line.EndsWith(".old", StringComparison.OrdinalIgnoreCase) && !line.Contains("..") && !line.Contains(":"))
                        .Where(line => !TryDeleteFile(Path.Combine(dir, line))).ToList();
                    if (left.Count == 0)
                        File.Delete(list);
                    else
                        File.WriteAllLines(list, left, new UTF8Encoding(false));
                }
            }
            catch (Exception)
            {
            }
            TryDelete(Staging(dir));
        }

        static bool TryDeleteFile(string path)
        {
            try
            {
                if (File.Exists(path))
                    File.Delete(path);
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        static void TryDelete(string folder)
        {
            try
            {
                if (Directory.Exists(folder))
                    Directory.Delete(folder, true);
            }
            catch (Exception)
            {
            }
        }

        static void Report(Action<string> progress, string text)
        {
            if (progress != null)
                progress(text);
        }
    }

    // --diagnose, --install-korean and --update (no window).
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
            i = Array.FindIndex(args, a => a.Equals("--update", StringComparison.OrdinalIgnoreCase));
            if (i >= 0)
            {
                string report = i + 1 < args.Length && !args[i + 1].StartsWith("--") ? args[i + 1] : Path.Combine(launcher.Dir, "update.txt");
                string line;
                int code = 0;
                try
                {
                    UpdateResult result = ReleaseUpdater.Run(launcher, null);
                    line = result.Version == null ? "already the newest: " + Releases.Version(launcher.Dir)
                                                  : "updated to " + result.Version + (result.Note.Length > 0 ? " - " + result.Note : "");
                }
                catch (Exception error)
                {
                    line = "failed: " + error.Message;
                    code = 1;
                }
                File.WriteAllText(report, line + "\r\n", new UTF8Encoding(false));
                return code;
            }
            return -1; // not a command: open the window
        }
    }
}
