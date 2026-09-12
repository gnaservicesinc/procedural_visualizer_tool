"""VideoToolbox backend for Remote's existing video track (macOS 27+ only).

PyAV's Packet is only the container passed to aiortc's H264 RTP packetizer;
video compression is performed directly by Apple's system framework.
"""
import ctypes
import fractions
import os
import sys
from pathlib import Path

from av import Packet
from PIL import Image


class Encoder:
    def __init__(self):
        root = Path(getattr(sys, '_MEIPASS', Path(__file__).parent))
        path = os.environ.get('PVT_REMOTE_TEST_VIDEOTOOLBOX_LIBRARY',
                              str(root / 'pvt-videotoolbox.dylib'))
        self.library = ctypes.CDLL(path)
        self.library.pvt_video_create.argtypes = [ctypes.c_int, ctypes.c_int]
        self.library.pvt_video_create.restype = ctypes.c_void_p
        self.library.pvt_video_encode.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
            ctypes.c_size_t, ctypes.c_int64, ctypes.c_int,
            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
        self.library.pvt_video_encode.restype = ctypes.c_int
        self.library.pvt_video_destroy.argtypes = [ctypes.c_void_p]
        self.library.pvt_video_destroy.restype = None
        self.handle = None
        self.size = None
        self.keyframe_pts = None

    def encode(self, image, pts):
        image = image.convert('RGB')
        # H264 baseline level 3.1 is negotiated with browsers; stay within its
        # 720p30 limit rather than silently sending a higher-level bitstream.
        image.thumbnail((1280, 720), Image.Resampling.LANCZOS)
        size = tuple(max(2, value - value % 2) for value in image.size)
        if image.size != size:
            image = image.resize(size)
        if size != self.size:
            self.close()
            self.handle = self.library.pvt_video_create(*size)
            if not self.handle:
                raise RuntimeError('VideoToolbox could not start Remote video')
            self.size = size
        keyframe = self.keyframe_pts is None or pts - self.keyframe_pts >= 90000
        data = image.tobytes('raw', 'BGRX')
        output, length = ctypes.c_void_p(), ctypes.c_size_t()
        status = self.library.pvt_video_encode(self.handle, data, len(data), pts,
            keyframe, ctypes.byref(output), ctypes.byref(length))
        if status:
            raise RuntimeError(f'VideoToolbox could not encode Remote video ({status})')
        if keyframe:
            self.keyframe_pts = pts
        packet = Packet(ctypes.string_at(output, length.value))
        packet.pts = pts
        packet.time_base = fractions.Fraction(1, 90000)
        return packet

    def close(self):
        if self.handle:
            self.library.pvt_video_destroy(self.handle)
        self.handle = None
        self.size = None
        self.keyframe_pts = None
