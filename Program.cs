using System.Diagnostics;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Text;
using System.Xml.Linq;

namespace CodexProxyLauncher;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();

        if (args.Contains("--self-test", StringComparer.OrdinalIgnoreCase))
        {
            var settings = SettingsStore.Load();
            Environment.ExitCode = LauncherService.TryResolveCodex(settings.CodexExePath, out _, out _) ? 0 : 2;
            return;
        }

        if (args.Contains("--self-test-wsl-proxy", StringComparer.OrdinalIgnoreCase))
        {
            var settings = SettingsStore.Load();
            var service = new LauncherService(settings, _ => { });
            Environment.ExitCode = service.TestWslProxyPropagation().Success ? 0 : 4;
            return;
        }

        if (args.Contains("--self-test-wsl-setup", StringComparer.OrdinalIgnoreCase))
        {
            var settings = SettingsStore.Load();
            var service = new LauncherService(settings, _ => { });
            var result = service.TestWslProxySetup();
            Console.Error.WriteLine(result.Message);
            Environment.ExitCode = result.Success ? 0 : 6;
            return;
        }

        if (args.Contains("--self-test-wsl-warning", StringComparer.OrdinalIgnoreCase))
        {
            var settings = SettingsStore.Load();
            var service = new LauncherService(settings, _ => { });
            var result = service.TestWslProxyWarning();
            Console.Error.WriteLine($"{result.ShowMessageBox}|{result.DialogTitle}|{result.DialogMessage}");
            Environment.ExitCode = result.ShowMessageBox ? 0 : 7;
            return;
        }

        if (args.Contains("--self-test-wsl-detection", StringComparer.OrdinalIgnoreCase))
        {
            Environment.ExitCode = WslBackendDetector.Detect(
                TryGetArgumentValue(args, "--codex-config", out var codexConfigPath) ? codexConfigPath : null).ShouldApplyProxy ? 0 : 5;
            return;
        }

        if (args.Contains("--start", StringComparer.OrdinalIgnoreCase))
        {
            var settings = SettingsStore.Load();
            var service = new LauncherService(settings, _ => { });
            Environment.ExitCode = service.StartCodex(CancellationToken.None).Success ? 0 : 3;
            return;
        }

        if (TryGetArgumentValue(args, "--snapshot-main", out var mainSnapshotPath))
        {
            using var form = new MainForm();
            SaveWindowSnapshot(form, mainSnapshotPath);
            return;
        }

        Application.Run(new MainForm());
    }

    private static bool TryGetArgumentValue(string[] args, string name, out string value)
    {
        for (var i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
            {
                value = args[i + 1];
                return true;
            }
        }

        value = "";
        return false;
    }

    private static void SaveWindowSnapshot(Form form, string path)
    {
        form.StartPosition = FormStartPosition.Manual;
        form.Location = new Point(-20000, -20000);
        form.ShowInTaskbar = false;
        form.Show();
        Application.DoEvents();
        Thread.Sleep(200);
        Application.DoEvents();

        using var bitmap = new Bitmap(form.Width, form.Height);
        form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".");
        bitmap.Save(path);
        form.Close();
    }
}

internal sealed class MainForm : Form
{
    private readonly TextBox _proxyAddress = new();
    private readonly TextBox _log = new();
    private readonly Button _startButton = new();
    private readonly Button _testButton = new();
    private LauncherSettings _settings = SettingsStore.Load();
    private bool _loadingSettings;

    public MainForm()
    {
        Text = "Codex 代理启动器";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(640, 500);
        Size = new Size(680, 540);
        Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? Icon;
        BackColor = Theme.Window;
        Font = new Font("Segoe UI", 9.5f);
        AutoScaleMode = AutoScaleMode.Dpi;
        ResizeRedraw = true;
        DoubleBuffered = true;

        BuildLayout();
        LoadSettingsIntoControls();
        _proxyAddress.TextChanged += (_, _) => SaveSettingsFromControls(false);
        AppendLog($"已加载配置：{SettingsStore.ConfigPath}");
        Shown += (_, _) =>
        {
            _proxyAddress.SelectionStart = _proxyAddress.TextLength;
            _proxyAddress.SelectionLength = 0;
            _startButton.Focus();
        };
    }

    private void BuildLayout()
    {
        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 3,
            Padding = new Padding(44, 28, 44, 24),
            BackColor = Theme.Window
        };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 128));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 58));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        Controls.Add(root);

        var proxyPanel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Theme.Window,
            Margin = new Padding(0, 8, 0, 6)
        };
        proxyPanel.Controls.Add(new Label
        {
            Text = "代理地址",
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 10.5f),
            ForeColor = Theme.Text,
            Location = new Point(0, 0)
        });
        var inputHost = Ui.CreateTextBoxHost(_proxyAddress);
        inputHost.Location = new Point(0, 48);
        inputHost.Size = new Size(1, 42);
        inputHost.Anchor = AnchorStyles.Left | AnchorStyles.Top;
        proxyPanel.Resize += (_, _) => inputHost.Width = Math.Max(1, proxyPanel.ClientSize.Width - 1);
        proxyPanel.Layout += (_, _) => inputHost.Width = Math.Max(1, proxyPanel.ClientSize.Width - 1);
        proxyPanel.Controls.Add(inputHost);
        root.Controls.Add(proxyPanel, 0, 0);

        var actions = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.RightToLeft,
            WrapContents = false,
            BackColor = Theme.Window,
            Padding = new Padding(0, 4, 0, 0)
        };
        _startButton.Text = "启动 Codex";
        StyleButton(_startButton, Theme.Accent, Color.White);
        _startButton.Click += async (_, _) => await RunActionAsync("start");
        _testButton.Text = "测试代理";
        StyleButton(_testButton, Theme.ButtonNeutral, Theme.Text);
        _testButton.Click += async (_, _) => await RunActionAsync("test");
        actions.Controls.Add(_startButton);
        actions.Controls.Add(_testButton);
        root.Controls.Add(actions, 0, 1);

        _log.Multiline = true;
        _log.ReadOnly = true;
        _log.ScrollBars = ScrollBars.Vertical;
        _log.BorderStyle = BorderStyle.None;
        _log.BackColor = Color.White;
        _log.ForeColor = Theme.Text;
        _log.Font = new Font("Consolas", 10f);
        _log.Dock = DockStyle.Fill;

        var logPanel = new RoundedPanel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.White,
            Padding = new Padding(12),
            Margin = new Padding(0)
        };
        logPanel.Controls.Add(_log);
        root.Controls.Add(logPanel, 0, 2);
    }

    private static void StyleButton(Button button, Color backColor, Color foreColor)
    {
        button.Size = new Size(122, 38);
        button.Margin = new Padding(8, 0, 0, 0);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = 0;
        button.BackColor = backColor;
        button.ForeColor = foreColor;
        button.Font = new Font("Segoe UI Semibold", 9.5f);
        button.Cursor = Cursors.Hand;
    }

    private void LoadSettingsIntoControls()
    {
        _loadingSettings = true;
        try
        {
            _proxyAddress.Text = _settings.ProxyAddress;
        }
        finally
        {
            _loadingSettings = false;
        }
    }

    private void SaveSettingsFromControls(bool showMessage)
    {
        if (_loadingSettings)
        {
            return;
        }

        _settings.ProxyAddress = _proxyAddress.Text.Trim();
        SettingsStore.Save(_settings);
        if (showMessage)
        {
            AppendLog("配置已保存。");
        }
    }

    private async Task RunActionAsync(string action)
    {
        SetBusy(true);
        SaveSettingsFromControls(false);
        var service = new LauncherService(_settings, AppendLog);
        OperationResult result;

        try
        {
            result = action switch
            {
                "start" => await Task.Run(() => service.StartCodex(CancellationToken.None)),
                "test" => await Task.Run(() => service.TestProxy()),
                _ => OperationResult.Fail("未知操作。")
            };
        }
        catch (Exception ex)
        {
            result = OperationResult.Fail(ex.Message);
        }

        AppendLog(result.Message);
        SetBusy(false);

        if (result.ShowMessageBox)
        {
            MessageBox.Show(this, result.DialogMessage, result.DialogTitle, MessageBoxButtons.OK, result.DialogIcon);
        }
    }

    private void SetBusy(bool busy)
    {
        _startButton.Enabled = !busy;
        _testButton.Enabled = !busy;
        Cursor = busy ? Cursors.WaitCursor : Cursors.Default;
    }

    private void AppendLog(string message)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => AppendLog(message));
            return;
        }

        AppLog.Append(message);
        _log.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
    }
}

internal sealed class LauncherSettings
{
    public string ProxyAddress { get; set; } = "http://127.0.0.1:10808";
    public string ChromiumProxy { get; set; } = "";
    public string NoProxy { get; set; } = "localhost,127.0.0.1,::1";
    public string CodexExePath { get; set; } = "";
    public int StartupWaitSeconds { get; set; } = 20;
    public bool TemporarilySetUserProxyEnvironment { get; set; } = false;
    public bool EnableWslProxy { get; set; } = false;
}

internal static class SettingsStore
{
    public static string StateDir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CodexProxyLauncher");
    public static string ConfigPath => Path.Combine(AppContext.BaseDirectory, "config.txt");

    public static LauncherSettings Load()
    {
        var settings = new LauncherSettings();

        try
        {
            if (!File.Exists(ConfigPath))
            {
                Save(settings);
                return settings;
            }

            foreach (var rawLine in File.ReadAllLines(ConfigPath, Encoding.UTF8))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line.StartsWith('#'))
                {
                    continue;
                }

                var separator = line.IndexOf('=');
                if (separator <= 0)
                {
                    continue;
                }

                var key = line[..separator].Trim();
                var value = line[(separator + 1)..].Trim();
                ApplySetting(settings, key, value);
            }
        }
        catch
        {
            settings = new LauncherSettings();
        }

        return settings;
    }

    public static void Save(LauncherSettings settings)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(ConfigPath) ?? ".");
        File.WriteAllText(ConfigPath, BuildConfigText(settings), Encoding.UTF8);
    }

    private static void ApplySetting(LauncherSettings settings, string key, string value)
    {
        switch (NormalizeKey(key))
        {
            case "proxyaddress":
            case "proxy_address":
            case "proxy":
            case "http_proxy":
            case "all_proxy":
                settings.ProxyAddress = value;
                break;
            case "chromiumproxy":
            case "chromium_proxy":
                settings.ChromiumProxy = value;
                break;
            case "noproxy":
            case "no_proxy":
                settings.NoProxy = value;
                break;
            case "codexexepath":
            case "codex_exe_path":
                settings.CodexExePath = value;
                break;
            case "startupwaitseconds":
            case "startup_wait_seconds":
                if (int.TryParse(value, out var startupWait))
                {
                    settings.StartupWaitSeconds = Math.Clamp(startupWait, 3, 90);
                }
                break;
            case "temporarilysetuserproxyenvironment":
            case "temporarily_set_user_proxy_environment":
                if (bool.TryParse(value, out var setUserEnv))
                {
                    settings.TemporarilySetUserProxyEnvironment = setUserEnv;
                }
                break;
            case "enablewslproxy":
            case "enable_wsl_proxy":
                if (bool.TryParse(value, out var enableWslProxy))
                {
                    settings.EnableWslProxy = enableWslProxy;
                }
                break;
        }
    }

    private static string BuildConfigText(LauncherSettings settings) =>
        $"""
        # Codex Proxy Launcher configuration
        # The GUI only edits proxy_address. Change the other values here, then restart the launcher.

        proxy_address={settings.ProxyAddress}
        chromium_proxy={settings.ChromiumProxy}
        no_proxy={settings.NoProxy}
        codex_exe_path={settings.CodexExePath}
        startup_wait_seconds={settings.StartupWaitSeconds}
        temporarily_set_user_proxy_environment={settings.TemporarilySetUserProxyEnvironment.ToString().ToLowerInvariant()}
        enable_wsl_proxy={settings.EnableWslProxy.ToString().ToLowerInvariant()}
        """;

    private static string NormalizeKey(string key) =>
        new string(key.Where(c => c != '-' && !char.IsWhiteSpace(c)).ToArray()).ToLowerInvariant();
}

internal static class AppLog
{
    public static string Path => System.IO.Path.Combine(AppContext.BaseDirectory, "log.txt");

    public static void Append(string message)
    {
        try
        {
            File.AppendAllText(Path, $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {message}{Environment.NewLine}", Encoding.UTF8);
        }
        catch
        {
            // Logging must never block the launcher.
        }
    }
}

internal readonly record struct WslBackendDetection(bool ShouldApplyProxy, string Message);

internal static class WslBackendDetector
{
    private static readonly string CodexConfigPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        ".codex",
        "config.toml");

    public static WslBackendDetection Detect(string? configPath = null)
    {
        var path = string.IsNullOrWhiteSpace(configPath) ? CodexConfigPath : configPath;
        if (ConfigEnablesWslBackend(path))
        {
            return new WslBackendDetection(true, "检测到 Codex 使用了 WSL 后端：runCodexInWindowsSubsystemForLinux = true。");
        }

        return new WslBackendDetection(false, "Codex 配置未开启 runCodexInWindowsSubsystemForLinux，跳过 WSL 代理注入。");
    }

    private static bool ConfigEnablesWslBackend(string configPath)
    {
        try
        {
            if (!File.Exists(configPath))
            {
                return false;
            }

            foreach (var rawLine in File.ReadLines(configPath, Encoding.UTF8))
            {
                var commentStart = rawLine.IndexOf('#');
                var line = (commentStart >= 0 ? rawLine[..commentStart] : rawLine).Trim();
                if (line.Length == 0 || line.StartsWith('#'))
                {
                    continue;
                }

                var separator = line.IndexOf('=');
                if (separator <= 0)
                {
                    continue;
                }

                var key = line[..separator].Trim();
                if (!string.Equals(key, "runCodexInWindowsSubsystemForLinux", StringComparison.Ordinal))
                {
                    continue;
                }

                var value = line[(separator + 1)..].Trim().Trim('"', '\'');
                if (bool.TryParse(value, out var enabled))
                {
                    return enabled;
                }
            }
        }
        catch
        {
            return false;
        }

        return false;
    }
}

internal static class WslProxyProbe
{
    public static OperationResult Verify(Dictionary<string, string> environment, string expectedProxy, string expectedNoProxy)
    {
        try
        {
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = "wsl.exe",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8
                }
            };

            process.StartInfo.ArgumentList.Add("-e");
            process.StartInfo.ArgumentList.Add("sh");
            process.StartInfo.ArgumentList.Add("-c");
            process.StartInfo.ArgumentList.Add("printf '%s\n%s\n%s\n%s\n%s\n' \"$HTTP_PROXY\" \"$HTTPS_PROXY\" \"$ALL_PROXY\" \"$NO_PROXY\" \"$WSLENV\"");

            foreach (var item in environment)
            {
                process.StartInfo.Environment[item.Key] = item.Value;
            }

            process.Start();
            var outputTask = process.StandardOutput.ReadToEndAsync();
            var errorTask = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(10000))
            {
                try
                {
                    process.Kill(entireProcessTree: true);
                }
                catch
                {
                    // Best effort cleanup for a hung probe process.
                }
                return OperationResult.Fail("WSL 代理验证超时。");
            }

            var output = outputTask.GetAwaiter().GetResult();
            var error = errorTask.GetAwaiter().GetResult();
            if (process.ExitCode != 0)
            {
                return OperationResult.Fail($"WSL 代理验证失败：{error.Trim()}");
            }

            var lines = output.Split('\n').Select(line => line.TrimEnd('\r')).ToArray();
            if (lines.Length < 5)
            {
                return OperationResult.Fail("WSL 代理验证输出不完整。");
            }

            if (lines[0] != expectedProxy ||
                lines[1] != expectedProxy ||
                lines[2] != expectedProxy ||
                lines[3] != expectedNoProxy ||
                !lines[4].Contains("HTTP_PROXY/u", StringComparison.Ordinal) ||
                !lines[4].Contains("NO_PROXY/u", StringComparison.Ordinal))
            {
                return OperationResult.Fail("WSL 未收到完整代理环境变量。");
            }

            return OperationResult.Ok("WSL 可以收到启动器注入的代理环境变量。");
        }
        catch (Exception ex)
        {
            return OperationResult.Fail($"WSL 代理验证失败：{ex.Message}");
        }
    }
}

internal static class WslProxyConfigurator
{
    public static OperationResult Configure(Uri proxyUri, string noProxy)
    {
        try
        {
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = "wsl.exe",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8
                }
            };

            process.StartInfo.ArgumentList.Add("-e");
            process.StartInfo.ArgumentList.Add("sh");
            process.StartInfo.ArgumentList.Add("-lc");
            process.StartInfo.ArgumentList.Add(BuildScript(proxyUri, noProxy));

            process.Start();
            var outputTask = process.StandardOutput.ReadToEndAsync();
            var errorTask = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(15000))
            {
                try
                {
                    process.Kill(entireProcessTree: true);
                }
                catch
                {
                    // Best effort cleanup for a hung WSL setup process.
                }

                return OperationResult.Fail("WSL 内代理设置超时。");
            }

            var output = outputTask.GetAwaiter().GetResult().Trim();
            var error = errorTask.GetAwaiter().GetResult().Trim();
            if (process.ExitCode != 0)
            {
                if (process.ExitCode == 13 &&
                    TryParseUnreachableProxy(error, output, out var host, out var port))
                {
                    return OperationResult.ProxyUnreachable(BuildUnreachableProxyMessage(host, port));
                }

                var message = string.IsNullOrWhiteSpace(error) ? output : error;
                return OperationResult.Fail(message);
            }

            return OperationResult.Ok(string.IsNullOrWhiteSpace(output) ? "WSL 代理环境已更新。" : output);
        }
        catch (Exception ex)
        {
            return OperationResult.Fail($"WSL 内代理设置失败：{ex.Message}");
        }
    }

    private static string BuildScript(Uri proxyUri, string noProxy)
    {
        var loopbackProxy = IsLoopbackHost(proxyUri.Host);
        var proxyPrefix = proxyUri.Scheme + "://" + (string.IsNullOrEmpty(proxyUri.UserInfo) ? "" : proxyUri.UserInfo + "@");
        var proxySuffix = ":" + proxyUri.Port + GetPathSuffix(proxyUri);
        var proxyUrl = proxyUri.GetComponents(UriComponents.AbsoluteUri, UriFormat.UriEscaped);
        var proxyHost = proxyUri.Host;
        var proxyPort = proxyUri.Port.ToString();

        var script = new StringBuilder();
        script.AppendLine("set -eu");
        script.AppendLine($"CODEX_PROXY_PORT={ShQuote(proxyPort)}");
        script.AppendLine($"CODEX_NO_PROXY={ShQuote(noProxy)}");
        script.AppendLine("codex_test_tcp() {");
        script.AppendLine("  command -v bash >/dev/null 2>&1 || return 2");
        script.AppendLine("  if command -v timeout >/dev/null 2>&1; then");
        script.AppendLine("    CODEX_TEST_HOST=\"$1\" CODEX_TEST_PORT=\"$2\" timeout 4 bash -lc ': >/dev/tcp/$CODEX_TEST_HOST/$CODEX_TEST_PORT' >/dev/null 2>&1");
        script.AppendLine("  else");
        script.AppendLine("    CODEX_TEST_HOST=\"$1\" CODEX_TEST_PORT=\"$2\" bash -lc ': >/dev/tcp/$CODEX_TEST_HOST/$CODEX_TEST_PORT' >/dev/null 2>&1");
        script.AppendLine("  fi");
        script.AppendLine("}");
        script.AppendLine("codex_escape_double() {");
        script.AppendLine("  printf '%s' \"$1\" | sed 's/\\\\/\\\\\\\\/g; s/\"/\\\\\"/g; s/\\$/\\\\$/g; s/`/\\\\`/g'");
        script.AppendLine("}");
        script.AppendLine("codex_write_export() {");
        script.AppendLine("  CODEX_EXPORT_VALUE=\"$(codex_escape_double \"$2\")\"");
        script.AppendLine("  printf 'export %s=\"%s\"\\n' \"$1\" \"$CODEX_EXPORT_VALUE\"");
        script.AppendLine("}");

        if (loopbackProxy)
        {
            script.AppendLine($"CODEX_PROXY_PREFIX={ShQuote(proxyPrefix)}");
            script.AppendLine($"CODEX_PROXY_SUFFIX={ShQuote(proxySuffix)}");
            script.AppendLine("CODEX_WINDOWS_HOST=\"$(awk '/^nameserver[ \\t]+/ {print $2; exit}' /etc/resolv.conf 2>/dev/null || true)\"");
            script.AppendLine("if [ -z \"$CODEX_WINDOWS_HOST\" ]; then");
            script.AppendLine("  CODEX_WINDOWS_HOST=\"$(ip route 2>/dev/null | awk '/^default / {print $3; exit}' || true)\"");
            script.AppendLine("fi");
            script.AppendLine("CODEX_PROXY_HOST=\"\"");
            script.AppendLine("for CODEX_CANDIDATE_HOST in 127.0.0.1 localhost \"$CODEX_WINDOWS_HOST\"; do");
            script.AppendLine("  if [ -n \"$CODEX_CANDIDATE_HOST\" ] && codex_test_tcp \"$CODEX_CANDIDATE_HOST\" \"$CODEX_PROXY_PORT\"; then");
            script.AppendLine("    CODEX_PROXY_HOST=\"$CODEX_CANDIDATE_HOST\"");
            script.AppendLine("    break");
            script.AppendLine("  fi");
            script.AppendLine("done");
            script.AppendLine("if [ -z \"$CODEX_PROXY_HOST\" ]; then");
            script.AppendLine("  CODEX_PROXY_HOST=\"$CODEX_WINDOWS_HOST\"");
            script.AppendLine("  CODEX_PROXY_REACHABLE=0");
            script.AppendLine("else");
            script.AppendLine("  CODEX_PROXY_REACHABLE=1");
            script.AppendLine("fi");
            script.AppendLine("CODEX_PROXY_URL=\"${CODEX_PROXY_PREFIX}${CODEX_PROXY_HOST}${CODEX_PROXY_SUFFIX}\"");
        }
        else
        {
            script.AppendLine($"CODEX_PROXY_HOST={ShQuote(proxyHost)}");
            script.AppendLine($"CODEX_PROXY_URL={ShQuote(proxyUrl)}");
            script.AppendLine("if codex_test_tcp \"$CODEX_PROXY_HOST\" \"$CODEX_PROXY_PORT\"; then");
            script.AppendLine("  CODEX_PROXY_REACHABLE=1");
            script.AppendLine("else");
            script.AppendLine("  CODEX_PROXY_REACHABLE=0");
            script.AppendLine("fi");
        }

        script.AppendLine("mkdir -p \"$HOME/.codex\"");
        script.AppendLine("CODEX_PROXY_FILE=\"$HOME/.codex/proxy-env.sh\"");
        script.AppendLine("{");
        script.AppendLine("  printf '%s\\n' '# Generated by Codex Proxy Launcher. Edit config.txt in Windows instead.'");
        script.AppendLine("  for CODEX_PROXY_NAME in HTTP_PROXY http_proxy HTTPS_PROXY https_proxy ALL_PROXY all_proxy WS_PROXY ws_proxy WSS_PROXY wss_proxy NPM_CONFIG_PROXY npm_config_proxy NPM_CONFIG_HTTPS_PROXY npm_config_https_proxy GLOBAL_AGENT_HTTP_PROXY global_agent_http_proxy; do");
        script.AppendLine("    codex_write_export \"$CODEX_PROXY_NAME\" \"$CODEX_PROXY_URL\"");
        script.AppendLine("  done");
        script.AppendLine("  codex_write_export NO_PROXY \"$CODEX_NO_PROXY\"");
        script.AppendLine("  codex_write_export no_proxy \"$CODEX_NO_PROXY\"");
        script.AppendLine("} > \"$CODEX_PROXY_FILE\"");
        script.AppendLine("chmod 600 \"$CODEX_PROXY_FILE\"");
        script.AppendLine("CODEX_MARKER='# codex-proxy-gui proxy'");
        script.AppendLine("CODEX_SOURCE_LINE='[ -f \"$HOME/.codex/proxy-env.sh\" ] && . \"$HOME/.codex/proxy-env.sh\"'");
        script.AppendLine("for CODEX_RC in \"$HOME/.profile\" \"$HOME/.bashrc\" \"$HOME/.zshrc\"; do");
        script.AppendLine("  touch \"$CODEX_RC\"");
        script.AppendLine("  if ! grep -Fq \"$CODEX_MARKER\" \"$CODEX_RC\"; then");
        script.AppendLine("    printf '\\n%s\\n%s\\n' \"$CODEX_MARKER\" \"$CODEX_SOURCE_LINE\" >> \"$CODEX_RC\"");
        script.AppendLine("  fi");
        script.AppendLine("done");
        script.AppendLine(". \"$CODEX_PROXY_FILE\"");
        script.AppendLine("printf 'WSL_PROXY=%s\\n' \"$HTTP_PROXY\"");
        script.AppendLine("if [ \"$CODEX_PROXY_REACHABLE\" != \"1\" ]; then");
        script.AppendLine("  printf 'CODEX_PROXY_UNREACHABLE %s %s\\n' \"$CODEX_PROXY_HOST\" \"$CODEX_PROXY_PORT\" >&2");
        script.AppendLine("  exit 13");
        script.AppendLine("fi");
        return script.ToString().Replace("\r\n", "\n", StringComparison.Ordinal);
    }

    private static bool TryParseUnreachableProxy(string error, string output, out string host, out string port)
    {
        foreach (var line in (error + "\n" + output).Split('\n'))
        {
            var trimmed = line.Trim();
            const string marker = "CODEX_PROXY_UNREACHABLE ";
            if (!trimmed.StartsWith(marker, StringComparison.Ordinal))
            {
                continue;
            }

            var parts = trimmed[marker.Length..].Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 2)
            {
                host = parts[0];
                port = parts[1];
                return true;
            }
        }

        host = "";
        port = "";
        return false;
    }

    private static string BuildUnreachableProxyMessage(string host, string port) =>
        $"WSL 无法连接代理 {host}:{port}。请确认 Windows 代理允许 WSL 访问，必要时开启代理软件的 Allow LAN/局域网访问。";

    private static bool IsLoopbackHost(string host) =>
        host.Equals("localhost", StringComparison.OrdinalIgnoreCase) ||
        host.Equals("::1", StringComparison.OrdinalIgnoreCase) ||
        host.StartsWith("127.", StringComparison.Ordinal);

    private static string GetPathSuffix(Uri uri)
    {
        var pathAndQuery = uri.GetComponents(UriComponents.PathAndQuery, UriFormat.UriEscaped);
        if (string.IsNullOrWhiteSpace(pathAndQuery) || pathAndQuery == "/")
        {
            return "";
        }

        return "/" + pathAndQuery.TrimStart('/');
    }

    private static string ShQuote(string value) => "'" + value.Replace("'", "'\"'\"'") + "'";
}

internal sealed class LauncherService(LauncherSettings settings, Action<string> log)
{
    private static readonly string LogPath = AppLog.Path;

    public OperationResult TestProxy()
    {
        var proxy = settings.ProxyAddress.Trim();
        if (string.IsNullOrWhiteSpace(proxy))
        {
            return OperationResult.Fail("请先填写代理地址。");
        }

        if (!Uri.TryCreate(proxy, UriKind.Absolute, out var uri) || uri.Port <= 0)
        {
            return OperationResult.Fail($"代理地址格式不正确：{proxy}");
        }

        log($"正在测试代理端口：{uri.Host}:{uri.Port}...");
        using var client = new System.Net.Sockets.TcpClient();
        var task = client.ConnectAsync(uri.Host, uri.Port);
        if (!task.Wait(TimeSpan.FromSeconds(2)))
        {
            return OperationResult.Fail("代理端口无法连接。");
        }

        return OperationResult.Ok("代理端口可以连接。");
    }

    public OperationResult TestWslProxyPropagation()
    {
        var proxy = settings.ProxyAddress.Trim();
        if (string.IsNullOrWhiteSpace(proxy))
        {
            return OperationResult.Fail("请先填写代理地址。");
        }

        var proxyEnv = BuildProxyEnvironment(proxy);
        AddWslProxyEnvironment(proxyEnv);
        return WslProxyProbe.Verify(proxyEnv, proxy, settings.NoProxy.Trim());
    }

    public OperationResult TestWslProxySetup()
    {
        var proxy = settings.ProxyAddress.Trim();
        if (string.IsNullOrWhiteSpace(proxy))
        {
            return OperationResult.Fail("请先填写代理地址。");
        }

        if (!Uri.TryCreate(proxy, UriKind.Absolute, out var proxyUri) || proxyUri.Port <= 0)
        {
            return OperationResult.Fail($"代理地址格式不正确：{proxy}");
        }

        return WslProxyConfigurator.Configure(proxyUri, settings.NoProxy.Trim());
    }

    public OperationResult TestWslProxyWarning()
    {
        var setup = TestWslProxySetup();
        if (setup.Success || !setup.WslProxyUnreachable)
        {
            return setup;
        }

        const string warning = "开启了 WSL，但是代理端口不通，可能会导致对话卡住。可以尝试开启代理软件的局域网访问。";
        return WithWslProxyWarning(OperationResult.Ok("测试 WSL 代理端口警告。"), warning);
    }

    public OperationResult StartCodex(CancellationToken cancellationToken)
    {
        WslBackendDetection? wslDetection = null;
        string? wslProxyWarning = null;
        if (settings.EnableWslProxy)
        {
            try
            {
                wslDetection = WslBackendDetector.Detect();
                log(wslDetection.Value.Message);
            }
            catch (Exception ex)
            {
                log($"WSL 后端检测失败，将继续启动 Codex：{ex.Message}");
            }
        }
        else
        {
            log("config.txt 已关闭 WSL 代理注入。");
        }

        if (!TryResolveCodex(settings.CodexExePath, out var exe, out var resolveMessage))
        {
            return OperationResult.Fail(resolveMessage);
        }

        var proxy = settings.ProxyAddress.Trim();
        if (string.IsNullOrWhiteSpace(proxy))
        {
            return OperationResult.Fail("请填写代理地址。");
        }

        if (!Uri.TryCreate(proxy, UriKind.Absolute, out var proxyUri) || proxyUri.Port <= 0)
        {
            return OperationResult.Fail($"代理地址格式不正确：{proxy}");
        }

        var chromiumProxy = FirstNonEmpty(settings.ChromiumProxy, proxy);
        var arguments = new[]
        {
            $"--proxy-server={chromiumProxy}",
            $"--proxy-bypass-list={BuildChromiumBypassList(settings.NoProxy)}"
        };

        var proxyEnv = BuildProxyEnvironment(proxy);
        if (wslDetection?.ShouldApplyProxy == true)
        {
            try
            {
                AddWslProxyEnvironment(proxyEnv);
                log("已为 Codex 的 WSL 后端加入代理环境变量。");
            }
            catch (Exception ex)
            {
                log($"WSL 代理注入失败，将继续启动 Codex：{ex.Message}");
            }

            try
            {
                var wslSetup = WslProxyConfigurator.Configure(proxyUri, settings.NoProxy.Trim());
                if (wslSetup.Success)
                {
                    log($"已在 WSL 内写入代理配置：{wslSetup.Message}");
                }
                else
                {
                    log($"WSL 内代理设置失败，将继续启动 Codex：{wslSetup.Message}");
                    if (wslSetup.WslProxyUnreachable)
                    {
                        wslProxyWarning = "开启了 WSL，但是代理端口不通，可能会导致对话卡住。可以尝试开启代理软件的局域网访问。";
                        log(wslProxyWarning);
                    }
                }
            }
            catch (Exception ex)
            {
                log($"WSL 内代理设置失败，将继续启动 Codex：{ex.Message}");
            }
        }

        if (FindCodexProcesses(exe).Count > 0)
        {
            return WithWslProxyWarning(OperationResult.Prompt("Codex 已经在运行，请先关闭 Codex 后再启动。"), wslProxyWarning);
        }

        log("正在启动 Codex...");
        WriteLauncherLog($"启动：exe={exe}");

        var previousUserEnv = new Dictionary<string, string?>(StringComparer.Ordinal);
        uint startedPid;

        try
        {
            if (IsWindowsAppsCodexPath(exe))
            {
                if (settings.TemporarilySetUserProxyEnvironment)
                {
                    log("正在为启动过程临时写入代理环境变量...");
                    foreach (var item in proxyEnv)
                    {
                        previousUserEnv[item.Key] = Environment.GetEnvironmentVariable(item.Key, EnvironmentVariableTarget.User);
                        Environment.SetEnvironmentVariable(item.Key, item.Value, EnvironmentVariableTarget.User);
                    }
                }

                var appUserModelId = ResolveAppUserModelId(exe);
                var argumentString = string.Join(" ", arguments.Select(QuoteArgument));
                log($"使用 Windows 应用入口启动：{appUserModelId}");
                var activator = (IApplicationActivationManager)(object)new ApplicationActivationManager();
                var hresult = activator.ActivateApplication(appUserModelId, argumentString, ActivateOptions.NoErrorUi, out startedPid);
                if (hresult != 0)
                {
                    return WithWslProxyWarning(OperationResult.Fail($"通过 Windows 应用入口启动失败。HRESULT：0x{hresult:X8}"), wslProxyWarning);
                }
            }
            else
            {
                var psi = new ProcessStartInfo
                {
                    FileName = exe,
                    WorkingDirectory = Path.GetDirectoryName(exe) ?? Environment.CurrentDirectory,
                    UseShellExecute = false,
                    Arguments = string.Join(" ", arguments.Select(QuoteArgument))
                };
                foreach (var item in proxyEnv)
                {
                    psi.Environment[item.Key] = item.Value;
                }

                var process = Process.Start(psi);
                if (process is null)
                {
                    return WithWslProxyWarning(OperationResult.Fail("启动 Codex 失败。"), wslProxyWarning);
                }

                startedPid = (uint)process.Id;
            }
        }
        finally
        {
            if (previousUserEnv.Count > 0)
            {
                foreach (var item in previousUserEnv)
                {
                    Environment.SetEnvironmentVariable(item.Key, item.Value, EnvironmentVariableTarget.User);
                }
                log("已恢复当前用户代理环境变量。");
            }
        }

        var deadline = DateTimeOffset.Now.AddSeconds(settings.StartupWaitSeconds);
        while (DateTimeOffset.Now < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (FindCodexProcesses(exe).Count > 0)
            {
                return WithWslProxyWarning(OperationResult.Ok($"Codex 已通过代理启动。PID：{startedPid}"), wslProxyWarning);
            }
            Thread.Sleep(500);
        }

        return WithWslProxyWarning(OperationResult.Fail($"已尝试启动 Codex，但在 {settings.StartupWaitSeconds} 秒内未找到正在运行的匹配进程。日志：{LogPath}"), wslProxyWarning);
    }

    private static OperationResult WithWslProxyWarning(OperationResult result, string? warning)
    {
        if (string.IsNullOrWhiteSpace(warning))
        {
            return result;
        }

        return result with
        {
            ShowMessageBox = true,
            DialogTitle = "WSL 代理端口不通",
            DialogMessage = result.ShowMessageBox
                ? $"{warning}{Environment.NewLine}{Environment.NewLine}{result.DialogMessage}"
                : warning,
            DialogIcon = MessageBoxIcon.Warning
        };
    }

    private static void AddWslProxyEnvironment(Dictionary<string, string> proxyEnv)
    {
        var variableNames = new[]
        {
            "HTTP_PROXY",
            "http_proxy",
            "HTTPS_PROXY",
            "https_proxy",
            "ALL_PROXY",
            "all_proxy",
            "NO_PROXY",
            "no_proxy",
            "WS_PROXY",
            "ws_proxy",
            "WSS_PROXY",
            "wss_proxy",
            "NPM_CONFIG_PROXY",
            "npm_config_proxy",
            "NPM_CONFIG_HTTPS_PROXY",
            "npm_config_https_proxy",
            "GLOBAL_AGENT_HTTP_PROXY",
            "global_agent_http_proxy"
        };

        var existingWslEnv = Environment.GetEnvironmentVariable("WSLENV", EnvironmentVariableTarget.User)
            ?? Environment.GetEnvironmentVariable("WSLENV")
            ?? "";
        var entries = existingWslEnv.Split(':', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();
        foreach (var variableName in variableNames)
        {
            var prefix = variableName + "/";
            if (!entries.Any(entry => string.Equals(entry, variableName, StringComparison.Ordinal) ||
                                      entry.StartsWith(prefix, StringComparison.Ordinal)))
            {
                entries.Add(variableName + "/u");
            }
        }

        proxyEnv["WSLENV"] = string.Join(":", entries);
    }

    private Dictionary<string, string> BuildProxyEnvironment(string proxy)
    {
        var noProxy = settings.NoProxy.Trim();
        return new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["HTTP_PROXY"] = proxy,
            ["http_proxy"] = proxy,
            ["HTTPS_PROXY"] = proxy,
            ["https_proxy"] = proxy,
            ["ALL_PROXY"] = proxy,
            ["all_proxy"] = proxy,
            ["WS_PROXY"] = proxy,
            ["ws_proxy"] = proxy,
            ["WSS_PROXY"] = proxy,
            ["wss_proxy"] = proxy,
            ["NO_PROXY"] = noProxy,
            ["no_proxy"] = noProxy,
            ["NPM_CONFIG_PROXY"] = proxy,
            ["npm_config_proxy"] = proxy,
            ["NPM_CONFIG_HTTPS_PROXY"] = proxy,
            ["npm_config_https_proxy"] = proxy,
            ["GLOBAL_AGENT_HTTP_PROXY"] = proxy,
            ["global_agent_http_proxy"] = proxy
        };
    }

    public static bool TryResolveCodex(string configuredPath, out string exe, out string message)
    {
        exe = "";
        if (!string.IsNullOrWhiteSpace(configuredPath))
        {
            var expanded = Environment.ExpandEnvironmentVariables(configuredPath.Trim('"'));
            if (File.Exists(expanded))
            {
                exe = Path.GetFullPath(expanded);
                message = "已使用 config.txt 中设置的 Codex.exe。";
                return true;
            }
            message = $"config.txt 中设置的 Codex.exe 不存在：{expanded}";
            return false;
        }

        var candidates = EnumerateCodexCandidates()
            .Where(File.Exists)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderByDescending(IsWindowsAppsCodexPath)
            .ThenByDescending(GetVersionScore)
            .ToList();

        if (candidates.Count == 0)
        {
            message = "未能自动找到 Codex.exe。可在 config.txt 中设置 codex_exe_path。";
            return false;
        }

        exe = candidates[0];
        message = $"已找到 Codex.exe：{exe}";
        return true;
    }

    private static IEnumerable<string> EnumerateCodexCandidates()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var programFiles = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        var programFilesX86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);

        foreach (var path in new[]
        {
            Path.Combine(local, "Programs", "Codex", "Codex.exe"),
            Path.Combine(local, "Programs", "OpenAI Codex", "Codex.exe"),
            Path.Combine(local, "Programs", "OpenAI", "Codex", "Codex.exe"),
            Path.Combine(programFiles, "Codex", "Codex.exe"),
            Path.Combine(programFiles, "OpenAI Codex", "Codex.exe"),
            Path.Combine(programFilesX86, "Codex", "Codex.exe"),
            Path.Combine(local, "Microsoft", "WindowsApps", "Codex.exe")
        })
        {
            yield return path;
        }

        var windowsApps = Path.Combine(programFiles, "WindowsApps");
        IEnumerable<string> packageDirs = [];
        try
        {
            if (Directory.Exists(windowsApps))
            {
                packageDirs = Directory.EnumerateDirectories(windowsApps, "OpenAI.Codex_*");
            }
        }
        catch
        {
            packageDirs = [];
        }

        foreach (var dir in packageDirs)
        {
            yield return Path.Combine(dir, "app", "Codex.exe");
        }
    }

    private static int GetVersionScore(string path)
    {
        var folder = Path.GetFileName(Path.GetDirectoryName(Path.GetDirectoryName(path) ?? "") ?? "");
        var digits = new string(folder.Where(c => char.IsDigit(c) || c == '.').ToArray());
        return Version.TryParse(digits.Trim('.'), out var version) ? version.Major * 1_000_000 + version.Minor * 10_000 + version.Build : 0;
    }

    private static List<Process> FindCodexProcesses(string exe)
    {
        var processes = new List<Process>();
        foreach (var process in Process.GetProcesses())
        {
            try
            {
                var path = process.MainModule?.FileName;
                if (!string.IsNullOrWhiteSpace(path) &&
                    (string.Equals(path, exe, StringComparison.OrdinalIgnoreCase) ||
                     IsWindowsAppsCodexPath(path)))
                {
                    processes.Add(process);
                }
            }
            catch
            {
                // If the process path cannot be inspected, do not block launch.
                // VS Code and CLI integrations can also run codex.exe.
            }
        }
        return processes;
    }

    private static bool IsWindowsAppsCodexPath(string path) =>
        path.Contains(@"\WindowsApps\OpenAI.Codex_", StringComparison.OrdinalIgnoreCase) &&
        path.EndsWith(@"\app\Codex.exe", StringComparison.OrdinalIgnoreCase);

    private static string ResolveAppUserModelId(string exe)
    {
        var appDir = Path.GetDirectoryName(exe) ?? throw new InvalidOperationException("Codex.exe 路径无效。");
        var packageDir = Path.GetDirectoryName(appDir) ?? throw new InvalidOperationException("Codex MSIX 包目录无效。");
        var manifestPath = Path.Combine(packageDir, "AppxManifest.xml");
        if (!File.Exists(manifestPath))
        {
            throw new FileNotFoundException($"未找到 Codex 应用清单：{manifestPath}", manifestPath);
        }

        var doc = XDocument.Load(manifestPath);
        XNamespace ns = doc.Root?.Name.Namespace ?? "";
        var identityName = doc.Root?.Element(ns + "Identity")?.Attribute("Name")?.Value;
        var appId = doc.Root?.Element(ns + "Applications")?.Element(ns + "Application")?.Attribute("Id")?.Value;
        var folder = Path.GetFileName(packageDir);
        var marker = folder.LastIndexOf("__", StringComparison.Ordinal);
        if (string.IsNullOrWhiteSpace(identityName) || string.IsNullOrWhiteSpace(appId) || marker < 0)
        {
            throw new InvalidOperationException($"无法从应用清单解析 Codex 启动入口：{manifestPath}");
        }

        var publisherId = folder[(marker + 2)..];
        return $"{identityName}_{publisherId}!{appId}";
    }

    private static string BuildChromiumBypassList(string noProxy)
    {
        var items = new[] { "<local>" }
            .Concat((noProxy ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            .Distinct(StringComparer.OrdinalIgnoreCase);
        return string.Join(";", items);
    }

    private static string QuoteArgument(string value)
    {
        if (value.All(c => !char.IsWhiteSpace(c) && c != '"'))
        {
            return value;
        }
        return "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }

    private static string FirstNonEmpty(params string[] values) => values.FirstOrDefault(v => !string.IsNullOrWhiteSpace(v))?.Trim() ?? "";

    private static void WriteLauncherLog(string message)
    {
        try
        {
            AppLog.Append(message);
        }
        catch
        {
            // Logging must never block the launcher.
        }
    }

    [ComImport]
    [Guid("2e941141-7f97-4756-ba1d-9decde894a3d")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IApplicationActivationManager
    {
        int ActivateApplication(
            [MarshalAs(UnmanagedType.LPWStr)] string appUserModelId,
            [MarshalAs(UnmanagedType.LPWStr)] string arguments,
            ActivateOptions options,
            out uint processId);

        int ActivateForFile(
            [MarshalAs(UnmanagedType.LPWStr)] string appUserModelId,
            IntPtr itemArray,
            [MarshalAs(UnmanagedType.LPWStr)] string verb,
            out uint processId);

        int ActivateForProtocol(
            [MarshalAs(UnmanagedType.LPWStr)] string appUserModelId,
            IntPtr itemArray,
            out uint processId);
    }

    [ComImport]
    [Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")]
    private sealed class ApplicationActivationManager;

    [Flags]
    private enum ActivateOptions
    {
        None = 0,
        DesignMode = 1,
        NoErrorUi = 2,
        NoSplashScreen = 4
    }
}

internal readonly record struct OperationResult(
    bool Success,
    string Message,
    bool ShowMessageBox = false,
    string DialogTitle = "提示",
    string DialogMessage = "",
    MessageBoxIcon DialogIcon = MessageBoxIcon.Information,
    bool WslProxyUnreachable = false)
{
    public static OperationResult Ok(string message) => new(true, message);
    public static OperationResult Fail(string message) => new(false, $"错误：{message}");
    public static OperationResult ProxyUnreachable(string message) => new(false, $"错误：{message}", WslProxyUnreachable: true);
    public static OperationResult Prompt(string message) => new(false, message, true, "Codex 已在运行", message);
    public static OperationResult Warning(bool success, string dialogMessage, string dialogTitle, string message) =>
        new(success, message, true, dialogTitle, dialogMessage, MessageBoxIcon.Warning);
}

internal static class Ui
{
    public static Control CreateTextBoxHost(TextBox textBox)
    {
        var host = new InputPanel
        {
            BackColor = Color.White,
            Padding = new Padding(10, 7, 10, 8),
            Margin = new Padding(0)
        };

        textBox.BorderStyle = BorderStyle.None;
        textBox.AutoSize = false;
        textBox.Font = new Font("Segoe UI", 10.5f);
        textBox.Dock = DockStyle.Fill;
        textBox.BackColor = Color.White;
        textBox.ForeColor = Theme.Text;
        host.Controls.Add(textBox);
        return host;
    }
}

internal sealed class InputPanel : Panel
{
    public InputPanel()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.UserPaint,
            true);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        using var pen = new Pen(Theme.InputBorder);
        var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
        e.Graphics.DrawRectangle(pen, bounds);
    }
}

internal sealed class RoundedPanel : Panel
{
    public RoundedPanel()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.UserPaint,
            true);
    }

    protected override void OnPaintBackground(PaintEventArgs e)
    {
        using var parentBrush = new SolidBrush(Parent?.BackColor ?? Theme.Window);
        e.Graphics.FillRectangle(parentBrush, ClientRectangle);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
        using var path = RoundedRect(bounds, 8);
        using var brush = new SolidBrush(BackColor);
        e.Graphics.FillPath(brush, path);
        using var pen = new Pen(Theme.Border);
        e.Graphics.DrawPath(pen, path);
        base.OnPaint(e);
    }

    private static GraphicsPath RoundedRect(Rectangle bounds, int radius)
    {
        var path = new GraphicsPath();
        var diameter = radius * 2;
        path.AddArc(bounds.Left, bounds.Top, diameter, diameter, 180, 90);
        path.AddArc(bounds.Right - diameter, bounds.Top, diameter, diameter, 270, 90);
        path.AddArc(bounds.Right - diameter, bounds.Bottom - diameter, diameter, diameter, 0, 90);
        path.AddArc(bounds.Left, bounds.Bottom - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
        return path;
    }
}

internal static class Theme
{
    public static readonly Color Window = Color.FromArgb(247, 248, 250);
    public static readonly Color Text = Color.FromArgb(28, 32, 39);
    public static readonly Color Border = Color.FromArgb(222, 226, 232);
    public static readonly Color InputBorder = Color.FromArgb(128, 139, 153);
    public static readonly Color Accent = Color.FromArgb(20, 108, 221);
    public static readonly Color ButtonNeutral = Color.FromArgb(232, 237, 244);
}
