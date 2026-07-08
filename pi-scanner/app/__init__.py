"""Careflow RS-485 scanner/profiling station (Pi 5).

Turns an unknown Modbus RTU device — probed *through* a Careflow node's `scan-*` console — into a
reusable `device-profiles/profiles/<model>.json`, then hands it to the existing profile-driven
flash/provision/decoder pipeline. See pi-scanner/README.md and the plan in
~/.claude/plans/generic-jingling-owl.md.
"""

__all__ = []
