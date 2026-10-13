INSTALLATION 08  -  version 1.1
The Master Chief Collection, offline.

Installation 08 is a small launcher that runs the games in Halo: The Master Chief
Collection on their own, without MCC's own launcher, Steam or Easy Anti-Cheat. It is
made for playing offline on a PC, and on a jailbroken PS5 through prospero-win.

It does not include any game files. You need your own copy of MCC.


WHAT IT DOES
  Campaign      Halo CE, Halo 2, Halo 3, ODST, Reach and Halo 4. Continue from your
                last save, pick any mission and difficulty, Anniversary or classic
                graphics in Halo CE and Halo 2 (Back switches in the game).
  Firefight     Reach and ODST, with every game type MCC ships.
  Spartan Ops   Halo 4's 50 missions.
  Custom games  Halo CE, Halo 2, Halo 3, Reach and Halo 4: every map, the stock game
                types, MCC's whole matchmaking set and your own game types (see below).
  Forge         Halo 3, Reach and Halo 4.
  Split-screen  Up to 4 players on one screen in all of the above (Players on each
                mode's page). Halo 2 split-screen uses the classic graphics.
  Controls      Per game: remap the controller and the keyboard and mouse.
  Settings      Volume, mouse, invert look, field of view, PlayStation or Xbox button
                names, resolution and fullscreen or window.

There is no online play. Everything is local.


SETUP (PC)
  1. Copy installation08.exe into your MCC folder, next to the halo1, halo2, halo3 ...
     folders. For Steam that is usually:
       C:\Program Files (x86)\Steam\steamapps\common\Halo The Master Chief Collection
  2. Run installation08.exe.
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
  For a detailed log, add the line  verbose_log = 1  to installation08.ini.


KNOWN LIMITS
  - Returning from a game to the main menu can take several seconds while the game
    shuts down.
  - Halo 4's custom loadouts are not set up, so Spartan Ops and loadout game types
    start with DMR and DMR.
  - Resolution and display changes take effect the next time it starts.
  - The in-game button prompts are the games' own (Xbox).
  - The loading screen's bar is an estimate: the games don't report how far along
    they are, so it fills to the end when the game says the map has loaded.
  - Split-screen with two or more controllers at once is untested on the PS5.
  - Halo 2 split-screen: classic graphics only, and player 1 can't use the keyboard.
