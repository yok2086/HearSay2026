#ifndef SECRETS_H
#define SECRETS_H

// ---- WiFi (eduroam, WPA2-Enterprise) ----
#define WIFI_SSID       "eduroam"
#define EAP_IDENTITY    "e1121325@nus.edu.sg"
#define EAP_USERNAME    "e1121325@nus.edu.sg"
#define EAP_PASSWORD    "NusnetSCrnjzjb123!"

// ---- SSH (ESP32 -> stujump)  ----
#define SOC_USERNAME    "nathan"
#define SOC_PASSWORD    "NusnetSCrnjzjb123!"

// ---- SSH private key ----
static const char *SSH_PRIVATE_KEY = R"KEY(
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW
QyNTUxOQAAACBc+5dU+4AprMvpi9NzTAIiSMGkNbsSFxwbsntLJBOH/AAAAKj9tvfO/bb3
zgAAAAtzc2gtZWQyNTUxOQAAACBc+5dU+4AprMvpi9NzTAIiSMGkNbsSFxwbsntLJBOH/A
AAAEAZcjlJwrs0GG2XogpXmtmbSWzi1bLfsQuDBkriZD7XGFz7l1T7gCmsy+mL03NMAiJI
waQ1uxIXHBuye0skE4f8AAAAJU5hdGhhblRhbkBOYXRoYW5zLU1hY0Jvb2stUHJvLTQubG
9jYWw=
-----END OPENSSH PRIVATE KEY-----
)KEY";

#endif