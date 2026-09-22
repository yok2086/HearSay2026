#ifndef SECRETS_H
#define SECRETS_H

// ---- WiFi (eduroam, WPA2-Enterprise) ----
#define WIFI_SSID       "eduroam"
#define EAP_IDENTITY    "e1121325@nus.edu.sg"
#define EAP_USERNAME    "e1121325@nus.edu.sg"
#define EAP_PASSWORD    "your_nusnet_password"

// ---- SSH stage 1 (ESP32 -> stujump) password fallback ----
// Public-key auth is tried first (see SSH_PRIVATE_KEY below); this is only
// used if that fails - which is expected for stujump, since you likely
// can't register a public key on an NUS-managed bastion yourself.
#define SOC_USERNAME    "nathan"
#define SOC_PASSWORD    "NusnetSCrnjzjb123!"

// ---- SSH private key, tried first for BOTH hops ----
// Generate with (no passphrase - this code doesn't support one):
//   ssh-keygen -t ed25519 -f ~/.ssh/hearsay_esp32_key -N ""
// If libssh_esp32 rejects ed25519 at runtime ("Private key import failed"),
// regenerate as RSA instead:
//   ssh-keygen -t rsa -b 4096 -f ~/.ssh/hearsay_esp32_key -N ""
//
// Paste the FULL contents of the resulting private key file below,
// including the -----BEGIN/-----END lines. Add the matching .pub file's
// contents to ~/.ssh/authorized_keys on the Ultra96 (you have shell access
// there) - this makes public-key auth succeed for the ultra96 hop even
// though it'll fall back to password for the stujump hop.
static const char *SSH_PRIVATE_KEY = R"KEY(
-----BEGIN OPENSSH PRIVATE KEY-----
PASTE_YOUR_PRIVATE_KEY_CONTENTS_HERE
-----END OPENSSH PRIVATE KEY-----
)KEY";

#endif