INSTALLATION 08 FOR PS5

Installation 08 runs the games of Halo: The Master Chief Collection offline, on the PS5,
from your own copy of MCC. This app has everything it needs except the MCC files.

You need a jailbroken PS5 with ShadowMountPlus (to install homebrew from
/data/homebrew) and an FTP client on your PC.


1. INSTALL THE APP

Upload the PPSA99908 folder to /data/homebrew on the PS5, so you have
/data/homebrew/PPSA99908/eboot.bin. ShadowMountPlus adds "Installation 08" to the home
screen.

If the app doesn't start after an FTP upload, the upload may have dropped the execute
permission: set eboot.bin and sce_module/libc.prx to 755 (most FTP clients have a
"permissions" or "chmod" option).


2. OPEN IT ONCE

Start Installation 08 from the home screen. The first start copies the app's Windows
files into /data/installation08 (a few hundred MB): the screen stays black for up to a
minute. When Installation 08 appears, close it: it has no games until step 3.
(You can also skip this step and create /data/installation08/mcc yourself.)


3. COPY YOUR MCC FOLDERS

Upload them to /data/installation08/mcc:
  halo1, halo2, halo3, halo3odst, haloreach, halo4 and groundhog (H2A multiplayer), the
  ones you want, and from the mcc folder only mcc/binaries/win64, keeping its path.

So you end up with, for example:
  /data/installation08/mcc/halo3/...
  /data/installation08/mcc/mcc/binaries/win64/...


4. PLAY

Open Installation 08. You're greeted by its splash screen and then the menu.
Quitting from the menu closes the app.


PLAYING WITH OTHERS
In any lobby, the Lobby setting decides who can join: Local (just this console), LAN
(PS5s and PCs on your network), Private (friends anywhere, with the lobby's join code)
or Public (anyone, from Browse games). To join, choose Browse games on the main menu:
it lists LAN and Public lobbies, and has Join with a code for a Private one. Every
machine needs the same version of Installation 08. Online play needs nothing set up
on your router.


WHERE THINGS ARE

/data/installation08/prefixes/installation08/drive_c/Games/Installation08
    installation08.exe, its settings (installation08.ini), log, saves and results
/data/installation08/mcc        your MCC files
/data/installation08/logs       the app's own logs, if something goes wrong

UPDATING INSTALLATION 08
Replace installation08.exe in the folder above. Your settings and saves stay.

REINSTALLING
The app only copies files that are missing, so to reset a broken setup delete
/data/installation08/prefixes (keep mcc) and open the app again.

COMING FROM PROSPERO WIN
This app is separate from Prospero Win and doesn't touch /data/prospero-win. To keep
your settings and saves, copy installation08.ini, saves, profiles and config from
/data/prospero-win/prefixes/installation08/drive_c/Games/Installation08 into the
folder above after step 2. You can move your MCC folders from /data/prospero-win/mcc
to /data/installation08/mcc instead of uploading them again.


ABOUT THIS APP
Built on Prospero Win (Wine for the PS5) by mpereiraesaa and contributors, with Wine,
DXVK, Mesa and the other parts listed in THIRD_PARTY.md and SOURCES.txt, under their own
licences (LICENSES). The changes to Prospero Win are in installation08-app.patch.
Not affiliated with or endorsed by Microsoft, 343 Industries or Halo Studios.
