#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <animation/PhysicsEngine.h>
#include <set>
using namespace osgVerse;

static bool checkGravity(const PhysicsEngine& engine) { return true; }

static bool readGravity(osgDB::InputStream& is, PhysicsEngine& engine)
{
    osg::Vec3 gravity; is >> gravity;
    engine.setGravity(gravity);
    return true;
}

static bool writeGravity(osgDB::OutputStream& os, const PhysicsEngine& engine)
{
    os << engine.getGravity() << std::endl;
    return true;
}

static bool checkDisabledBodies(const PhysicsEngine& engine)
{ return !engine.getDisabledBodies().empty(); }

static bool readDisabledBodies(osgDB::InputStream& is, PhysicsEngine& engine)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    std::set<std::string> names;
    for (unsigned int i = 0; i < size; ++i)
    {
        std::string name; is.readWrappedString(name);
        names.insert(name);
    }
    is >> is.END_BRACKET;
    engine.setDisabledBodies(names);
    return true;
}

static bool writeDisabledBodies(osgDB::OutputStream& os, const PhysicsEngine& engine)
{
    const std::set<std::string>& names = engine.getDisabledBodies();
    os.writeSize(names.size()); os << os.BEGIN_BRACKET << std::endl;
    for (std::set<std::string>::const_iterator itr = names.begin(); itr != names.end(); ++itr)
    { os.writeWrappedString(*itr); os << std::endl; }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(PhysicsEngine,
                        new osgVerse::PhysicsEngine,
                        osgVerse::PhysicsEngine,
                        "osg::Object osgVerse::PhysicsEngine")
{
    // Rigid bodies, collision shapes and constraints are backend objects (opaque
    // b3* handles) and have to be recreated by applications after loading; only
    // the world settings are persisted here
    ADD_USER_SERIALIZER(Gravity);
    ADD_USER_SERIALIZER(DisabledBodies);
}
