# Blades Dashboard Recomp
Blade is a recompilation of the Xbox 360 1888 dashboard using a fork of rexglue-sdk.  It includes some modifications so that it can launch games that were recompiled using rexglue-sdk.  

# Development
## Prerequisites
## How to Build
1. Clone the project
2. Clone my branch of [rexglue-sdk](https://github.com/stocktaylor/rexglue-sdk)
3. Clone [rextools](https://github.com/stocktaylor/rextools)
4. Clone [rexlauncher](https://github.com/stocktaylor/rexlaunchers)

## Assets
The build copies the files the dashboard needs out of the 1888 dashboard's flash dump into `assets/` (`cmake/extract_assets.cmake`, file list in `cmake/blade_assets.cmake`): `dash.xex` as `default.xex`, the `.xzp` packages and the `.xtt` fonts. It checks `dash.xex` against the hash the recompiled code came from, and runs again only when a dump file changes. Codegen reads `assets/default.xex`, and Blade uses `assets/` as its game data root when run without `--game_data_root`.

The dump is read from `dash/` by default; point elsewhere with `-DBLADE_DUMP_DIR=<folder>`. Without a dump, an existing `assets/` folder is used as is.

## Stock dashboard build
The launcher changes (Games > Demos listing `launcher.toml`, the `strings.toml` relabels, Display settings driving the window and launched games, skipping the Screen Resolution confirmation) are on by default. Configure with `-DBLADE_LAUNCHER=OFF` to build the unchanged dashboard, keeping only the fixes it needs to run:

```
cmake --preset win-amd64-relwithdebinfo -B out/build/stock -DBLADE_LAUNCHER=OFF
```