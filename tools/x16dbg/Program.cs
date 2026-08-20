using System;
using System.Threading.Tasks;

namespace X16Debug;

class Program
{
    const string DefaultHost = "127.0.0.1";
    const int DefaultPort = 9009;

    static async Task<int> Main(string[] args)
    {
        string host = DefaultHost;
        int port = DefaultPort;
        bool interactive = false;
        string? command = null;
        string[] commandArgs = Array.Empty<string>();

        // Parse args
        int i = 0;
        while (i < args.Length)
        {
            switch (args[i])
            {
                case "-host":
                    if (i + 1 >= args.Length) { PrintUsage(); return 1; }
                    host = args[++i];
                    break;
                case "-port":
                    if (i + 1 >= args.Length) { PrintUsage(); return 1; }
                    port = int.Parse(args[++i]);
                    break;
                case "-i":
                case "--interactive":
                    interactive = true;
                    break;
                case "-h":
                case "--help":
                    PrintUsage();
                    return 0;
                default:
                    // First non-flag arg is the command, rest are command args
                    command = args[i];
                    if (i + 1 < args.Length)
                        commandArgs = args[(i + 1)..];
                    i = args.Length; // stop parsing
                    continue;
            }
            i++;
        }

        // If no command and not explicitly interactive, default to interactive
        if (command == null && !interactive)
            interactive = true;

        using var client = new X16DebugClient();

        try
        {
            Console.Write($"Connecting to {host}:{port}...");
            await client.ConnectAsync(host, port);
            Console.WriteLine(" OK");
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($" Failed: {ex.Message}");
            Console.Error.WriteLine($"Make sure x16emu is running with -debugport {port}");
            return 1;
        }

        // Perform DAP initialization handshake
        try
        {
            Console.Write("DAP handshake...");
            await client.InitializeAsync();
            Console.WriteLine(" OK");
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($" Failed: {ex.Message}");
            Console.Error.WriteLine("DAP initialization failed. Is the emulator running a DAP debug server?");
            return 1;
        }

        if (interactive)
        {
            await InteractiveMode.RunAsync(client);
            await TryDisconnect(client);
            return 0;
        }
        else
        {
            int rc = await Commands.RunOneShotAsync(client, command!, commandArgs);
            await TryDisconnect(client);
            return rc;
        }
    }

    // Leave the way a DAP client is supposed to, rather than dropping the socket
    // and making the server infer it. terminateDebuggee stays false so quitting
    // the debugger never takes the emulator with it.
    static async Task TryDisconnect(X16DebugClient client)
    {
        try { await client.DisconnectAsync(false); }
        catch { /* the server may already be gone; nothing useful to do */ }
    }

    static void PrintUsage()
    {
        Console.WriteLine("x16dbg - Commander X16 Emulator Debug Client (DAP)");
        Console.WriteLine();
        Console.WriteLine("Usage:");
        Console.WriteLine("  x16dbg [options]                  Interactive mode");
        Console.WriteLine("  x16dbg [options] <command> [args] One-shot command mode");
        Console.WriteLine();
        Console.WriteLine("Options:");
        Console.WriteLine("  -host <ip>      Host to connect to (default: 127.0.0.1)");
        Console.WriteLine("  -port <port>    Port to connect to (default: 9009)");
        Console.WriteLine("  -i              Force interactive mode");
        Console.WriteLine("  -h, --help      Show this help");
        Console.WriteLine();
        Console.WriteLine("Commands:");
        Console.WriteLine("  step                    Step one instruction");
        Console.WriteLine("  continue                Continue execution");
        Console.WriteLine("  break                   Pause execution");
        Console.WriteLine("  status                  Show emulator status");
        Console.WriteLine("  reset                   Reset CPU");
        Console.WriteLine("  regs                    Show all registers");
        Console.WriteLine("  setreg <reg> <val>      Set a register (e.g., setreg a FF)");
        Console.WriteLine("  mem <addr>-<end>        Read memory range (e.g., mem 1030-1040)");
        Console.WriteLine("  mem <addr> <len>        Read memory by length (e.g., mem 0800 10)");
        Console.WriteLine("  setmem <addr> <hex>     Write memory (e.g., setmem 0400 DEADBEEF)");
        Console.WriteLine("  vram <addr>-<end>       Read VERA video RAM (17-bit space)");
        Console.WriteLine("  vram <addr> <len>       Read VRAM by length (e.g., vram 1B000 20)");
        Console.WriteLine("  setvram <addr> <hex>    Write VRAM (e.g., setvram 1B000 DEADBEEF)");
        Console.WriteLine("  joy <port> [buttons]    Drive a SNES controller port (1-4)");
        Console.WriteLine("  bp add <addr>           Add breakpoint");
        Console.WriteLine("  bp remove <addr>        Remove breakpoint");
        Console.WriteLine("  bp list                 List all breakpoints");
        Console.WriteLine("  bp clear                Clear all breakpoints");
        Console.WriteLine();
        Console.WriteLine("Examples:");
        Console.WriteLine("  x16dbg                          Start interactive session");
        Console.WriteLine("  x16dbg step                     Step one instruction");
        Console.WriteLine("  x16dbg mem 1030-1040            Dump memory $1030-$1040");
        Console.WriteLine("  x16dbg bp add 080D              Set breakpoint at $080D");
        Console.WriteLine("  x16dbg -port 9010 status        Connect to port 9010");
        Console.WriteLine();
        Console.WriteLine("  VRAM -- VERA's video RAM, which is not in the CPU map. The CPU only");
        Console.WriteLine("  reaches it through the data port, so these are the only way to see it:");
        Console.WriteLine("  x16dbg vram 1B000 20            Dump 32 bytes of the text screen");
        Console.WriteLine("  x16dbg vram 0-FF                Dump the first 256 bytes of VRAM");
        Console.WriteLine("  x16dbg setvram 1F9C0 DEADBEEF   Poke 4 bytes into sprite attributes");
        Console.WriteLine();
        Console.WriteLine("  Joysticks -- a virtual SNES pad, no gamepad and no -joy1..-joy4 needed.");
        Console.WriteLine("  The button list is the whole held state, so anything left off is released:");
        Console.WriteLine("  x16dbg joy 1                    Show what port 1 is reporting");
        Console.WriteLine("  x16dbg joy 1 right a            Hold Right and A on port 1");
        Console.WriteLine("  x16dbg joy 1 start              Now hold only Start; Right and A let go");
        Console.WriteLine("  x16dbg joy 1 none               Connected, nothing held (JOY(1) = $00)");
        Console.WriteLine("  x16dbg joy 1 release            Empty the port again (JOY(1) = $FF)");
        Console.WriteLine();
        Console.WriteLine("Note: each one-shot command is its own debug session, and the emulator");
        Console.WriteLine("resumes when a session that paused it goes away. Commands that only look at");
        Console.WriteLine("the machine are fine that way, but anything depending on staying stopped --");
        Console.WriteLine("break, setreg, stepping -- belongs in interactive mode, where one session");
        Console.WriteLine("stays open. Memory, VRAM and joystick state persist either way.");
    }
}
