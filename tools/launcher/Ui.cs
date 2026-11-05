// The launcher window (borderless, sidebar of pages, in-window dialogs and toasts) and the helpers that build pages.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using System.Windows.Shell;
using System.Windows.Threading;

namespace Yktk3Launcher
{
    public interface IPage
    {
        FrameworkElement View { get; }
        void Shown();  // the page is selected (data is read again)
        void Hidden();
    }

    public class DialogAction
    {
        public string Label;
        public Action Run;
        public bool Primary;

        public DialogAction(string label, Action run, bool primary = false)
        {
            Label = label;
            Run = run;
            Primary = primary;
        }
    }

    public class MainWindow : Window
    {
        public readonly Launcher Launcher;
        readonly Grid root = new Grid();
        readonly Grid host = new Grid();
        readonly Grid overlay = new Grid { Visibility = Visibility.Collapsed };
        readonly StackPanel toasts = new StackPanel { HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 24, 24) };
        readonly Dictionary<string, RadioButton> navButtons = new Dictionary<string, RadioButton>();
        readonly Dictionary<string, IPage> pages = new Dictionary<string, IPage>();
        readonly Dictionary<string, Func<IPage>> factories = new Dictionary<string, Func<IPage>>();
        public readonly TextBlock UpdateBadge = new TextBlock();
        IPage current;

        public MainWindow(Launcher launcher)
        {
            Launcher = launcher;
            Title = "YKTK3 PC 런처";
            Width = 1180;
            Height = 780;
            MinWidth = 980;
            MinHeight = 640;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            FontFamily = Ui.Res<FontFamily>("UiFont");
            Foreground = Ui.Brush("Text");
            Background = new SolidColorBrush(Color.FromRgb(0x07, 0x0B, 0x14));
            WindowStyle = WindowStyle.None;
            WindowChrome.SetWindowChrome(this, new WindowChrome
            {
                CaptionHeight = 44, ResizeBorderThickness = new Thickness(6), GlassFrameThickness = new Thickness(0),
                CornerRadius = new CornerRadius(0), UseAeroCaptionButtons = false,
            });
            // Maximized, a borderless window reaches past the screen by its resize border.
            StateChanged += delegate { root.Margin = WindowState == WindowState.Maximized ? new Thickness(7) : new Thickness(0); };

            root.Children.Add(Glow(0.88, 0.05, Color.FromArgb(0x40, 0x22, 0xD3, 0xEE)));
            root.Children.Add(Glow(0.05, 1.0, Color.FromArgb(0x38, 0xA8, 0x55, 0xF7)));
            var frame = new Grid();
            frame.RowDefinitions.Add(new RowDefinition { Height = new GridLength(44) });
            frame.RowDefinitions.Add(new RowDefinition());
            root.Children.Add(frame);
            frame.Children.Add(TitleBar());
            var body = new Grid();
            Grid.SetRow(body, 1);
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(232) });
            body.ColumnDefinitions.Add(new ColumnDefinition());
            frame.Children.Add(body);
            body.Children.Add(Sidebar());
            host.Margin = new Thickness(12, 4, 28, 0);
            Grid.SetColumn(host, 1);
            body.Children.Add(host);
            root.Children.Add(overlay);
            root.Children.Add(toasts);
            Content = root;

            Add("home", "", "홈", delegate { return new HomePage(this); });
            Add("video", "", "화면", delegate { return new VideoPage(this); });
            Add("controllers", "", "컨트롤러", delegate { return new ControllerPage(this); });
            Add("card", "", "메모리카드", delegate { return new CardPage(this); });
            Add("cheats", "", "치트", delegate { return new CheatPage(this); });
            Add("logs", "", "로그", delegate { return new LogPage(this); });
            Add("tools", "", "도구", delegate { return new ToolsPage(this); });
            Navigate("home");
            Closing += delegate
            {
                if (current != null)
                    current.Hidden();
            };
            if (Updater.RepoRoot(launcher.Dir) != null)
                Updater.CheckAsync(launcher, delegate(UpdateInfo info)
                {
                    LastUpdate = info;
                    ShowUpdateBadge(info);
                });
        }

        public UpdateInfo LastUpdate;

        public void ShowUpdateBadge(UpdateInfo info)
        {
            bool newer = info != null && info.Error == null && info.Behind > 0;
            UpdateBadge.Text = newer ? "↑ 새 버전 (커밋 " + info.Behind + "개)" : "";
            UpdateBadge.Visibility = newer ? Visibility.Visible : Visibility.Collapsed;
        }

        public void CloseDialog()
        {
            overlay.Visibility = Visibility.Collapsed;
        }

        static Rectangle Glow(double x, double y, Color color)
        {
            var brush = new RadialGradientBrush { Center = new Point(x, y), GradientOrigin = new Point(x, y), RadiusX = 0.65, RadiusY = 0.75 };
            brush.GradientStops.Add(new GradientStop(color, 0));
            brush.GradientStops.Add(new GradientStop(Color.FromArgb(0, color.R, color.G, color.B), 1));
            return new Rectangle { Fill = brush, IsHitTestVisible = false };
        }

        UIElement TitleBar()
        {
            var bar = new DockPanel { LastChildFill = false };
            var logo = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(20, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            logo.Children.Add(new Border
            {
                Width = 22, Height = 22, CornerRadius = new CornerRadius(6), Background = Ui.Brush("PlayGradient"),
                Child = new TextBlock { Text = "3", FontWeight = FontWeights.Bold, FontSize = 13, Foreground = Brushes.White, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center },
            });
            logo.Children.Add(new TextBlock { Text = "YKTK3 PC 런처", Margin = new Thickness(10, 0, 0, 0), FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = Ui.Brush("SubText"), VerticalAlignment = VerticalAlignment.Center });
            DockPanel.SetDock(logo, Dock.Left);
            bar.Children.Add(logo);
            Button close = Chrome("", "CloseButton", delegate { Close(); });
            Button maximize = Chrome("", "ChromeButton", delegate { WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized; });
            Button minimize = Chrome("", "ChromeButton", delegate { WindowState = WindowState.Minimized; });
            StateChanged += delegate { maximize.Content = WindowState == WindowState.Maximized ? "" : ""; };
            foreach (Button button in new[] { close, maximize, minimize })
            {
                DockPanel.SetDock(button, Dock.Right);
                bar.Children.Add(button);
            }
            UpdateBadge.Visibility = Visibility.Collapsed;
            UpdateBadge.Foreground = Ui.Brush("Accent");
            UpdateBadge.FontSize = 12;
            UpdateBadge.VerticalAlignment = VerticalAlignment.Center;
            UpdateBadge.Margin = new Thickness(0, 0, 16, 0);
            UpdateBadge.Cursor = System.Windows.Input.Cursors.Hand;
            UpdateBadge.MouseLeftButtonUp += delegate { Navigate("tools"); };
            WindowChrome.SetIsHitTestVisibleInChrome(UpdateBadge, true);
            DockPanel.SetDock(UpdateBadge, Dock.Right);
            bar.Children.Add(UpdateBadge);
            return bar;
        }

        Button Chrome(string glyph, string style, Action click)
        {
            var button = new Button { Content = glyph, Style = Ui.Style(style), Height = 44 };
            button.Click += delegate { click(); };
            WindowChrome.SetIsHitTestVisibleInChrome(button, true);
            return button;
        }

        readonly StackPanel nav = new StackPanel { Margin = new Thickness(0, 12, 0, 0) };

        UIElement Sidebar()
        {
            var side = new DockPanel();
            var footer = new StackPanel { Margin = new Thickness(28, 0, 16, 20) };
            footer.Children.Add(Ui.Text("プロ野球チームをつくろう！3", "Caption"));
            footer.Children.Add(Ui.Text("게임 데이터 미포함\n본인 디스크 이미지 필요", "Caption"));
            ((TextBlock)footer.Children[1]).Foreground = Ui.Brush("Muted");
            ((TextBlock)footer.Children[1]).Margin = new Thickness(0, 4, 0, 0);
            DockPanel.SetDock(footer, Dock.Bottom);
            side.Children.Add(footer);
            side.Children.Add(nav);
            return side;
        }

        void Add(string key, string glyph, string label, Func<IPage> factory)
        {
            factories[key] = factory;
            var content = new StackPanel { Orientation = Orientation.Horizontal };
            content.Children.Add(new TextBlock { Text = glyph, Style = Ui.Style("Icon"), Width = 28 });
            content.Children.Add(new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center });
            var button = new RadioButton { Content = content, Style = Ui.Style("Nav"), GroupName = "nav" };
            button.Checked += delegate { Show(key); };
            navButtons[key] = button;
            nav.Children.Add(button);
        }

        public void Navigate(string key)
        {
            if (navButtons[key].IsChecked == true)
                Show(key);
            else
                navButtons[key].IsChecked = true;
        }

        void Show(string key)
        {
            if (current != null)
                current.Hidden();
            IPage page;
            if (!pages.TryGetValue(key, out page))
                pages[key] = page = factories[key]();
            current = page;
            host.Children.Clear();
            host.Children.Add(page.View);
            page.Shown();
            page.View.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(180)));
        }

        public void Refresh()
        {
            if (current != null)
                current.Shown();
        }

        // Starts the game (the window hides while it runs and comes back when it ends).
        public void Play(bool safe)
        {
            if (current != null)
                current.Hidden(); // e.g. the controller page lets go of the pads
            string problem = Launcher.Runner.Start(safe);
            if (problem != null)
            {
                ShowProblem(problem);
                Refresh();
            }
            else
                Hide();
        }

        public void ShowProblem(string problem)
        {
            Dialog("", Ui.Brush("Warn"), "게임을 시작할 수 없습니다", problem, new DialogAction("확인", null, true));
        }

        public void GameFinished(RunResult result)
        {
            Show();
            Activate();
            Refresh();
            Diagnosis d = result.Diagnosis;
            if (d == null)
            {
                Toast("플레이 시간 " + result.Ran.ToString(@"h\:mm\:ss"));
                return;
            }
            var actions = new List<DialogAction>();
            if (d.SafeModeHelps)
                actions.Add(new DialogAction("안전 설정으로 다시 실행", delegate { Play(true); }, true));
            actions.Add(new DialogAction("로그 보기", delegate { Navigate("logs"); }));
            actions.Add(new DialogAction("닫기", null));
            Dialog("", Ui.Brush("Bad"), d.Title, d.Detail + "\n\n" + d.Advice + (result.SafeMode ? "\n\n(이번 실행은 이미 안전 설정이었습니다.)" : ""), actions.ToArray());
        }

        // A dialog inside the window (dark backdrop, card in the middle).
        public void Dialog(string glyph, Brush glyphBrush, string title, string message, params DialogAction[] actions)
        {
            overlay.Children.Clear();
            overlay.Background = new SolidColorBrush(Color.FromArgb(0xB0, 0x02, 0x04, 0x0A));
            var card = new Border { Style = Ui.Style("Card"), MaxWidth = 560, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Padding = new Thickness(28), Background = new SolidColorBrush(Color.FromRgb(0x10, 0x17, 0x28)) };
            card.Effect = new DropShadowEffect { Color = Colors.Black, BlurRadius = 40, ShadowDepth = 0, Opacity = 0.6 };
            var stack = new StackPanel();
            var head = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 14) };
            head.Children.Add(new TextBlock { Text = glyph, Style = Ui.Style("Icon"), FontSize = 22, Foreground = glyphBrush, Margin = new Thickness(0, 0, 12, 0) });
            var titleText = Ui.Text(title, "H2");
            titleText.Margin = new Thickness(0);
            titleText.VerticalAlignment = VerticalAlignment.Center;
            head.Children.Add(titleText);
            stack.Children.Add(head);
            stack.Children.Add(Ui.Text(message, "Body"));
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 22, 0, 0) };
            foreach (DialogAction action in actions)
            {
                DialogAction a = action;
                buttons.Children.Add(Ui.Button(a.Label, delegate
                {
                    overlay.Visibility = Visibility.Collapsed;
                    if (a.Run != null)
                        a.Run();
                }, a.Primary ? "AccentButton" : null, null, new Thickness(8, 0, 0, 0)));
            }
            stack.Children.Add(buttons);
            card.Child = stack;
            overlay.Children.Add(card);
            overlay.Visibility = Visibility.Visible;
            card.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(160)));
        }

        public void Confirm(string title, string message, string yes, Action run)
        {
            Dialog("", Ui.Brush("Accent"), title, message, new DialogAction(yes, run, true), new DialogAction("취소", null));
        }

        public void Error(string title, Exception error)
        {
            Dialog("", Ui.Brush("Bad"), title, error.Message, new DialogAction("확인", null, true));
        }

        public void Toast(string text)
        {
            var toast = new Border
            {
                Background = new SolidColorBrush(Color.FromRgb(0x14, 0x1E, 0x33)), BorderBrush = Ui.Brush("Accent"), BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(12), Padding = new Thickness(16, 10, 16, 10), Margin = new Thickness(0, 8, 0, 0),
                Child = new TextBlock { Text = text, Foreground = Ui.Brush("Text"), FontSize = 13 },
                Effect = new DropShadowEffect { Color = Color.FromRgb(0x22, 0xD3, 0xEE), BlurRadius = 18, ShadowDepth = 0, Opacity = 0.35 },
            };
            toasts.Children.Add(toast);
            var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(500)) { BeginTime = TimeSpan.FromSeconds(2.6) };
            fade.Completed += delegate { toasts.Children.Remove(toast); };
            toast.BeginAnimation(OpacityProperty, fade);
        }
    }

    // Page building blocks with the theme's styles.
    public static class Ui
    {
        public static T Res<T>(string key)
        {
            return (T)Application.Current.Resources[key];
        }

        public static Brush Brush(string key)
        {
            return Res<Brush>(key);
        }

        public static Style Style(string key)
        {
            return Res<Style>(key);
        }

        public static TextBlock Text(string text, string style = "Body")
        {
            return new TextBlock { Text = text, Style = Style(style) };
        }

        public static TextBlock Icon(string glyph, double size = 16, Brush brush = null)
        {
            var icon = new TextBlock { Text = glyph, Style = Style("Icon"), FontSize = size };
            if (brush != null)
                icon.Foreground = brush;
            return icon;
        }

        // A scrolling page: title, a line under it, then the content.
        public static FrameworkElement Page(string title, string subtitle, params UIElement[] content)
        {
            var stack = new StackPanel { Margin = new Thickness(16, 8, 16, 24) };
            var head = Text(title, "H1");
            stack.Children.Add(head);
            if (subtitle != null)
            {
                var sub = Text(subtitle, "Caption");
                sub.Margin = new Thickness(0, 4, 0, 20);
                stack.Children.Add(sub);
            }
            foreach (UIElement element in content)
                stack.Children.Add(element);
            return new ScrollViewer { Content = stack, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        }

        public static Border Card(string title, params UIElement[] children)
        {
            var stack = new StackPanel();
            if (title != null)
                stack.Children.Add(Text(title, "H2"));
            foreach (UIElement child in children)
                stack.Children.Add(child);
            return new Border { Style = Style("Card"), Child = stack };
        }

        // label (and a hint under it) on the left, the control on the right.
        public static FrameworkElement Setting(string label, string hint, UIElement control)
        {
            var grid = new Grid { Margin = new Thickness(0, 0, 0, 14) };
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var left = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 20, 0) };
            var name = Text(label, "Body");
            name.FontWeight = FontWeights.SemiBold;
            left.Children.Add(name);
            if (hint != null)
            {
                var h = Text(hint, "Caption");
                h.Margin = new Thickness(0, 2, 0, 0);
                left.Children.Add(h);
            }
            grid.Children.Add(left);
            var element = control as FrameworkElement;
            if (element != null)
                element.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(control, 1);
            grid.Children.Add(control);
            return grid;
        }

        public static CheckBox Switch(bool on, Action<bool> changed, string text = null)
        {
            var box = new CheckBox { Style = Style("Switch"), IsChecked = on, Content = text };
            box.Checked += delegate { changed(true); };
            box.Unchecked += delegate { changed(false); };
            return box;
        }

        class Choice
        {
            public string Value, Label;

            public override string ToString()
            {
                return Label;
            }
        }

        // A drop-down of (value, label) pairs; a value it does not list (written by hand) is kept as an extra item.
        public static ComboBox Choices(string value, Action<string> changed, params string[] valueLabelPairs)
        {
            var box = new ComboBox { MinWidth = 300 };
            for (int i = 0; i + 1 < valueLabelPairs.Length; i += 2)
                box.Items.Add(new Choice { Value = valueLabelPairs[i], Label = valueLabelPairs[i + 1] });
            Choice selected = box.Items.Cast<Choice>().FirstOrDefault(c => c.Value == value);
            if (selected == null)
                box.Items.Add(selected = new Choice { Value = value, Label = value + " (직접 쓴 값)" });
            box.SelectedItem = selected;
            box.SelectionChanged += delegate
            {
                var choice = box.SelectedItem as Choice;
                if (choice != null)
                    changed(choice.Value);
            };
            return box;
        }

        public static FrameworkElement Slider(double min, double max, double value, double step, Func<double, string> format, Action<double> changed)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal };
            var slider = new Slider { Minimum = min, Maximum = max, Value = value, Width = 240, IsSnapToTickEnabled = true, TickFrequency = step, SmallChange = step, LargeChange = step * 5, VerticalAlignment = VerticalAlignment.Center };
            var label = new TextBlock { Text = format(value), Width = 56, Foreground = Ui.Brush("SubText"), TextAlignment = TextAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
            // Saved when the drag ends (or after a click/key), not at every step.
            var save = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(400) };
            save.Tick += delegate
            {
                save.Stop();
                changed(slider.Value);
            };
            slider.ValueChanged += delegate
            {
                label.Text = format(slider.Value);
                save.Stop();
                save.Start();
            };
            panel.Children.Add(slider);
            panel.Children.Add(label);
            return panel;
        }

        public static Button Button(string text, Action click, string style = null, string glyph = null, Thickness? margin = null)
        {
            object content = text;
            if (glyph != null)
            {
                var row = new StackPanel { Orientation = Orientation.Horizontal };
                row.Children.Add(new TextBlock { Text = glyph, Style = Style("Icon"), FontSize = 14, Margin = new Thickness(0, 0, 8, 0) });
                row.Children.Add(new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center });
                content = row;
            }
            var button = new Button { Content = content };
            if (style != null)
                button.Style = Style(style);
            if (margin.HasValue)
                button.Margin = margin.Value;
            button.Click += delegate { click(); };
            return button;
        }

        public static Border Chip(string glyph, string text, Brush color)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal };
            row.Children.Add(new TextBlock { Text = glyph, Style = Style("Icon"), FontSize = 12, Foreground = color, Margin = new Thickness(0, 0, 7, 0) });
            row.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Ui.Brush("Text"), VerticalAlignment = VerticalAlignment.Center });
            return new Border { Style = Style("Chip"), Child = row, HorizontalAlignment = HorizontalAlignment.Left };
        }

        public static StackPanel Row(params UIElement[] children)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal };
            foreach (UIElement child in children)
                row.Children.Add(child);
            return row;
        }

        public static UniformGrid Columns(int count, params UIElement[] children)
        {
            var grid = new UniformGrid { Columns = count };
            foreach (UIElement child in children)
                grid.Children.Add(child);
            return grid;
        }

        public static string FolderDialog(string description, string start)
        {
            using (var dialog = new System.Windows.Forms.FolderBrowserDialog { Description = description, ShowNewFolderButton = true })
            {
                if (!string.IsNullOrEmpty(start) && System.IO.Directory.Exists(start))
                    dialog.SelectedPath = start;
                return dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK ? dialog.SelectedPath : null;
            }
        }

        public static void OpenFolder(string path)
        {
            System.IO.Directory.CreateDirectory(path);
            System.Diagnostics.Process.Start("explorer.exe", "\"" + path + "\"");
        }
    }
}
