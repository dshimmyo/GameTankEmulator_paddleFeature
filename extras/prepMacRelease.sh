cp -p build/GameTankEmulator BrickGameMac/BrickGame
cp /opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib BrickGameMac/
install_name_tool -change /opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib @loader_path/libSDL2-2.0.0.dylib BrickGameMac/BrickGame
otool -L BrickGameMac/BrickGame
