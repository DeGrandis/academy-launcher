using System;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace AcademyLauncher
{
    // Online play over the runtime's virtual network (native_port/runtime/HleNet.cpp) and the relay (server/relay).
    public static class Online
    {
        public const int DefaultPort = 3074;
        const uint MagicQuery = 0x514E5743;  // "CWNQ"
        const uint MagicReply = 0x524E5743;  // "CWNR"
        const string CodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O or 1/I

        // The network build: games only see games with the same value (first 8 hex digits of the preset fingerprint).
        public static string BuildId(Preset preset)
        {
            return ModBuilder.Fingerprint(preset).Substring(0, 8);
        }

        public static string NewJoinCode()
        {
            var bytes = new byte[6];
            using (var random = RandomNumberGenerator.Create()) random.GetBytes(bytes);
            var code = new StringBuilder();
            foreach (byte value in bytes) code.Append(CodeAlphabet[value % CodeAlphabet.Length]);
            return code.ToString();
        }

        public static string NormalizeCode(string code)
        {
            var result = new StringBuilder();
            foreach (char c in (code ?? "").ToUpperInvariant())
            {
                if (CodeAlphabet.IndexOf(c) >= 0) result.Append(c);
            }
            return result.ToString();
        }

        // FNV-1a of the upper-cased code, as the runtime and the relay compute it.
        public static uint RoomId(string code)
        {
            uint hash = 2166136261u;
            foreach (byte value in Encoding.ASCII.GetBytes(code.ToUpperInvariant()))
            {
                hash = (hash ^ value) * 16777619u;
            }
            return hash;
        }

        public sealed class QueryResult
        {
            public bool Answered;
            public int Players;
            public uint Build;
        }

        // Asks a game (direct) or the relay (for a room) who is there.
        public static QueryResult Query(string address, string code, int timeoutMs = 2500)
        {
            var result = new QueryResult();
            IPEndPoint endpoint = Resolve(address);
            using (var client = new UdpClient(AddressFamily.InterNetwork))
            {
                client.Client.ReceiveTimeout = timeoutMs;
                var packet = new byte[8];
                BitConverter.GetBytes(MagicQuery).CopyTo(packet, 0);
                BitConverter.GetBytes(code != null ? RoomId(code) : 0u).CopyTo(packet, 4);
                DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
                for (int attempt = 0; attempt < 3 && DateTime.UtcNow < deadline; attempt++)
                {
                    client.Send(packet, packet.Length, endpoint);
                    try
                    {
                        var from = new IPEndPoint(IPAddress.Any, 0);
                        byte[] reply = client.Receive(ref from);
                        if (reply.Length >= 16 && BitConverter.ToUInt32(reply, 0) == MagicReply)
                        {
                            result.Answered = true;
                            result.Players = BitConverter.ToInt32(reply, 8);
                            result.Build = BitConverter.ToUInt32(reply, 12);
                            return result;
                        }
                    }
                    catch (SocketException)
                    {
                        // timeout (or "port unreachable"); try again
                    }
                }
            }
            return result;
        }

        public static IPEndPoint Resolve(string address)
        {
            string host = address.Trim();
            int port = DefaultPort;
            int colon = host.LastIndexOf(':');
            if (colon > 0)
            {
                port = int.Parse(host.Substring(colon + 1));
                host = host.Substring(0, colon);
            }
            IPAddress ip;
            if (!IPAddress.TryParse(host, out ip))
            {
                ip = null;
                foreach (IPAddress candidate in Dns.GetHostAddresses(host))
                {
                    if (candidate.AddressFamily == AddressFamily.InterNetwork) { ip = candidate; break; }
                }
                if (ip == null) throw new SocketException((int)SocketError.HostNotFound);
            }
            return new IPEndPoint(ip, port);
        }
    }
}
