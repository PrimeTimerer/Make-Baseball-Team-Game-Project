// Home and video settings pages.
using System;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;

namespace Yktk3Launcher
{
    // A page built again each time it is shown, so it always shows the files as they are.
    public abstract class PageBase : IPage
    {
        protected readonly MainWindow Window;
        readonly ContentControl view = new ContentControl();

        protected PageBase(MainWindow window)
        {
            Window = window;
        }

        protected Launcher Launcher { get { return Window.Launcher; } }
        public FrameworkElement View { get { return view; } }

        public virtual void Shown()
        {
            view.Content = Build();
        }

        public virtual void Hidden()
        {
        }

        protected abstract FrameworkElement Build();
    }

    public class HomePage : PageBase
    {
        public HomePage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            string disc = Launcher.DiscImage;
            PcInfo pc = PcInfo.Read();
            var stack = new StackPanel { Margin = new Thickness(16, 8, 16, 24) };
            stack.Children.Add(Hero(disc));
            FrameworkElement banner = RecommendationBanner(pc);
            if (banner != null)
                stack.Children.Add(banner);
            Border discCard = DiscCard(disc), record = RecordCard(), pcCard = PcCard(pc);
            discCard.Margin = record.Margin = new Thickness(0, 0, 16, 16);
            stack.Children.Add(Ui.Columns(3, discCard, record, pcCard));
            return new ScrollViewer { Content = stack, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        }

        FrameworkElement Hero(string disc)
        {
            var hero = new Grid { Height = 370, Margin = new Thickness(0, 0, 0, 18) };
            hero.SizeChanged += delegate { hero.Clip = new RectangleGeometry(new Rect(0, 0, hero.ActualWidth, hero.ActualHeight), 24, 24); };
            var gradient = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 1) };
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x0E, 0x1B, 0x3C), 0));
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x16, 0x10, 0x36), 0.62));
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x0A, 0x12, 0x24), 1));
            hero.Children.Add(new Border { Background = gradient, CornerRadius = new CornerRadius(24), BorderBrush = Ui.Brush("Line"), BorderThickness = new Thickness(1) });
            FrameworkElement art = FieldArt();
            art.HorizontalAlignment = HorizontalAlignment.Right;
            art.VerticalAlignment = VerticalAlignment.Bottom;
            art.Margin = new Thickness(0, 0, 10, -6);
            hero.Children.Add(art);

            string cover = Launcher.State("CoverImage", "");
            if (cover.Length > 0 && File.Exists(cover))
            {
                try
                {
                    var image = new BitmapImage();
                    image.BeginInit();
                    image.CacheOption = BitmapCacheOption.OnLoad; // the file is not kept open
                    image.UriSource = new Uri(cover);
                    image.EndInit();
                    hero.Children.Add(new Border
                    {
                        Width = 228, Height = 310, CornerRadius = new CornerRadius(16), HorizontalAlignment = HorizontalAlignment.Right,
                        VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 48, 0),
                        Background = new ImageBrush(image) { Stretch = Stretch.UniformToFill }, BorderBrush = Ui.Brush("Line"), BorderThickness = new Thickness(1),
                        Effect = new DropShadowEffect { Color = Color.FromRgb(0x22, 0xD3, 0xEE), BlurRadius = 36, ShadowDepth = 0, Opacity = 0.45 },
                    });
                }
                catch (Exception)
                {
                }
            }

            var left = new StackPanel { Margin = new Thickness(44, 30, 40, 30), VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Left };
            var caption = Ui.Text("PLAYSTATION 2  ·  SEGA  ·  2005", "Caption");
            caption.Foreground = Ui.Brush("Accent");
            caption.FontWeight = FontWeights.SemiBold;
            left.Children.Add(caption);
            left.Children.Add(new TextBlock
            {
                Text = "プロ野球チームを\nつくろう！3", FontFamily = Ui.Res<FontFamily>("TitleFont"), FontSize = 44, FontWeight = FontWeights.Bold,
                Foreground = Brushes.White, LineHeight = 54, Margin = new Thickness(0, 10, 0, 0),
                Effect = new DropShadowEffect { Color = Color.FromRgb(0x63, 0x66, 0xF1), BlurRadius = 26, ShadowDepth = 0, Opacity = 0.7 },
            });
            var subtitle = Ui.Text("Make Baseball Team Game Project  ·  PC 포트", "Caption");
            subtitle.FontSize = 14;
            subtitle.Margin = new Thickness(0, 10, 0, 18);
            left.Children.Add(subtitle);

            var chips = new WrapPanel { MaxWidth = 560, Margin = new Thickness(0, 0, 0, 18) };
            chips.Children.Add(disc != null ? Ui.Chip("", "디스크 확인됨", Ui.Brush("Good")) : Ui.Chip("", "디스크 없음", Ui.Brush("Warn")));
            string card = CardStore.CardPath(Launcher);
            if (File.Exists(card))
            {
                int saves = -1;
                try
                {
                    saves = Ps2Card.Load(card).Saves().Count;
                }
                catch (Exception)
                {
                }
                chips.Children.Add(Ui.Chip("", saves >= 0 ? "메모리카드 · 세이브 " + saves + "개" : "메모리카드", Ui.Brush("Accent")));
            }
            int cheats = Cheats.EnabledNames(Launcher).Count;
            if (cheats > 0)
                chips.Children.Add(Ui.Chip("", "치트 " + cheats + "개 켜짐", Ui.Brush("Accent2")));
            if (Launcher.State("AutoBackup", "1") == "1")
                chips.Children.Add(Ui.Chip("", "시작 전 자동 백업", Ui.Brush("SubText")));
            left.Children.Add(chips);

            var play = new Button { Style = Ui.Style("PlayButton"), Content = Ui.Row(Ui.Icon("", 20, Brushes.White), new TextBlock { Text = "  플레이", VerticalAlignment = VerticalAlignment.Center }) };
            play.Click += delegate { Window.Play(false); };
            var safe = Ui.Button("안전 설정으로 실행", delegate { Window.Play(true); }, null, "", new Thickness(16, 0, 0, 0));
            safe.ToolTip = "이번 한 번만 1배 해상도, HD 텍스처·필터·치트 끔으로 실행합니다 (설정은 바뀌지 않습니다).";
            var buttons = Ui.Row(play, safe);
            buttons.VerticalAlignment = VerticalAlignment.Center;
            left.Children.Add(buttons);
            var keys = Ui.Text("게임 중  F1 단축키 목록 · F2 메뉴 · F7 200% · F8 터보 · F5 빠른 저장 · F9 불러오기", "Caption");
            keys.Margin = new Thickness(4, 14, 0, 0);
            left.Children.Add(keys);
            hero.Children.Add(left);

            var coverButton = Ui.Button("표지 이미지", PickCover, null, "");
            coverButton.HorizontalAlignment = HorizontalAlignment.Right;
            coverButton.VerticalAlignment = VerticalAlignment.Top;
            coverButton.Margin = new Thickness(0, 16, 16, 0);
            coverButton.Padding = new Thickness(12, 6, 12, 6);
            coverButton.Opacity = 0.85;
            hero.Children.Add(coverButton);
            return hero;
        }

        // A baseball field in neon lines.
        static FrameworkElement FieldArt()
        {
            var canvas = new Canvas { Width = 560, Height = 360, Opacity = 0.55, IsHitTestVisible = false };
            var line = new SolidColorBrush(Color.FromRgb(0x22, 0xD3, 0xEE));
            var violet = new SolidColorBrush(Color.FromRgb(0xA8, 0x55, 0xF7));
            Action<string, Brush, double> add = delegate(string data, Brush brush, double thickness)
            {
                canvas.Children.Add(new System.Windows.Shapes.Path { Data = Geometry.Parse(data), Stroke = brush, StrokeThickness = thickness, StrokeLineJoin = PenLineJoin.Round });
            };
            add("M 300,340 L 88,128 M 300,340 L 512,128", line, 1.6);                // foul lines
            add("M 88,128 A 300,300 0 0 1 512,128", violet, 2.2);                   // fence
            add("M 183,223 A 165,165 0 0 1 417,223", line, 1.2);                    // infield edge
            add("M 300,340 L 410,230 L 300,120 L 190,230 Z", line, 2.0);            // diamond
            add("M 410,222 L 418,230 L 410,238 L 402,230 Z M 300,112 L 308,120 L 300,128 L 292,120 Z M 190,222 L 198,230 L 190,238 L 182,230 Z", line, 2.0);
            var mound = new Ellipse { Width = 18, Height = 18, Stroke = line, StrokeThickness = 1.6 };
            Canvas.SetLeft(mound, 291);
            Canvas.SetTop(mound, 221);
            canvas.Children.Add(mound);
            canvas.Effect = new DropShadowEffect { Color = Color.FromRgb(0x22, 0xD3, 0xEE), BlurRadius = 14, ShadowDepth = 0, Opacity = 1 };
            return canvas;
        }

        void PickCover()
        {
            Action pick = delegate
            {
                var dialog = new Microsoft.Win32.OpenFileDialog { Title = "표지 이미지", Filter = "이미지 (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp" };
                if (dialog.ShowDialog(Window) != true)
                    return;
                try
                {
                    string target = System.IO.Path.Combine(Launcher.Dir, "launcher-cover" + System.IO.Path.GetExtension(dialog.FileName).ToLowerInvariant());
                    File.Copy(dialog.FileName, target, true);
                    Launcher.SetState("CoverImage", target);
                    Window.Refresh();
                }
                catch (Exception error)
                {
                    Window.Error("표지 이미지를 쓰지 못했습니다", error);
                }
            };
            if (Launcher.State("CoverImage", "").Length == 0)
            {
                pick();
                return;
            }
            Window.Dialog("", Ui.Brush("Accent"), "표지 이미지", "홈과 Steam 바로가기에 쓰는 그림입니다. 직접 가진 표지 스캔이나 좋아하는 그림을 고르세요.",
                          new DialogAction("다른 이미지 고르기", pick, true),
                          new DialogAction("지우기", delegate
                          {
                              Launcher.SetState("CoverImage", "");
                              Window.Refresh();
                          }),
                          new DialogAction("취소", null));
        }

        FrameworkElement RecommendationBanner(PcInfo pc)
        {
            string signature = pc.Gpu + "|" + pc.ScreenWidth + "x" + pc.ScreenHeight;
            if (Launcher.State("RecommendedFor", "") == signature)
                return null;
            string why;
            int scale = pc.RecommendedScale(out why);
            var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            var title = Ui.Text("이 PC에 맞는 설정을 찾았습니다", "Body");
            title.FontWeight = FontWeights.SemiBold;
            text.Children.Add(title);
            text.Children.Add(Ui.Text(pc.Gpu + " · " + why + " 내부 해상도 " + scale + "배(" + 640 * scale + "×" + 448 * scale + "), 이방성 필터 " + (pc.WeakGpu ? "4x" : "16x") + ", FXAA를 적용합니다.", "Caption"));
            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var icon = Ui.Icon("", 22, Ui.Brush("Accent"));
            icon.Margin = new Thickness(0, 0, 16, 0);
            grid.Children.Add(icon);
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            var buttons = Ui.Row(Ui.Button("적용", delegate
            {
                Recommendation.Apply(Launcher, pc);
                Launcher.SetState("RecommendedFor", signature);
                Window.Toast("추천 설정을 적용했습니다");
                Window.Refresh();
            }, "AccentButton"), Ui.Button("그대로 두기", delegate
            {
                Launcher.SetState("RecommendedFor", signature);
                Window.Refresh();
            }, null, null, new Thickness(8, 0, 0, 0)));
            buttons.Margin = new Thickness(16, 0, 0, 0);
            Grid.SetColumn(buttons, 2);
            grid.Children.Add(buttons);
            return new Border { Style = Ui.Style("Card"), BorderBrush = Ui.Brush("Accent"), Child = grid };
        }

        Border DiscCard(string disc)
        {
            var status = disc != null ? Ui.Chip("", "SLPM-65853 확인됨", Ui.Brush("Good")) : Ui.Chip("", "찾지 못함", Ui.Brush("Warn"));
            status.HorizontalAlignment = HorizontalAlignment.Left;
            string setting = Launcher.Config("Game", "Iso", "");
            var name = Ui.Text(disc != null ? System.IO.Path.GetFileName(disc) : "디스크 이미지가 없습니다", "Body");
            name.FontWeight = FontWeights.SemiBold;
            var where = Ui.Text(disc != null ? (setting.Length == 0 ? "game 폴더에서 자동으로 찾음 · " : "") + System.IO.Path.GetDirectoryName(disc) : "game 폴더에 .iso를 넣거나 직접 고르세요.", "Caption");
            where.Margin = new Thickness(0, 2, 0, 14);
            var buttons = new WrapPanel();
            buttons.Children.Add(Ui.Button("고르기", delegate
            {
                var dialog = new Microsoft.Win32.OpenFileDialog { Title = "SLPM-65853 디스크 이미지", Filter = "디스크 이미지 (*.iso)|*.iso|모든 파일 (*.*)|*.*" };
                if (dialog.ShowDialog(Window) != true)
                    return;
                if (!Disc.HasGame(dialog.FileName))
                {
                    Window.Dialog("", Ui.Brush("Warn"), "SLPM-65853 디스크가 아닙니다", "이 이미지에는 SLPM_658.53이 없습니다: " + dialog.FileName, new DialogAction("확인", null, true));
                    return;
                }
                Launcher.SetConfig("Game", "Iso", dialog.FileName);
                Window.Refresh();
            }, null, "", new Thickness(0, 0, 8, 0)));
            if (setting.Length > 0)
                buttons.Children.Add(Ui.Button("자동으로 찾기", delegate
                {
                    Launcher.SetConfig("Game", "Iso", "");
                    Window.Refresh();
                }, null, ""));
            return Ui.Card("디스크", status, name, where, buttons);
        }

        Border RecordCard()
        {
            long seconds;
            long.TryParse(Launcher.State("PlayTimeSeconds", "0"), out seconds);
            string last = Launcher.State("LastPlayed", "");
            var stack = new StackPanel();
            stack.Children.Add(Line("마지막 플레이", last.Length > 0 ? last : "아직 없음"));
            stack.Children.Add(Line("총 플레이 시간", seconds / 3600 + "시간 " + seconds / 60 % 60 + "분"));
            LogInfo run = Logs.List(Launcher.LogsDir).FirstOrDefault();
            if (run != null)
            {
                bool ok = run.ExitCode == "0";
                stack.Children.Add(Line("최근 실행", run.ExitCode == null ? "기록 없음" : ok ? "정상 종료" : "오류로 끝남 (코드 " + run.ExitCode + ")", ok ? null : Ui.Brush("Bad")));
                if (!ok && run.ExitCode != null)
                    stack.Children.Add(Ui.Button("로그 보기", delegate { Window.Navigate("logs"); }, null, "", new Thickness(0, 6, 0, 0)));
            }
            return Ui.Card("플레이 기록", stack);
        }

        Border PcCard(PcInfo pc)
        {
            string why;
            int scale = pc.RecommendedScale(out why);
            string current = Launcher.Config("Video", "InternalResolution", "1");
            var stack = new StackPanel();
            stack.Children.Add(Line("그래픽카드", pc.Gpu + " (" + PcInfo.Size(pc.Vram) + ")"));
            stack.Children.Add(Line("모니터", pc.ScreenWidth + "×" + pc.ScreenHeight));
            stack.Children.Add(Line("내부 해상도", "지금 " + current + "배 · 추천 " + scale + "배"));
            if (current != scale.ToString())
                stack.Children.Add(Ui.Button("추천대로 바꾸기", delegate
                {
                    Recommendation.Apply(Launcher, pc);
                    Window.Toast("내부 해상도 " + scale + "배로 바꿨습니다");
                    Window.Refresh();
                }, null, "", new Thickness(0, 6, 0, 0)));
            return Ui.Card("이 PC", stack);
        }

        static FrameworkElement Line(string label, string value, Brush valueBrush = null)
        {
            var grid = new Grid { Margin = new Thickness(0, 0, 0, 8) };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(96) });
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.Children.Add(Ui.Text(label, "Caption"));
            var text = Ui.Text(value, "Body");
            if (valueBrush != null)
                text.Foreground = valueBrush;
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            return grid;
        }
    }

    public static class Recommendation
    {
        public static void Apply(Launcher launcher, PcInfo pc)
        {
            string why;
            launcher.SetConfig("Video", "InternalResolution", pc.RecommendedScale(out why).ToString());
            launcher.SetConfig("Video", "AnisotropicFiltering", pc.WeakGpu ? "4" : "16");
            launcher.SetConfig("Video", "FXAA", "1");
        }
    }

    public class VideoPage : PageBase
    {
        public VideoPage(MainWindow window) : base(window)
        {
        }

        string Get(string key, string fallback)
        {
            return Launcher.Config("Video", key, fallback);
        }

        void Set(string key, string value)
        {
            Launcher.SetConfig("Video", key, value);
            Window.Toast("저장했습니다");
        }

        UIElement Pick(string key, string fallback, params string[] valueLabelPairs)
        {
            return Ui.Choices(Get(key, fallback), delegate(string v) { Set(key, v); }, valueLabelPairs);
        }

        UIElement Toggle(string key, string fallback)
        {
            return Ui.Switch(Get(key, fallback) == "1", delegate(bool on) { Set(key, on ? "1" : "0"); });
        }

        UIElement Level(string key, string fallback, int min, int max)
        {
            double value;
            if (!double.TryParse(Get(key, fallback), out value))
                value = double.Parse(fallback);
            return Ui.Slider(min, max, value, 1, delegate(double v) { return ((int)v).ToString(); }, delegate(double v) { Set(key, ((int)v).ToString()); });
        }

        protected override FrameworkElement Build()
        {
            string mode = Get("StartFullscreen", "0") == "1" ? "fullscreen" : Get("StartMaximized", "0") == "1" ? "maximized" : "window";
            var hdFolder = new TextBox { Text = Get("HDTexturesFolder", ""), Width = 300 };
            hdFolder.LostFocus += delegate
            {
                if (hdFolder.Text.Trim() != Get("HDTexturesFolder", ""))
                    Set("HDTexturesFolder", hdFolder.Text.Trim());
            };
            var hdBrowse = Ui.Button("찾아보기", delegate
            {
                string folder = Ui.FolderDialog("HD 텍스처 팩 폴더 (replacements 폴더가 들어 있는 곳)", hdFolder.Text.Trim());
                if (folder == null)
                    return;
                hdFolder.Text = folder;
                Set("HDTexturesFolder", folder);
            }, null, null, new Thickness(8, 0, 0, 0));
            PcInfo pc = PcInfo.Read();
            string why;
            int scale = pc.RecommendedScale(out why);

            return Ui.Page("화면", "바꾸면 바로 config.ini에 저장되고, 다음에 게임을 시작할 때 적용됩니다.",
                Ui.Card("해상도와 창",
                    Ui.Setting("내부 해상도", "높을수록 선명합니다. 2배는 그래픽카드 부하가 4배입니다.",
                               Pick("InternalResolution", "1", "1", "1배 · 640×448 (원본)", "2", "2배 · 1280×896", "3", "3배 · 1920×1344",
                                    "4", "4배 · 2560×1792", "5", "5배 · 3200×2240", "6", "6배 · 3840×2688", "7", "7배 · 4480×3136", "8", "8배 · 5120×3584")),
                    Ui.Setting("시작할 때", "게임 중에는 F11로 전체 화면을 켜고 끕니다.",
                               Ui.Choices(mode, delegate(string v)
                               {
                                   Launcher.SetConfig("Video", "StartMaximized", v == "maximized" ? "1" : "0");
                                   Set("StartFullscreen", v == "fullscreen" ? "1" : "0");
                               }, "window", "창", "maximized", "최대화한 창", "fullscreen", "전체 화면")),
                    Ui.Setting("화면 비율", "이 게임은 4:3 화면입니다.", Pick("Stretch", "0", "0", "4:3 유지 (남는 곳은 검은 여백)", "1", "창에 맞춰 늘리기"))),
                Ui.Card("화질",
                    Ui.Setting("FXAA 안티에일리어싱", "계단 현상을 가볍게 줄입니다.", Toggle("FXAA", "1")),
                    Ui.Setting("화면 필터", "게임 화면을 창 크기로 늘릴 때의 필터입니다.", Pick("ScreenFilter", "1", "0", "없음 (선명한 픽셀)", "1", "부드럽게 (기본)", "2", "선명하게")),
                    Ui.Setting("텍스처 필터", null, Pick("TextureFiltering", "2", "0", "최근접 (픽셀)", "1", "바이리니어 강제", "2", "PS2와 같게 (기본)", "3", "바이리니어 강제 (스프라이트 제외)")),
                    Ui.Setting("트라이리니어 필터", null, Pick("TrilinearFiltering", "-1", "-1", "자동 (기본)", "0", "끔", "1", "PS2와 같게", "2", "강제")),
                    Ui.Setting("이방성 필터", "비스듬한 바닥(그라운드)이 선명해집니다.", Pick("AnisotropicFiltering", "0", "0", "끔", "2", "2x", "4", "4x", "8", "8x", "16", "16x")),
                    Ui.Setting("블렌딩 정확도", "높을수록 반투명 효과가 정확하지만 무거워집니다.", Pick("BlendingAccuracy", "1", "0", "최소", "1", "기본 (권장)", "2", "중간", "3", "높음", "4", "전체", "5", "최대 (무거움)")),
                    Ui.Setting("디더링", "켜면 흰 유니폼에 검은 점이 생깁니다.", Pick("Dithering", "0", "0", "끔 (기본)", "1", "스케일", "2", "언스케일", "3", "32비트 강제"))),
                Ui.Card("선명도와 색",
                    Ui.Setting("샤프닝 (AMD CAS)", null, Pick("Sharpening", "0", "0", "끔", "1", "선명하게만", "2", "선명하게 + 크기 조정")),
                    Ui.Setting("샤프닝 강도", null, Level("SharpeningStrength", "50", 0, 100)),
                    Ui.Setting("색 보정", "아래 밝기·대비·채도·감마를 적용합니다 (50 = 그대로).", Toggle("ColorCorrection", "0")),
                    Ui.Setting("밝기", null, Level("Brightness", "50", 1, 100)),
                    Ui.Setting("대비", null, Level("Contrast", "50", 1, 100)),
                    Ui.Setting("채도", null, Level("Saturation", "50", 1, 100)),
                    Ui.Setting("감마", null, Level("Gamma", "50", 1, 100)),
                    Ui.Setting("TV 셰이더", null, Pick("TVShader", "0", "0", "없음", "1", "주사선", "2", "대각선", "3", "삼각형", "4", "물결", "5", "CRT (Lottes)", "6", "4xRGSS", "7", "NxAGSS"))),
                Ui.Card("HD 텍스처",
                    Ui.Setting("HD 텍스처 팩 사용", "PCSX2 형식(PNG/DDS) 텍스처 팩을 씁니다.", Toggle("HDTextures", "1")),
                    Ui.Setting("팩 폴더", "replacements 폴더가 들어 있는 곳. 비우면 기본 위치를 씁니다.", Ui.Row(hdFolder, hdBrowse)),
                    Ui.Setting("시작할 때 모두 불러오기", "끊김은 줄지만 메모리를 많이 씁니다.", Toggle("PreloadHDTextures", "0"))),
                Ui.Card("게임 중",
                    Ui.Setting("FPS 표시", "게임 중 F3으로도 켜고 끕니다.", Ui.Switch(Launcher.Config("Runtime", "PS2X_SHOW_FPS", "0") == "1", delegate(bool on)
                    {
                        Launcher.SetConfig("Runtime", "PS2X_SHOW_FPS", on ? "1" : "0");
                        Window.Toast("저장했습니다");
                    })),
                    Ui.Setting("시작 속도", "게임 중에는 F7(200%)·F8(터보)로 바꿉니다. 100%보다 빠른 동안은 소리가 꺼집니다.",
                               Ui.Choices(Launcher.Config("Runtime", "PS2X_SPEED", "100"), delegate(string v)
                               {
                                   Launcher.SetConfig("Runtime", "PS2X_SPEED", v);
                                   Window.Toast("저장했습니다");
                               }, "100", "100% (보통)", "200", "200%", "0", "터보 (PC가 낼 수 있는 최대 속도)")),
                    Ui.Text("단축키: F1 단축키 목록 · F2 게임 메뉴(치트·선수 치트) · F3 FPS · F4 치트 다시 읽기 · F5 빠른 저장 · F9 빠른 불러오기 · F7 200% 켜기/끄기 · F8 터보 켜기/끄기 · F11 전체 화면", "Body"),
                    Ui.Text("개발용 (Ctrl과 함께): Ctrl+F6 성능 프로필 기록 · Ctrl+F7 프로필 표시 · Ctrl+F8 디버그 덤프", "Caption"),
                    Ui.Text("경기 배속은 게임 기능입니다: 경기 중 R2로 올리고 L2로 내립니다.", "Caption")),
                Ui.Card("이 PC",
                    Ui.Text(pc.Cpu + " · " + pc.Threads + "스레드 · RAM " + PcInfo.Size(pc.Ram), "Body"),
                    Ui.Text(pc.Gpu + " · VRAM " + PcInfo.Size(pc.Vram) + " · 모니터 " + pc.ScreenWidth + "×" + pc.ScreenHeight, "Body"),
                    Ui.Text(why + " 추천: 내부 해상도 " + scale + "배.", "Caption"),
                    Ui.Row(Ui.Button("추천 설정 적용", delegate
                    {
                        Recommendation.Apply(Launcher, pc);
                        Window.Toast("추천 설정을 적용했습니다");
                        Window.Refresh();
                    }, "AccentButton", "", new Thickness(0, 14, 8, 0)), Ui.Button("기본값으로 되돌리기", delegate
                    {
                        Window.Confirm("화면 설정 되돌리기", "화면 설정을 게임의 기본값으로 되돌립니다. 디스크·치트 설정은 그대로입니다.", "되돌리기", delegate
                        {
                            IniFile defaults = Defaults.Config(Launcher);
                            int start, end;
                            defaults.SectionRange("Video", out start, out end);
                            for (int i = start; i >= 0 && i < end; i++)
                            {
                                string k, v;
                                if (IniFile.KeyValue(defaults.Lines[i], out k, out v))
                                    Launcher.SetConfig("Video", k, v);
                            }
                            Window.Toast("화면 설정을 기본값으로 되돌렸습니다");
                            Window.Refresh();
                        });
                    }, null, "", new Thickness(0, 14, 0, 0)))));
        }
    }

    // The game's own defaults: yktk3.exe --write-settings into a temporary folder.
    public static class Defaults
    {
        static string dir;

        static string Dir(Launcher launcher)
        {
            if (dir != null)
                return dir;
            string folder = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "yktk3-launcher-defaults");
            Directory.CreateDirectory(folder);
            foreach (string name in new[] { "config.ini", "controls.ini" })
            {
                string file = System.IO.Path.Combine(folder, name);
                if (File.Exists(file) && File.GetLastWriteTimeUtc(file) < File.GetLastWriteTimeUtc(launcher.Exe))
                    File.Delete(file); // written by an older yktk3.exe
            }
            Game.WriteDefaultSettings(launcher.Dir, folder);
            return dir = folder;
        }

        public static IniFile Config(Launcher launcher)
        {
            return new IniFile(System.IO.Path.Combine(Dir(launcher), "config.ini"));
        }

        public static IniFile Controls(Launcher launcher)
        {
            return new IniFile(System.IO.Path.Combine(Dir(launcher), "controls.ini"));
        }
    }
}
