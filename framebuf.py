"""Shared LED-matrix frame publisher.

Both the Flask app and clock.py hook their luma device through hook_device(),
so every panel flush is mirrored to /dev/shm. The web UI polls /frame (which
returns read_frame()) to draw a live replica of the physical display.
"""
import os
import tempfile

FRAME_PATH = "/dev/shm/led_frame"


def publish(image):
    """Write the current panel image to /dev/shm as 'W H <bits>' (row-major)."""
    try:
        img = image.convert("1")
        w, h = img.size
        px = img.load()
        bits = "".join("1" if px[x, y] else "0"
                       for y in range(h) for x in range(w))
        data = "%d %d %s" % (w, h, bits)
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(FRAME_PATH))
        with os.fdopen(fd, "w") as f:
            f.write(data)
        os.replace(tmp, FRAME_PATH)          # atomic; readers never see partial
    except Exception:
        pass


def hook_device(device):
    """Wrap device.display so every flush also publishes the frame."""
    original = device.display

    def display(image):
        original(image)
        publish(image)

    device.display = display
    return device


def read_frame():
    try:
        with open(FRAME_PATH) as f:
            return f.read()
    except Exception:
        return None
