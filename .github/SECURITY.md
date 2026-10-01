# Security Policy

## Supported versions

| Version | Supported |
| --- | --- |
| 1.0.x | Yes |
| < 1.0 | No |

Fixes land on `main` and reach users through tagged releases, so there is no separate
maintenance branch for older versions.

## Reporting a vulnerability

Report privately through GitHub's **Security → Report a vulnerability** tab on the
repository. That opens a private advisory visible only to you and the maintainer.

Please do not open a public issue for a security problem.

### What to include

- Affected version or commit
- Your OS, audio interface, and input/output device
- Reproduction steps, ideally the smallest set of steps that still shows the problem
- What you expected and what happened instead
- Impact: what an attacker gains, and what they need in order to gain it

### What to expect

- Acknowledgement within 7 days
- An assessment within 14 days, including whether the report is accepted
- A fix and a tagged release for confirmed issues, with credit in the release notes
  unless you prefer otherwise

## Threat model

OpenSmaartLab runs as a normal desktop application under your own user account. It
opens no listening sockets, makes no outbound network requests, and reads no files
outside the ones you select in a file dialog.

Consequently the practical risks are:

| Area | Risk | Mitigation |
| --- | --- | --- |
| Calibration and CSV file loading | Parsing attacker-supplied files | Parsers reject malformed input and never execute content |
| Snapshot storage | Path handling on save | File names are sanitised; paths are written under the user's documents directory |
| Colour persistence | Writes a small config file | Fixed key/value format, no code execution |
| Audio device access | Requires OS permission | Granted by the OS, not by this application |

Any finding that leads to arbitrary code execution, or to reading or writing files
outside the directories you chose, should be treated as high severity.

## Scope

In scope:

- The code in this repository
- Release binaries published from this repository

Out of scope:

- Vulnerabilities in third-party dependencies, unless they are reachable through a
  path this application does not need
- Physical access to your machine
- Issues requiring you to deliberately compile a modified build
- Measurement accuracy. Wrong readings are a correctness bug, not a security issue;
  please report those as ordinary issues.

## Notes on measurement security

Two properties matter for the integrity of a measurement and are worth protecting
even though they are not classic vulnerabilities:

- **Calibration integrity.** A modified calibration file changes every SPL reading.
  Treat calibration files as trusted input from your microphone's manufacturer.
- **Bluetooth output.** A2DP encodes to SBC, which alters the signal. Using
  Bluetooth for measurement produces numbers that are wrong rather than insecure.
  Use analogue or USB output.

## Independence

This is an independent implementation. It is not affiliated with or endorsed by
Rational Acoustics and contains no third-party Smaart source code, artwork, or
algorithms. Report security issues for this project to the maintainer listed below,
not to any third party.