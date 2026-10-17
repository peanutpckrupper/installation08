# Installation 08

**Halo: The Master Chief Collection on your jailbroken PS5, offline.**

Installation 08 runs MCC's six Halo games on the PS5 as its own app, built on [prospero-win](https://github.com/mpereiraesaa/prospero-win). It doesn't use MCC's own launcher, Steam or Easy Anti-Cheat. You copy the app and your own MCC game files to the console over FTP. There's nothing to build and nothing to install on your PC except an FTP program.

Installation 08 includes no game files. You need your own copy of Halo: The Master Chief Collection on Steam.

<p align="center">
  <img src="docs/images/splash.png" width="49%"> <img src="docs/images/menu.png" width="49%">
</p>

## Download

Get the latest from [Releases](../../releases):

|File|For|
|-|-|
|`Installation08-PS5-<version>.zip`|**The PS5 app** (PPSA99908): everything except your game files|
|`Installation08-<version>.zip`|`installation08.exe` for a Windows PC, to play LAN games with a PS5 (or on the PC itself)|

## What you can play

|||
|-|-|
|**Campaign**|Halo CE, Halo 2, Halo 3, ODST, Reach and Halo 4. Continue from your last save, or pick any mission and difficulty. Anniversary or classic graphics in Halo CE and Halo 2|
|**Firefight**|Reach and ODST, with every game type MCC ships|
|**Spartan Ops**|All 50 Halo 4 missions|
|**Custom games**|Halo CE, Halo 2, Halo 2: Anniversary multiplayer, Halo 3, Reach and Halo 4: every map, the stock and matchmaking game types, and your own, sorted by team size (4v4, 8v8 …) or kind of game|
|**Forge**|Halo 3, H2A, Reach and Halo 4. Your maps are saved and playable in custom games|
|**LAN**|Campaign co-op, Spartan Ops, Firefight, custom games and Forge with other PS5s and PCs on your network: set a lobby to LAN, and others join from Browse games|
|**Online**|The same with players anywhere: set a lobby to Private (friends join with its code) or Public (listed in Browse games)|
|**Your Spartan**|Armor, colors and service tag per game, and Halo 4 loadouts|
|**Settings**|MCC's own Controls, Video, Audio and Accessibility settings, and per-game button remapping for the DualSense|
|**Split-screen**|Up to 4 players|

## Setup on the PS5

You need:

* a jailbroken PS5 with ShadowMountPlus (to install homebrew from `/data/homebrew`);
* an FTP server running on the console;
* [FileZilla](https://filezilla-project.org) (or any FTP program) on your PC;
* MCC installed through Steam on your PC.

Then:

1. Copy the `PPSA99908` folder from `Installation08-PS5-<version>.zip` into `/data/homebrew/` on the PS5. **Installation 08** appears on the home screen.
2. Open it once. The first start sets itself up in `/data/installation08` (up to a minute of black screen).
3. Copy your MCC game folders, and `mcc/binaries/win64`, into `/data/installation08/mcc/`.
4. Open Installation 08. The first time, it asks for your player name.

[README-PS5.txt](ps5/README-PS5.txt) has the details, including updating and moving over from prospero-win.

Already playing through prospero-win? That still works: replace `installation08.exe` in its prefix with the 1.3 one.

## LAN games: PS5 and PC together

PS5s and PCs running Installation 08 play together on the same network. To add a PC:

1. On the PC, copy `installation08.exe` (from `Installation08-<version>.zip`) into your MCC folder, next to `halo1`, `halo2`, `halo3` … For Steam that's usually `C:\\\\Program Files (x86)\\\\Steam\\\\steamapps\\\\common\\\\Halo The Master Chief Collection`. Then run it.
2. **To host:** on any machine, open a lobby from the main menu (**Campaign**, **Multiplayer**, **Firefight**, **Spartan Ops** or **Forge**) and set **Lobby** to **LAN**.
3. **To join:** on the other machines, choose **Browse games** on the main menu and pick the host's lobby, or use **Join by IP address**.
4. The lobby shows each player as Ready, or what they're missing. For example, campaign co-op needs that game's campaign installed on every machine. When everyone is ready, the host presses **Start**. After the game, everyone comes back to the lobby.

Good to know:

* **Versions:** every machine needs the same version of Installation 08.
* **Firewall:** LAN play uses UDP port 27080. On a PC, allow it through Windows' firewall on private networks when asked.
* **PS5 with a PC:** the PS5 package includes Microsoft's `ucrtbase.dll`, which keeps them in step. The PS5's Wine does some of the games' math very slightly differently from Windows, which is enough for a PS5 and a PC to drift out of sync in a co-op game.
* **Windows Defender** may be wary of a new, unsigned program that loads game files. If it flags the exe, add an exclusion. More in the [PC README](dist/README.txt).

## Online games

Every lobby has a **Lobby** setting:

|Setting|Who can join|
|-|-|
|**Local**|Nobody: just this machine (split-screen included)|
|**LAN**|Players on your network, from **Browse games**|
|**Private**|Friends anywhere, with the lobby's **join code** (such as `C3CS-BAR1-U28W-QMRH`), from **Browse games** > **Join with a code**|
|**Public**|Anyone: the lobby is listed in **Browse games**|

There's nothing to set up: no router settings, no port forwarding. Every machine only connects out, to the Installation 08 online server, which introduces the players so they can connect directly, and passes their packets on when they can't (some mobile networks, VPNs and strict routers).

Good to know:

* **Security:** every packet between machines is encrypted and authenticated, end to end. For a Private lobby the server never sees the join code and can't join; a Public lobby gives the server its code, so anyone browsing can join. Nobody outside your network can join a LAN lobby. A Private lobby's code is new each time it's set to Private.
* **Trust:** players who connect directly can see each other's IP addresses, and the games' own network code was written for Xbox Live. Public lobbies are open to strangers: Private is the safer choice for playing with friends.
* **Co-op timing:** in campaign, Spartan Ops and Firefight the host runs the game and sends every update to the others, so joined players always play a little behind the host. Installation 08 evens out when those updates arrive, which keeps joined players' games smooth on an uneven connection; the delay it adds adjusts to the connection (about 10 ms at home, up to about 50 ms on mobile data). A steady connection on the host's side gives everyone the best game.
* **Upload:** the host needs about 100 kbit/s of upload for each player who joins (an 8v8 game: about 1.5 Mbit/s).
* **Versions:** every machine needs the same version of Installation 08, as for LAN games.

## Known limits

* LAN co-op is tested in all six games, with a PS5 or a PC hosting. Online play is new: tested between a PS5 at home and a PC on a phone's mobile data, with either one hosting.
* LAN co-op starts from the beginning of a mission. The games don't continue a network co-op game from a checkpoint.
* A game type's own options (starting weapons and so on) can't be edited yet. Any game type file you have can be played.
* In-game button prompts are the games' own (Xbox).
* Two controllers at once for split-screen is untested on the PS5. PS4 controllers can't be used in PS5 apps.

## Building

One C++ source file and Monocypher (one C file), built with MinGW-w64 (Linux or WSL: `sudo apt-get install g++-mingw-w64-x86-64`):

```sh
cd src
x86\\\_64-w64-mingw32-windres installation08.rc -O coff -o res.o
x86\\\_64-w64-mingw32-gcc -O2 -c monocypher/monocypher.c -o monocypher.o
x86\\\_64-w64-mingw32-g++ -std=c++17 -O2 -municode -static -Wall -Wno-unused-parameter \\\\
    -o installation08.exe installation08.cpp monocypher.o res.o \\\\
    -ld3d11 -ldxgi -lgdi32 -ld2d1 -ldwrite -ladvapi32 -lws2\\\_32 -lole32 -lwindowscodecs -luuid -lbcrypt
```

The exe is left unstripped (no `-s`): stripped builds are flagged by Windows Defender far more often.

The online server's address isn't in the source: builds name it with `-DI08\_SERVER="host\[:port]"` (GitHub Actions takes it from the repository secret `I08\_SERVER`). Without it the exe is LAN only.

Every push is also built by GitHub Actions, and the exe is attached to each run.

The PS5 app is prospero-win's title built for one game. See [MAKING-THE-PS5-PACKAGE.txt](ps5/MAKING-THE-PS5-PACKAGE.txt).

## Repository

|Folder|What's in it|
|-|-|
|`src/`|The launcher's source, version resource and MCC's armor item list|
|`ps5/`|The PS5 app's files and package script (`app/`), the PS5 README, and the older prospero-win recipe|
|`dist/`|The README shipped with the PC download|

## Credits

Installation 08 is made by **peanutpckrupper**

It stands on other people's work:

* [**prospero-win**](https://github.com/mpereiraesaa/prospero-win) by mpereiraesaa and its contributors: Wine on the PS5. The PS5 app is its title, built for one game, with its Wine, Mesa and RADV builds.
* [**PS5-Lapy-JB-Daemon**](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) and the [**PS5 Native App Boilerplate**](https://github.com/mpereiraesaa/ps5-native-app-boilerplate), also by mpereiraesaa: the app's access to `/data`, and the base for building PS5 apps.
* [**The PS5 payload SDK**](https://github.com/ps5-payload-dev/sdk) by John Törnblom and the ps5-payload-dev contributors: the toolchain behind the app.
* [**Wine**](https://www.winehq.org), [**DXVK**](https://github.com/doitsujin/dxvk) and [**Mesa**](https://www.mesa3d.org): Windows, Direct3D 11 and OpenGL on top of Vulkan.
* [**Monocypher**](https://monocypher.org) by Loup Vaillant: the encryption behind LAN and online play.
* [**Titillium Web**](https://fonts.google.com/specimen/Titillium+Web), from the Accademia di Belle Arti di Urbino: the menus' font.
* **ShadowMountPlus**, which installs the app from `/data/homebrew`, and everyone behind the PS5 jailbreak that makes homebrew possible.
* **Bungie, 343 Industries and Halo Studios**, for the games.



## Third-party components

`installation08.exe` contains:

* the [Titillium Web](https://fonts.google.com/specimen/Titillium+Web) font (SIL Open Font License 1.1, [src/fonts/OFL.txt](src/fonts/OFL.txt)), for its menus;
* [Monocypher](https://monocypher.org) 4.0.2 (BSD 2-clause or CC0, [src/monocypher/LICENCE.md](src/monocypher/LICENCE.md)), for encrypting network play.

The PS5 app contains:

* [prospero-win](https://github.com/mpereiraesaa/prospero-win)'s PS5 title (LGPL 2.1 or later), built as PPSA99908 with the changes in [ps5/app/installation08-app.patch](ps5/app/installation08-app.patch): it starts Installation 08 directly and sets itself up on first start.
* prospero-win's [Wine](https://gitlab.winehq.org/wine/wine) build (LGPL 2.1 or later), Mesa (MIT) and RADV, as in a prospero-win release; their sources are listed in the app's `SOURCES.txt` and licences in `LICENSES/`.
* The [Lapy](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) helper, built for PPSA99908, which gives the app access to `/data`.
* [DXVK](https://github.com/doitsujin/dxvk) 2.6.2 (zlib licence).
* Microsoft's `ucrtbase.dll` (the Universal C Runtime), from the Windows SDK's redistributable files, deployed next to `installation08.exe` as Microsoft allows for app-local use.

## Disclaimer

Installation 08 is a fan project. It is not affiliated with or endorsed by Microsoft, Xbox Game Studios or Halo Studios. Halo and The Master Chief Collection are trademarks of Microsoft. It includes no game files. Online play connects only to the Installation 08 server, which keeps no accounts and logs no addresses.

