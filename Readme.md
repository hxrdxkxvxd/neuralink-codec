# Neuralink Compression Challenge

**Result: 3.4575x lossless compression**

## Approach

- 8-tap adaptive predictor (Sign-Sign LMS)
- 4-context probability model with adaptive thresholds
- Range coding

## Build & Run
```bash
# Download data
curl -O https://content.neuralink.com/compression-challenge/data.zip

# Build & Evaluate
chmod +x build.sh eval.sh
./build.sh
./eval.sh
```

## Acknowledgments

Based on prior work by **phoboslab**, notably the
[neuralink-compression](https://github.com/phoboslab/neuralink-compression) project, which achieved a **3.35× compression ratio**.