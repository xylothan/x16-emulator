using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading.Tasks;

namespace X16Debug;

/// <summary>
/// One-shot command implementations for CLI mode.
/// </summary>
public static class Commands
{
    public static async Task<int> RunOneShotAsync(X16DebugClient client, string cmd, string[] args)
    {
        try
        {
            switch (cmd.ToLower())
            {
                case "step":
                case "s":
                    return await DoStep(client);
                case "continue":
                case "c":
                    return await DoContinue(client);
                case "break":
                case "b":
                    return await DoBreak(client);
                case "status":
                case "t":
                    return await DoStatus(client);
                case "reset":
                    return await DoReset(client);
                case "regs":
                case "r":
                    return await DoRegs(client);
                case "setreg":
                case "p":
                    return await DoSetReg(client, args);
                case "mem":
                case "m":
                    return await DoMem(client, args);
                case "setmem":
                    return await DoSetMem(client, args);
                case "vram":
                case "v":
                    return await DoVram(client, args);
                case "setvram":
                    return await DoSetVram(client, args);
                case "joy":
                case "j":
                    return await DoJoy(client, args);
                case "bp":
                    return await DoBp(client, args);
                default:
                    Console.Error.WriteLine($"Unknown command: {cmd}");
                    return 1;
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Error: {ex.Message}");
            return 1;
        }
    }

    static async Task<int> DoStep(X16DebugClient client)
    {
        var r = await client.StepInAsync();
        if (!DapSuccess(r))
            return 1;
        Console.WriteLine("OK (step issued, awaiting stopped notification)");
        return 0;
    }

    static async Task<int> DoContinue(X16DebugClient client)
    {
        var r = await client.ContinueAsync();
        PrintDapResult(r, "Running...");
        return DapSuccess(r) ? 0 : 1;
    }

    static async Task<int> DoBreak(X16DebugClient client)
    {
        var r = await client.PauseAsync();
        if (DapSuccess(r))
        {
            // After pause, fetch and display registers
            try
            {
                await PrintRegistersFromServer(client);
            }
            catch
            {
                Console.WriteLine("Paused (registers not yet available)");
            }
        }
        else
            PrintDapError(r);
        return DapSuccess(r) ? 0 : 1;
    }

    static async Task<int> DoStatus(X16DebugClient client)
    {
        var threads = await client.GetThreadsAsync();
        if (DapSuccess(threads))
        {
            var threadList = threads["body"]?["threads"]?.AsArray();
            if (threadList != null && threadList.Count > 0)
            {
                string name = threadList[0]?["name"]?.GetValue<string>() ?? "CPU";
                Console.WriteLine($"Thread: {name}");
            }

            // Try to get stack trace for current position
            try
            {
                var stack = await client.GetStackTraceAsync(1);
                var frames = stack["body"]?["stackFrames"]?.AsArray();
                if (frames != null && frames.Count > 0)
                {
                    string frameName = frames[0]?["name"]?.GetValue<string>() ?? "unknown";
                    Console.WriteLine($"Frame: {frameName}");
                }
                await PrintRegistersFromServer(client);
            }
            catch
            {
                Console.WriteLine("(CPU may be running, registers not available)");
            }
        }
        else
            PrintDapError(threads);
        return 0;
    }

    static async Task<int> DoReset(X16DebugClient client)
    {
        var r = await client.EvaluateAsync("reset");
        PrintDapResult(r);
        return 0;
    }

    static async Task<int> DoRegs(X16DebugClient client)
    {
        try
        {
            await PrintRegistersFromServer(client);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Error: {ex.Message}");
            return 1;
        }
        return 0;
    }

    /// <summary>
    /// Map the short names a user types onto the names the Registers scope uses.
    /// </summary>
    static string ResolveRegisterName(string reg) => reg.ToLowerInvariant() switch
    {
        "a" => "A",
        "x" => "X",
        "y" => "Y",
        "sp" or "s" => "SP",
        "pc" => "PC",
        "p" or "status" or "flags" => "P (Status)",
        "d" or "dp" => "D",
        "dbr" => "DBR",
        "pbr" or "k" => "PBR",
        "rambank" or "ram" or "bka" => "RAM Bank",
        "rombank" or "rom" or "bko" => "ROM Bank",
        _ => reg.ToUpperInvariant(),
    };

    static async Task<int> DoSetReg(X16DebugClient client, string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("Usage: setreg <reg> <hex_value>   (e.g., setreg a FF)");
            return 1;
        }

        // setVariable, not evaluate: the evaluate expression "A=FF" is parsed as
        // a read, answers success and changes nothing.
        string name = ResolveRegisterName(args[0]);
        var r = await client.SetVariableAsync(name, $"${args[1].TrimStart('$')}");
        if (!DapSuccess(r)) { PrintDapError(r); return 1; }

        string? shown = r["body"]?["value"]?.GetValue<string>();
        Console.WriteLine($"OK: {name} = {shown ?? "$" + args[1]}");
        return 0;
    }

    /// <summary>
    /// Parse the address/length forms both mem and vram accept:
    /// "1030-1040" (inclusive range), "0800 10" (hex length), or "0800" (16 bytes).
    /// </summary>
    static bool TryParseRange(string[] args, out uint addr, out int length, out string error)
    {
        addr = 0; length = 0; error = "";
        try
        {
            if (args[0].Contains('-'))
            {
                var parts = args[0].Split('-');
                uint start = Convert.ToUInt32(parts[0], 16);
                uint end = Convert.ToUInt32(parts[1], 16);
                if (end < start) { error = "end address is before the start"; return false; }
                addr = start;
                length = (int)(end - start + 1);
            }
            else if (args.Length >= 2)
            {
                addr = Convert.ToUInt32(args[0], 16);
                length = (int)Convert.ToUInt32(args[1], 16);
            }
            else
            {
                addr = Convert.ToUInt32(args[0], 16);
                length = 16;
            }
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
        if (length <= 0) { error = "length must be at least 1"; return false; }
        return true;
    }

    static async Task<int> DoMem(X16DebugClient client, string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("Usage: mem <addr>-<end>  or  mem <addr> <len>");
            return 1;
        }
        if (!TryParseRange(args, out uint addrVal, out int length, out string err))
        {
            Console.Error.WriteLine($"Error: {err}");
            return 1;
        }

        var r = await client.ReadMemoryAsync((int)addrVal, length);
        return PrintMemoryResponse(r, addrVal, 4);
    }

    static async Task<int> DoSetMem(X16DebugClient client, string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("Usage: setmem <addr> <hex_data>   (e.g., setmem 0400 DEADBEEF)");
            return 1;
        }
        return await WriteBytes(client, $"${args[0]}", args[1]);
    }

    // ─── VRAM ───────────────────────────────────────────────────────
    //
    // VERA's video RAM is a separate 17-bit space, reached with a "vram:"
    // memoryReference. The CPU cannot address it directly -- it goes through the
    // data port a byte at a time -- so these are not just mem with a different
    // number.

    static async Task<int> DoVram(X16DebugClient client, string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("Usage: vram <addr>-<end>  or  vram <addr> <len>");
            return 1;
        }
        if (!TryParseRange(args, out uint addrVal, out int length, out string err))
        {
            Console.Error.WriteLine($"Error: {err}");
            return 1;
        }

        var r = await client.ReadMemoryAsync($"vram:{addrVal:X}", length);
        return PrintMemoryResponse(r, addrVal, 5);
    }

    static async Task<int> DoSetVram(X16DebugClient client, string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("Usage: setvram <addr> <hex_data>   (e.g., setvram 1B000 DEADBEEF)");
            return 1;
        }
        uint addr;
        try { addr = Convert.ToUInt32(args[0], 16); }
        catch (Exception ex) { Console.Error.WriteLine($"Error: {ex.Message}"); return 1; }

        return await WriteBytes(client, $"vram:{addr:X}", args[1]);
    }

    /// <summary>
    /// Shared tail of setmem/setvram: decode the hex payload and write it, then
    /// read the same span back so the caller sees what actually landed rather
    /// than a bare OK.
    /// </summary>
    static async Task<int> WriteBytes(X16DebugClient client, string reference, string hex)
    {
        string clean = hex.Replace(" ", "").Replace("$", "");
        if (clean.Length == 0 || clean.Length % 2 != 0)
        {
            Console.Error.WriteLine("Error: hex data must be an even number of hex digits");
            return 1;
        }

        byte[] data;
        try { data = Convert.FromHexString(clean); }
        catch (Exception ex) { Console.Error.WriteLine($"Error: {ex.Message}"); return 1; }

        var w = await client.WriteMemoryAsync(reference, data);
        if (!DapSuccess(w)) { PrintDapError(w); return 1; }

        int written = w["body"]?["bytesWritten"]?.GetValue<int>() ?? data.Length;
        Console.WriteLine($"Wrote {written} byte{(written == 1 ? "" : "s")} to {reference}");

        var r = await client.ReadMemoryAsync(reference, data.Length);
        if (DapSuccess(r))
        {
            uint baseAddr = 0;
            string numeric = reference.StartsWith("vram:", StringComparison.OrdinalIgnoreCase)
                ? reference[5..] : reference;
            numeric = numeric.TrimStart('$');
            try { baseAddr = Convert.ToUInt32(numeric, 16); } catch { }
            PrintMemoryResponse(r, baseAddr, reference.StartsWith("vram:", StringComparison.OrdinalIgnoreCase) ? 5 : 4);
        }
        return 0;
    }

    // ─── Joystick ───────────────────────────────────────────────────

    static readonly string[] JoyButtonNames =
    {
        "up", "down", "left", "right", "a", "b", "x", "y", "start", "select", "l", "r"
    };

    static async Task<int> DoJoy(X16DebugClient client, string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("Usage: joy <port 1-4> [buttons...|release|connect]");
            Console.Error.WriteLine($"       buttons: {string.Join(", ", JoyButtonNames)}");
            Console.Error.WriteLine("       joy 1              query port 1 without changing it");
            Console.Error.WriteLine("       joy 1 a right      hold A and Right, release everything else");
            Console.Error.WriteLine("       joy 1 none         release every button, stay connected");
            Console.Error.WriteLine("       joy 1 release      hand the port back to a real gamepad");
            return 1;
        }

        // Ports are 1-4 here to match -joy1..-joy4 and BASIC's JOY(n); the DAP
        // request indexes them from 0.
        if (!int.TryParse(args[0], out int port) || port < 1 || port > 4)
        {
            Console.Error.WriteLine("Error: port must be 1, 2, 3 or 4");
            return 1;
        }
        int index = port - 1;

        JsonNode r;
        if (args.Length == 1)
        {
            r = await client.JoystickAsync(index);
        }
        else if (args[1].Equals("release", StringComparison.OrdinalIgnoreCase) ||
                 args[1].Equals("off", StringComparison.OrdinalIgnoreCase))
        {
            r = await client.JoystickAsync(index, enabled: false);
        }
        else if (args[1].Equals("connect", StringComparison.OrdinalIgnoreCase) ||
                 args[1].Equals("none", StringComparison.OrdinalIgnoreCase))
        {
            // A connected controller with nothing held. Distinct from an empty
            // port: the KERNAL can tell the difference, and a game that waits for
            // a controller needs to see one.
            r = await client.JoystickAsync(index, buttons: Array.Empty<string>());
        }
        else
        {
            var held = args[1..];
            foreach (var b in held)
            {
                if (Array.IndexOf(JoyButtonNames, b.ToLowerInvariant()) < 0)
                {
                    Console.Error.WriteLine($"Error: unknown button '{b}'");
                    Console.Error.WriteLine($"       known: {string.Join(", ", JoyButtonNames)}");
                    return 1;
                }
            }
            r = await client.JoystickAsync(index, buttons: held);
        }

        if (!DapSuccess(r)) { PrintDapError(r); return 1; }
        PrintJoystick(port, r);
        return 0;
    }

    static void PrintJoystick(int port, JsonNode r)
    {
        var b = r["body"];
        if (b == null) { Console.WriteLine("OK"); return; }

        int mask = b["mask"]?.GetValue<int>() ?? 0xFFFF;
        bool driven = b["enabled"]?.GetValue<bool>() ?? false;
        bool bound = b["controllerBound"]?.GetValue<bool>() ?? false;

        var pressed = new List<string>();
        var arr = b["buttons"]?.AsArray();
        if (arr != null)
            foreach (var n in arr)
                if (n != null) pressed.Add(n.GetValue<string>());

        string source = driven ? "virtual" : bound ? "gamepad" : "empty";
        string held = pressed.Count > 0 ? string.Join(" ", pressed) : "(nothing held)";

        Console.WriteLine($"  Joystick {port}: {held}");
        Console.WriteLine($"    mask=${mask:X4}  source={source}");
    }

    /// <summary>
    /// Decode a readMemory response and hex-dump it. width is the number of
    /// address digits: 4 for the CPU map, 5 for VRAM's 17-bit space.
    /// </summary>
    static int PrintMemoryResponse(JsonNode r, uint baseAddr, int width)
    {
        if (!DapSuccess(r)) { PrintDapError(r); return 1; }

        string? b64Data = r["body"]?["data"]?.GetValue<string>();
        if (b64Data == null)
        {
            Console.Error.WriteLine("Error: no data in response");
            return 1;
        }
        PrintMemoryDump(baseAddr, Convert.ToHexString(Convert.FromBase64String(b64Data)), width);
        return 0;
    }

    static async Task<int> DoBp(X16DebugClient client, string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("Usage: bp add|remove|list|clear [addr]");
            return 1;
        }

        switch (args[0].ToLower())
        {
            case "add":
            case "a":
                if (args.Length < 2) { Console.Error.WriteLine("Usage: bp add <addr>"); return 1; }
                var ra = await client.AddBreakpointAsync((int)Convert.ToUInt32(args[1], 16));
                PrintDapResult(ra);
                return DapSuccess(ra) ? 0 : 1;

            case "remove":
            case "r":
                if (args.Length < 2) { Console.Error.WriteLine("Usage: bp remove <addr>"); return 1; }
                var rr = await client.RemoveBreakpointAsync((int)Convert.ToUInt32(args[1], 16));
                PrintDapResult(rr);
                return DapSuccess(rr) ? 0 : 1;

            case "list":
            case "l":
                var rl = await client.ListBreakpointsAsync();
                if (DapSuccess(rl))
                {
                    string? result = rl["body"]?["result"]?.GetValue<string>();
                    if (string.IsNullOrWhiteSpace(result) || result == "none")
                        Console.WriteLine("No breakpoints set");
                    else
                        Console.WriteLine($"Breakpoints: {result}");
                }
                else
                    PrintDapError(rl);
                return 0;

            case "clear":
            case "x":
                var rc = await client.ClearBreakpointsAsync();
                PrintDapResult(rc);
                return 0;

            default:
                Console.Error.WriteLine($"Unknown bp action: {args[0]}");
                return 1;
        }
    }

    // ─── DAP helpers ────────────────────────────────────────────────

    static bool DapSuccess(JsonNode r) =>
        r["success"]?.GetValue<bool>() == true;

    static void PrintDapError(JsonNode r)
    {
        string? msg = r["message"]?.GetValue<string>();
        if (msg != null)
            Console.Error.WriteLine($"Error: {msg}");
        else
            Console.Error.WriteLine("Error: request failed");
    }

    static void PrintDapResult(JsonNode r, string? successMsg = null)
    {
        if (DapSuccess(r))
            Console.WriteLine(successMsg ?? "OK");
        else
            PrintDapError(r);
    }

    /// <summary>
    /// Fetch registers via DAP (stackTrace -> scopes -> variables) and print them.
    /// </summary>
    public static async Task PrintRegistersFromServer(X16DebugClient client)
    {
        var varsResponse = await client.GetRegistersAsync();
        var variables = varsResponse["body"]?["variables"]?.AsArray();
        if (variables == null)
        {
            Console.Error.WriteLine("No register data available");
            return;
        }

        // Build a dictionary from the DAP variables array
        var regs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var v in variables)
        {
            string? name = v?["name"]?.GetValue<string>();
            string? val = v?["value"]?.GetValue<string>();
            if (name != null && val != null)
                regs[name] = val;
        }

        PrintRegs(regs);
    }

    // ─── Display helpers ────────────────────────────────────────────

    /// <summary>
    /// Print registers from a DAP variables dictionary (name -> hex value).
    /// </summary>
    public static void PrintRegs(Dictionary<string, string> regs)
    {
        // DAP variable values are formatted as "$XX (decimal)" or "$XX [flags]"
        // Register names from DAP: PC, A, X, Y, SP, "P (Status)", "RAM Bank", "ROM Bank"
        string a = GetReg(regs, "A", "??");
        string x = GetReg(regs, "X", "??");
        string y = GetReg(regs, "Y", "??");
        string sp = GetReg(regs, "SP", "??");
        string pc = GetReg(regs, "PC", "????");
        string p = GetReg(regs, "P (Status)", GetReg(regs, "P", "??"));
        string bka = GetReg(regs, "RAM Bank", GetReg(regs, "BKA", "??"));
        string bko = GetReg(regs, "ROM Bank", GetReg(regs, "BKO", "??"));

        // Extract just the hex portion from DAP values like "$02 (2)" -> "02"
        string aHex = ExtractHex(a);
        string xHex = ExtractHex(x);
        string yHex = ExtractHex(y);
        string pHex = ExtractHex(p);
        string pcHex = ExtractHex(pc);
        string spHex = ExtractHex(sp);
        string bkaHex = ExtractHex(bka);
        string bkoHex = ExtractHex(bko);

        byte pByte = 0;
        try { pByte = Convert.ToByte(pHex, 16); } catch { }
        string flags = FormatFlags(pByte);

        try
        {
            Console.WriteLine($"  PC=${pcHex}  A=${aHex} ({Convert.ToByte(aHex, 16),3})  X=${xHex} ({Convert.ToByte(xHex, 16),3})  Y=${yHex} ({Convert.ToByte(yHex, 16),3})");
        }
        catch
        {
            Console.WriteLine($"  PC=${pcHex}  A=${aHex}  X=${xHex}  Y=${yHex}");
        }
        Console.WriteLine($"  SP=${spHex}  P=${pHex} [{flags}]  RAM Bank=${bkaHex}  ROM Bank=${bkoHex}");
    }

    static string GetReg(Dictionary<string, string> regs, string name, string fallback)
    {
        if (regs.TryGetValue(name, out var val))
            return val;
        return fallback;
    }

    /// <summary>
    /// Extract hex value from DAP format like "$02 (2)" or "$C1C6 (49606)" or "$1E [---B-I--]"
    /// Returns just the hex digits (e.g., "02", "C1C6", "1E")
    /// </summary>
    static string ExtractHex(string val)
    {
        if (string.IsNullOrEmpty(val) || val == "??" || val == "????") return val;

        // Strip leading $
        string s = val;
        if (s.StartsWith("$")) s = s[1..];
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) s = s[2..];

        // Take only hex digits before any space or other delimiter
        int end = 0;
        while (end < s.Length && Uri.IsHexDigit(s[end])) end++;
        return end > 0 ? s[..end] : val;
    }

    static string FormatFlags(byte p)
    {
        char n = (p & 0x80) != 0 ? 'N' : '-';
        char v = (p & 0x40) != 0 ? 'V' : '-';
        char b = (p & 0x10) != 0 ? 'B' : '-';
        char d = (p & 0x08) != 0 ? 'D' : '-';
        char i = (p & 0x04) != 0 ? 'I' : '-';
        char z = (p & 0x02) != 0 ? 'Z' : '-';
        char c = (p & 0x01) != 0 ? 'C' : '-';
        return $"{n}{v}-{b}{d}{i}{z}{c}";
    }

    public static void PrintMemoryDump(uint baseAddr, string hexData, int addrWidth = 4)
    {
        int numBytes = hexData.Length / 2;
        int offset = 0;

        while (offset < numBytes)
        {
            uint lineAddr = baseAddr + (uint)offset;
            int lineStart = offset;
            int lineEnd = Math.Min(offset + 16, numBytes);

            var hexPart = new StringBuilder();
            var asciiPart = new StringBuilder();

            for (int i = lineStart; i < lineEnd; i++)
            {
                string byteHex = hexData.Substring(i * 2, 2);
                hexPart.Append(byteHex);
                hexPart.Append(' ');

                byte b = Convert.ToByte(byteHex, 16);
                asciiPart.Append(b >= 0x20 && b < 0x7F ? (char)b : '.');
            }

            // Pad if less than 16 bytes
            for (int i = lineEnd - lineStart; i < 16; i++)
            {
                hexPart.Append("   ");
                asciiPart.Append(' ');
            }

            Console.WriteLine($"  {lineAddr.ToString($"X{addrWidth}")}: {hexPart} |{asciiPart}|");
            offset = lineEnd;
        }
    }
}
