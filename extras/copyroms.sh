echo "diffing roms/brickgame.gtr:"

diff roms/brickgame.gtr BrickGameMac/BrickGame.app/Contents/MacOS/gamedata.gtr 
diff roms/brickgame.gtr BrickGameWin/gamedata.gtr 
diff roms/brickgame.gtr SteamStaging/content/mac_depot/BrickGame.app/Contents/MacOS/gamedata.gtr 
diff roms/brickgame.gtr SteamStaging/content/windows_depot/gamedata.gtr 

cp roms/brickgame.gtr BrickGameMac/BrickGame.app/Contents/MacOS/gamedata.gtr 
cp roms/brickgame.gtr BrickGameWin/gamedata.gtr 
cp roms/brickgame.gtr SteamStaging/content/mac_depot/BrickGame.app/Contents/MacOS/gamedata.gtr 
cp roms/brickgame.gtr SteamStaging/content/windows_depot/gamedata.gtr 

echo "diffing roms/BrickGameDemo.gtr:"

diff roms/BrickGameDemo.gtr BrickGameDemoMac/BrickGame.app/Contents/MacOS/gamedata.gtr
diff roms/BrickGameDemo.gtr BrickGameDemoWin/gamedata.gtr 
diff roms/BrickGameDemo.gtr SteamDemoStaging/content/mac_depot/BrickGame.app/Contents/MacOS/gamedata.gtr 
diff roms/BrickGameDemo.gtr SteamDemoStaging/content/windows_depot/gamedata.gtr

cp roms/BrickGameDemo.gtr BrickGameDemoMac/BrickGame.app/Contents/MacOS/gamedata.gtr
cp roms/BrickGameDemo.gtr BrickGameDemoWin/gamedata.gtr 
cp roms/BrickGameDemo.gtr SteamDemoStaging/content/mac_depot/BrickGame.app/Contents/MacOS/gamedata.gtr 
cp roms/BrickGameDemo.gtr SteamDemoStaging/content/windows_depot/gamedata.gtr 

echo "final diffs 8 files:"

diff roms/brickgame.gtr BrickGameMac/BrickGame.app/Contents/MacOS/gamedata.gtr
diff roms/brickgame.gtr BrickGameWin/gamedata.gtr 
diff roms/brickgame.gtr SteamStaging/content/mac_depot/BrickGame.app/Contents/MacOS/gamedata.gtr 
diff roms/brickgame.gtr SteamStaging/content/windows_depot/gamedata.gtr 
diff roms/BrickGameDemo.gtr BrickGameDemoMac/BrickGame.app/Contents/MacOS/gamedata.gtr
diff roms/BrickGameDemo.gtr BrickGameDemoWin/gamedata.gtr 
diff roms/BrickGameDemo.gtr SteamDemoStaging/content/mac_depot/BrickGame.app/Contents/MacOS/gamedata.gtr 
diff roms/BrickGameDemo.gtr SteamDemoStaging/content/windows_depot/gamedata.gtr 

