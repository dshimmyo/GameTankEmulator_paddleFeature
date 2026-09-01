echo "diffing mac builds from build/GameTankEmulator:"

diff build/GameTankEmulator BrickGameMac/BrickGame.app/Contents/MacOS/BrickGame
diff build/GameTankEmulator BrickGameDemoMac/BrickGame.app/Contents/MacOS/BrickGame
diff build/GameTankEmulator SteamStaging/content/mac_depot/BrickGame.app/Contents/MacOS/BrickGame
diff build/GameTankEmulator SteamDemoStaging/content/mac_depot/BrickGame.app/Contents/MacOS/BrickGame

echo "copying mac builds from build/GameTankEmulator:"

cp -p build/GameTankEmulator BrickGameMac/BrickGame.app/Contents/MacOS/BrickGame
cp -p build/GameTankEmulator BrickGameDemoMac/BrickGame.app/Contents/MacOS/BrickGame
cp -p build/GameTankEmulator SteamStaging/content/mac_depot/BrickGame.app/Contents/MacOS/BrickGame
cp -p build/GameTankEmulator SteamDemoStaging/content/mac_depot/BrickGame.app/Contents/MacOS/BrickGame


echo "final diffs 4 files:"

echo "diff build/GameTankEmulator BrickGameMac/BrickGame.app/Contents/MacOS/BrickGame:"
diff build/GameTankEmulator BrickGameMac/BrickGame.app/Contents/MacOS/BrickGame

echo "diff build/GameTankEmulator BrickGameDemoMac/BrickGame.app/Contents/MacOS/BrickGame:"
diff build/GameTankEmulator BrickGameDemoMac/BrickGame.app/Contents/MacOS/BrickGame

echo "diff build/GameTankEmulator SteamStaging/content/mac_depot/BrickGame.app/Contents/MacOS/BrickGame:"
diff build/GameTankEmulator SteamStaging/content/mac_depot/BrickGame.app/Contents/MacOS/BrickGame

echo "diff build/GameTankEmulator SteamDemoStaging/content/mac_depot/BrickGame.app/Contents/MacOS/BrickGame:"
diff build/GameTankEmulator SteamDemoStaging/content/mac_depot/BrickGame.app/Contents/MacOS/BrickGame


