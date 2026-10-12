# Security policy

## Reporting a vulnerability

Please report privately through GitHub: open the Security tab of this repository (eduke32-aa-menu) and choose
"Report a vulnerability" (private vulnerability reporting). Do not open a public issue for a vulnerability.

## Scope

In scope: the DLL this repository builds and the scripts that build and install it (for example a way to make the
DLL run attacker-chosen code, or an install script that overwrites a file it did not create).

Out of scope: cheating in itself. The menu is a game mod; its features are not vulnerabilities.

## Response

This is a single-maintainer project. Reports are handled on a best-effort basis, with no guaranteed response time.
Releases are built by GitHub Actions and carry a build provenance attestation and a SHA-256 checksum, so you can
check that a downloaded DLL came from this repository's workflow.
