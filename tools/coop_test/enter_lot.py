#!/usr/bin/env python3
"""Drives the menus of one game instance (DISPLAY env) into the SAME household's lot every time.

Household choice = the right-most plumbob of the Desiderata Valley neighborhood view ("Jocque"), so two
instances always land on the same lot. Based on TheSimsRust/tools/ts2boot.py (xinput helpers).
"""
import os
import sys
import time

TOOLS = os.environ.get("TS2_TOOLS", "/home/bruno/PROJETOS/TheSimsRust/tools")
sys.path.insert(0, TOOLS)
import ts2boot  # noqa: E402
import xinput   # noqa: E402


class LotBoot(ts2boot.Boot):
    @staticmethod
    def chooser(im):
        sub = im[270:700, 250:1030]
        r, g, b = sub[..., 0], sub[..., 1], sub[..., 2]
        frac = ((b > 190) & (r > 110) & (r < 200) & (g > 110) & (g < 200)).mean()
        return frac > float(os.environ.get("CHOOSER_FRAC", "0.12"))

    def plumbobs_stable(self, tries=12):
        for _ in range(tries):
            pbs = xinput.find_plumbobs(self.disp)
            if len(pbs) >= 2:
                return pbs
            time.sleep(1)
        return []

    def ready(self):
        return False

    def go(self):
        if not os.environ.get("SKIP_HOOD"):
            self.act("key", "F10")  # the plugin panel is visible on start and would swallow clicks
            self.say("waiting for chooser")
            self.wait(self.chooser, "neighborhood chooser", press="Escape", every=4)
            time.sleep(4)
            self.act("move", "637", "500")
            time.sleep(1)
            self.act("click", "637", "500")
            time.sleep(4)
            self.act("click", "599", "745")
            self.say("loading neighborhood")
            time.sleep(20)
        t0 = time.time()
        while True:
            self.check_time()
            time.sleep(4)
            im = self.shot()
            if (os.environ.get("SKIP_HOOD") or not self.chooser(im)) and len(xinput.find_plumbobs(self.disp)) >= 2:
                break
            if time.time() - t0 > 240:
                sys.exit("neighborhood never showed plumbobs")
        self.say("neighborhood view")
        time.sleep(3)
        self.dismiss_dialogs()
        time.sleep(8)  # let the camera finish its intro pan
        for attempt in range(8):
            self.check_time()
            pbs = self.plumbobs_stable()
            if not pbs:
                continue
            x, y = max(pbs, key=lambda p: p[0])
            self.say(f"plumbobs={len(pbs)} picking right-most at {x},{y}")
            self.act("move", str(x - 8), str(y + 55))
            time.sleep(1)
            self.act("click", str(x), str(y + 55))
            time.sleep(3)
            if self.has_play(self.shot()):
                self.dump("lotcard")
                self.act("play")
                self.say("entering lot")
                return
            self.dismiss_dialogs()
            self.act("key", "Escape")
            time.sleep(2)
        sys.exit("could not open the lot card")


if __name__ == "__main__":
    disp = os.environ.get("DISPLAY", ":99")
    tag = disp.replace(":", "d")
    LotBoot(disp, None, (637, 500), 0, 900, f"/tmp/ts2boot_{tag}").go()
