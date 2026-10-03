# Security Policy

## Supported versions

ImGuiQuic is pre-1.0 software. Security fixes are applied to the latest code on
the `main` branch; older revisions are not maintained separately.

## Reporting a vulnerability

Do not disclose vulnerabilities in public issues or discussions. Use
[GitHub's private vulnerability reporting](https://github.com/n3in2019/imgui-quic/security/advisories/new)
and include:

- the affected commit or version
- operating system, browser, and toolchain
- minimal reproduction steps
- expected and observed impact
- any suggested mitigation

You should receive an acknowledgement within seven days. Details and timelines
will be coordinated privately until a fix is available.

## Deployment

The native application exposes only a QUIC endpoint. It requires a shared token,
an exact page Origin allowlist and admission caps including handshakes. Token
comparison is constant-time. Authentication and handshake phases have deadlines;
input records have byte/record budgets. Shared tokens do not identify individual users.

Serve the frontend separately from a trusted HTTPS origin for remote use, and
configure a trusted TLS certificate for the QUIC endpoint. Local development
certificate pins are for loopback testing. Treat launch URLs as credentials:
their fragments contain the token and may appear in browser history.

QUIC/HTTP3/TLS uses pinned Google QUICHE and BoringSSL. Preserve the dependency
notices and update pinned dependencies when applying security fixes. Additional
network rate limiting remains a deployment responsibility.

## Draw transport

Distribute matching frontend and native builds. Reconstructed frames are limited
to 16 MiB with nine cached snapshots. Each client has at most eight outstanding
frames and a 24 KiB encoded budget, allowing one oversized frame alone. Stalled
ACKs disconnect after ten seconds, or 60 seconds during initial setup. Connection
caps must account for per-client memory. Clients share the authoritative native
UI context and its input effects.

See the [endpoint guide](tools/webtransport/README.md) for configuration.
