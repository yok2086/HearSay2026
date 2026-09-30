using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Renci.SshNet;
using Renci.SshNet.Common;

namespace CG4002.Comms
{
    // Forwards the phone's 127.0.0.1:<port> to the Ultra96's localhost:<port>, where
    // run_hearsay_pipeline.sh reverse-forwards HearSayDummyServer.py from the Mac.
    // Same stujump -> ultra96 hop as SshMqttClient, but independent of the broker and
    // of any scene, so the HTTP server stays reachable across scene loads.
    public static class ServerTunnel
    {
        static readonly object gate = new object();
        static CancellationTokenSource cts;
        static volatile bool ready;
        static string status = "Not started";

        public static bool Ready => ready;
        public static string Status { get { lock (gate) return status; } }
        static void SetStatus(string text) { lock (gate) status = text; }

        public static void EnsureStarted(int port)
        {
            CancellationToken token;
            lock (gate)
            {
                if (cts != null) return;
                cts = new CancellationTokenSource();
                token = cts.Token;
            }
            _ = Task.Run(() => RunAsync(new ConnectionSecrets(), port, token));
        }

        public static void Stop()
        {
            lock (gate) { cts?.Cancel(); cts = null; }
        }

        // stujump asks for password only from NUS VPN addresses, but key + password
        // from eduroam, so offer every method and let the server pick.
        static SshClient CreateSsh(string host, int port, string username, string password, PrivateKeyFile key)
        {
            var interactive = new KeyboardInteractiveAuthenticationMethod(username);
            interactive.AuthenticationPrompt += (_, args) => {
                foreach (var prompt in args.Prompts)
                    if (prompt.Request.IndexOf("password", StringComparison.OrdinalIgnoreCase) >= 0)
                        prompt.Response = password;
            };
            var info = new ConnectionInfo(host, port, username,
                new PrivateKeyAuthenticationMethod(username, key),
                new PasswordAuthenticationMethod(username, password), interactive) {
                Timeout = TimeSpan.FromSeconds(20)
            };
            return new SshClient(info) { KeepAliveInterval = TimeSpan.FromSeconds(10) };
        }

        static async Task RunAsync(ConnectionSecrets secrets, int port, CancellationToken cancellation)
        {
            try
            {
                secrets.Validate();
                while (!cancellation.IsCancellationRequested)
                {
                    string hop = "stujump";
                    try
                    {
                        SetStatus("Connecting to SSH jump host...");
                        using (var stream = new MemoryStream(Encoding.UTF8.GetBytes(secrets.sshPrivateKey)))
                        using (var key = string.IsNullOrEmpty(secrets.sshKeyPassphrase)
                            ? new PrivateKeyFile(stream) : new PrivateKeyFile(stream, secrets.sshKeyPassphrase))
                        using (var jump = CreateSsh(secrets.jumpHost, secrets.jumpPort,
                            secrets.socUsername, secrets.socPassword, key))
                        {
                            // Blocking Connect: ConnectAsync times out on Android (IL2CPP).
                            jump.Connect();
                            using (var sshPort = new ForwardedPortLocal("127.0.0.1", 0,
                                secrets.ultra96Host, (uint)secrets.ultra96Port))
                            {
                                jump.AddForwardedPort(sshPort); sshPort.Start();
                                hop = "Ultra96";
                                SetStatus("Connecting to Ultra96 SSH...");
                                using (var ultra = CreateSsh("127.0.0.1", (int)sshPort.BoundPort,
                                    secrets.ultra96Username, secrets.ultra96Password, key))
                                {
                                    ultra.Connect();
                                    using (var serverPort = new ForwardedPortLocal("127.0.0.1", (uint)port,
                                        "127.0.0.1", (uint)port))
                                    {
                                        ultra.AddForwardedPort(serverPort); serverPort.Start();
                                        ready = true;
                                        SetStatus("Tunnel up");
                                        while (jump.IsConnected && ultra.IsConnected && serverPort.IsStarted)
                                            await Task.Delay(500, cancellation).ConfigureAwait(false);
                                        throw new IOException("Tunnel closed.");
                                    }
                                }
                            }
                        }
                    }
                    catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { break; }
                    catch (SshAuthenticationException error)
                    { SetStatus("SSH authentication failed at " + hop + ": " + error.Message); return; }
                    catch (Exception error)
                    {
                        var root = error.GetBaseException();
                        SetStatus("Tunnel down at " + hop + ": " + root.GetType().Name + ": " + root.Message + " (retrying)");
                    }
                    finally { ready = false; }
                    try { await Task.Delay(3000, cancellation).ConfigureAwait(false); }
                    catch (OperationCanceledException) { break; }
                }
                SetStatus("Stopped");
            }
            catch (Exception error) { SetStatus("Tunnel failed: " + error.Message); }
            finally
            {
                ready = false;
                // Allow a later EnsureStarted to retry after auth/config failures.
                lock (gate) { if (cts != null && cts.Token == cancellation) cts = null; }
            }
        }
    }
}
