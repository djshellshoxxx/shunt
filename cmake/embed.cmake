# Usage: cmake -DIN=file -DOUT=file.cpp -DSYMBOL=name -P embed.cmake
file(READ "${IN}" HEXDATA HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEXDATA}")
file(WRITE "${OUT}" "#include <cstddef>\nextern const unsigned char ${SYMBOL}[] = {${BYTES}0x00};\nextern const size_t ${SYMBOL}_size = sizeof(${SYMBOL}) - 1;\n")
