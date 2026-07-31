# Security Policy

## Reporting A Vulnerability

Do not open a public issue for a suspected vulnerability. Use GitHub private
vulnerability reporting when available, or contact the project maintainers
privately with the affected version, reproduction steps, and impact.

## Security Considerations

- Provisioning bundles contain Wi-Fi credentials, device tokens, and optional
  Spotify credentials. Keep them private and never commit them.
- Generated NVS images are not encrypted production credential storage.
- The satellite WebSocket uses encrypted but unverified TLS for local-network
  deployments. Treat the local network as trusted; this connection is
  vulnerable to a network man-in-the-middle attack.
- Spotify HTTPS uses ESP-IDF's public certificate bundle and verifies server
  certificates.

## Supported Versions

Only the latest version on the `main` branch receives security updates.
