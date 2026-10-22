#!/usr/bin/env python3
"""Start GUI regression checks with private display and session-bus services."""
import os
import signal
import sys

# Dome launchers can inherit blocked termination signals. Test services must
# receive SIGTERM so xvfb-run and dbus-run-session can clean up normally.
signal.pthread_sigmask(signal.SIG_SETMASK, [])
xvfb, dbus, executable = sys.argv[1:]
os.execv(xvfb, [xvfb, "-a", dbus, "--", executable])
