// Controllers: the pads SDL sees (numbered as yktk3.exe numbers them) and controls.ini's [Pad1]/[Pad2] bindings in
// PCSX2's format (Keyboard/<key>, SDL-<n>/<element>).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace Yktk3Launcher
{
    static class Sdl
    {
        const string Dll = "SDL3.dll";
        public const uint InitGamepad = 0x00002000u;

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SDL_Init(uint flags);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void SDL_QuitSubSystem(uint flags);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SDL_SetHint([MarshalAs(UnmanagedType.LPStr)] string name, [MarshalAs(UnmanagedType.LPStr)] string value);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr SDL_GetGamepads(out int count);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void SDL_free(IntPtr memory);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr SDL_OpenGamepad(uint id);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void SDL_CloseGamepad(IntPtr gamepad);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr SDL_GetGamepadName(IntPtr gamepad);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int SDL_GetGamepadPlayerIndex(IntPtr gamepad);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int SDL_GetGamepadType(IntPtr gamepad);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SDL_GetGamepadButton(IntPtr gamepad, int button);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern short SDL_GetGamepadAxis(IntPtr gamepad, int axis);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void SDL_PumpEvents();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SDL_PollEvent(IntPtr sdlEvent);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SDL_RumbleGamepad(IntPtr gamepad, ushort low, ushort high, uint milliseconds);

        public static string Utf8(IntPtr text)
        {
            if (text == IntPtr.Zero)
                return null;
            int length = 0;
            while (Marshal.ReadByte(text, length) != 0)
                length++;
            var bytes = new byte[length];
            Marshal.Copy(text, bytes, 0, length);
            return Encoding.UTF8.GetString(bytes);
        }
    }

    public class Pad
    {
        public int Slot;
        public uint Id;
        public IntPtr Handle;
        public string Name;
        public int Type;
    }

    // The connected pads in yktk3.exe's numbering (ps2_host_gamepads.cpp addGamepad): the player index SDL gives a pad
    // (an Xbox pad's XInput number) when it is free, else the lowest free number.
    public class PadReader : IDisposable
    {
        public static readonly string[] ButtonNames = { "FaceSouth", "FaceEast", "FaceWest", "FaceNorth", "Back", "Guide", "Start", "LeftStick", "RightStick",
                                                        "LeftShoulder", "RightShoulder", "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Misc1",
                                                        "Paddle1", "Paddle2", "Paddle3", "Paddle4", "Touchpad" };
        public static readonly string[] AxisNames = { "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger" };
        public readonly Pad[] Slots = new Pad[16];
        public readonly bool Available;
        readonly IntPtr eventBuffer = Marshal.AllocHGlobal(256);

        public PadReader()
        {
            try
            {
                Sdl.SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
                Available = Sdl.SDL_Init(Sdl.InitGamepad);
            }
            catch (Exception)
            {
                Available = false; // no SDL3.dll next to the launcher
            }
            if (Available)
                Update(true);
        }

        // Pumps SDL; with devices, also opens new pads and closes removed ones. Returns true when the set changed.
        public bool Update(bool devices)
        {
            if (!Available)
                return false;
            Sdl.SDL_PumpEvents();
            while (Sdl.SDL_PollEvent(eventBuffer))
            {
            }
            if (!devices)
                return false;
            int count;
            IntPtr ids = Sdl.SDL_GetGamepads(out count);
            var present = new List<uint>();
            for (int i = 0; i < count; i++)
                present.Add((uint)Marshal.ReadInt32(ids, i * 4));
            if (ids != IntPtr.Zero)
                Sdl.SDL_free(ids);
            bool changed = false;
            for (int s = 0; s < Slots.Length; s++)
            {
                if (Slots[s] != null && !present.Contains(Slots[s].Id))
                {
                    Sdl.SDL_CloseGamepad(Slots[s].Handle);
                    Slots[s] = null;
                    changed = true;
                }
            }
            foreach (uint id in present)
            {
                if (Slots.Any(p => p != null && p.Id == id))
                    continue;
                IntPtr handle = Sdl.SDL_OpenGamepad(id);
                if (handle == IntPtr.Zero)
                    continue;
                int slot = Sdl.SDL_GetGamepadPlayerIndex(handle);
                if (slot < 0 || slot >= Slots.Length || Slots[slot] != null)
                    slot = Array.IndexOf(Slots, null);
                if (slot < 0)
                {
                    Sdl.SDL_CloseGamepad(handle);
                    continue;
                }
                Slots[slot] = new Pad { Slot = slot, Id = id, Handle = handle, Name = Sdl.Utf8(Sdl.SDL_GetGamepadName(handle)) ?? "컨트롤러", Type = Sdl.SDL_GetGamepadType(handle) };
                changed = true;
            }
            return changed;
        }

        public IEnumerable<Pad> Connected { get { return Slots.Where(p => p != null); } }

        // An element's state as a PCSX2 binding would read it: buttons pressed, "+Axis"/"-Axis" past half way.
        public bool Active(int slot, string element)
        {
            Pad pad = slot >= 0 && slot < Slots.Length ? Slots[slot] : null;
            if (pad == null)
                return false;
            int button = Array.IndexOf(ButtonNames, element);
            if (button >= 0)
                return Sdl.SDL_GetGamepadButton(pad.Handle, button);
            if (element.Length > 1 && (element[0] == '+' || element[0] == '-'))
            {
                int axis = Array.IndexOf(AxisNames, element.Substring(1));
                if (axis >= 0)
                {
                    double value = Sdl.SDL_GetGamepadAxis(pad.Handle, axis) / 32767.0;
                    return element[0] == '+' ? value > 0.5 : value < -0.5;
                }
            }
            return false;
        }

        // The first element pressed now that was not pressed in "before" (a capture), as "SDL-<n>/<element>".
        public string Pressed(HashSet<string> before)
        {
            foreach (string active in ActiveNow())
                if (!before.Contains(active))
                    return active;
            return null;
        }

        public HashSet<string> ActiveNow()
        {
            var active = new HashSet<string>();
            foreach (Pad pad in Connected)
            {
                for (int b = 0; b < ButtonNames.Length; b++)
                    if (Sdl.SDL_GetGamepadButton(pad.Handle, b))
                        active.Add("SDL-" + pad.Slot + "/" + ButtonNames[b]);
                for (int a = 0; a < AxisNames.Length; a++)
                {
                    double value = Sdl.SDL_GetGamepadAxis(pad.Handle, a) / 32767.0;
                    if (value > 0.5)
                        active.Add("SDL-" + pad.Slot + "/+" + AxisNames[a]);
                    else if (value < -0.5 && a < 4) // triggers only go one way
                        active.Add("SDL-" + pad.Slot + "/-" + AxisNames[a]);
                }
            }
            return active;
        }

        public void Rumble(int slot)
        {
            Pad pad = slot >= 0 && slot < Slots.Length ? Slots[slot] : null;
            if (pad != null)
                Sdl.SDL_RumbleGamepad(pad.Handle, 0xC000, 0xC000, 600);
        }

        public void Dispose()
        {
            if (Available)
            {
                foreach (Pad pad in Connected)
                    Sdl.SDL_CloseGamepad(pad.Handle);
                Sdl.SDL_QuitSubSystem(Sdl.InitGamepad);
            }
            Marshal.FreeHGlobal(eventBuffer);
        }

        public static string TypeName(int type)
        {
            switch (type)
            {
                case 2: return "Xbox 360";
                case 3: return "Xbox One/Series";
                case 4: return "PS3";
                case 5: return "PS4";
                case 6: return "PS5";
                case 7: return "Switch Pro";
                case 8:
                case 9:
                case 10: return "Joy-Con";
                default: return "일반 패드";
            }
        }

        // An element's name as printed on that kind of pad.
        public static string Friendly(string element, int type)
        {
            bool ps = type >= 4 && type <= 6, nintendo = type >= 7 && type <= 10;
            switch (element)
            {
                case "FaceSouth": return ps ? "×" : nintendo ? "B" : "A";
                case "FaceEast": return ps ? "○" : nintendo ? "A" : "B";
                case "FaceWest": return ps ? "□" : nintendo ? "Y" : "X";
                case "FaceNorth": return ps ? "△" : nintendo ? "X" : "Y";
                case "Back": return ps ? (type == 6 ? "Create" : type == 5 ? "Share" : "Select") : nintendo ? "−" : "View";
                case "Start": return ps ? (type == 4 ? "Start" : "Options") : nintendo ? "+" : "Menu";
                case "Guide": return ps ? "PS" : nintendo ? "Home" : "Guide";
                case "LeftShoulder": return ps ? "L1" : nintendo ? "L" : "LB";
                case "RightShoulder": return ps ? "R1" : nintendo ? "R" : "RB";
                case "+LeftTrigger": return ps ? "L2" : nintendo ? "ZL" : "LT";
                case "+RightTrigger": return ps ? "R2" : nintendo ? "ZR" : "RT";
                case "LeftStick": return ps ? "L3" : "LS 누름";
                case "RightStick": return ps ? "R3" : "RS 누름";
                case "DPadUp": return "십자 ↑";
                case "DPadDown": return "십자 ↓";
                case "DPadLeft": return "십자 ←";
                case "DPadRight": return "십자 →";
                case "-LeftY": return "왼스틱 ↑";
                case "+LeftY": return "왼스틱 ↓";
                case "-LeftX": return "왼스틱 ←";
                case "+LeftX": return "왼스틱 →";
                case "-RightY": return "오른스틱 ↑";
                case "+RightY": return "오른스틱 ↓";
                case "-RightX": return "오른스틱 ←";
                case "+RightX": return "오른스틱 →";
                case "Touchpad": return "터치패드";
                case "Misc1": return "캡처";
                default: return element;
            }
        }
    }

    // controls.ini edited in place: [PadN] keeps its other settings and lines; a button may have a keyboard and a pad
    // binding (two lines with the same key), each replaced on its own.
    public class ControlsFile
    {
        public static readonly string[] Buttons = { "Up", "Right", "Down", "Left", "Triangle", "Circle", "Cross", "Square", "Select", "Start",
                                                    "L1", "L2", "R1", "R2", "L3", "R3", "LUp", "LRight", "LDown", "LLeft", "RUp", "RRight", "RDown", "RLeft" };
        readonly IniFile ini;

        public ControlsFile(string path)
        {
            ini = new IniFile(path);
        }

        static string Section(int pad)
        {
            return "Pad" + pad;
        }

        List<int> Lines(int pad, string key)
        {
            var found = new List<int>();
            int start, end;
            ini.SectionRange(Section(pad), out start, out end);
            for (int i = start; i >= 0 && i < end; i++)
            {
                string k, v;
                if (IniFile.KeyValue(ini.Lines[i], out k, out v) && string.Equals(k, key, StringComparison.OrdinalIgnoreCase))
                    found.Add(i);
            }
            return found;
        }

        static string ValueOf(string line)
        {
            string k, v;
            return IniFile.KeyValue(line, out k, out v) ? v : "";
        }

        static bool IsKeyboard(string binding)
        {
            return binding.StartsWith("Keyboard/", StringComparison.OrdinalIgnoreCase);
        }

        public string Get(int pad, string button, bool keyboard)
        {
            foreach (int i in Lines(pad, button))
            {
                string value = ValueOf(ini.Lines[i]);
                if (IsKeyboard(value) == keyboard)
                    return value;
            }
            return null;
        }

        // binding null = remove that kind's binding.
        public void Set(int pad, string button, bool keyboard, string binding)
        {
            List<int> lines = Lines(pad, button);
            int replaced = -1;
            for (int n = lines.Count - 1; n >= 0; n--)
            {
                int i = lines[n];
                if (IsKeyboard(ValueOf(ini.Lines[i])) != keyboard)
                    continue;
                ini.Lines.RemoveAt(i);
                replaced = i; // the first such line, after the loop
            }
            if (binding == null)
                return;
            string line = button + " = " + binding;
            if (replaced >= 0)
            {
                ini.Lines.Insert(replaced, line);
                return;
            }
            lines = Lines(pad, button);
            int start, end;
            ini.SectionRange(Section(pad), out start, out end);
            if (start < 0)
            {
                ini.Lines.Add("[" + Section(pad) + "]");
                ini.Lines.Add(line);
                return;
            }
            // keyboard before the pad, as the game writes them
            int at = lines.Count > 0 ? (keyboard ? lines[0] : lines[lines.Count - 1] + 1) : LastSetting(start, end) + 1;
            ini.Lines.Insert(at, line);
        }

        int LastSetting(int start, int end)
        {
            int last = start - 1;
            for (int i = start; i < end; i++)
            {
                string k, v;
                if (IniFile.KeyValue(ini.Lines[i], out k, out v))
                    last = i;
            }
            return last;
        }

        public string Setting(int pad, string key, string fallback)
        {
            List<int> lines = Lines(pad, key);
            return lines.Count > 0 ? ValueOf(ini.Lines[lines[0]]) : fallback;
        }

        public void SetSetting(int pad, string key, string value)
        {
            List<int> lines = Lines(pad, key);
            if (lines.Count > 0)
            {
                string line = ini.Lines[lines[0]];
                ini.Lines[lines[0]] = line.Substring(0, line.IndexOf('=') + 1) + " " + value;
                return;
            }
            int start, end;
            ini.SectionRange(Section(pad), out start, out end);
            if (start >= 0)
                ini.Lines.Insert(start, key + " = " + value);
        }

        // The pad number most of this player's bindings use (-1: none).
        public int Device(int pad)
        {
            int start, end;
            ini.SectionRange(Section(pad), out start, out end);
            var counts = new Dictionary<int, int>();
            for (int i = start; i >= 0 && i < end; i++)
            {
                Match m = Regex.Match(ValueOf(ini.Lines[i]), @"^SDL-(\d+)/");
                if (m.Success)
                {
                    int slot = int.Parse(m.Groups[1].Value);
                    int n;
                    counts.TryGetValue(slot, out n);
                    counts[slot] = n + 1;
                }
            }
            return counts.Count == 0 ? -1 : counts.OrderByDescending(kv => kv.Value).First().Key;
        }

        // Every pad binding of this player (and the vibration) on pad number slot.
        public void SetDevice(int pad, int slot)
        {
            int start, end;
            ini.SectionRange(Section(pad), out start, out end);
            for (int i = start; i >= 0 && i < end; i++)
                ini.Lines[i] = Regex.Replace(ini.Lines[i], @"SDL-\d+/", "SDL-" + slot + "/");
        }

        // The player's whole section as the game's defaults have it.
        public void Reset(int pad, IniFile defaults)
        {
            int start, end, dStart, dEnd;
            ini.SectionRange(Section(pad), out start, out end);
            defaults.SectionRange(Section(pad), out dStart, out dEnd);
            if (start < 0 || dStart < 0)
                return;
            var body = defaults.Lines.GetRange(dStart, dEnd - dStart).ToList();
            while (body.Count > 0 && body[body.Count - 1].Trim().Length == 0)
                body.RemoveAt(body.Count - 1);
            while (end > start && ini.Lines[end - 1].Trim().Length == 0)
                end--;
            ini.Lines.RemoveRange(start, end - start);
            ini.Lines.InsertRange(start, body);
        }

        public void Save()
        {
            ini.Save();
        }

        // PCSX2 key names (ps2_host_input.cpp kKeyNames) for WPF keys; null for keys the game does not read.
        public static string KeyName(Key key)
        {
            if (key >= Key.A && key <= Key.Z)
                return key.ToString();
            if (key >= Key.D0 && key <= Key.D9)
                return ((int)(key - Key.D0)).ToString();
            if (key >= Key.NumPad0 && key <= Key.NumPad9)
                return "Keypad+" + (int)(key - Key.NumPad0);
            if (key >= Key.F1 && key <= Key.F12)
                return "F" + (int)(key - Key.F1 + 1);
            switch (key)
            {
                case Key.Tab: return "Tab";
                case Key.Back: return "Backspace";
                case Key.Return: return "Return";
                case Key.Insert: return "Insert";
                case Key.Delete: return "Delete";
                case Key.Pause: return "Pause";
                case Key.Home: return "Home";
                case Key.End: return "End";
                case Key.Left: return "Left";
                case Key.Up: return "Up";
                case Key.Right: return "Right";
                case Key.Down: return "Down";
                case Key.PageUp: return "PageUp";
                case Key.PageDown: return "PageDown";
                case Key.LeftShift: return "LeftShift";
                case Key.RightShift: return "RightShift";
                case Key.LeftCtrl: return "LeftControl";
                case Key.RightCtrl: return "RightControl";
                case Key.LeftAlt: return "LeftAlt";
                case Key.RightAlt: return "RightAlt";
                case Key.Capital: return "CapsLock";
                case Key.Space: return "Space";
                case Key.OemComma: return "Comma";
                case Key.OemMinus: return "Minus";
                case Key.OemPeriod: return "Period";
                case Key.OemQuestion: return "Slash";
                case Key.OemSemicolon: return "Semicolon";
                case Key.OemPlus: return "Equal";
                case Key.OemOpenBrackets: return "BracketLeft";
                case Key.OemPipe: return "Backslash";
                case Key.OemCloseBrackets: return "BracketRight";
                case Key.OemQuotes: return "Apostrophe";
                case Key.OemTilde: return "QuoteLeft";
                case Key.NumLock: return "NumLock";
                case Key.Scroll: return "ScrollLock";
                case Key.Snapshot: return "Print";
                case Key.Apps: return "Menu";
                default: return null;
            }
        }

        // device: the player's pad number; a binding on it shows without the number.
        public static string Show(string binding, PadReader pads, int device = -1)
        {
            if (binding == null)
                return "—";
            if (IsKeyboard(binding))
            {
                string key = binding.Substring(9);
                switch (key.ToLowerInvariant())
                {
                    case "up": return "↑";
                    case "down": return "↓";
                    case "left": return "←";
                    case "right": return "→";
                    case "return": return "Enter";
                    default: return key;
                }
            }
            Match m = Regex.Match(binding, @"^SDL-(\d+)/(.+)$");
            if (!m.Success)
                return binding;
            int slot = int.Parse(m.Groups[1].Value);
            Pad pad = pads != null && slot < pads.Slots.Length ? pads.Slots[slot] : null;
            string name = PadReader.Friendly(m.Groups[2].Value, pad != null ? pad.Type : 3);
            return slot == device ? name : "패드" + (slot + 1) + " · " + name;
        }
    }

    public class ControllerPage : PageBase
    {
        static readonly string[,] Labels =
        {
            { "Up", "방향키 ↑" }, { "Down", "방향키 ↓" }, { "Left", "방향키 ←" }, { "Right", "방향키 →" },
            { "Triangle", "△" }, { "Circle", "○" }, { "Cross", "×" }, { "Square", "□" },
            { "Select", "SELECT" }, { "Start", "START" },
            { "L1", "L1" }, { "L2", "L2" }, { "R1", "R1" }, { "R2", "R2" }, { "L3", "L3 (스틱 누름)" }, { "R3", "R3 (스틱 누름)" },
            { "LUp", "왼스틱 ↑" }, { "LDown", "왼스틱 ↓" }, { "LLeft", "왼스틱 ←" }, { "LRight", "왼스틱 →" },
            { "RUp", "오른스틱 ↑" }, { "RDown", "오른스틱 ↓" }, { "RLeft", "오른스틱 ←" }, { "RRight", "오른스틱 →" },
        };
        PadReader pads;
        DispatcherTimer timer;
        int tick;
        int player = 1;
        readonly List<KeyValuePair<Border, string>> live = new List<KeyValuePair<Border, string>>();
        WrapPanel deviceTiles;
        // capture: the button being set (null when not capturing), the pad state when it started
        string captureButton;
        bool captureKeyboard;
        HashSet<string> captureBefore;

        public ControllerPage(MainWindow window) : base(window)
        {
            Window.PreviewKeyDown += OnKey;
        }

        public override void Shown()
        {
            if (pads == null)
                pads = new PadReader();
            base.Shown();
            if (timer == null)
            {
                timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
                timer.Tick += delegate { Poll(); };
            }
            timer.Start();
        }

        public override void Hidden()
        {
            if (timer != null)
                timer.Stop();
            captureButton = null;
            if (pads != null)
                pads.Dispose();
            pads = null;
        }

        void Poll()
        {
            if (pads == null)
                return;
            bool devices = ++tick % 30 == 0;
            if (pads.Update(devices))
            {
                Window.Refresh(); // a pad came or went
                return;
            }
            foreach (KeyValuePair<Border, string> row in live)
            {
                Match m = Regex.Match(row.Value ?? "", @"^SDL-(\d+)/(.+)$");
                bool on = m.Success && pads.Active(int.Parse(m.Groups[1].Value), m.Groups[2].Value);
                row.Key.Background = on ? Ui.Brush("AccentSoft") : Brushes.Transparent;
            }
            if (captureButton != null && captureBefore != null)
            {
                string pressed = pads.Pressed(captureBefore);
                if (pressed != null)
                    Bind(pressed);
                else
                    captureBefore.IntersectWith(pads.ActiveNow()); // released elements can count again
            }
        }

        void OnKey(object sender, KeyEventArgs e)
        {
            if (captureButton == null)
                return;
            e.Handled = true;
            Key key = e.Key == Key.System ? e.SystemKey : e.Key;
            if (key == Key.Escape)
            {
                captureButton = null;
                Window.CloseDialog();
                return;
            }
            string name = ControlsFile.KeyName(key);
            if (name != null)
                Bind("Keyboard/" + name);
        }

        void Bind(string binding)
        {
            string button = captureButton;
            captureButton = null;
            Window.CloseDialog();
            var controls = new ControlsFile(Launcher.ControlsPath);
            controls.Set(player, button, binding.StartsWith("Keyboard/"), binding);
            controls.Save();
            Window.Toast(player + "P " + LabelOf(button) + " → " + (binding.StartsWith("Keyboard/") ? "키보드 " : "") + ControlsFile.Show(binding, pads));
            Window.Refresh();
        }

        static string LabelOf(string button)
        {
            for (int i = 0; i < Labels.GetLength(0); i++)
                if (Labels[i, 0] == button)
                    return Labels[i, 1];
            return button;
        }

        void Capture(string button, bool keyboard)
        {
            captureButton = button;
            captureKeyboard = keyboard;
            captureBefore = pads != null ? pads.ActiveNow() : new HashSet<string>();
            Window.Dialog("", Ui.Brush("Accent"), player + "P " + LabelOf(button),
                          "지정할 키보드 키나 컨트롤러 버튼·스틱을 누르세요.\nEsc: 취소",
                          new DialogAction(keyboard ? "키보드 지정 지우기" : "패드 지정 지우기", delegate
                          {
                              captureButton = null;
                              var controls = new ControlsFile(Launcher.ControlsPath);
                              controls.Set(player, button, keyboard, null);
                              controls.Save();
                              Window.Refresh();
                          }),
                          new DialogAction("취소", delegate { captureButton = null; }));
        }

        protected override FrameworkElement Build()
        {
            live.Clear();
            var controls = new ControlsFile(Launcher.ControlsPath);
            deviceTiles = new WrapPanel();
            if (!pads.Available)
                deviceTiles.Children.Add(Ui.Text("SDL3.dll을 찾지 못해 컨트롤러를 읽을 수 없습니다. 키보드 설정은 할 수 있습니다.", "Caption"));
            else if (!pads.Connected.Any())
                deviceTiles.Children.Add(Ui.Text("연결된 컨트롤러가 없습니다. 연결하면 바로 여기에 나타납니다.", "Caption"));
            foreach (Pad pad in pads.Connected)
            {
                var tile = new Border { Style = Ui.Style("Chip"), Padding = new Thickness(14, 10, 14, 10), CornerRadius = new CornerRadius(12) };
                var text = new StackPanel { Margin = new Thickness(10, 0, 0, 0) };
                var name = Ui.Text(pad.Name, "Body");
                name.FontWeight = FontWeights.SemiBold;
                text.Children.Add(name);
                text.Children.Add(Ui.Text("패드 " + (pad.Slot + 1) + " (SDL-" + pad.Slot + ") · " + PadReader.TypeName(pad.Type), "Caption"));
                tile.Child = Ui.Row(Ui.Icon("", 22, Ui.Brush("Accent")), text);
                deviceTiles.Children.Add(tile);
            }

            var tabs = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 16) };
            for (int p = 1; p <= 2; p++)
            {
                int which = p;
                var tab = Ui.Button(p + "P", delegate
                {
                    player = which;
                    Window.Refresh();
                }, p == player ? "AccentButton" : null, null, new Thickness(0, 0, 8, 0));
                tab.MinWidth = 90;
                tabs.Children.Add(tab);
            }

            int device = controls.Device(player);
            var deviceChoices = new List<string>();
            for (int s = 0; s < 8; s++)
            {
                Pad pad = pads.Slots[s];
                deviceChoices.Add(s.ToString());
                deviceChoices.Add("패드 " + (s + 1) + " (SDL-" + s + ")" + (pad != null ? " · " + pad.Name : " · 연결 안 됨"));
            }
            var devicePick = Ui.Choices(device.ToString(), delegate(string v)
            {
                var c = new ControlsFile(Launcher.ControlsPath);
                c.SetDevice(player, int.Parse(v));
                c.Save();
                Window.Toast(player + "P 컨트롤러: 패드 " + (int.Parse(v) + 1));
                Window.Refresh();
            }, deviceChoices.ToArray());

            var mapping = new Grid();
            mapping.ColumnDefinitions.Add(new ColumnDefinition());
            mapping.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(24) });
            mapping.ColumnDefinitions.Add(new ColumnDefinition());
            var halves = new[] { new StackPanel(), new StackPanel() };
            for (int i = 0; i < Labels.GetLength(0); i++)
            {
                string button = Labels[i, 0];
                string keyBinding = controls.Get(player, button, true), padBinding = controls.Get(player, button, false);
                var row = new Grid();
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(118) });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(8) });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                var label = Ui.Text(Labels[i, 1], "Body");
                label.VerticalAlignment = VerticalAlignment.Center;
                label.Margin = new Thickness(10, 0, 0, 0);
                row.Children.Add(label);
                Button keyCell = Cell("", ControlsFile.Show(keyBinding, pads), delegate { Capture(button, true); });
                Grid.SetColumn(keyCell, 1);
                row.Children.Add(keyCell);
                Button padCell = Cell("", ControlsFile.Show(padBinding, pads, device), delegate { Capture(button, false); });
                Grid.SetColumn(padCell, 3);
                row.Children.Add(padCell);
                var border = new Border { CornerRadius = new CornerRadius(10), Padding = new Thickness(0, 3, 0, 3), Child = row };
                live.Add(new KeyValuePair<Border, string>(border, padBinding));
                halves[i < 10 ? 0 : 1].Children.Add(border);
            }
            mapping.Children.Add(halves[0]);
            Grid.SetColumn(halves[1], 2);
            mapping.Children.Add(halves[1]);

            double deadzone, scale, motor;
            double.TryParse(controls.Setting(player, "Deadzone", "0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out deadzone);
            if (!double.TryParse(controls.Setting(player, "AxisScale", "1.33"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out scale))
                scale = 1.33;
            if (!double.TryParse(controls.Setting(player, "LargeMotorScale", "1"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out motor))
                motor = 1;
            Action<string, string> setSetting = delegate(string key, string value)
            {
                var c = new ControlsFile(Launcher.ControlsPath);
                c.SetSetting(player, key, value);
                c.Save();
                Window.Toast("저장했습니다");
            };
            int rumbleSlot = device;

            return Ui.Page("컨트롤러", "PCSX2와 같은 형식의 controls.ini에 저장됩니다. 게임을 다시 시작하면 적용됩니다.",
                Ui.Card("연결된 컨트롤러", deviceTiles),
                tabs,
                Ui.Card(player + "P 버튼",
                    Ui.Setting("사용할 컨트롤러", "패드 번호는 게임이 매기는 번호입니다 (Xbox 패드는 XInput 번호).", devicePick),
                    Ui.Text("칸을 누른 뒤 지정할 키나 버튼을 누르세요. 컨트롤러로 누르면 해당 줄이 빛납니다.", "Caption"),
                    new Border { Height = 12 },
                    mapping),
                Ui.Card(player + "P 스틱과 진동",
                    Ui.Setting("데드존", "스틱이 이만큼 기울기 전까지는 움직임으로 치지 않습니다.",
                               Ui.Slider(0, 50, Math.Round(deadzone * 100), 1, delegate(double v) { return (int)v + "%"; },
                                         delegate(double v) { setSetting("Deadzone", (v / 100).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture)); })),
                    Ui.Setting("스틱 감도", "기본 133%.",
                               Ui.Slider(50, 200, Math.Round(scale * 100), 1, delegate(double v) { return (int)v + "%"; },
                                         delegate(double v) { setSetting("AxisScale", (v / 100).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture)); })),
                    Ui.Setting("진동", null, Ui.Row(Ui.Switch(motor > 0, delegate(bool on)
                    {
                        var c = new ControlsFile(Launcher.ControlsPath);
                        c.SetSetting(player, "LargeMotorScale", on ? "1" : "0");
                        c.SetSetting(player, "SmallMotorScale", on ? "1" : "0");
                        c.Save();
                        Window.Toast(on ? "진동 켬" : "진동 끔");
                    }), Ui.Button("진동 테스트", delegate { pads.Rumble(rumbleSlot); }, null, "", new Thickness(16, 0, 0, 0)))),
                    Ui.Button(player + "P를 기본값으로", delegate
                    {
                        Window.Confirm(player + "P 기본값", player + "P의 버튼·스틱·진동 설정을 게임의 기본값으로 되돌립니다.", "되돌리기", delegate
                        {
                            var c = new ControlsFile(Launcher.ControlsPath);
                            c.Reset(player, Defaults.Controls(Launcher));
                            c.Save();
                            Window.Toast(player + "P를 기본값으로 되돌렸습니다");
                            Window.Refresh();
                        });
                    }, null, "", new Thickness(0, 6, 0, 0))));
        }

        static Button Cell(string glyph, string text, Action click)
        {
            var button = Ui.Button(text, click, null, glyph);
            button.HorizontalContentAlignment = HorizontalAlignment.Left;
            button.Padding = new Thickness(12, 6, 12, 6);
            return button;
        }
    }
}
