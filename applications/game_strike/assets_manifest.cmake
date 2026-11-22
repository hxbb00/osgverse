# GameStrike external asset pack manifest (free / CC0 packs only).
# Each entry: "name|subdir|url1;url2;..."  -- mirrors are tried in download order.
# Packs land in <CMAKE_INSTALL_PREFIX>/games/Strike/<subdir>/.
# Append new entries here (e.g. weapon models, muzzle flash .efkefc effects); the
# game reads this directory relative to the executable: ../games/Strike/.

set(VERSE_STRIKE_ASSET_PACKS
    # Kenney "Prototype Textures" (CC0): grid blockout textures used on the arena.
    # Primary: OpenGameArt direct link; fallback: kenney.nl (may be bot-protected)
    "prototype-textures|PrototypeTextures|https://opengameart.org/sites/default/files/kenney_prototypeTextures.zip"
)
