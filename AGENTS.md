# ydPenetrationTest Codex Instructions

Before modifying code, read:

- `docs/CODEX_CONTEXT.md`
- `README.md`
- `docs/TEST_MAPPING.md`

## Project goal

This is a C++ YD API automated functional-test framework for programmatic external-system / penetration testing.

It is NOT the EMA trading strategy project.

## Safety

- Never remove the `--live` safety gate.
- Tests that submit or cancel real orders must require `--