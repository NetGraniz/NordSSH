"""Real X11 keyboard/clipboard checks, ONLY inside a fresh disposable Xvfb.

Uses XTest keycodes from the server's US map, then repeats the same physical
keys after switching its layout to Russian. Does not touch a user's display.
"""
import ctypes
import ctypes.util
import os
import pathlib
import select
import struct
import subprocess
import sys
import time

if os.environ.get("NORDSSH_ISOLATED_DESKTOP") != "1":
    raise SystemExit("Run this fixture only in a new isolated Xvfb server")

driver = subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve())],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          env={**os.environ, "QT_QPA_PLATFORM": "xcb"})


def exact(size):
    data = b""
    deadline = time.monotonic() + 5
    while len(data) < size:
        if time.monotonic() > deadline:
            raise TimeoutError("Native X11 driver stalled")
        if select.select([driver.stdout], [], [], 0.1)[0]:
            chunk = os.read(driver.stdout.fileno(), size - len(data))
            if not chunk:
                raise AssertionError("Native X11 driver closed")
            data += chunk
    return data


def request(kind, data=b""):
    driver.stdin.write(kind + struct.pack("!I", len(data)) + data); driver.stdin.flush()
    header = exact(5)
    size = struct.unpack("!I", header[1:])[0]
    if size > 262144:
        raise AssertionError("Native X11 reply exceeds cap")
    return exact(size)


def events():
    result = b""
    deadline = time.monotonic() + 0.2
    while time.monotonic() < deadline:
        result += request(b"E")
        time.sleep(0.01)
    return result


x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
x11.XkbLockGroup.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
display = x11.XOpenDisplay(None)
if not display:
    raise SystemExit("Cannot open isolated Xvfb display")


def chord(keys):
    for code in keys:
        xtst.XTestFakeKeyEvent(display, code, 1, 0)
    for code in reversed(keys):
        xtst.XTestFakeKeyEvent(display, code, 0, 0)
    x11.XSync(display, 0)
    return events()


try:
    if request(b"P") != b"xcb":
        raise AssertionError("Physical input fixture must use actual Qt X11")
    for _ in range(50):
        window = subprocess.run(["xdotool", "search", "--pid", str(driver.pid), "--name", "NordSSH isolated"],
                                capture_output=True, text=True)
        if window.returncode == 0:
            break
        request(b"E"); time.sleep(0.02)
    if window.returncode:
        raise AssertionError("Native X11 window was not created")
    subprocess.run(["xdotool", "windowfocus", window.stdout.splitlines()[0]], check=True)
    events()
    subprocess.run(["setxkbmap", "-layout", "us"], check=True)
    control = x11.XKeysymToKeycode(display, 0xffe3)
    shift = x11.XKeysymToKeycode(display, 0xffe1)
    v = x11.XKeysymToKeycode(display, ord("v"))
    c = x11.XKeysymToKeycode(display, ord("c"))
    a = x11.XKeysymToKeycode(display, ord("a"))
    insert = x11.XKeysymToKeycode(display, 0xff63)
    if not all([control, shift, v, c, a, insert]):
        raise AssertionError("X11 US physical keycode map unavailable")
    text = "X11 clipboard Привет".encode()
    request(b"B", text)
    # A different process owns the selection, so Qt must use the OS clipboard
    # protocol rather than just reading its own cached QMimeData.
    # Keep both layouts configured, like a normal bilingual desktop. Qt's
    # documented shortcut fallback needs an available Latin keymap.
    subprocess.run(["setxkbmap", "-layout", "us,ru"], check=True)
    for group, layout in enumerate(("us", "ru")):
        if not x11.XkbLockGroup(display, 0x0100, group):  # XkbUseCoreKbd
            raise AssertionError("Cannot select isolated XKB layout group")
        x11.XSync(display, 0)
        events()
        subprocess.run(["xclip", "-selection", "clipboard"], input=text, check=True)
        events()
        if chord([control, v]) != text:
            raise AssertionError(f"Real {layout} Ctrl+V did not paste exactly once")
        if chord([control, shift, v]) != text:
            raise AssertionError(f"Real {layout} Ctrl+Shift+V did not paste exactly once")
        if chord([shift, insert]) != text:
            raise AssertionError(f"Real {layout} Shift+Insert did not paste exactly once")
        if chord([control, c]) != b"\x03":
            raise AssertionError(f"Real {layout} Ctrl+C did not interrupt exactly once")
        if chord([a]) != (b"a" if layout == "us" else "ф".encode()):
            raise AssertionError(f"Real {layout} printable input did not match selected layout")
        request(b"F", b"\x1b[2J\x1b[HCOPY-TEXT")
        request(b"S", struct.pack("!I", 9))
        if chord([control, shift, c]):
            raise AssertionError(f"Real {layout} Copy sent terminal input")
        reader = subprocess.Popen(["xclip", "-selection", "clipboard", "-o"], stdout=subprocess.PIPE)
        events()
        copied, _ = reader.communicate(timeout=2)
        if reader.returncode or copied != b"COPY-TEXT":
            raise AssertionError(f"Real {layout} Copy did not write correct OS clipboard text")
        print(f"PASS X11 native physical {layout} shortcuts + QClipboard", flush=True)
finally:
    x11.XCloseDisplay(display)
    driver.stdin.close()
    try:
        driver.wait(3)
    except subprocess.TimeoutExpired:
        driver.kill(); driver.wait(2)
    if driver.returncode:
        raise AssertionError("Native X11 driver failed")
