"""Readable audio labels; the original device identity is kept by the caller."""

import re


def friendly_device_name(name):
    """Shorten technical ALSA descriptions; the caller keeps the original for opening."""
    lower = name.casefold().strip()

    if not lower:
        return name

    is_monitor = lower.endswith(".monitor")

    if is_monitor:
        lower = lower[: -len(".monitor")]

    def monitor(label):
        return f"Monitor {label}" if is_monitor else label

    if lower in ("default", "sysdefault", "default sink", "default source"):
        return monitor("Sistem (audio bawaan OS)")

    if lower.startswith("default alsa"):
        return monitor("Sistem (audio bawaan OS)")

    if "pipewire sound server" in lower or "pulseaudio sound server" in lower:
        return monitor("Sistem (PipeWire)")

    if lower in ("pulse", "pipewire"):
        return monitor("Sistem (PipeWire)")

    # PortAudio reports ALSA plugin devices under their bare plugin names.
    if lower in ("dmix", "dsnoop"):
        return monitor("Kartu suara (mix stereo)")
    if lower in ("upmix", "vdownmix"):
        return monitor("Kartu suara (campuran kanal)")
    if lower == "speex":
        return monitor("Mic (resample + pengurangan noise)")
    if lower in ("lavrate", "samplerate", "speexrate"):
        return monitor("Kartu suara (resampler)")

    # PulseAudio route names: alsa_output.<card>.HiFi__<Friendly Name>_sink
    if "alsa_output." in lower or "alsa_input." in lower:
        # Route names end in ".<HiFi__Speaker__sink>" and monitors add ".monitor".
        route = name
        if is_monitor:
            route = route[: -len(".monitor")]

        route = route[route.rfind(".") + 1:]

        for suffix in ("_sink", "_source"):
            if route.endswith(suffix):
                route = route[: -len(suffix)]

        parts = [part for part in route.split("__") if part.strip()]

        if not parts:
            return monitor("Sistem (PipeWire)")

        return monitor(parts[-1].replace("_", " ").strip())

    if lower.startswith("bluez_output."):
        return monitor("Speaker Bluetooth")
    if lower.startswith("bluez_input."):
        return monitor("Mic Bluetooth")

    # PortAudio exposes ALSA hardware as "<card>: - (hw:N,M)" / "<card>: HDMI 1 (hw:0,3)".
    if "(hw:" in lower:
        card = name.split(":", 1)[0].strip()
        hw = name[name.index("(hw:"):]
        return monitor(f"{card} {hw}" if card else hw)

    # "pci-0000_00_1f.3-platform-skl_hda_dsp_generic" / "usb-Generic_USB_Audio-00"
    if "platform-" in lower:
        return name.split("platform-", 1)[1].split(".")[0].split("_")[0].replace("_", " ").strip()

    card = name.split(", ;")[0].strip() if ", ;" in name else ""
    card_label = card or "Kartu suara"

    if "direct hardware device" in lower:
        return f"{card_label} (langsung)"
    if "direct sample mixing device" in lower:
        return f"{card_label} (mix stereo)"
    if "plugin for channel downmix" in lower or "plugin for channel upmix" in lower:
        return f"{card_label} (campuran kanal)"
    if "rate converter plugin" in lower:
        return f"{card_label} (resampler)"
    if "speex dsp" in lower:
        return f"{card_label} (Mic: resample + pengurangan noise)"
    if "spacialization" in lower:
        return "Kartu suara (campuran kanal)"

    return name.strip()


def device_label(name, is_input, channels):
    lower = name.casefold()
    if "monitor" in lower or "loopback" in lower:
        kind = "Loopback / monitor"
    elif is_input and ("microphone" in lower or "headset" in lower or re.search(r"\bmic\b", lower)):
        kind = "Mic"
    elif not is_input and any(word in lower for word in ("headphone", "headset", "earphone", "earbud")):
        kind = "Headphone"
    elif not is_input and "speaker" in lower:
        kind = "Speaker"
    elif is_input:
        kind = "Line in"
    else:
        kind = "Output audio"
    if any(word in lower for word in ("bluetooth", "bluez", "a2dp")):
        kind += " Bluetooth"
    elif "usb" in lower:
        kind += " USB"
    return f"{kind} — {friendly_device_name(name)} · {channels} ch"