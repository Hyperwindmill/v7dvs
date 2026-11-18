#!/bin/sh
# Save the reboot the two system files would otherwise need. Each step is
# allowed to fail: inside a container there is neither udev nor a kernel to
# load modules into, and the files are in place for the next boot regardless.
udevadm control --reload-rules 2>/dev/null || true
modprobe snd-aloop 2>/dev/null || true
exit 0
