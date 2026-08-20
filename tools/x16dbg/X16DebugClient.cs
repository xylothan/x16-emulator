using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace X16Debug;

/// <summary>
/// TCP client for the x16emu DAP (Debug Adapter Protocol) debug server.
/// Uses Content-Length framing: "Content-Length: N\r\n\r\n{json}"
/// </summary>
public class X16DebugClient : IDisposable
{
    private TcpClient? _tcp;
    private Stream? _stream;
    private int _nextSeq = 1;
    private readonly object _lock = new();
    private readonly Dictionary<int, TaskCompletionSource<JsonNode>> _pending = new();
    private readonly List<TaskCompletionSource<JsonNode>> _eventWaiters = new();
    private CancellationTokenSource? _cts;
    private Task? _listenerTask;

    public event Action<JsonNode>? OnEvent;
    public bool IsConnected => _tcp?.Connected == true;

    public async Task ConnectAsync(string host, int port, int timeoutMs = 5000)
    {
        _tcp = new TcpClient();
        using var cts = new CancellationTokenSource(timeoutMs);
        await _tcp.ConnectAsync(host, port, cts.Token);
        _stream = _tcp.GetStream();

        _cts = new CancellationTokenSource();
        _listenerTask = Task.Run(() => ListenerLoop(_cts.Token));
    }

    /// <summary>
    /// Perform the DAP initialization handshake: initialize -> wait for initialized event -> configurationDone.
    /// Must be called after ConnectAsync before any other commands.
    /// </summary>
    public async Task<JsonNode> InitializeAsync()
    {
        var initArgs = new JsonObject
        {
            ["clientID"] = "x16dbg",
            ["clientName"] = "X16 Debug Client",
            ["adapterID"] = "x16emu",
            ["supportsVariableType"] = true,
            ["supportsReadMemoryRequest"] = true,
        };

        var initResponse = await SendRequestAsync("initialize", initArgs);

        // Wait for the "initialized" event from the server
        await WaitForEventAsync("initialized", 5000);

        // Complete configuration
        await SendRequestAsync("configurationDone");

        return initResponse;
    }

    // ─── Core DAP transport ─────────────────────────────────────────

    /// <summary>
    /// Send a DAP request and wait for its matching response.
    /// </summary>
    public async Task<JsonNode> SendRequestAsync(string command, JsonNode? arguments = null, int timeoutMs = 10000)
    {
        int seq;
        var tcs = new TaskCompletionSource<JsonNode>();

        lock (_lock)
        {
            seq = _nextSeq++;
            _pending[seq] = tcs;
        }

        var msg = new JsonObject
        {
            ["seq"] = seq,
            ["type"] = "request",
            ["command"] = command,
        };
        if (arguments != null)
            msg["arguments"] = arguments;

        await SendDapMessageAsync(msg);

        using var cts = new CancellationTokenSource(timeoutMs);
        cts.Token.Register(() => tcs.TrySetCanceled());

        return await tcs.Task;
    }

    /// <summary>
    /// Wait for a specific DAP event by name.
    /// </summary>
    public Task<JsonNode> WaitForEventAsync(string eventName, int timeoutMs = 10000)
    {
        var tcs = new TaskCompletionSource<JsonNode>();
        Action<JsonNode>? handler = null;
        handler = (evt) =>
        {
            if (evt["event"]?.GetValue<string>() == eventName)
            {
                OnEvent -= handler;
                tcs.TrySetResult(evt);
            }
        };
        OnEvent += handler;

        var cts = new CancellationTokenSource(timeoutMs);
        cts.Token.Register(() =>
        {
            OnEvent -= handler;
            tcs.TrySetCanceled();
        });

        return tcs.Task;
    }

    private async Task SendDapMessageAsync(JsonObject msg)
    {
        string json = msg.ToJsonString();
        byte[] body = Encoding.UTF8.GetBytes(json);
        string header = $"Content-Length: {body.Length}\r\n\r\n";
        byte[] headerBytes = Encoding.ASCII.GetBytes(header);

        byte[] packet = new byte[headerBytes.Length + body.Length];
        Buffer.BlockCopy(headerBytes, 0, packet, 0, headerBytes.Length);
        Buffer.BlockCopy(body, 0, packet, headerBytes.Length, body.Length);

        await _stream!.WriteAsync(packet);
        await _stream.FlushAsync();
    }

    private async Task ListenerLoop(CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested && _stream != null)
            {
                var node = await ReadDapMessageAsync(ct);
                if (node == null) break;

                string? type = node["type"]?.GetValue<string>();

                if (type == "response")
                {
                    int reqSeq = node["request_seq"]!.GetValue<int>();
                    lock (_lock)
                    {
                        if (_pending.TryGetValue(reqSeq, out var tcs))
                        {
                            _pending.Remove(reqSeq);
                            tcs.SetResult(node);
                        }
                    }
                }
                else if (type == "event")
                {
                    OnEvent?.Invoke(node);
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (IOException) { }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[listener] Error: {ex.Message}");
        }
    }

    /// <summary>
    /// Read one DAP message: parse Content-Length header, then read that many bytes of JSON.
    /// </summary>
    private async Task<JsonNode?> ReadDapMessageAsync(CancellationToken ct)
    {
        // Read headers line by line until we hit the empty line (\r\n\r\n)
        int contentLength = -1;
        while (true)
        {
            string? headerLine = await ReadLineAsync(ct);
            if (headerLine == null) return null; // connection closed

            if (headerLine.Length == 0)
                break; // end of headers

            if (headerLine.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
            {
                string val = headerLine.Substring("Content-Length:".Length).Trim();
                contentLength = int.Parse(val);
            }
        }

        if (contentLength < 0)
            return null;

        // Read exactly contentLength bytes
        byte[] buffer = new byte[contentLength];
        int totalRead = 0;
        while (totalRead < contentLength)
        {
            int read = await _stream!.ReadAsync(buffer, totalRead, contentLength - totalRead, ct);
            if (read == 0) return null; // connection closed
            totalRead += read;
        }

        string json = Encoding.UTF8.GetString(buffer);
        return JsonNode.Parse(json);
    }

    /// <summary>
    /// Read a single \r\n-terminated line from the raw stream (for DAP headers).
    /// </summary>
    private async Task<string?> ReadLineAsync(CancellationToken ct)
    {
        var sb = new StringBuilder();
        byte[] buf = new byte[1];
        bool prevCR = false;

        while (true)
        {
            int read = await _stream!.ReadAsync(buf, 0, 1, ct);
            if (read == 0) return null;

            char c = (char)buf[0];
            if (c == '\n' && prevCR)
            {
                // Remove the trailing CR we appended
                if (sb.Length > 0 && sb[sb.Length - 1] == '\r')
                    sb.Length--;
                return sb.ToString();
            }
            prevCR = c == '\r';
            sb.Append(c);
        }
    }

    // ─── Execution commands ─────────────────────────────────────────

    public Task<JsonNode> StepInAsync() =>
        SendRequestAsync("stepIn", new JsonObject { ["threadId"] = 1 });

    public Task<JsonNode> StepOverAsync() =>
        SendRequestAsync("next", new JsonObject { ["threadId"] = 1 });

    public Task<JsonNode> StepOutAsync() =>
        SendRequestAsync("stepOut", new JsonObject { ["threadId"] = 1 });

    public Task<JsonNode> ContinueAsync() =>
        SendRequestAsync("continue", new JsonObject { ["threadId"] = 1 });

    public Task<JsonNode> PauseAsync() =>
        SendRequestAsync("pause", new JsonObject { ["threadId"] = 1 });

    public Task<JsonNode> DisconnectAsync(bool terminateDebuggee = false) =>
        SendRequestAsync("disconnect", new JsonObject { ["terminateDebuggee"] = terminateDebuggee });

    // ─── State inspection ───────────────────────────────────────────

    public Task<JsonNode> GetThreadsAsync() =>
        SendRequestAsync("threads");

    public Task<JsonNode> GetStackTraceAsync(int threadId = 1) =>
        SendRequestAsync("stackTrace", new JsonObject { ["threadId"] = threadId });

    public Task<JsonNode> GetScopesAsync(int frameId) =>
        SendRequestAsync("scopes", new JsonObject { ["frameId"] = frameId });

    public Task<JsonNode> GetVariablesAsync(int variablesReference) =>
        SendRequestAsync("variables", new JsonObject { ["variablesReference"] = variablesReference });

    /// <summary>
    /// Helper: stackTrace -> scopes -> variables to get the "Registers" scope.
    /// The emulator uses variablesReference=1 for Registers.
    /// </summary>
    public async Task<JsonNode> GetRegistersAsync()
    {
        var stack = await GetStackTraceAsync(1);
        var frames = stack["body"]?["stackFrames"]?.AsArray();
        if (frames == null || frames.Count == 0)
            throw new InvalidOperationException("No stack frames available");

        int frameId = frames[0]!["id"]!.GetValue<int>();
        var scopes = await GetScopesAsync(frameId);
        var scopeArray = scopes["body"]?["scopes"]?.AsArray();
        if (scopeArray == null)
            throw new InvalidOperationException("No scopes available");

        // Find the "Registers" scope (variablesReference=1 by convention)
        int registersRef = -1;
        foreach (var scope in scopeArray)
        {
            string? name = scope?["name"]?.GetValue<string>();
            if (name != null && name.Equals("Registers", StringComparison.OrdinalIgnoreCase))
            {
                registersRef = scope!["variablesReference"]!.GetValue<int>();
                break;
            }
        }

        // Fallback: use variablesReference=1 directly
        if (registersRef < 0)
            registersRef = 1;

        return await GetVariablesAsync(registersRef);
    }

    public Task<JsonNode> ReadMemoryAsync(int address, int count) =>
        ReadMemoryAsync($"${address:X4}", count);

    /// <summary>
    /// Read memory by raw DAP reference. A bare address is CPU space; a "vram:"
    /// prefix reaches VERA's 17-bit video RAM, which is not in the CPU map at all.
    /// </summary>
    public Task<JsonNode> ReadMemoryAsync(string memoryReference, int count) =>
        SendRequestAsync("readMemory", new JsonObject
        {
            ["memoryReference"] = memoryReference,
            ["count"] = count
        });

    /// <summary>
    /// Write memory by raw DAP reference, same address rules as ReadMemoryAsync.
    /// This is the only way to write VRAM: the CPU reaches it through VERA's data
    /// port one auto-incrementing byte at a time, so there is no CPU address for it.
    /// </summary>
    public Task<JsonNode> WriteMemoryAsync(string memoryReference, byte[] data) =>
        SendRequestAsync("writeMemory", new JsonObject
        {
            ["memoryReference"] = memoryReference,
            ["data"] = Convert.ToBase64String(data)
        });

    // ─── Virtual joysticks ──────────────────────────────────────────

    /// <summary>
    /// Drive one of the four SNES controller ports. Needs no gamepad attached and
    /// no -joy1..-joy4 flag.
    ///
    /// buttons is the complete held set, so anything left out of it is released;
    /// passing null for every argument but index queries the port without
    /// changing it. Set enabled=false to hand the port back to a physical pad.
    /// </summary>
    public Task<JsonNode> JoystickAsync(int index, IEnumerable<string>? buttons = null,
                                        int? mask = null, bool? enabled = null)
    {
        var args = new JsonObject { ["index"] = index };
        if (buttons != null)
        {
            var arr = new JsonArray();
            foreach (var b in buttons) arr.Add(b);
            args["buttons"] = arr;
        }
        if (mask.HasValue) args["mask"] = mask.Value;
        if (enabled.HasValue) args["enabled"] = enabled.Value;
        return SendRequestAsync("x16/joystick", args);
    }

    public Task<JsonNode> EvaluateAsync(string expression) =>
        SendRequestAsync("evaluate", new JsonObject
        {
            ["expression"] = expression,
            ["frameId"] = 0
        });

    /// <summary>
    /// Set a variable in a scope. The emulator uses variablesReference=1 for the
    /// Registers scope. This is the request that actually writes a register --
    /// the evaluate expression "A=FF" is parsed as a read and silently does
    /// nothing.
    /// </summary>
    public Task<JsonNode> SetVariableAsync(string name, string value, int variablesReference = 1) =>
        SendRequestAsync("setVariable", new JsonObject
        {
            ["variablesReference"] = variablesReference,
            ["name"] = name,
            ["value"] = value
        });

    // ─── Breakpoints (via evaluate) ─────────────────────────────────

    public Task<JsonNode> AddBreakpointAsync(int address) =>
        EvaluateAsync($"bp_add {address:X4}");

    public Task<JsonNode> RemoveBreakpointAsync(int address) =>
        EvaluateAsync($"bp_remove {address:X4}");

    public Task<JsonNode> ListBreakpointsAsync() =>
        EvaluateAsync("bp_list");

    public Task<JsonNode> ClearBreakpointsAsync() =>
        EvaluateAsync("bp_clear");

    public void Dispose()
    {
        _cts?.Cancel();
        _stream?.Dispose();
        _tcp?.Dispose();
        _listenerTask?.Wait(1000);
    }
}
