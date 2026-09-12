"""Exercise the system encoder, using a separate decoder as the oracle."""
import sys
import unittest

import av
from PIL import Image


@unittest.skipUnless(sys.platform == 'darwin', 'Apple system video backend')
class VideoToolboxTests(unittest.TestCase):
    def test_frames_resize_and_keyframe_recovery(self):
        from pvt_remote.macos_video import Encoder
        encoder = Encoder()
        try:
            decoder = av.CodecContext.create('h264', 'r')
            for pts in (0, 3000, 6000):
                packet = encoder.encode(Image.new('RGB', (640, 360), (30, 150, 120)), pts)
                frame, = decoder.decode(packet)
                self.assertEqual((frame.width, frame.height), (640, 360))
                self.assertEqual(packet.pts, pts)
                for actual, expected in zip(frame.to_image().getpixel((300, 200)), (30, 150, 120)):
                    self.assertLessEqual(abs(actual - expected), 5)
            # A receiver that lost earlier frames can restart on the periodic IDR.
            packet = encoder.encode(Image.new('RGB', (640, 360)), 90000)
            self.assertEqual(len(av.CodecContext.create('h264', 'r').decode(packet)), 1)
            # Size changes recreate the native session and start with a keyframe.
            packet = encoder.encode(Image.new('RGB', (1921, 1081)), 93000)
            frame, = av.CodecContext.create('h264', 'r').decode(packet)
            self.assertLessEqual(frame.width, 1280)
            self.assertLessEqual(frame.height, 720)
            self.assertEqual((frame.width % 2, frame.height % 2), (0, 0))
        finally:
            encoder.close()
            encoder.close()
