using System;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace X16Debug;

/// <summary>
/// Interactive text-menu mode for live debugging.
/// </summary>
public static class InteractiveMode
{
    // Hold a reference to the client for the event handler to use for register fetching
    private static X16DebugClient? _activeClient;

    public static async Task RunAsync(X16DebugClient client)
    {
        _activeClient = client;
        client.OnEvent += OnEvent;

        PrintBanner();

        while (true)
        {
            Console.Write("> ");
            string? input = Console.ReadLine();
            if (input == null) break; // EOF
            input = input.Trim();
            if (string.IsNullOrEmpty(input)) continue;

            // Single-char hotkeys
            if (input.Length == 1)
            {
                switch (input[0])
                {
                    case 's': await DoStep(client); continue;
                    case 'c': await DoContinue(client); continue;
                    case 'b': await DoBreak(client); continue;
                    case 'r': await DoRegs(client); continue;
                    case 't': await DoStatus(client); continue;
                    case 'l': await DoBpList(client); continue;
                    case 'x': await DoBpClear(client); continue;
                    case 'q': return;
                    case '?':
                    case 'h': PrintHelp(); continue;
                }
            }

            // Parse full commands
            var parts = input.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            string cmd = parts[0].ToLower();
            string[] args = parts.Length > 1 ? parts[1..] : Array.Empty<string>();

            switch (cmd)
            {
                case "step":
                case "s":
                    await DoStep(client);
                    break;
                case "continue":
                case "c":
                    await DoContinue(client);
                    break;
                case "break":
                case "b":
                    await DoBreak(client);
                    break;
                case "status":
                case "t":
                    await DoStatus(client);
                    break;
                case "reset":
                    await DoReset(client);
                    break;
                case "regs":
                case "r":
                    await DoRegs(client);
                    break;
                case "setreg":
                case "p":
                    if (args.Length < 2)
                        Console.WriteLine("Usage: setreg <reg> <hex_value>");
                    else
                        await Commands.RunOneShotAsync(client, "setreg", args);
                    break;
                case "mem":
                case "m":
                    if (args.Length < 1)
                        Console.WriteLine("Usage: mem <addr>-<end>  or  mem <addr> <len>");
                    else
                        await Commands.RunOneShotAsync(client, "mem", args);
                    break;
                case "setmem":
                    if (args.Length < 2)
                        Console.WriteLine("Usage: setmem <addr> <hex_data>");
                    else
                        await Commands.RunOneShotAsync(client, "setmem", args);
                    break;
                case "vram":
                case "v":
                    if (args.Length < 1)
                        Console.WriteLine("Usage: vram <addr>-<end>  or  vram <addr> <len>");
                    else
                        await Commands.RunOneShotAsync(client, "vram", args);
                    break;
                case "setvram":
                    if (args.Length < 2)
                        Console.WriteLine("Usage: setvram <addr> <hex_data>");
                    else
                        await Commands.RunOneShotAsync(client, "setvram", args);
                    break;
                case "joy":
                case "j":
                    await Commands.RunOneShotAsync(client, "joy", args);
                    break;
                case "bp":
                    await Commands.RunOneShotAsync(client, "bp", args);
                    break;
                case "quit":
                case "q":
                    return;
                case "help":
                case "?":
                    PrintHelp();
                    break;
                default:
                    Console.WriteLine($"Unknown command: {cmd}. Type 'h' for help.");
                    break;
            }
        }

        client.OnEvent -= OnEvent;
        _activeClient = null;
    }

    static void OnEvent(JsonNode evt)
    {
        string eventType = evt["event"]?.GetValue<string>() ?? "unknown";

        if (eventType == "stopped")
        {
            var body = evt["body"];
            string reason = body?["reason"]?.GetValue<string>() ?? "unknown";

            Console.WriteLine();
            Console.WriteLine($"! STOPPED ({reason})");

            // DAP stopped events don't include registers; fetch them asynchronously
            if (_activeClient != null)
            {
                _ = Task.Run(async () =>
                {
                    try
                    {
                        await Commands.PrintRegistersFromServer(_activeClient);
                    }
                    catch { /* registers may not be available yet */ }
                    Console.Write("> ");
                });
            }
            else
            {
                Console.Write("> ");
            }
        }
        else if (eventType == "output")
        {
            var body = evt["body"];
            string? output = body?["output"]?.GetValue<string>();
            if (output != null)
            {
                Console.Write(output);
            }
        }
        else
        {
            Console.WriteLine($"\n! Event: {eventType}");
            Console.Write("> ");
        }
    }

    static async Task DoStep(X16DebugClient client)
    {
        try
        {
            var r = await client.StepInAsync();
            if (r["success"]?.GetValue<bool>() != true)
                Console.WriteLine($"Error: {r["message"]}");
            // The stopped event will arrive via OnEvent and print registers
        }
        catch (Exception ex) { Console.WriteLine($"Error: {ex.Message}"); }
    }

    static async Task DoContinue(X16DebugClient client)
    {
        try
        {
            var r = await client.ContinueAsync();
            if (r["success"]?.GetValue<bool>() == true)
                Console.WriteLine("Running...");
            else
                Console.WriteLine($"Error: {r["message"]}");
        }
        catch (Exception ex) { Console.WriteLine($"Error: {ex.Message}"); }
    }

    static async Task DoBreak(X16DebugClient client)
    {
        try
        {
            var r = await client.PauseAsync();
            if (r["success"]?.GetValue<bool>() == true)
                Console.WriteLine("Pause requested...");
            else
                Console.WriteLine($"Error: {r["message"]}");
            // The stopped event will arrive via OnEvent and print registers
        }
        catch (Exception ex) { Console.WriteLine($"Error: {ex.Message}"); }
    }

    static async Task DoStatus(X16DebugClient client)
    {
        try
        {
            await Commands.RunOneShotAsync(client, "status", Array.Empty<string>());
        }
        catch (Exception ex) { Console.WriteLine($"Error: {ex.Message}"); }
    }

    static async Task DoReset(X16DebugClient client)
    {
        try
        {
            var r = await client.EvaluateAsync("reset");
            if (r["success"]?.GetValue<bool>() == true)
            {
                Console.WriteLine("Reset OK");
                try { await Commands.PrintRegistersFromServer(client); }
                catch { /* may not be available immediately */ }
            }
            else
                Console.WriteLine($"Error: {r["message"]}");
        }
        catch (Exception ex) { Console.WriteLine($"Error: {ex.Message}"); }
    }

    static async Task DoRegs(X16DebugClient client)
    {
        try
        {
            await Commands.PrintRegistersFromServer(client);
        }
        catch (Exception ex) { Console.WriteLine($"Error: {ex.Message}"); }
    }

    static async Task DoBpList(X16DebugClient client)
    {
        await Commands.RunOneShotAsync(client, "bp", new[] { "list" });
    }

    static async Task DoBpClear(X16DebugClient client)
    {
        await Commands.RunOneShotAsync(client, "bp", new[] { "clear" });
    }

    static void PrintBanner()
    {
        Console.WriteLine("╔═══════════════════════════════════════╗");
        Console.WriteLine("║  X16 Debug Client (DAP) - Connected   ║");
        Console.WriteLine("╠═══════════════════════════════════════╣");
        Console.WriteLine("║  [s] Step         [c] Continue        ║");
        Console.WriteLine("║  [b] Break        [r] Registers       ║");
        Console.WriteLine("║  [m] Memory       [v] VRAM            ║");
        Console.WriteLine("║  [j] Joystick     [p] Set register    ║");
        Console.WriteLine("║  [t] Status       [l] List BPs        ║");
        Console.WriteLine("║  [x] Clear BPs    [q] Quit            ║");
        Console.WriteLine("╠═══════════════════════════════════════╣");
        Console.WriteLine("║  Type full command or ? for help      ║");
        Console.WriteLine("╚═══════════════════════════════════════╝");
    }

    static void PrintHelp()
    {
        Console.WriteLine();
        Console.WriteLine("Commands:");
        Console.WriteLine("  s, step              Step one instruction");
        Console.WriteLine("  c, continue          Continue execution");
        Console.WriteLine("  b, break             Pause execution");
        Console.WriteLine("  r, regs              Show registers");
        Console.WriteLine("  t, status            Show emulator status");
        Console.WriteLine("  reset                Reset CPU");
        Console.WriteLine("  m, mem <range>       Read memory (e.g., mem 1030-1040, mem 0800 10)");
        Console.WriteLine("  setmem <addr> <hex>  Write memory (e.g., setmem 0400 DEADBEEF)");
        Console.WriteLine("  v, vram <range>      Read VERA video RAM (e.g., vram 1B000 20)");
        Console.WriteLine("  setvram <addr> <hex> Write VRAM (e.g., setvram 1B000 DEADBEEF)");
        Console.WriteLine("  j, joy <port> [btns] Drive a SNES controller port 1-4");
        Console.WriteLine("  p, setreg <r> <val>  Set register (e.g., setreg a FF)");
        Console.WriteLine("  bp add <addr>        Add breakpoint");
        Console.WriteLine("  bp remove <addr>     Remove breakpoint");
        Console.WriteLine("  l, bp list           List breakpoints");
        Console.WriteLine("  x, bp clear          Clear all breakpoints");
        Console.WriteLine("  q, quit              Disconnect and exit");
        Console.WriteLine("  ?, help              Show this help");
        Console.WriteLine();
        Console.WriteLine("VRAM is VERA's 17-bit video RAM. It is not in the CPU map, so `mem 1B000`");
        Console.WriteLine("and `vram 1B000` are two different memories at the same number:");
        Console.WriteLine("  vram 1B000 20        Dump 32 bytes of the text screen");
        Console.WriteLine("  setvram 1F9C0 DEAD   Poke two bytes into sprite attributes");
        Console.WriteLine();
        Console.WriteLine("Joysticks need no gamepad and no -joy1..-joy4 flag. The button list is the");
        Console.WriteLine("complete held state, so anything left off it is released:");
        Console.WriteLine("  joy 1                Show what port 1 is reporting");
        Console.WriteLine("  joy 1 right a        Hold Right and A");
        Console.WriteLine("  joy 1 start          Hold only Start; Right and A let go");
        Console.WriteLine("  joy 1 none           Connected with nothing held (JOY(1) = $00)");
        Console.WriteLine("  joy 1 release        Empty the port again (JOY(1) = $FF)");
        Console.WriteLine("  buttons: up down left right a b x y start select l r");
        Console.WriteLine();
    }
}
