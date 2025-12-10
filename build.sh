#!/bin/bash
set -e

echo "Building Neuralink Compression Challenge submission..."

gcc -O3 -o encode encode.c -lm -std=c99
echo "✓ Built encode"

gcc -O3 -o decode decode.c -lm -std=c99
echo "✓ Built decode"

echo ""
echo "Build complete!"
echo "  encode: $(wc -c < encode) bytes"
echo "  decode: $(wc -c < decode) bytes"
echo ""
echo "Run ./eval.sh to test (requires data.zip)"