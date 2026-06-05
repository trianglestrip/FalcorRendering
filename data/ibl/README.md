# Filament default IBL assets

Pre-generated Image-Based Lighting (IBL) resources matching Filament's default
`lightroom_14b` environment used by `gltf_viewer` and `pbrt_kitchen`.

## Runtime layout

At runtime (after CMake `copy_data_folder`), assets are available under:

```
bin/<Config>/data/ibl/
  dfg.bin
  dfg.dds
  lightroom_14b/
    lightroom_14b_ibl.dds      # prefiltered specular cubemap (5 mips)
    lightroom_14b_skybox.dds   # blurred skybox cubemap
    sh.txt                     # 9-band diffuse irradiance SH coefficients
```

`FilamentIBL::loadDefault()` loads:

- `data/ibl/lightroom_14b/lightroom_14b_ibl.dds`
- `data/ibl/dfg.dds`
- `data/ibl/lightroom_14b/sh.txt` (optional; falls back to roughness-one mip)

## Source HDR

- **File:** `D:/gitProject/filament/third_party/environments/lightroom_14b.hdr`
- **License:** CC0 (see Filament `third_party/environments/CC0.html`)
- **Tool:** Filament `cmgen` (`D:/gitProject/filament/out/bin/Release/cmgen.exe`)

## Regeneration

From the FalcorRendering repository root:

```bat
set CMGEN=D:\gitProject\filament\out\bin\Release\cmgen.exe
set HDR=D:\gitProject\filament\third_party\environments\lightroom_14b.hdr

REM 1) Prefiltered IBL + skybox faces (DDS)
%CMGEN% -x data\ibl\lightroom_14b --quiet --format=dds --size=256 --extract-blur=0.1 %HDR%

REM 2) Pack per-face DDS into combined cubemaps for Falcor ImageIO
py -3 scripts\pack_cmgen_cubemap_dds.py ^
  --src data\ibl\lightroom_14b\lightroom_14b ^
  --ibl-out data\ibl\lightroom_14b\lightroom_14b_ibl.dds ^
  --skybox-out data\ibl\lightroom_14b\lightroom_14b_skybox.dds

copy /Y data\ibl\lightroom_14b\lightroom_14b\sh.txt data\ibl\lightroom_14b\sh.txt

REM 3) DFG LUT (engine-level, material-independent)
%CMGEN% --quiet --size=128 --ibl-dfg-multiscatter --ibl-dfg-cloth --ibl-dfg=data\ibl\dfg.bin
%CMGEN% --quiet --size=128 --ibl-dfg-multiscatter --ibl-dfg-cloth --ibl-dfg=data\ibl\dfg.dds
```

### Notes

- `cmgen --format=dds` deploy writes **per-face** DDS files, not combined cubemaps.
  Filament's native combined outputs use KTX (`*_ibl.ktx`, `*_skybox.ktx`). Falcor uses
  `ImageIO::loadTextureFromDDS`, so `scripts/pack_cmgen_cubemap_dds.py` packs the face
  DDS files into `lightroom_14b_ibl.dds` and `lightroom_14b_skybox.dds`.
- `dfg.bin` is the raw RG(+B cloth)16F LUT from cmgen. `dfg.dds` is the loadable
  128×128 DDS used by `FilamentIBL`.
