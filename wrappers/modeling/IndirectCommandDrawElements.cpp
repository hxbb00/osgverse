#include <osg/Version>
#if OSG_VERSION_GREATER_THAN(3, 4, 1)
#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <modeling/GeometryMerger.h>
using namespace osgVerse;

static bool checkCommands(const IndirectCommandDrawElements& pset)
{ return !pset.getCommands().empty(); }

static bool readCommands(osgDB::InputStream& is, IndirectCommandDrawElements& pset)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    std::vector<osg::DrawElementsIndirectCommand>& commands = pset.getCommands();
    commands.resize(size);
    for (unsigned int i = 0; i < size; ++i)
    {
        osg::DrawElementsIndirectCommand& cmd = commands[i];
        is >> is.PROPERTY("Command") >> cmd.count >> cmd.instanceCount >> cmd.firstIndex
           >> cmd.baseVertex >> cmd.baseInstance;
    }
    is >> is.END_BRACKET;
    return true;
}

static bool writeCommands(osgDB::OutputStream& os, const IndirectCommandDrawElements& pset)
{
    const std::vector<osg::DrawElementsIndirectCommand>& commands = pset.getCommands();
    os.writeSize(commands.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < commands.size(); ++i)
    {
        const osg::DrawElementsIndirectCommand& cmd = commands[i];
        os << os.PROPERTY("Command") << cmd.count << cmd.instanceCount << cmd.firstIndex
           << cmd.baseVertex << cmd.baseInstance << std::endl;
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(IndirectCommandDrawElements,
                        new osgVerse::IndirectCommandDrawElements,
                        osgVerse::IndirectCommandDrawElements,
                        "osg::Object osg::BufferData osg::IndirectCommandDrawElements "
                        "osgVerse::IndirectCommandDrawElements")
{
    ADD_USER_SERIALIZER(Commands);
}
#endif
