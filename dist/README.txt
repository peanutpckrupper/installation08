INSTALLATION 08 FOR PC  -  version 1.3
The Master Chief Collection, offline.

Installation 08 runs the games in Halo: The Master Chief Collection on their own,
without MCC's own launcher, Steam or Easy Anti-Cheat. It is made for jailbroken PS5s
(as its own PS5 app; see the PS5 package and its README.txt).

This is the PC version: the same program, for playing LAN games with a PS5 (see LAN
GAMES below), or for playing on the PC itself.

It does not include any game files. You need your own copy of MCC.


WHAT IT DOES
  Campaign      Halo CE, Halo 2, Halo 3, ODST, Reach and Halo 4. Continue from your
                last save, pick any mission and difficulty, Anniversary or classic
                graphics in Halo CE and Halo 2 (Back switches in the game).
  LAN co-op     Campaign co-op for up to 4 players on your network (see LAN PLAY
                below).
  Firefight     Reach and ODST, with every game type MCC ships.
  Spartan Ops   Halo 4's 50 missions.
  Custom games  Halo CE, Halo 2, H2A multiplayer, Halo 3, Reach and Halo 4: every
                map (H2A: with its classic and Forge layouts), the stock game
                types, MCC's whole matchmaking set and your own game types (see below).
  Forge         Halo 3, H2A, Reach and Halo 4, and your maps are saved (see below).
  Lobbies       Every mode has a lobby; set it to LAN and others on your
                network can join: campaign co-op, Spartan Ops, Firefight, custom
                games and Forge. Private and Public lobbies are online.
  Your Spartan  Armor, colors and service tag per game; Halo 4 loadouts.
  Split-screen  Up to 4 players on one screen in all of the above (Players in each
                lobby). Halo 2 split-screen uses the classic graphics.
  Controls      Per game: remap the controller (Settings > Button layout).
  Settings      MCC's own settings, in Controls, Video, Audio and Accessibility:
                look sensitivity, dead zones, invert, crouch and zoom behavior,
                field of view, graphics quality, frame rate limit, HUD scale,
                volumes, subtitles, color blind modes and more.

Online play is with friends who have your lobby's join code, or anyone through a
Public lobby (see ONLINE PLAY).


SETUP (PC)
  1. Copy installation08.exe into your MCC folder, next to the halo1, halo2, halo3 ...
     folders. For Steam that is usually:
       C:\Program Files (x86)\Steam\steamapps\common\Halo The Master Chief Collection
  2. Run installation08.exe. The first time, it asks for your player name (the name
     in the games and in LAN lobbies); Customize > Your name changes it later.
  (Or put it anywhere and start it with --mcc "<your MCC folder>".)

  Windows Defender may be wary of a new, unsigned program that loads game files. If it
  flags it, add an exclusion for the file or the folder.


SETUP (PS5)
  On the PS5, Installation 08 is its own app. Use the PS5 package
  (Installation08-PS5-1.3.zip) and follow its README.txt: you copy the app and your
  MCC game folders to the console over FTP, and nothing else.


CONTROLS
  Installation 08 is played with controllers (an Xbox controller, or anything that
  works as one). Menus: A select, B back, Start pauses in a game. A keyboard can still
  move around Installation 08's own menus (arrows, Enter, Esc), not the games.
  Split-screen: each player needs a controller; players take them in the order
  they're connected. The lobby's panel shows who has which.
  The PS5 does not allow PS4 (DualShock 4) controllers in PS5 apps, so they can't
  be used.


THE MENUS
  The main menu lists what you can play: Campaign, Multiplayer (custom games),
  Firefight, Spartan Ops and Forge. Each opens a lobby, where you pick the game and
  its options (mission and difficulty, or map and game type), and press Start.
  Continue, at the top, picks up your last campaign where you saved it.
  Browse games lists the lobbies others are hosting on your network.
  Customize has your name, and your armor, colors and loadouts in each game.
  Settings has MCC's settings, and Button layout for each game's controls.


LAN PLAY
  To host: open a lobby (Campaign, Multiplayer, Firefight, Spartan Ops or Forge)
  and set Lobby to LAN. Others see your lobby change as you choose.
  To join: Browse games on the main menu lists your network's lobbies; pick one.
  Or use Join by IP address there.
  The lobby shows each player as Ready, or what they're missing (the game, or
  that map's part of it, for example a campaign that isn't installed). The host
  can start without players who can't play it; they stay in the lobby.
  After a game, everyone comes back to the lobby, still joined.
  The host's game type and saved Forge map are sent to everyone, so nobody needs
  the same files. Everyone needs the game (and, for co-op, its campaign) installed.
  Campaign co-op and Spartan Ops take up to 4 players.
  Co-op starts from the beginning of a mission: co-op checkpoints can't be
  continued in a later session. The host's pause menu has End co-op.
  With a PS5: the PS5 package includes Microsoft's ucrtbase.dll, which keeps its
  math the same as the PC's (without it, co-op games drift out of sync).
  LAN play uses UDP port 27080. Windows asks to allow it through the firewall the
  first time; allow it on private networks.
  Co-op is tested in all six games, between two PCs and between a PS5 and a PC
  with either one hosting.


ONLINE PLAY
  Every lobby has a Lobby setting: Local (just this machine), LAN (your network),
  Private (friends anywhere, with the lobby's join code) or Public (anyone: listed
  in Browse games).
  To join a Private lobby: Browse games > Join with a code, then type its 16
  letters and numbers. Public lobbies are in the Browse games list.
  Nothing to set up on routers: every machine connects out to the Installation 08
  online server, which introduces players so they connect directly, or passes their
  packets on when they can't. Everything is encrypted end to end. Players who
  connect directly can see each other's IP addresses; Public lobbies are open to
  strangers, so Private is the safer choice with friends.
  The host needs about 100 kbit/s of upload per player who joins.
  In co-op (campaign, Spartan Ops, Firefight) the host runs the game and joined
  players follow its updates. Installation 08 evens out when those arrive so joined
  players' games stay smooth; the small delay this adds follows the connection.


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
  - Halo 2 split-screen: classic graphics only.
