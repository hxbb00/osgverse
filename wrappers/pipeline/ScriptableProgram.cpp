#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/ShaderLibrary.h>
using namespace osgVerse;

static bool checkDefinitions(const ScriptableProgram& prog)
{ return !prog.getDefinitionMap().empty(); }

static bool readDefinitions(osgDB::InputStream& is, ScriptableProgram& prog)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size; ++i)
    {
        int type = 0; unsigned int count = 0;
        is >> is.PROPERTY("Type") >> type;
        count = is.readSize(); is >> is.BEGIN_BRACKET;
        for (unsigned int j = 0; j < count; ++j)
        {
            std::string code; is.readWrappedString(code);
            prog.addDefinitions((osg::Shader::Type)type, code);
        }
        is >> is.END_BRACKET;
    }
    is >> is.END_BRACKET; prog.dirty();
    return true;
}

static bool writeDefinitions(osgDB::OutputStream& os, const ScriptableProgram& prog)
{
    const std::map<osg::Shader::Type, ScriptableProgram::CodeSegmentList>& defs = prog.getDefinitionMap();
    os.writeSize(defs.size()); os << os.BEGIN_BRACKET << std::endl;
    for (std::map<osg::Shader::Type, ScriptableProgram::CodeSegmentList>::const_iterator
         itr = defs.begin(); itr != defs.end(); ++itr)
    {
        os << os.PROPERTY("Type") << (int)itr->first << std::endl;
        os.writeSize(itr->second.size()); os << os.BEGIN_BRACKET << std::endl;
        for (size_t i = 0; i < itr->second.size(); ++i)
        { os.writeWrappedString(itr->second[i]); os << std::endl; }
        os << os.END_BRACKET << std::endl;
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

static bool checkSegments(const ScriptableProgram& prog)
{ return !prog.getSegmentMap().empty(); }

static bool readSegments(osgDB::InputStream& is, ScriptableProgram& prog)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size; ++i)
    {
        int type = 0; is >> is.PROPERTY("Type") >> type;
        unsigned int count = is.readSize(); is >> is.BEGIN_BRACKET;
        for (unsigned int j = 0; j < count; ++j)
        {
            int pos = 0; is >> is.PROPERTY("Position") >> pos;
            unsigned int num = is.readSize(); is >> is.BEGIN_BRACKET;
            for (unsigned int k = 0; k < num; ++k)
            {
                std::string code; is.readWrappedString(code);
                prog.addSegment((osg::Shader::Type)type, pos, code);
            }
            is >> is.END_BRACKET;
        }
        is >> is.END_BRACKET;
    }
    is >> is.END_BRACKET; prog.dirty();
    return true;
}

static bool writeSegments(osgDB::OutputStream& os, const ScriptableProgram& prog)
{
    const std::map<osg::Shader::Type, ScriptableProgram::SegmentMap>& segs = prog.getSegmentMap();
    os.writeSize(segs.size()); os << os.BEGIN_BRACKET << std::endl;
    for (std::map<osg::Shader::Type, ScriptableProgram::SegmentMap>::const_iterator
         itr = segs.begin(); itr != segs.end(); ++itr)
    {
        os << os.PROPERTY("Type") << (int)itr->first << std::endl;
        os.writeSize(itr->second.size()); os << os.BEGIN_BRACKET << std::endl;
        for (ScriptableProgram::SegmentMap::const_iterator sitr = itr->second.begin();
             sitr != itr->second.end(); ++sitr)
        {
            os << os.PROPERTY("Position") << sitr->first << std::endl;
            os.writeSize(sitr->second.size()); os << os.BEGIN_BRACKET << std::endl;
            for (size_t i = 0; i < sitr->second.size(); ++i)
            { os.writeWrappedString(sitr->second[i]); os << std::endl; }
            os << os.END_BRACKET << std::endl;
        }
        os << os.END_BRACKET << std::endl;
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(ScriptableProgram,
                        new osgVerse::ScriptableProgram,
                        osgVerse::ScriptableProgram,
                        "osg::Object osg::StateAttribute osg::Program osgVerse::ScriptableProgram")
{
    ADD_USER_SERIALIZER(Definitions);
    ADD_USER_SERIALIZER(Segments);
}
