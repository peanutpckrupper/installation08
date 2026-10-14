INSTALLATION 08 FOR PC  -  version 1.2
The Master Chief Collection, offline.

Installation 08 runs the games in Halo: The Master Chief Collection on their own,
without MCC's own launcher, Steam or Easy Anti-Cheat. It is made for jailbroken PS5s
(through prospero-win; see the PS5 package and its README-PS5.txt).

This is the PC version: the same program, for playing LAN games with a PS5 (see LAN
GAMES below), or for playing on the PC itself.

It does not include any game files. You need your own copy of MCC.


WHAT IT DOES
  Campaign      Halo CE, Halo 2, Halo 3, ODST, Reach and Halo 4. Continue from your
                last save, pick any mission and difficulty, Anniversary or classic
                graphics in Halo CE and Halo 2 (Back switches in the game).
  LAN co-op     Campaign co-op for up to 4 players on your network (see LAN GAMES
                below).
  Firefight     Reach and ODST, with every game type MCC ships.
  Spartan Ops   Halo 4's 50 missions.
  Custom games  Halo CE, Halo 2, Halo 3, Reach and Halo 4: every map, the stock game
                types, MCC's whole matchmaking set and your own game types (see below).
  Forge         Halo 3, Reach and Halo 4, and your maps are saved (see below).
  LAN lobby     Host a game for your network: campaign co-op, Spartan Ops,
                Firefight, custom games and Forge.
  Your Spartan  Armor, colors and service tag per game; Halo 4 loadouts.
  Split-screen  Up to 4 players on one screen in all of the above (Players on each
                mode's page). Halo 2 split-screen uses the classic graphics.
  Controls      Per game: remap the controller and the keyboard and mouse.
  Settings      MCC's own settings, in Controls, Video, Audio and Accessibility:
                look sensitivity, dead zones, invert, crouch and zoom behavior,
                field of view, graphics quality, frame rate limit, HUD scale,
                volumes, subtitles, color blind modes and more.

There is no online play. Everything is local, or on your own network.


SETUP (PC)
  1. Copy installation08.exe into your MCC folder, next to the halo1, halo2, halo3 ...
     folders. For Steam that is usually:
       C:\Program Files (x86)\Steam\steamapps\common\Halo The Master Chief Collection
  2. Run installation08.exe. The first time, it asks for your player name (the name
     in the games and in LAN lobbies); Settings > Your name changes it later.
  (Or put it anywhere and start it with --mcc "<your MCC folder>".)

  Windows Defender may be wary of a new, unsigned program that loads game files. If it
  flags it, add an exclusion for the file or the folder.


SETUP (PS5, prospero-win)
  On the PS5, Installation 08 runs inside prospero-win. Use the PS5 package
  (Installation08-PS5.zip) and follow its README-PS5.txt: you copy the package and
  your MCC game folders to the console over FTP, and nothing else.


CONTROLS
  Menus: A / Enter select, B / Esc back, Start / Esc pauses in a game.
  Split-screen: each player needs a controller; or set "Player 1 uses" to Keyboard and
  mouse so player 1 plays on the keyboard and controller 1 goes to player 2.
  (Halo 2 split-screen: controllers only for now.)
  PS5: split-screen works with keyboard and mouse as player 1 and a DualSense as
  player 2. Two DualSense controllers at once is untested (only one was available).
  The PS5 does not allow PS4 (DualShock 4) controllers in PS5 apps, so they can't
  be used.


LAN GAMES
  Everything happens in LAN games on the main menu.
  To host: choose Host a game. That opens your lobby, where you pick the game, the
  mode (Campaign, Firefight, Spartan Ops, Custom game or Forge) and its options
  (mission and difficulty, or map and game type). Others see your lobby change as
  you choose.
  To join: your network's games are listed under LAN games; pick one. Or use Join
  by IP address.
  The lobby shows each player as Ready, or what they're missing (the game, or
  that map's part of it, for example a campaign that isn't installed). The host
  can start without players who can't play it; they stay in the lobby.
  After a game, everyone comes back to the lobby, still joined.
  The host's game type and saved Forge map are sent to everyone, so nobody needs
  the same files. Everyone needs the game (and, for co-op, its campaign) installed.
  Campaign co-op and Spartan Ops take up to 4 players.
  Co-op starts from the beginning of a mission: co-op checkpoints can't be
  continued in a later session. The host's pause menu has End co-op.
  With a PS5: the PS5 setup copies Windows' ucrtbase.dll to the PS5, which keeps
  its math the same as the PC's (without it, co-op games drift out of sync).
  LAN play uses UDP port 27080. Windows asks to allow it through the firewall the
  first time; allow it on private networks.
  Tested between two PCs, and between a PS5 and a PC with either one hosting.


FORGE MAPS
  In Forge, pause and choose Save map. The first save makes a new map variant;
  after that Save map saves over it, and Save as a new map makes another copy.
  Saved maps are in map_variants\<game>\<map>\ next to the exe, and show up under
  Map variant on the Custom game and Forge pages when that map is chosen. To rename
  one, rename its .mvar file.


ARMOR, COLORS AND LOADOUTS
  On a game's page, Armor and colors sets your armor pieces, colors, visor and
  (Halo 2, Halo 3, Reach) Spartan or Elite for that game. The service tag (up to
  four letters and numbers) is shared by every game. Halo CE, Halo 2 and ODST have
  colors only.
  Halo 4's page also has Loadouts: five loadouts for game types that allow your own.
  Anything that isn't set keeps the game's own default.


YOUR OWN GAME TYPES
  Copy game type files (.bin) you made in MCC into a game_types folder next to the
  exe, one folder per game, named like the game's MCC folder:
    game_types\haloreach\   game_types\halo3\   game_types\halo4\   ...
  MCC keeps them in %USERPROFILE%\AppData\LocalLow\MCC\LocalFiles\<id>\<game>\GameType.
  They show up under "Your game types" in Custom game.


FILES IT WRITES (next to the exe)
  installation08.ini   your settings
  installation08.log   what happened last time it ran (attach this to a bug report)
  saves\               campaign saves, one per game
  map_variants\        your saved Forge maps
  profiles\            settings each game keeps for itself (changed in its own menus)
  screenshots\         F12 in a game saves a screenshot here
  lan_game_types\      game types and maps a LAN host sent you
  lan_map_variants\
  For a detailed log, add the line  verbose_log = 1  to installation08.ini.


KNOWN LIMITS
  - Returning from a game to the main menu can take several seconds while the game
    shuts down.
  - LAN co-op is tested in Halo 2, Halo 3 and Halo 4; Halo CE, ODST and Reach
    co-op are untested.
  - LAN co-op starts from the beginning of a mission. The games don't continue a
    network co-op game from a checkpoint (Halo 3 was tried, and either goes
    offline or ignores the save).
  - Graphics quality changes take effect from the next game you start.
  - A game type's own options (starting weapons and so on) can't be edited here
    yet; any game type file you have can be played.
  - Resolution and display changes take effect the next time it starts.
  - The in-game button prompts are the games' own (Xbox).
  - The loading screen's bar is an estimate: the games don't report how far along
    they are, so it fills to the end when the game says the map has loaded.
  - Split-screen with two or more controllers at once is untested on the PS5.
  - Halo 2 split-screen: classic graphics only, and player 1 can't use the keyboard.
