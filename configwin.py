#!/usr/bin/env python3
"""
The config window: tkinter, and the only file in the project that knows a GUI
exists. GPLv3.

    ./launcher.py --configure     opens it

It holds no policy of its own. The fields are the keys of the frozen config
contract, one widget each, and what leaves the window is the same dict that
lives in the JSON — validated by the launcher's own `links()`, so a value the
window accepts is a value the rig accepts. The one piece of logic here is
turning field text back into numbers, and `parse()` keeps it out of the
widgets so `launcher.py --selftest` can check it without a display.

tkinter rather than anything heavier because it is in the standard library on
every platform this will ever run on, and eight fields do not need more.
"""

RCA_CHOICES = ("timecode:A", "timecode:B", "output", "none")
NAMES = ("DVS Controller", "Numark V7 MIDI")

# (key path, label, hint). Order is the window's order.
FIELDS = (
    ("rca.pair1", "RCA pair 1", "what the first pair of RCAs carries"),
    ("rca.pair2", "RCA pair 2",
     "both output = app audio on the RCAs; both none = MIDI only, RCAs silent"),
    ("midi_name", "MIDI port name",
     "what the DJ app sees; Numark V7 MIDI makes VirtualDJ drive the V7 itself"),
    ("level", "Level", "0.5 measured; 1.0 clips the timecode and the music"),
    ("counts_per_rev", "Platter counts/rev", "3600 measured with the motor"),
    ("pitch.range", "Pitch range", "0.08 vinyl, 0.16, 0.50; negative inverts"),
    ("pitch.center", "Pitch centre", "learn it: ./launcher.py --calibrate"),
    ("pitch.zero", "Pitch-zero LED", "tolerance in pitch; 0.0005 is 0.3 mm"),
)
INTS = ("counts_per_rev", "pitch.center")


def flatten(cfg):
    """Config -> {key path: text}, for the widgets."""
    return {"rca.pair1": cfg["rca"]["pair1"], "rca.pair2": cfg["rca"]["pair2"],
            "midi_name": cfg["midi_name"], "level": str(cfg["level"]),
            "counts_per_rev": str(cfg["counts_per_rev"]),
            "pitch.range": str(cfg["pitch"]["range"]),
            "pitch.center": str(cfg["pitch"]["center"]),
            "pitch.zero": str(cfg["pitch"]["zero"])}


def parse(text):
    """{key path: text} -> config dict. Raises ValueError naming the field."""
    out = {"rca": {}, "pitch": {}}
    for key, _, _ in FIELDS:
        raw = text[key].strip()
        if key.startswith("rca.") or key == "midi_name":
            if not raw:
                raise ValueError(f"{key}: empty")
            value = raw
        else:
            try:
                value = int(raw) if key in INTS else float(raw)
            except ValueError:
                raise ValueError(f"{key}: {raw!r} is not a number") from None
        head, _, tail = key.partition(".")
        if tail:
            out[head][tail] = value
        else:
            out[head] = value
    return out


def edit(cfg, validate, title="v7dvs"):
    """Show the window. Returns (cfg, start_now) or None if cancelled."""
    import tkinter as tk
    from tkinter import messagebox, ttk

    result = []
    root = tk.Tk()
    root.title(title)
    frame = ttk.Frame(root, padding=10)
    frame.grid(sticky="nsew")
    pad = {"padx": 6, "pady": 3}

    vars_ = {}
    for row, (key, label, hint) in enumerate(FIELDS):
        ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", **pad)
        var = tk.StringVar(value=flatten(cfg)[key])
        if key.startswith("rca."):
            widget = ttk.Combobox(frame, textvariable=var, values=RCA_CHOICES,
                                  state="readonly", width=16)
        elif key == "midi_name":
            widget = ttk.Combobox(frame, textvariable=var, values=NAMES, width=16)
        else:
            widget = ttk.Entry(frame, textvariable=var, width=18)
        widget.grid(row=row, column=1, sticky="we", **pad)
        ttk.Label(frame, text=hint, foreground="gray40").grid(
            row=row, column=2, sticky="w", **pad)
        vars_[key] = var

    def leave(start):
        try:
            out = parse({k: v.get() for k, v in vars_.items()})
            validate(out)
        except ValueError as err:
            messagebox.showerror(title, str(err))
            return
        result.append((out, start))
        root.destroy()

    actions = ttk.Frame(frame)
    actions.grid(row=len(FIELDS), column=0, columnspan=3, sticky="e",
                 pady=(10, 0))
    ttk.Button(actions, text="Cancel", command=root.destroy).grid(row=0, column=0)
    ttk.Button(actions, text="Save", command=lambda: leave(False)).grid(
        row=0, column=1, padx=6)
    ttk.Button(actions, text="Save and start", command=lambda: leave(True)).grid(
        row=0, column=2)

    root.mainloop()
    return result[0] if result else None


def running(title, status, alive, poll_ms=500):
    """The window that exists while the rig runs: one line and a Stop button.

    Returns when Stop is pressed, the window is closed, Cmd-Q is pressed, or
    alive() turns false; the caller stops its children either way. Nothing
    here knows what a child is. Started from a .app there is no terminal and
    nothing to Ctrl-C, so without this the rig could only be stopped from
    Activity Monitor.
    """
    import tkinter as tk
    from tkinter import ttk

    root = tk.Tk()
    root.title(title)
    # On Aqua, Cmd-Q runs ::tk::mac::Quit, which by default exits the process
    # -- past the launcher's cleanup, leaving v7d holding the V7. Make it a
    # close instead. Harmless on any other Tk.
    root.createcommand("::tk::mac::Quit", root.destroy)
    frame = ttk.Frame(root, padding=14)
    frame.grid()
    ttk.Label(frame, text=status).grid(row=0, column=0, padx=6, pady=(0, 10))
    ttk.Button(frame, text="Stop", command=root.destroy).grid(row=1, column=0)

    def watch():
        if not alive():
            root.destroy()
        else:
            root.after(poll_ms, watch)

    root.after(poll_ms, watch)
    root.mainloop()

