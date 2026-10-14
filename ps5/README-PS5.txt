INSTALLATION 08 FOR PS5  -  version 1.2
Play Halo: The Master Chief Collection on your jailbroken PS5, offline.

Installation 08 runs inside prospero-win, the app that runs Windows games on the
PS5. You copy two things to your console over FTP: this package, and the game
files from your own copy of MCC. No PC tools, no building, nothing to install on
the PC except an FTP program.

This package does not include any game files. You need Halo: The Master Chief
Collection on Steam, installed on your PC.


BEFORE YOU START
  [ ] prospero-win is installed on your PS5 and opens (its own guide:
      github.com/mpereiraesaa/prospero-win, "Getting started").
  [ ] Your PS5 is running an FTP server, and you know your PS5's IP address
      (Settings > Network > Connection Status on the PS5).
  [ ] FileZilla is installed on your PC (filezilla-project.org, the free "Client").
  [ ] MCC is installed on your PC through Steam.
  [ ] About 50-130 GB free on the PS5, depending on which Halo games you copy.
  [ ] A Windows PC (one file, ucrtbase.dll, comes from its Windows folder).


STEP 1  -  Connect to your PS5
  1. Open FileZilla.
  2. At the top, fill in:
       Host:  your PS5's IP address (for example 192.168.1.50)
       Port:  2121  (or the port your FTP server shows when it starts)
     Leave Username and Password empty, and click Quickconnect.
  3. The right side of FileZilla now shows the PS5's files. The left side is your PC.


STEP 2  -  Copy this package to the PS5
  1. On the right (PS5) side, type /data/ in the "Remote site" box and press Enter.
     You will see a folder called prospero-win there.
  2. On the left (PC) side, open the folder you unzipped this package into. You will
     see a folder called prospero-win there too.
  3. Drag the prospero-win folder from the left side into an empty spot on the right
     side, so it lands in /data/. FileZilla adds its contents to the prospero-win
     folder already there; nothing of yours is replaced. If it asks about files that
     already exist, choose "Skip". It is a few hundred MB.
  4. One file from Windows, which Installation 08 needs and the package can't include:
     a. On the left (PC) side, type  C:\Windows\System32  in the "Local site" box
        and press Enter, and find  ucrtbase.dll  in the list.
     b. On the right (PS5) side, open
          /data/prospero-win/prefixes/installation08/drive_c/Games/Installation08/
        (installation08.exe is there).
     c. Drag ucrtbase.dll from the left into that folder on the right.
     Without it Installation 08 doesn't start. (Why: the games' math must match
     Windows' exactly for LAN games with a PC, and Wine's version differs slightly.)


STEP 3  -  Add Installation 08 to prospero-win's game list
  On the right side, open /data/prospero-win/profiles/.
  - If there is a file called profiles.lst there: right-click it, choose View/Edit,
    add this line at the end, save, close the editor, and when FileZilla asks,
    click Yes to upload it:
        installation08.profile
  - If there is no profiles.lst, there is nothing to do.


STEP 4  -  Copy your Halo games to the PS5
  1. Find MCC on your PC: in Steam, right-click Halo: The Master Chief Collection >
     Manage > Browse local files. A folder opens with halo1, halo2, halo3 ... in it.
     Copy its address from the address bar and paste it into the "Local site" box on
     the left side of FileZilla.
  2. On the right side, open /data/prospero-win/mcc/.
  3. Drag the game folders you want from the left to the right:
       halo1       Halo: Combat Evolved
       halo2       Halo 2
       halo3       Halo 3
       halo3odst   Halo 3: ODST
       haloreach   Halo: Reach
       halo4       Halo 4
  4. Every game also needs one folder of shared files from MCC:
     a. On the right side, inside /data/prospero-win/mcc/, right-click >
        Create directory, name it  mcc  and open it.
     b. Inside that, create a directory named  binaries  and open it.
     c. On the left side, open the mcc folder, then binaries.
     d. Drag the win64 folder from the left into binaries on the right.
     When you are done, the PS5 has:
       /data/prospero-win/mcc/halo3/              (and your other games)
       /data/prospero-win/mcc/mcc/binaries/win64/
  The games are large. Copying runs at about 15-30 MB/s, so Halo 3 alone takes
  about 15 minutes and all six games a couple of hours. Leave the PS5 on and awake.


STEP 5  -  Play
  Open prospero-win on the PS5 and choose Installation 08. The first time, it asks
  for your player name (the name in the games and in LAN lobbies): pick letters with
  the D-pad and Cross, Square deletes. Settings > Your name changes it later.
  Then pick a game, and Campaign, Firefight, Spartan Ops, Custom game or Forge.
  If a game runs slowly, lower Settings > Video > Resolution in Installation 08 to
  1600x900 or 1280x720, or lower the quality settings on the same page.
  LAN games on the main menu: join a game hosted by a PC (or another PS5) on your
  network, campaign co-op included. See README.txt, LAN GAMES.


CONTROLS
  The DualSense works like an Xbox controller. A USB keyboard and mouse work too.
  Options       pause menu (restart, change mission, settings, quit)
  Options + Create, held for a second: close Installation 08
  Split-screen: plug in more controllers and set Players on a game's page.
  (PS4 controllers can't be used: the PS5 doesn't allow them in PS5 apps.)


LAN GAMES (WITH OTHER PS5s AND PCs)
  Installation 08 on a PS5 plays with other PS5s and PCs on the same network:
  campaign co-op, Spartan Ops, Firefight, custom games and Forge.
  To add a PC: copy installation08.exe (from Installation08-1.2.zip) into the PC's
  MCC folder, next to halo1, halo2, halo3 ... and run it. Allow it through the
  Windows firewall on private networks when asked.
  The ucrtbase.dll you copied in step 2.4 is what keeps a PS5 and a PC in step:
  Wine does some of the games' math very slightly differently from Windows, enough
  for a LAN game between them to drift apart ("lost connection" after a minute or so).
  To host: on any machine, LAN games on the main menu > Host a game. Choose the
  game, the mode and its options.
  To join: LAN games on the main menu, then pick the host's game from the list
  (or Join by IP address).
  The lobby shows who is ready and what anyone is missing (for co-op, every machine
  needs that game's campaign). The host presses Start; after the game everyone
  comes back to the lobby. Co-op starts from the beginning of a mission: co-op
  checkpoints can't be continued in a later session.
  Every machine needs the same version of Installation 08.


UPDATING INSTALLATION 08
  Copy the new installation08.exe over FTP into
    /data/prospero-win/prefixes/installation08/drive_c/Games/Installation08/
  replacing the old one. Your settings and saves are in that folder and are kept.
  From 1.1 to 1.2, two more things, together (one without the other and Installation
  08 won't start):
    1. Copy ucrtbase.dll from your PC into that same folder (step 2.4).
    2. On the right side, right-click /data/prospero-win/profiles/installation08.profile
       > View/Edit, and change the line that starts with  dll_overrides =  to:
         dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi,xaudio2_9,ucrtbase=n
       Save, close the editor, and click Yes when FileZilla asks to upload it.


IF SOMETHING GOES WRONG
  "No Halo games found"
      The game folders aren't where they should be. Check that you have, for example,
      /data/prospero-win/mcc/halo3/halo3.dll  (not mcc/Halo The Master Chief
      Collection/halo3/...).
  No sound, or a crash when a video should play
      The mcc/binaries/win64 folder is missing: see step 4.4.
  Installation 08 isn't in prospero-win's list
      Check step 3, and that /data/prospero-win/profiles/installation08.profile exists.
  It goes straight back to prospero-win's list
      Check that ucrtbase.dll is next to installation08.exe (step 2.4).
      Otherwise something else failed; the logs say what. Send these with your bug report:
        /data/prospero-win/prefixes/installation08/drive_c/Games/Installation08/installation08.log
        the newest files in /data/prospero-win/logs/
