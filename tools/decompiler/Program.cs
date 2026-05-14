using System;
using System.IO;
using System.Linq;
using UELib;
using UELib.Core;

namespace BioShockDecompiler
{
    class Program
    {
        static void Main(string[] args)
        {
            string scriptDir = args.Length > 0 ? args[0] :
                @"D:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\BakedScripts\pc";
            string outputDir = args.Length > 1 ? args[1] :
                @"z:\Bioshock1SDK\docs\reverse-engineering\decompiled";

            string[] packages = {
                "Core.U", "Engine.U", "ShockGame.U", "ShockAI.U",
                "Scripting.U", "VengeanceShared.U", "Tyrion.U"
            };

            Directory.CreateDirectory(outputDir);

            foreach (string pkgName in packages)
            {
                string pkgPath = Path.Combine(scriptDir, pkgName);
                if (!File.Exists(pkgPath))
                {
                    Console.WriteLine($"[SKIP] {pkgName} not found");
                    continue;
                }

                Console.WriteLine($"[LOAD] {pkgName}...");
                try
                {
                    var package = UnrealLoader.LoadPackage(pkgPath, FileAccess.Read);
                    package.InitializePackage();

                    string pkgOutputDir = Path.Combine(outputDir, Path.GetFileNameWithoutExtension(pkgName));
                    Directory.CreateDirectory(pkgOutputDir);

                    int classCount = 0;
                    int funcCount = 0;

                    foreach (var obj in package.Objects)
                    {
                        if ((int)obj <= 0) continue; // skip imports

                        if (obj is UClass cls)
                        {
                            classCount++;
                            string className = cls.Name ?? "Unknown";
                            string filePath = Path.Combine(pkgOutputDir, className + ".uc");

                            try
                            {
                                string decompiled = cls.Decompile();
                                File.WriteAllText(filePath, decompiled);
                            }
                            catch (Exception ex)
                            {
                                File.WriteAllText(filePath, $"// Decompilation failed: {ex.Message}");
                            }
                        }

                        if (obj is UFunction)
                            funcCount++;
                    }

                    Console.WriteLine($"  -> {classCount} classes, {funcCount} functions -> {pkgOutputDir}");
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[ERROR] {pkgName}: {ex.Message}");
                }
            }

            Console.WriteLine("\nDone! Decompiled source saved to: " + outputDir);
        }
    }
}
