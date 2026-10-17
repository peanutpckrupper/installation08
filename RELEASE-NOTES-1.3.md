## Installation 08 1.3

Halo: The Master Chief Collection on your jailbroken PS5. Installation 08 is now its own PS5 app: install it, copy your MCC folders over, and open it from the home screen. Prospero Win isn't needed any more. And it plays online: host from home, and friends join from anywhere.

**New**
- **A PS5 app.** `Installation08-PS5-1.3.zip` holds `PPSA99908`, an app for ShadowMountPlus that starts straight into Installation 08. It brings its own copy of Wine and sets itself up in `/data/installation08` the first time it opens. MCC goes in `/data/installation08/mcc`. Quitting returns to the home screen.
- **Online play.** Set a lobby to **Private** and friends anywhere join with its join code (Browse games > Join with a code), or set it to **Public** and it's listed in Browse games for anyone. Campaign co-op, Spartan Ops, Firefight, custom games and Forge, between PS5s and PCs. Nothing to set up on your router: the Installation 08 server introduces the players and passes their traffic on when they can't connect directly. Every packet is encrypted end to end, and the server never sees a Private lobby's code. Tested with a PS5 at home and a PC on a phone's mobile data, each hosting.
- **Smooth co-op for joined players.** In campaign, Spartan Ops and Firefight (every game but Halo CE, which works differently), a joined machine now evens out the timing of the host's updates instead of playing them the moment they land. Online co-op no longer stutters for the players who join, and LAN co-op is smoother too. It costs the joined players a little extra delay, which adjusts to the connection: about 10 ms at home, up to about 50 ms over a phone's mobile data.
- **Halo 2: Anniversary multiplayer.** Custom games and Forge on every H2A map, with its classic and Forge layouts.
- **Lobbies for every mode.** Campaign, Multiplayer, Firefight, Spartan Ops and Forge each open a lobby. Set **Lobby** to **LAN** and others on your network join from **Browse games** on the main menu. This replaces the LAN games menu.
- **Custom games sorted** by team size (4v4, 8v8 …) or kind of game.
- **LAN play is encrypted** too, and only machines on your own network can join a LAN lobby.
- **Three or more machines** in one LAN game work reliably. Tested with a PS5 and two PCs.

**Fixes**
- On the PS5, choosing Return to main menu during a LAN game of H2A no longer crashes.
- On the PS5, returning to the menu after a game with three or more players no longer crashes.
- A LAN game no longer fails to start on a machine that missed the host's start message.
- A player who leaves a LAN game stops sending game traffic straight away.
- The quit option says where it goes: home screen (PS5 app), Prospero Win, or desktop.

**Changed**
- Installation 08 is played with controllers. A keyboard still works in its own menus, but keyboard and mouse play in the games is gone.
- The PS5 package includes Microsoft's `ucrtbase.dll`, so there's no file to copy from Windows any more.

**Coming from 1.2 on Prospero Win**
- Either install the PS5 app (README-PS5.txt, "Coming from Prospero Win", covers moving your settings, saves and MCC folders), or keep using Prospero Win and replace `installation08.exe` in its prefix with the 1.3 one.

**Known limits**
- LAN co-op starts from the beginning of a mission. The games don't continue a network co-op game from a checkpoint.
- A game type's own options (starting weapons and so on) can't be edited yet.
- Online play is new. Players who connect directly can see each other's IP addresses, so Private is the safer choice with friends; Public lobbies are open to anyone.

**Downloads**
- `Installation08-PS5-1.3.zip`: the PS5 app. Unzip it and copy `PPSA99908` to `/data/homebrew` on the PS5.
- `Installation08-1.3.zip`: `installation08.exe` for a Windows PC, to play LAN games with a PS5, or on the PC itself.

Every machine needs 1.3, for LAN and online games. No game files are included, so you need your own copy of Halo: The Master Chief Collection.
The PS5 app contains prospero-win's title and Wine runtime (LGPL 2.1+, source at github.com/mpereiraesaa/prospero-win; the app's changes are in its `installation08-app.patch`), Mesa (MIT), DXVK 2.6.2 (zlib) and the Lapy helper. `SOURCES.txt` and `LICENSES/` in the app list them all.
