unset CPLUS_INCLUDE_PATH 
unset LIBRARY_PATH 
unset CPATH 
make clean
make OS=wasm ROMFILE=roms/paddleTest.gtr
