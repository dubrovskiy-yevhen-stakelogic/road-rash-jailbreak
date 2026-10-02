"""Run the actual Quest installers against an isolated ADB process double.

No headset, downloads or game images are used. Synthetic files cover first install,
reinstall, native stderr, transport failures and failed disc verification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


ADB_SOURCE = r'''
using System;
using System.IO;
using System.Security.Cryptography;
public static class AdbDouble {
    static string root, scenario, remote, pack;
    static int Fail(string text) { Console.Error.WriteLine(text); return 1; }
    static string Hash(string path) {
        using (var sha = SHA1.Create()) { return BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(path))).Replace("-", "").ToLowerInvariant(); }
    }
    public static int Main(string[] raw) {
        root = Environment.GetEnvironmentVariable("RRJB_ADB_TEST_ROOT");
        scenario = Environment.GetEnvironmentVariable("RRJB_ADB_TEST_CASE");
        remote = Path.Combine(root, "remote-disc.bin"); pack = Path.Combine(root, "hd-pack");
        File.AppendAllText(Path.Combine(root, "commands.jsonl"), String.Join("\t", raw) + "\n");
        int start = raw.Length > 1 && raw[0] == "-s" ? 2 : 0;
        string[] a = new string[raw.Length-start]; Array.Copy(raw,start,a,0,a.Length);
        if (a[0] == "devices") {
            if (scenario == "devices_fail") return Fail("error: cannot connect to daemon");
            if (scenario.Contains("daemon")) Console.Error.WriteLine("* daemon not running; starting now at tcp:5037");
            Console.WriteLine("List of devices attached");
            Console.WriteLine("FAKE\t" + (scenario == "unauthorized" ? "unauthorized" : "device"));
            if (scenario == "multiple") Console.WriteLine("SECOND\tdevice");
            return 0;
        }
        if (a[0] == "install") {
            if (a.Length != 3 || a[1] != "-r") return Fail("unsafe install arguments");
            if (scenario == "apk_fail") return Fail("INSTALL_FAILED_UPDATE_INCOMPATIBLE");
            if (scenario == "install_stderr") Console.Error.WriteLine("Performing Streamed Install");
            Console.WriteLine("Success"); return 0;
        }
        if (a[0] == "push") {
            if (scenario == "push_fail") return Fail("error: device disconnected");
            if (Directory.Exists(a[1])) {
                if (scenario == "hd_daemon") Console.Error.WriteLine("2 files pushed, 0 skipped");
                return 0;
            }
            File.Copy(a[1],remote,true);
            if (scenario == "corrupt_hash") { byte[] d = File.ReadAllBytes(remote); d[0] ^= 1; File.WriteAllBytes(remote,d); }
            if (scenario == "corrupt_size") File.AppendAllText(remote,"x");
            if (scenario == "push_stderr") Console.Error.WriteLine("1 file pushed, 0 skipped");
            return 0;
        }
        if (a[0] != "shell") return Fail("Unexpected command");
        string cmd = a[1];
        if (cmd == "mkdir") return scenario == "mkdir_fail" ? Fail("Permission denied") : 0;
        if (cmd.StartsWith("if [ -f ")) {
            if (scenario == "hash_fail") return Fail("error: device offline");
            Console.WriteLine(File.Exists(remote) ? Hash(remote)+"  /sdcard/Android/data/com.rrjb.vr/files/disc.bin" : "missing");
            return 0;
        }
        if (cmd == "sha1sum") {
            if (a[2].EndsWith("index.txt")) { Console.WriteLine(Hash(Path.Combine(pack,"index.txt"))+"  "+a[2]); return 0; }
            if (!File.Exists(remote)) return Fail("sha1sum: /sdcard/Android/data/com.rrjb.vr/files/disc.bin: No such file or directory");
            Console.WriteLine(Hash(remote)+"  "+a[2]); return 0;
        }
        if (cmd == "stat") { Console.WriteLine(new FileInfo(remote).Length); return 0; }
        if (cmd == "dumpsys") { Console.WriteLine("versionName=0.1.0"); return 0; }
        if (cmd.StartsWith("find ")) {
            string[] files = Directory.GetFiles(pack,"*",SearchOption.AllDirectories);
            if (cmd.Contains("wc -l")) Console.WriteLine(files.Length);
            else { long total=0; foreach(string f in files) total += new FileInfo(f).Length; Console.WriteLine(total); }
            return 0;
        }
        if (cmd.StartsWith("rm ") || cmd == "rm" || cmd == "mv" || cmd == "chmod") return 0;
        return Fail("Unexpected shell command: "+cmd);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--shell", default="powershell.exe")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = output / "AdbDouble.cs"
    source.write_text(ADB_SOURCE, encoding="utf-8")
    adb = output / "adb with spaces.exe"
    compiler = Path(os.environ["WINDIR"]) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"
    subprocess.run([str(compiler), "/nologo", "/target:exe", f"/out:{adb}", str(source)], check=True)
    installer = args.repo / "scripts/install-quest-player.ps1"
    image_data = bytes(range(256)) * 8
    results = []

    def run_case(name, success, pushes, initial=None, script=None, serial=None, hd=False, entry=False):
        case = output / (name + (" entry" if entry else "") + " path with spaces")
        case.mkdir()
        runtime = case / "runtime"
        disc_dir = runtime / "runtime/disc"
        disc_dir.mkdir(parents=True)
        image = disc_dir / "disc with spaces.bin"
        image.write_bytes(image_data)
        (disc_dir / "disc-manifest.json").write_text(json.dumps({
            "exeSha1": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
            "image": image.name, "imageBytes": len(image_data),
        }), encoding="utf-8")
        apk = case / "test package.apk"
        apk.write_bytes(b"synthetic package")
        save = case / "app-private-save"
        save.write_bytes(b"save must be preserved")
        if initial is not None:
            (case / "remote-disc.bin").write_bytes(initial)
        env = dict(os.environ, RRJB_ADB_TEST_ROOT=str(case), RRJB_ADB_TEST_CASE=name)
        if Path(args.shell).name.lower() == "powershell.exe":
            # Do not inherit PowerShell Core modules when testing Windows PowerShell.
            env["PSModulePath"] = str(Path(os.environ["WINDIR"]) / "System32/WindowsPowerShell/v1.0/Modules")
        command = [args.shell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                   "-File", str(script or installer), "-Adb", str(adb)]
        if entry:
            # Test player entry-point verification and ADB discovery. Disc preparation
            # is stubbed because these synthetic bytes are not a retail disc image.
            package = case / "player-package"
            (package / "scripts").mkdir(parents=True)
            (package / "tools").mkdir()
            for name in ["install-player.ps1", "install-quest-player.ps1", "platform-tools.ps1", "transfer-saves.ps1"]:
                (package / "scripts" / name).write_bytes((args.repo / "scripts" / name).read_bytes())
            (package / "scripts/install.ps1").write_text(
                "param($DiscImage,$InstallDir,$BuildDir,[switch]$NoBuild,[switch]$SkipLaunchCheck,$HdMedia,$Upscaler)\n"
                "if (!$NoBuild) { throw 'Expected prebuilt tools' }\n", encoding="utf-8")
            for name in ["rrgame.exe", "rrtool.exe"]:
                (package / "tools" / name).write_bytes(b"synthetic tool")
            (package / "RoadRashJailbreak-VR-0.1.0.apk").write_bytes(apk.read_bytes())
            files = [{"path": p.relative_to(package).as_posix(), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                     for p in package.rglob("*") if p.is_file()]
            (package / "release-manifest.json").write_text(json.dumps({"version": "0.1.0", "files": files}), encoding="utf-8")
            command = [args.shell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                       str(package / "scripts/install-player.ps1"), "-Adb", str(adb), "-Target", "Quest",
                       "-InstallDir", str(runtime), "-HdMedia", "Original"]
        elif hd:
            pack = case / "hd-pack"
            pack.mkdir()
            (pack / "profile.txt").write_text("synthetic profile")
            (pack / "index.txt").write_text("synthetic index")
            command += ["-Pack", str(pack)]
        else:
            command += ["-Runtime", str(runtime), "-Apk", str(apk), "-NoHd"]
        if serial:
            command += ["-Serial", serial]
        run = subprocess.run(command, env=env, capture_output=True, timeout=40)
        log = (run.stdout + run.stderr).decode("utf-8", errors="replace")
        (case / "installer.log").write_text(log, encoding="utf-8")
        commands = (case / "commands.jsonl").read_text().splitlines()
        clean = [line.split("\t")[2:] if line.startswith("-s\t") else line.split("\t") for line in commands]
        actual_pushes = sum(item[0] == "push" for item in clean)
        assert (run.returncode == 0) == success, (name, run.returncode, log)
        assert actual_pushes == pushes, (name, actual_pushes, log)
        assert save.read_bytes() == b"save must be preserved", name
        assert not any(item[0] == "uninstall" or item[:3] == ["shell", "pm", "clear"] for item in clean), name
        if success and not hd:
            assert (case / "remote-disc.bin").read_bytes() == image_data, name
            assert "Disc verified on the headset:" in log, name
        if script and not hd:
            assert "No such file or directory" in log and "NativeCommandError" in log, log
        results.append({"case": name, "entryPoint": entry, "status": "passed", "pushes": actual_pushes})

    if args.baseline:
        run_case("baseline_missing", False, 0, script=args.baseline)
    for name in ["first", "daemon", "install_stderr", "push_stderr"]:
        run_case(name, True, 1)
    run_case("identical", True, 0, image_data)
    run_case("different", True, 1, b"older copy")
    run_case("selected", True, 1, serial="FAKE")
    for name in ["hash_fail", "apk_fail", "mkdir_fail", "devices_fail", "unauthorized", "multiple"]:
        run_case(name, False, 0)
    for name in ["push_fail", "corrupt_hash", "corrupt_size"]:
        run_case(name, False, 1)
    run_case("hd_daemon", True, 1, script=args.repo / "scripts/install-quest-hd.ps1", hd=True)
    run_case("entry_daemon", True, 1, entry=True)
    # Device enumeration must fail before any game files are copied.
    run_case("devices_fail", False, 0, entry=True)
    (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"{len(results)}/{len(results)} Quest installer process checks passed ({args.shell})")


if __name__ == "__main__":
    main()
