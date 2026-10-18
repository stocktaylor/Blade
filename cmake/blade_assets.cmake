# blade - the files the dashboard needs from its flash dump, as
# <file in the dump>=<file in the assets folder>. Included by CMakeLists.txt
# and cmake/extract_assets.cmake.
set(BLADE_ASSET_FILES
    dash.xex=default.xex
    gamercrd.xzp=gamercrd.xzp
    mplayer.xzp=mplayer.xzp
    shrdres.xzp=shrdres.xzp
    xam.xzp=xam.xzp
    xenonclatin.xtt=xenonclatin.xtt
    xenonjklatin.xtt=xenonjklatin.xtt
)
# blade_manifest.toml's header: the dash.xex the recompiled code came from.
set(BLADE_DASH_SHA256 "261b32510079b2d0770631529f752a7580902283426b87dbc4b09a2764bb780b")
