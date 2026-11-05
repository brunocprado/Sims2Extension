#!/usr/bin/env python3
"""Boots The Sims 2 on the private Xvfb display until a lot is running (no oracle plugin needed).

Reuses the menu navigation of TheSimsRust/tools/ts2boot.py but decides "lot ready" by elapsed time after the
lot card was confirmed (the post-fx plugin does not write the oracle events file).
"""
import os
import sys
import time

TOOLS = os.environ.get("TS2_TOOLS", "/home/bruno/PROJETOS/TheSimsRust/tools")
sys.path.insert(0, TOOLS)
import ts2boot  # noqa: E402


class FxBoot(ts2boot.Boot):
    entered = None

    @staticmethod
    def chooser(im):
        # ts2boot's threshold (0.22) is slightly too strict for the blurred backdrop at 1280x1024
        sub = im[270:700, 250:1030]
        r, g, b = sub[..., 0], sub[..., 1], sub[..., 2]
        frac = ((b > 190) & (r > 110) & (r < 200) & (g > 110) & (g < 200)).mean()
        return frac > float(os.environ.get("CHOOSER_FRAC", "0.12"))

    def say(self, msg):
        super().say(msg)
        if msg == "entering lot":
            self.entered = time.time()

    def ready(self):
        return self.entered is not None and time.time() - self.entered > float(os.environ.get("LOT_WAIT", "70"))


if __name__ == "__main__":
    disp = os.environ.get("DISPLAY", ":99")
    FxBoot(disp, None, (637, 500), int(os.environ.get("BOOT_LOT_PICK", "0")), 900, "/tmp/ts2boot").run()
