unset CPLUS_INCLUDE_PATH 
unset LIBRARY_PATH 
unset CPATH 
make clean-wasm
make OS=wasm ROMFILE=roms/paddleTest.gtr
cp -p roms/brickgame.gtr wasmbuild/brickgame.gtr
