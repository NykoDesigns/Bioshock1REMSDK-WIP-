using System;
using System.IO;
using System.Linq;
using System.Text;
using UELib;
using UELib.Core;

class Program
{
    static readonly string ScriptsDir = @"D:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\BakedScripts\pc";
    static readonly string OutputDir = @"Z:\Bioshock1SDK\docs\reverse-engineering\decompiled";

    static void Main(string[] args)
    {
        // Process specific packages or all
        string[] targetPackages = args.Length > 0 ? args :
            new[] { "Core.U", "Engine.U", "ShockGame.U", "ShockAI.U", "Scripting.U",
                    "VengeanceShared.U", "Tyrion.U", "FMODAudio.U" };

        Directory.CreateDirectory(OutputDir);

        foreach (var pkgName in targetPackages)
        {
            string pkgPath = Path.Combine(ScriptsDir, pkgName);
            if (!File.Exists(pkgPath))
            {
                Console.WriteLine($"[SKIP] {pkgName} not found");
                continue;
            }
            ProcessPackage(pkgPath, pkgName);
        }
        Console.WriteLine("\nDone.");
        Environment.ExitCode = 0; // Always succeed — errors are logged per-class
    }

    static void ProcessPackage(string pkgPath, string pkgName)
    {
        Console.WriteLine($"\n{'='} Processing {pkgName} {'='}");
        try
        {
            var pkg = UnrealLoader.LoadFullPackage(pkgPath, FileAccess.Read);
            pkg.InitializePackage(UnrealPackage.InitFlags.All);

            string baseName = Path.GetFileNameWithoutExtension(pkgName);
            string pkgOutDir = Path.Combine(OutputDir, baseName);
            Directory.CreateDirectory(pkgOutDir);

            int totalExports = pkg.Exports?.Count ?? 0;
            Console.WriteLine($"  Exports: {totalExports}");

            int classCount = 0, funcCount = 0, stateCount = 0, errorCount = 0;
            var summary = new StringBuilder();
            summary.AppendLine($"// {baseName} Decompilation Summary");
            summary.AppendLine($"// Exports: {totalExports}");
            summary.AppendLine();

            if (pkg.Exports == null) return;

            // Group exports by class for organized output
            foreach (var exp in pkg.Exports)
            {
                if (exp == null) continue;
                var obj = exp.Object;
                if (obj == null) continue;

                try
                {
                    if (obj is UClass cls)
                    {
                        classCount++;
                        string superName = cls.Super?.Name ?? "None";
                        string classFile = Path.Combine(pkgOutDir, $"{cls.Name}.uc");

                        var sb = new StringBuilder();
                        sb.AppendLine($"//=============================================================================");
                        sb.AppendLine($"// {baseName}.{cls.Name}");
                        sb.AppendLine($"//=============================================================================");

                        try
                        {
                            string decomp = cls.Decompile();
                            sb.AppendLine(decomp);
                        }
                        catch (Exception ex)
                        {
                            sb.AppendLine($"// DECOMPILATION ERROR: {ex.Message}");
                            errorCount++;
                        }

                        File.WriteAllText(classFile, sb.ToString());
                        summary.AppendLine($"class {cls.Name} extends {superName} -> {cls.Name}.uc");
                    }
                    else if (obj is UFunction func)
                    {
                        funcCount++;
                    }
                    else if (obj is UState state)
                    {
                        stateCount++;
                    }
                }
                catch (Exception ex)
                {
                    errorCount++;
                    if (errorCount <= 10)
                        Console.Error.WriteLine($"  [ERR] {exp.ObjectName}: {ex.Message}");
                }
            }

            // Write summary
            summary.AppendLine();
            summary.AppendLine($"// Classes: {classCount}, Functions: {funcCount}, States: {stateCount}, Errors: {errorCount}");
            File.WriteAllText(Path.Combine(pkgOutDir, "_SUMMARY.txt"), summary.ToString());

            Console.WriteLine($"  Classes: {classCount}, Functions: {funcCount}, States: {stateCount}, Errors: {errorCount}");
            Console.WriteLine($"  Output: {pkgOutDir}");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAILED: {ex.Message}");
        }
    }
}
