// A System.IO.Stream that tunnels the V9XR protocol to a remote-help machine
// through the update server (docs\remote-help.md), for v9xctl.ps1 -Relay.
// The same contract as mcp\v9x_relay.py: writes are buffered, then signed
// chunk by chunk with build\tools\v9xsign.exe (the private key stays in its
// file) and posted as soon as the caller reads; reads long-poll the server
// for the agent's replies. Loaded by V9xProtocol.ps1 with Add-Type.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

public sealed class V9xRelayStream : Stream
{
    const int RecordHeader = 76;
    const int WaitMax = 20;

    readonly string server;
    readonly string operatorKey;
    readonly string signer;
    readonly string signingKey;
    readonly string machine;
    readonly string session;
    readonly string installId;
    readonly string nonce;
    readonly MemoryStream pending = new MemoryStream();
    readonly List<byte> inbox = new List<byte>();
    uint downSeq = 1;
    uint upAfter;
    bool closed;
    int readTimeout = 60000;
    int writeTimeout = 60000;

    static V9xRelayStream()
    {
        // Windows PowerShell 5.1 on .NET Framework may not offer TLS 1.2 by default.
        ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
    }

    public V9xRelayStream(string server, string operatorKey, string signer,
                          string signingKey, string machine, int openWaitSeconds)
    {
        this.server = server.TrimEnd('/');
        this.operatorKey = operatorKey;
        this.signer = signer;
        this.signingKey = signingKey;
        this.machine = machine.ToUpperInvariant();
        Dictionary<string, object> opened = Json("open", "machine=" + Uri.EscapeDataString(this.machine), new byte[0], 40);
        session = (string)opened["session"];
        installId = (string)opened["install_id"];
        string state = (string)opened["state"];
        DateTime deadline = DateTime.UtcNow.AddSeconds(openWaitSeconds);
        Dictionary<string, object> status = opened;
        while (state == "pending" && DateTime.UtcNow < deadline) {
            int wait = (int)Math.Max(1, Math.Min(WaitMax, (deadline - DateTime.UtcNow).TotalSeconds));
            status = Json("status", "session=" + session + "&wait=" + wait, null, wait + 20);
            state = (string)status["state"];
        }
        if (state != "open") {
            CloseQuietly();
            if (state == "pending") {
                throw new IOException("Machine " + this.machine + " did not take the session within " +
                    openWaitSeconds + " s; it checks in every 60 s when idle. Is it online and set to On?");
            }
            object reason;
            status.TryGetValue("reason", out reason);
            throw new IOException("Machine " + this.machine + " refused the session (" + (reason ?? state) + ").");
        }
        nonce = (string)status["nonce"];
    }

    public string Machine { get { return machine; } }
    public int PipelineWindow { get { return 32; } }

    // --- HTTP ---------------------------------------------------------------

    byte[] Call(string op, string query, byte[] body, int timeoutSeconds,
                out int status, out WebHeaderCollection headers)
    {
        string url = server + "/index.php?bt_type=v9xr_" + op + (query.Length > 0 ? "&" + query : "");
        HttpWebRequest request = (HttpWebRequest)WebRequest.Create(url);
        request.Method = body != null ? "POST" : "GET";
        request.Timeout = timeoutSeconds * 1000;
        request.ReadWriteTimeout = timeoutSeconds * 1000;
        request.UserAgent = "v9x-relay";
        request.Headers["X-V9X-Operator"] = operatorKey;
        request.ContentType = "application/octet-stream";
        if (body != null) {
            request.ContentLength = body.Length;
            using (Stream output = request.GetRequestStream()) {
                output.Write(body, 0, body.Length);
            }
        }
        HttpWebResponse response;
        try {
            response = (HttpWebResponse)request.GetResponse();
        } catch (WebException exception) {
            response = exception.Response as HttpWebResponse;
            if (response == null) {
                throw new IOException("Relay server " + server + " unreachable: " + exception.Message);
            }
        }
        using (response) {
            status = (int)response.StatusCode;
            headers = response.Headers;
            using (Stream input = response.GetResponseStream())
            using (MemoryStream copy = new MemoryStream()) {
                input.CopyTo(copy);
                return copy.ToArray();
            }
        }
    }

    Dictionary<string, object> Json(string op, string query, byte[] body, int timeoutSeconds)
    {
        int status;
        WebHeaderCollection headers;
        byte[] data = Call(op, query, body, timeoutSeconds, out status, out headers);
        Dictionary<string, object> value;
        try {
            value = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(Encoding.UTF8.GetString(data));
        } catch (ArgumentException) {
            throw new IOException("Relay server answered " + status + " with something other than JSON.");
        }
        object success;
        if (status != 200 || value == null || !value.TryGetValue("success", out success) || !(success is bool) || !(bool)success) {
            object message = null;
            if (value != null) value.TryGetValue("message", out message);
            throw new IOException((string)message ?? ("Relay server error " + status + "."));
        }
        return value;
    }

    void CloseQuietly()
    {
        closed = true;
        try {
            int status;
            WebHeaderCollection headers;
            Call("close", "session=" + session, new byte[0], 15, out status, out headers);
        } catch (IOException) {
        }
    }

    // --- tunnel -------------------------------------------------------------

    void Flush(bool force)
    {
        if (pending.Length == 0) return;
        if (closed) throw new IOException("The relay session is closed.");
        byte[] data = pending.ToArray();
        pending.SetLength(0);
        string raw = Path.GetTempFileName();
        string signedPath = raw + ".bin";
        byte[] batch;
        uint next;
        try {
            File.WriteAllBytes(raw, data);
            ProcessStartInfo start = new ProcessStartInfo(signer,
                "relay \"@" + signingKey + "\" " + machine + " " + installId + " " + nonce + " " +
                session + " " + downSeq + " \"" + raw + "\" \"" + signedPath + "\"");
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            using (Process process = Process.Start(start)) {
                string output = process.StandardOutput.ReadToEnd();
                string error = process.StandardError.ReadToEnd();
                process.WaitForExit();
                if (process.ExitCode != 0) throw new IOException("v9xsign failed: " + error.Trim());
                next = uint.Parse(output.Trim());
            }
            batch = File.ReadAllBytes(signedPath);
        } finally {
            try { File.Delete(raw); } catch (IOException) { }
            try { File.Delete(signedPath); } catch (IOException) { }
        }
        // A big transfer goes up a few records per request.
        int offset = 0;
        while (offset < batch.Length) {
            int end = offset;
            for (int count = 0; count < 4 && end < batch.Length; ++count) {
                end += RecordHeader + (int)BitConverter.ToUInt32(batch, end + 8);
            }
            byte[] piece = new byte[end - offset];
            Buffer.BlockCopy(batch, offset, piece, 0, piece.Length);
            Json("down", "session=" + session, piece, 120);
            offset = end;
        }
        downSeq = next;
    }

    void Pull(int wait)
    {
        int status;
        WebHeaderCollection headers;
        byte[] data = Call("up", "session=" + session + "&after=" + upAfter + "&wait=" + wait, null, wait + 30,
                           out status, out headers);
        if (status != 200) throw new IOException("Relay server error " + status + " while reading.");
        int offset = 0;
        while (offset + 12 <= data.Length) {
            if (data[offset] != 'V' || data[offset + 1] != '9' || data[offset + 2] != 'X' || data[offset + 3] != 'U') {
                throw new IOException("Malformed relay reply.");
            }
            uint seq = BitConverter.ToUInt32(data, offset + 4);
            int length = (int)BitConverter.ToUInt32(data, offset + 8);
            if (offset + 12 + length > data.Length) throw new IOException("Truncated relay reply.");
            if (seq == upAfter + 1) {
                for (int index = 0; index < length; ++index) inbox.Add(data[offset + 12 + index]);
                upAfter = seq;
            }
            offset += 12 + length;
        }
        string state = headers["X-V9X-Session-State"] ?? "open";
        if ((state == "closed" || state == "refused") && inbox.Count == 0) {
            closed = true;
            throw new IOException("Relay session closed (" + (headers["X-V9X-Session-Reason"] ?? state) + ").");
        }
    }

    // --- Stream -------------------------------------------------------------

    public override bool CanRead { get { return true; } }
    public override bool CanWrite { get { return true; } }
    public override bool CanSeek { get { return false; } }
    public override bool CanTimeout { get { return true; } }
    public override int ReadTimeout { get { return readTimeout; } set { readTimeout = Math.Max(value, 30000); } }
    public override int WriteTimeout { get { return writeTimeout; } set { writeTimeout = value; } }
    public override long Length { get { throw new NotSupportedException(); } }
    public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
    public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
    public override void SetLength(long value) { throw new NotSupportedException(); }
    public override void Flush() { }

    public override void Write(byte[] buffer, int offset, int count)
    {
        if (closed) throw new IOException("The relay session is closed.");
        pending.Write(buffer, offset, count);
    }

    public override int Read(byte[] buffer, int offset, int count)
    {
        Flush(true);
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(readTimeout);
        while (inbox.Count == 0) {
            double remaining = (deadline - DateTime.UtcNow).TotalSeconds;
            if (remaining <= 0) throw new IOException("Timed out waiting for the agent through the relay.");
            Pull((int)Math.Max(1, Math.Min(WaitMax, remaining)));
        }
        int taken = Math.Min(count, inbox.Count);
        inbox.CopyTo(0, buffer, offset, taken);
        inbox.RemoveRange(0, taken);
        return taken;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && !closed) CloseQuietly();
        base.Dispose(disposing);
    }
}
