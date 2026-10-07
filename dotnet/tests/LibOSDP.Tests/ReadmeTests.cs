// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Text.RegularExpressions;

namespace LibOSDP.Tests;

public sealed partial class ReadmeTests
{
    private static string RepoRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "dotnet", "README.md")))
                return dir.FullName;
        }
        throw new DirectoryNotFoundException("libosdp checkout not found above the test binary.");
    }

    private static string Dedent(IEnumerable<string> lines)
    {
        var list = lines.ToList();
        int indent = list.Where(l => l.Trim().Length > 0).Min(l => l.Length - l.TrimStart().Length);
        return string.Join('\n', list.Select(l => l.Length >= indent ? l[indent..] : l.TrimStart())).Trim();
    }

    private static string SnippetFromSource(string file, string name)
    {
        string[] lines = File.ReadAllLines(file);
        int start = Array.FindIndex(lines, l => l.Trim() == $"// snippet:{name}");
        int end = Array.FindIndex(lines, start + 1, l => l.Trim() == "// end-snippet");
        Assert.True(start >= 0 && end > start, $"snippet {name} not found in {file}");
        return Dedent(lines[(start + 1)..end]);
    }

    private static string SnippetFromReadme(string readme, string name)
    {
        Match m = Regex.Match(readme, $@"<!-- snippet:{name} -->\s*```csharp\n(.*?)\n```", RegexOptions.Singleline);
        Assert.True(m.Success, $"snippet {name} not found in README.md");
        return Dedent(m.Groups[1].Value.Split('\n'));
    }

    [Theory]
    [InlineData("control-panel", "ControlPanelExample.cs")]
    [InlineData("peripheral-device", "PeripheralDeviceExample.cs")]
    public void ReadmeSnippetMatchesCompiledExample(string name, string file)
    {
        string root = RepoRoot();
        string readme = File.ReadAllText(Path.Combine(root, "dotnet", "README.md")).Replace("\r\n", "\n");
        string source = Path.Combine(root, "examples", "dotnet", file);
        Assert.Equal(SnippetFromSource(source, name), SnippetFromReadme(readme, name));
    }
}
