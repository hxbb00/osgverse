#ifndef VERSE_STRIKE_GAMEPATHS_H
#define VERSE_STRIKE_GAMEPATHS_H

#include <osgDB/FileUtils>
#include <string>

namespace game
{
    /** Locate the game asset directory (installed to <prefix>/games/Strike,
        i.e. one level above the bin/ folder the executable runs from). Returns
        an empty string when not found; everything then falls back to procedural. */
    inline std::string findGameAssetsDir()
    {
        static const char* candidates[] =
        {
            "../games/Strike",      // running from <prefix>/bin
            "games/Strike",         // running from <prefix>
            "../../games/Strike",   // running from <prefix>/bin/osgPlugins-x.y
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
        {
            if (osgDB::fileExists(std::string(candidates[i]) + "/PrototypeTextures"))
                return candidates[i];
        }
        return "";
    }
}

#endif
