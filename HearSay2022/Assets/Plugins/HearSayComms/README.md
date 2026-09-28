# HearSay communications dependencies

Managed libraries for SshMqttClient.cs, installed from the official NuGet registry:

- MQTTnet 4.3.7.1207 — lib/netstandard2.1/MQTTnet.dll (MIT)
- SSH.NET 2024.2.0 — lib/netstandard2.1/Renci.SshNet.dll (MIT)
- BouncyCastle.Cryptography 2.4.0 — lib/netstandard2.0/BouncyCastle.Cryptography.dll (MIT; LICENSE.md)
- System.Formats.Asn1 8.0.1 — lib/netstandard2.0/System.Formats.Asn1.dll (MIT)

Package download pattern: https://api.nuget.org/v3-flatcontainer/{lowercase-id}/{version}/{lowercase-id}.{version}.nupkg

Unity 2023 supplies the System.Memory/System.Buffers compatibility assemblies. Do not install duplicate framework DLLs or another copy of these libraries through NuGetForUnity.

MQTTnet 4 is intentional: this client uses the version 4 API. Adding these dependencies fixes compilation; Android IL2CPP and live SSH/MQTT connectivity require separate device testing. No scene, camera, caption, or credentials were changed by this dependency installation.
