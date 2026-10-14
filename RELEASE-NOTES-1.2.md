## Installation 08 1.2

Halo: The Master Chief Collection on your jailbroken PS5. This release adds LAN play, so PS5s and PCs on the same network play together, campaign co-op included.

**New**
- **LAN campaign co-op** for up to 4 players on your network, plus Spartan Ops co-op in Halo 4. Tested in Halo 2, Halo 3 and Halo 4, between PCs and between a PS5 and a PC with either one hosting. Halo CE, ODST and Reach co-op are untested.
- **A LAN lobby.** Choose Host a game under LAN games, then pick the game, mode and options (mission and difficulty, or map and game type). Players see each change and whether they can play it: Ready, or which game or map is missing. The host can start without players who can't play. After a game everyone returns to the lobby, still joined.
- **LAN custom games, Forge and Firefight** in every game that has them. The host's game type and Forge map are sent to everyone.
- **Your player name.** Asked the first time Installation 08 starts, with an on-screen keyboard for controllers (Square / X deletes). It's your name in the games and in LAN lobbies, and shows at the top right of the menus. Settings > Your name changes it.
- **MCC's own settings,** in Controls, Video, Audio and Accessibility: look sensitivity and acceleration, dead zones, flight inversion, vibration, toggle or hold crouch and zoom, vehicle FOV, crosshair position, HUD scale, frame rate limit (60 to unlimited), graphics quality, anti-aliasing, motion blur, Spartan voice, subtitles, and color blind modes.
- **Forge maps are saved.** Save map and Save as a new map are in the Forge pause menu, and there's a Map variant choice on the Custom game and Forge pages.
- **Armor and colors:** armor pieces, colors, visor and Spartan or Elite per game, and a service tag for every game.
- **Halo 4 loadouts:** five loadouts with every weapon, armor ability, tactical package and support upgrade, for game types that allow your own.
- **Settings changed in a game's own menus** are kept.
- **F12** in a game saves a screenshot to the screenshots folder.
- **A new main menu,** with one tile per game, and a splash screen at start.

**Fixes**
- Halo CE and Halo 2 now use high graphics quality. Before this they ran at the lowest settings.
- Starting a game after quitting one from its pause menu no longer freezes.
- Halo 2 no longer hangs when a game ends by itself (for example leaving from its own pause menu).
- On the PS5, returning to the menu after a game no longer crashes.
- Split-screen view settings for the games that ask the host for them.

**New PS5 setup step: one file from Windows.** Copy `C:\Windows\System32\ucrtbase.dll` from your PC next to `installation08.exe` on the PS5 (README-PS5.txt, step 2.4). The 1.2 profile requires it, and Installation 08 won't start without it. It keeps a PS5 and a PC in step in LAN games: the PS5's Wine does some of the games' math very slightly differently from Windows, enough for co-op between them to drift out of sync.

**Known limits**
- LAN co-op starts from the beginning of a mission. The games don't continue a network co-op game from a checkpoint.
- A game type's own options (starting weapons and so on) can't be edited yet.

**Downloads**
- `Installation08-PS5.zip`: the PS5 package for prospero-win. Already on 1.1? Replace `installation08.exe`, copy in `ucrtbase.dll` and change one profile line (README-PS5.txt, "Updating").
- `Installation08-1.2.zip`: `installation08.exe` for a Windows PC, to play LAN games with a PS5, or on the PC itself.

LAN machines all need 1.2. No game files are included, so you need your own copy of Halo: The Master Chief Collection.
The PS5 package contains Wine and prospero-win files (LGPL 2.1+, source at github.com/mpereiraesaa/prospero-win) and DXVK 2.6.2 (zlib).
