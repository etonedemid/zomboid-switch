Project Zomboid build 41 for Nintendo Switch (homebrew, native NRO)

You need: a Switch running Atmosphere homebrew, ~8 GB free on the SD card, and Project Zomboid on Steam
(this zip has no game data; you build it from your own copy).

1. Get the Linux build 41.78 game files (works from Windows, Linux or macOS):
   download DepotDownloader (github.com/SteamRE/DepotDownloader/releases) and run

     DepotDownloader -app 108600 -depot 108603 -manifest 8887512142279836582 -username YOUR_STEAM_NAME -dir pzdepot

   It asks for your password / Steam Guard (or add -qr to log in with the Steam app). 6.8 GB.

2. Build the SD files (needs Python 3):

     python3 setup-sd.py pzdepot/projectzomboid out

3. Copy to the root of the SD card:
     - the switch/ and pz41/ folders from this zip
     - out/pz41/ (merges into the same pz41/ folder)

4. Launch "Project Zomboid 41" from the homebrew menu in full-RAM mode (hold R while starting a game, not from the
   album). The first start indexes the data; loading into the world takes 3-4 minutes. Controller and touch work.

Saves and options live in sdmc:/pz41/zomboid. Low-memory texture settings are preset in options.ini - keep them, the
console has just enough memory with them. Log: sdmc:/pz41/lx.log.
