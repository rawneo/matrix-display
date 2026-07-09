#!/usr/bin/env python
import json
import math
import os
import random
import threading
import time
from datetime import datetime
from urllib.request import Request, urlopen

from luma.led_matrix.device import max7219
from luma.core.interface.serial import spi, noop
from luma.core.render import canvas
from luma.core.legacy import text, show_message
from luma.core.legacy.font import proportional, CP437_FONT, LCD_FONT, TINY_FONT

from flask import Flask, render_template, request, redirect

import framebuf

# index.html lives next to app.py instead of a templates/ subfolder
app = Flask(__name__, template_folder=".")

# The MAX7219 is a single shared resource. Serialize access so two
# concurrent requests can't interleave SPI writes and corrupt the display.
device_lock = threading.Lock()

# Background scroller state. Only one ledtext loop runs at a time; it keeps
# scrolling the message until its stop event is set.
ledtext_lock = threading.Lock()
ledtext_thread = None
ledtext_stop = threading.Event()


def init_device():
    serial = spi(port=0, device=0, gpio=noop())
    dev = max7219(serial, cascaded=7, block_orientation=-90,
                  rotate=0, blocks_arranged_in_reverse_order=False)
    dev.contrast(16)
    return dev


# Initialise at import time so the device exists no matter how the app is
# launched (python app.py, gunicorn, etc.), not only under __main__.
device = init_device()
framebuf.hook_device(device)          # mirror every flush to the web replica


def _draw_clock_face(draw, y, hours, minutes, month, date, colon=":"):
    text(draw, (0, y), hours, fill="white", font=proportional(LCD_FONT))
    text(draw, (11, y), colon, fill="white", font=proportional(TINY_FONT))
    text(draw, (13, y), minutes, fill="white", font=proportional(LCD_FONT))
    text(draw, (26, 0), month, fill="white", font=proportional(LCD_FONT))
    text(draw, (44, 0), date, fill="white", font=proportional(LCD_FONT))


def _run_clock(stop_event):
    """The default clock display (merged from examples/clock.py): HH:MM with a
    blinking colon + date, sliding animation on minute change, dim at night."""
    toggle = False
    while not stop_event.is_set():
        now = datetime.now()
        device.contrast(0 if now.hour < 6 or now.hour >= 21 else 16)
        toggle = not toggle
        hours, minutes = now.strftime('%H'), now.strftime('%M')
        month, date = now.strftime('%b'), now.strftime('%d')
        if now.second == 59:
            # minute change: slide the time down and back up with the new value
            for y in list(range(0, 9)) + list(range(9, -1, -1)):
                if stop_event.is_set():
                    return
                if y == 9:
                    hours = datetime.now().strftime('%H')
                    minutes = datetime.now().strftime('%M')
                with device_lock, canvas(device) as draw:
                    _draw_clock_face(draw, y, hours, minutes, month, date)
                time.sleep(0.1)
        else:
            with device_lock, canvas(device) as draw:
                _draw_clock_face(draw, 0, hours, minutes, month, date,
                                 colon=":" if toggle else " ")
            time.sleep(0.5)


# WMO weather codes -> short, panel-friendly descriptions.
WMO_CODES = {
    0: "Clear", 1: "Clear", 2: "Partly Cloudy", 3: "Overcast",
    45: "Fog", 48: "Fog",
    51: "Drizzle", 53: "Drizzle", 55: "Drizzle",
    56: "Icy Drizzle", 57: "Icy Drizzle",
    61: "Rain", 63: "Rain", 65: "Heavy Rain",
    66: "Freezing Rain", 67: "Freezing Rain",
    71: "Snow", 73: "Snow", 75: "Heavy Snow", 77: "Snow",
    80: "Showers", 81: "Showers", 82: "Heavy Showers",
    85: "Snow Showers", 86: "Snow Showers",
    95: "Thunderstorm", 96: "Thunderstorm", 99: "Thunderstorm",
}


def _get_json(url, timeout=8, tries=3):
    """GET JSON with a couple of retries so a transient network blip doesn't
    fail the request outright."""
    last = None
    for _ in range(tries):
        try:
            req = Request(url, headers={"User-Agent": "led-clock/1.0"})
            with urlopen(req, timeout=timeout) as resp:
                return json.load(resp)
        except Exception as exc:  # noqa: BLE001 - any network error is retryable
            last = exc
    raise last


def fetch_weather():
    """Return a one-line local forecast for the LED panel, or None on failure.
    Location comes from LED_LAT/LED_LON/LED_CITY if set, else IP geolocation."""
    lat = os.environ.get("LED_LAT")
    lon = os.environ.get("LED_LON")
    city = os.environ.get("LED_CITY", "")
    try:
        if not (lat and lon):
            geo = _get_json("http://ip-api.com/json/?fields=status,city,lat,lon")
            lat, lon = geo["lat"], geo["lon"]
            city = city or geo.get("city", "")

        url = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
               "&current=temperature_2m,relative_humidity_2m,weather_code"
               "&timezone=auto") % (lat, lon)
        data = _get_json(url)
        cur = data["current"]

        parts = []
        if city:
            parts.append(city)
        parts.append("%.0fC" % cur["temperature_2m"])
        desc = WMO_CODES.get(cur["weather_code"])
        if desc:
            parts.append(desc)
        hum = cur.get("relative_humidity_2m")
        if hum is not None:
            parts.append("Hum %d%%" % hum)
        return "  ".join(parts)
    except Exception:  # noqa: BLE001 - never let a fetch error crash the route
        return None


_dht_sensor = None


def read_dht():
    """Read the DHT11 on GPIO4. Returns (temp_c, humidity) as ints, or None.
    DHT11 reads fail often, so retry a few times."""
    global _dht_sensor
    try:
        import board
        import adafruit_dht
        if _dht_sensor is None:
            _dht_sensor = adafruit_dht.DHT11(board.D4)
        for _ in range(4):
            try:
                t = _dht_sensor.temperature
                h = _dht_sensor.humidity
                if t is not None and h is not None:
                    return int(round(t)), int(round(h))
            except RuntimeError:
                pass                        # transient checksum/timeout, retry
            time.sleep(2)
        return None
    except Exception:  # noqa: BLE001 - never let a sensor error crash the route
        return None


def _run_ledtext(msg, stop_event):
    """Scroll msg across the panel over and over until stop_event is set.
    The stop takes effect at the end of the current scroll pass."""
    while not stop_event.is_set():
        with device_lock:
            show_message(device, msg, fill="white",
                         font=proportional(CP437_FONT), scroll_delay=.05)


def _start_worker(target):
    """Stop any running display loop, then run target(stop_event) in a daemon
    thread. Whatever is running is stopped by stop_ledtext() / the Stop button."""
    global ledtext_thread, ledtext_stop
    with ledtext_lock:
        _stop_ledtext_locked()
        device.contrast(16)      # undo night-dimming / fireworks fades
        ledtext_stop = threading.Event()
        stop_event = ledtext_stop
        ledtext_thread = threading.Thread(target=target, args=(stop_event,), daemon=True)
        ledtext_thread.start()


def start_clock():
    """Switch the display back to the default clock."""
    _start_worker(_run_clock)


def start_ledtext(msg):
    """Scroll msg continuously until stopped."""
    _start_worker(lambda stop_event: _run_ledtext(msg, stop_event))


def stop_ledtext():
    with ledtext_lock:
        _stop_ledtext_locked()


def _stop_ledtext_locked():
    """Signal the scroller to stop and wait for it. Caller holds ledtext_lock."""
    global ledtext_thread
    if ledtext_thread and ledtext_thread.is_alive():
        ledtext_stop.set()
        ledtext_thread.join(timeout=15)
    ledtext_thread = None


def _run_charcycle(stop_event):
    """Step through the whole CP437 font on the panel, one glyph per block."""
    segments = device.width // 8
    for x in range(256):
        if stop_event.is_set():
            return
        with device_lock, canvas(device) as draw:
            for seg in range(segments):
                text(draw, (seg * 8, 0), chr(x), fill="white")
            time.sleep(0.1)
    # done: fall through to the clock within this same worker
    _run_clock(stop_event)


# Running-man animation: 8x8 sprite frames (legs/arms scissor between poses).
RUNNER_FRAMES = [
    (0b00011000, 0b00011000, 0b01011010, 0b00111100,   # arms out, legs apart
     0b00011000, 0b00011000, 0b00100100, 0b01000010),
    (0b00011000, 0b00011000, 0b00111100, 0b00011000,   # passing: arms/legs in
     0b00011000, 0b00011000, 0b00011000, 0b00111100),
    (0b00011000, 0b00011000, 0b01011010, 0b00111100,   # arms out, legs scissor
     0b00011000, 0b00011000, 0b01000010, 0b00100100),
    (0b00011000, 0b00011000, 0b00111100, 0b00011000,   # passing again
     0b00011000, 0b00011000, 0b00011000, 0b00111100),
]

# Pac-Man chomping, one set per facing direction (mouth open / closed).
PACMAN_LEFT = [
    (0b00111100, 0b01111110, 0b00011111, 0b00001111,   # mouth open (facing left)
     0b00001111, 0b00011111, 0b01111110, 0b00111100),
    (0b00111100, 0b01111110, 0b11111111, 0b11111111,   # mouth closed
     0b11111111, 0b11111111, 0b01111110, 0b00111100),
]
PACMAN_RIGHT = [
    (0b00111100, 0b01111110, 0b11111000, 0b11110000,   # mouth open (facing right)
     0b11110000, 0b11111000, 0b01111110, 0b00111100),
    (0b00111100, 0b01111110, 0b11111111, 0b11111111,   # mouth closed
     0b11111111, 0b11111111, 0b01111110, 0b00111100),
]

# Ghost with wavy feet (two frames for the shimmer).
GHOST_FRAMES = [
    (0b00111100, 0b01111110, 0b11011011, 0b11111111,
     0b11111111, 0b11111111, 0b11111111, 0b10101010),
    (0b00111100, 0b01111110, 0b11011011, 0b11111111,
     0b11111111, 0b11111111, 0b11111111, 0b01010101),
]

# Dancing man (a clear jig): arms-out bop -> kick right + arms left ->
# arms up -> kick left + arms right.
DANCER_FRAMES = [
    (0b00000000, 0b00011000, 0b00011000, 0b01111110,
     0b00011000, 0b00011000, 0b00011000, 0b00111100),
    (0b00000000, 0b00011000, 0b00011000, 0b11111000,
     0b00011000, 0b00011000, 0b00011110, 0b00010000),
    (0b00000000, 0b00011000, 0b01011010, 0b00011000,
     0b00011000, 0b00011000, 0b00011000, 0b00111100),
    (0b00000000, 0b00011000, 0b00011000, 0b00011111,
     0b00011000, 0b00011000, 0b01111000, 0b00001000),
]


def _blit(draw, sprite, x0, y0=0):
    """Draw an 8x8 sprite at offset (x0, y0), clipped to the panel."""
    for row in range(8):
        py = y0 + row
        if not 0 <= py < 8:
            continue
        bits = sprite[row]
        for col in range(8):
            if bits & (1 << (7 - col)):
                px = x0 + col
                if 0 <= px < device.width:
                    draw.point((px, py), fill="white")


def _run_runner(stop_event):
    """Pac-Man scene: a man flees while Pac-Man chomps a trail of pellets behind
    him and a ghost chases them both. The chase randomly reverses direction each
    time it crosses the panel. Loops until stopped."""
    W = device.width
    gap, gap2 = 11, 22
    direction = random.choice((-1, 1))          # -1 = leftward, +1 = rightward
    x = W if direction < 0 else -8
    frame = 0
    while not stop_event.is_set():
        pac_x = x - direction * gap             # Pac-Man trails behind the man
        ghost_x = x - direction * gap2          # ghost trails behind Pac-Man
        pac_frames = PACMAN_LEFT if direction < 0 else PACMAN_RIGHT
        front = pac_x if direction < 0 else pac_x + 7
        with device_lock, canvas(device) as draw:
            for p in range(2, W, 5):            # uneaten pellets ahead of Pac-Man
                ahead = p < front if direction < 0 else p > front
                if ahead:
                    draw.point((p, 3), fill="white")
            _blit(draw, GHOST_FRAMES[frame % len(GHOST_FRAMES)], ghost_x)
            _blit(draw, pac_frames[frame % len(pac_frames)], pac_x)
            _blit(draw, RUNNER_FRAMES[frame % len(RUNNER_FRAMES)], x)
        x += direction * 2
        if (direction < 0 and x < -8 - gap2) or (direction > 0 and x > W + gap2):
            direction = random.choice((-1, 1))  # fresh random direction each pass
            x = W if direction < 0 else -8
        frame += 1
        time.sleep(0.12)


def _run_dancer(stop_event):
    """Three dancing men across the panel, each a step out of phase, bouncing
    up and down on the beat."""
    positions = (8, 24, 40)          # evenly spaced across the 56px panel
    frame = 0
    while not stop_event.is_set():
        with device_lock, canvas(device) as draw:
            for i, x0 in enumerate(positions):
                step = (frame + i) % len(DANCER_FRAMES)
                bob = -1 if (frame + i) % 2 == 0 else 0     # hop on the beat
                _blit(draw, DANCER_FRAMES[step], x0, bob)
        frame += 1
        time.sleep(0.18)


def _run_kitt(stop_event):
    """Larson/KITT scanner: a bright bar sweeping back and forth with a tail."""
    W = device.width
    pos = 0
    step = 1
    tail = (8, 6, 4, 2)
    while not stop_event.is_set():
        with device_lock, canvas(device) as draw:
            for i, h in enumerate(tail):
                bx = pos - step * i
                if 0 <= bx < W:
                    y0 = (8 - h) // 2
                    draw.line((bx, y0, bx, y0 + h - 1), fill="white")
        pos += step
        if pos >= W - 1:
            step = -1
        elif pos <= 0:
            step = 1
        time.sleep(0.03)


def _run_equalizer(stop_event):
    """Music-style equalizer: vertical bars bobbing up and down."""
    W, H = device.width, 8
    bar_w, gap = 3, 1
    n = (W + gap) // (bar_w + gap)
    phase = [random.uniform(0, 6.283) for _ in range(n)]
    speed = [random.uniform(0.15, 0.45) for _ in range(n)]
    t = 0
    while not stop_event.is_set():
        with device_lock, canvas(device) as draw:
            for i in range(n):
                level = math.sin(phase[i] + t * speed[i]) * 0.5 + 0.5
                h = max(1, int(level * H))
                x0 = i * (bar_w + gap)
                draw.rectangle((x0, H - h, x0 + bar_w - 1, H - 1), fill="white")
        t += 1
        time.sleep(0.05)


def _run_rain(stop_event):
    """Matrix-style digital rain: falling pixel streaks."""
    W, H = device.width, 8
    tail = 3
    head = [0 if random.random() < 0.35 else None for _ in range(W)]
    speed = [random.choice((1, 1, 2)) for _ in range(W)]
    while not stop_event.is_set():
        with device_lock, canvas(device) as draw:
            for col in range(W):
                h = head[col]
                if h is None:
                    continue
                for seg in range(tail):
                    y = h - seg
                    if 0 <= y < H:
                        draw.point((col, y), fill="white")
        for col in range(W):
            if head[col] is None:
                if random.random() < 0.05:
                    head[col] = 0
                    speed[col] = random.choice((1, 1, 2))
            else:
                head[col] += speed[col]
                if head[col] - tail >= H:
                    head[col] = None
        time.sleep(0.07)


def _run_fireworks(stop_event):
    """Fireworks: a shell launches dim, bursts, then the sparks expand and
    fade smoothly (panel brightness eased out over many sub-frames)."""
    W, H = device.width, 8
    rings = 12                                       # keep expanding until next blast
    try:
        while not stop_event.is_set():
            bx = random.randint(7, W - 8)
            peak = random.randint(0, 2)
            device.contrast(6)                      # faint rising trail
            y = H - 1
            while y > peak and not stop_event.is_set():
                with device_lock, canvas(device) as draw:
                    draw.point((bx, y), fill="white")
                y -= 1
                time.sleep(0.04)
            for i in range(rings):                  # ring grows + fades to nothing
                if stop_event.is_set():
                    break
                r = i + 1
                device.contrast(max(1, int(255 * (1 - i / (rings - 1)))))
                with device_lock, canvas(device) as draw:
                    for ang in range(0, 360, 45):
                        px = int(round(bx + r * math.cos(math.radians(ang))))
                        py = int(round(peak + r * math.sin(math.radians(ang))))
                        if 0 <= px < W and 0 <= py < H:
                            draw.point((px, py), fill="white")
                time.sleep(0.09)
    finally:
        device.contrast(16)                         # restore default brightness


# A little spaceship (points left, its heading) cruising through the stars.
SHIP = (
    0b00000000,
    0b00000000,
    0b00001100,
    0b00011110,
    0b01111111,
    0b00011110,
    0b00001100,
    0b00000000,
)


def _run_starfield(stop_event):
    """Parallax starfield drifting right to left, with one bigger ship."""
    W, H = device.width, 8
    n = 18
    sx = [random.uniform(0, W) for _ in range(n)]
    sy = [random.randint(0, H - 1) for _ in range(n)]
    sp = [random.uniform(0.3, 1.6) for _ in range(n)]
    ship_x = float(W)
    while not stop_event.is_set():
        with device_lock, canvas(device) as draw:
            for i in range(n):
                draw.point((int(sx[i]), sy[i]), fill="white")
            _blit(draw, SHIP, int(ship_x))          # the bigger ship, on top
        for i in range(n):
            sx[i] -= sp[i]
            if sx[i] < 0:
                sx[i] = W - 1
                sy[i] = random.randint(0, H - 1)
                sp[i] = random.uniform(0.3, 1.6)
        ship_x -= 0.8
        if ship_x < -8:
            ship_x = float(W)
        time.sleep(0.05)


def _run_ecg(stop_event):
    """A heart-monitor ECG trace (PQRST) scrolling right to left."""
    W = device.width
    base = 5
    beat = ([base] * 6 + [4, 4] + [base] * 3 + [6, 0, 7, base] +
            [base] * 2 + [4, 3, 4] + [base] * 6)      # one heartbeat, row values
    wave = beat * (W // len(beat) + 2)
    n = len(wave)
    pos = 0
    while not stop_event.is_set():
        with device_lock, canvas(device) as draw:
            prev = wave[pos % n]
            for x in range(W):
                y = wave[(pos + x) % n]
                lo, hi = (prev, y) if prev <= y else (y, prev)
                draw.line((x, lo, x, hi), fill="white")   # connect to keep it continuous
                prev = y
        pos += 1
        time.sleep(0.04)


def _run_dht(stop_event):
    """Scroll the DHT11 temperature and humidity, refreshed each pass."""
    last = None
    while not stop_event.is_set():
        reading = read_dht()
        if reading:
            last = reading
        msg = "Room Temp %dC  Hum %d%%" % last if last else "DHT11 no data"
        with device_lock:
            show_message(device, msg, fill="white",
                         font=proportional(CP437_FONT), scroll_delay=.05)


@app.route('/')
def index():
    return render_template('index.html')


@app.route('/startclock', methods=['POST'])
def startclock():
    start_clock()
    return redirect("/")


@app.route('/stopclock', methods=['POST'])
def stopclock():
    stop_ledtext()
    with device_lock, canvas(device) as draw:
        text(draw, (0, 0), "STOP", fill="white", font=proportional(LCD_FONT))
    return redirect("/")


@app.route('/ledtext', methods=['POST'])
def ledtext():
    msg = request.form.get('ledtext', '').strip()
    if not msg:
        return redirect("/")

    start_ledtext(msg)
    return redirect("/")


@app.route('/stopledtext', methods=['POST'])
def stopledtext():
    start_clock()
    return redirect("/")


@app.route('/weather', methods=['POST'])
def weather():
    msg = fetch_weather()
    if msg:
        # Scrolls until the "Stop" button is pressed.
        start_ledtext(msg)
    else:
        stop_ledtext()
        with device_lock:
            show_message(device, "WEATHER N/A", fill="white",
                         font=proportional(CP437_FONT), scroll_delay=.05)
        start_clock()
    return redirect("/")


@app.route('/charset', methods=['POST'])
def charset():
    _start_worker(_run_charcycle)
    return redirect("/")


@app.route('/pacman', methods=['POST'])
def pacman():
    _start_worker(_run_runner)
    return redirect("/")


@app.route('/kitt', methods=['POST'])
def kitt():
    _start_worker(_run_kitt)
    return redirect("/")


@app.route('/equalizer', methods=['POST'])
def equalizer():
    _start_worker(_run_equalizer)
    return redirect("/")


@app.route('/rain', methods=['POST'])
def rain():
    _start_worker(_run_rain)
    return redirect("/")


@app.route('/fireworks', methods=['POST'])
def fireworks():
    _start_worker(_run_fireworks)
    return redirect("/")


@app.route('/starfield', methods=['POST'])
def starfield():
    _start_worker(_run_starfield)
    return redirect("/")


@app.route('/dancer', methods=['POST'])
def dancer():
    _start_worker(_run_dancer)
    return redirect("/")


@app.route('/ecg', methods=['POST'])
def ecg():
    _start_worker(_run_ecg)
    return redirect("/")


@app.route('/dht', methods=['POST'])
def dht():
    _start_worker(_run_dht)
    return redirect("/")


@app.route('/frame')
def frame():
    data = framebuf.read_frame()
    if not data:
        return "", 204
    return data, 200, {"Content-Type": "text/plain", "Cache-Control": "no-store"}


@app.route('/health')
def health():
    return {"status": "ok"}, 200


# Boot straight into the clock (replaces the old separate clock.service).
start_clock()


if __name__ == '__main__':
    # debug defaults OFF: this app is reachable over the Cloudflare tunnel,
    # and Werkzeug's debugger allows remote code execution. Opt in locally
    # with LED_DEBUG=1 only.
    debug = os.environ.get("LED_DEBUG", "").lower() in ("1", "true", "yes")
    host = os.environ.get("LED_HOST", "0.0.0.0")
    port = int(os.environ.get("LED_PORT", "5000"))
    app.run(debug=debug, host=host, port=port, threaded=True)
