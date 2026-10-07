#!/usr/bin/env python3
"""Validate UVC camera and USB microphone dependencies after Linux olddefconfig."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("config", type=Path, help="Resolved Linux .config")
arguments = parser.parse_args()
lines = set(arguments.config.read_text().splitlines())
required = (
    "MEDIA_SUPPORT", "MEDIA_SUPPORT_FILTER", "MEDIA_CAMERA_SUPPORT", "MEDIA_USB_SUPPORT",
    "VIDEO_DEV", "MEDIA_CONTROLLER", "USB_VIDEO_CLASS", "USB_VIDEO_CLASS_INPUT_EVDEV",
    "UVC_COMMON", "VIDEOBUF2_CORE", "VIDEOBUF2_V4L2", "VIDEOBUF2_MEMOPS", "VIDEOBUF2_VMALLOC",
    "SND_USB", "SND_USB_AUDIO", "SND_PCM", "SND_HWDEP", "SND_RAWMIDI", "BITREVERSE",
    "SND_USB_AUDIO_USE_MEDIA_CONTROLLER",
)
disabled = (
    "MEDIA_SUBDRV_AUTOSELECT", "MEDIA_ANALOG_TV_SUPPORT", "MEDIA_DIGITAL_TV_SUPPORT",
    "MEDIA_RADIO_SUPPORT", "MEDIA_SDR_SUPPORT", "MEDIA_PLATFORM_SUPPORT", "MEDIA_TEST_SUPPORT",
    "MEDIA_PCI_SUPPORT",
    "SND_USB_AUDIO_MIDI_V2",
)
missing = [f"CONFIG_{name}=y" for name in required if f"CONFIG_{name}=y" not in lines]
missing += [f"# CONFIG_{name} is not set" for name in disabled
            if f"# CONFIG_{name} is not set" not in lines]
if missing:
    parser.exit(1, "USB camera configuration did not survive olddefconfig:\n" + "\n".join(missing) + "\n")
print("USB UVC video, USB microphone audio and their dependencies are built into the kernel.")
