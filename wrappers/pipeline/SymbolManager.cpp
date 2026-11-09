#undef OBJECT_CAST
#define OBJECT_CAST dynamic_cast

#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/SymbolManager.h>
#include <cmath>
using namespace osgVerse;

static bool checkSettings(const SymbolManager& mgr) { return true; }

static bool readSettings(osgDB::InputStream& is, SymbolManager& mgr)
{
    double d0 = 0.0, d1 = 0.0, d2 = 0.0;
    osg::Vec3 factor, offset, scale; bool showIcons = true; std::string font;
    is >> is.PROPERTY("LodDistance0") >> d0;
    is >> is.PROPERTY("LodDistance1") >> d1;
    is >> is.PROPERTY("LodDistance2") >> d2;
    is >> is.PROPERTY("LodScaleFactor") >> factor;
    is >> is.PROPERTY("ShowIconsInMidDistance") >> showIcons;
    is >> is.PROPERTY("MidDistanceOffset") >> offset;
    is >> is.PROPERTY("MidDistanceScale") >> scale;
    is >> is.PROPERTY("FontFileName"); is.readWrappedString(font);

    mgr.setLodDistance(SymbolManager::LOD0, d0);
    mgr.setLodDistance(SymbolManager::LOD1, d1);
    mgr.setLodDistance(SymbolManager::LOD2, d2);
    mgr.setLodScaleFactor(factor);
    mgr.setShowIconsInMidDistance(showIcons);
    mgr.setMidDistanceTextOffset(offset);
    int numTiles = (scale.z() > 0.0f) ? (int)std::floor(1.0f / scale.z() + 0.5f) : 0;
    mgr.setMidDistanceTextScale(osg::Vec2(scale.x(), scale.y()), numTiles);
    if (!font.empty()) mgr.setFontFileName(font);
    return true;
}

static bool writeSettings(osgDB::OutputStream& os, const SymbolManager& mgr)
{
    os << os.PROPERTY("LodDistance0") << mgr.getLodDistance(SymbolManager::LOD0) << std::endl;
    os << os.PROPERTY("LodDistance1") << mgr.getLodDistance(SymbolManager::LOD1) << std::endl;
    os << os.PROPERTY("LodDistance2") << mgr.getLodDistance(SymbolManager::LOD2) << std::endl;
    os << os.PROPERTY("LodScaleFactor") << mgr.getLodScaleFactor() << std::endl;
    os << os.PROPERTY("ShowIconsInMidDistance") << mgr.getShowIconsInMidDistance() << std::endl;
    os << os.PROPERTY("MidDistanceOffset") << mgr.getMidDistanceTextOffset() << std::endl;
    os << os.PROPERTY("MidDistanceScale") << mgr.getMidDistanceTextScale() << std::endl;
    os << os.PROPERTY("FontFileName"); os.writeWrappedString(mgr.getFontFileName()); os << std::endl;
    return true;
}

static bool checkSymbols(const SymbolManager& mgr) { return !mgr.getSymols().empty(); }

static bool readSymbols(osgDB::InputStream& is, SymbolManager& mgr)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size; ++i)
    {
        int id = -1; Symbol* symbol = new Symbol;
        is >> id;
        is.readWrappedString(symbol->name);
        is.readWrappedString(symbol->fileName);
        is >> symbol->position >> symbol->tiling >> symbol->tiling2;
        is >> symbol->color >> symbol->textColor;
        is >> symbol->rotateAngle >> symbol->scale;
        symbol->id = id; mgr.updateSymbol(symbol);
    }
    is >> is.END_BRACKET;
    return true;
}

static bool writeSymbols(osgDB::OutputStream& os, const SymbolManager& mgr)
{
    const std::map<int, osg::ref_ptr<Symbol>>& symbols = mgr.getSymols();
    os.writeSize(symbols.size()); os << os.BEGIN_BRACKET << std::endl;
    for (std::map<int, osg::ref_ptr<Symbol>>::const_iterator itr = symbols.begin();
         itr != symbols.end(); ++itr)
    {
        const Symbol* symbol = itr->second.get(); os << itr->first;
        os.writeWrappedString(symbol->name);
        os.writeWrappedString(symbol->fileName);
        os << symbol->position << symbol->tiling << symbol->tiling2;
        os << symbol->color << symbol->textColor;
        os << symbol->rotateAngle << symbol->scale << std::endl;
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(SymbolManager,
                        new osgVerse::SymbolManager,
                        osgVerse::SymbolManager,
                        "osg::Object osg::Callback osg::NodeCallback osgVerse::SymbolManager")
{
    ADD_USER_SERIALIZER(Settings);
    ADD_USER_SERIALIZER(Symbols);
}
