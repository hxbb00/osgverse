#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/MultiEffectNode.h>
using namespace osgVerse;

static bool checkEffect(const MultiEffectGeode& node)
{ return node.getEffect() != NULL && node.getEffect()->getNumPasses() > 0; }

static bool readEffect(osgDB::InputStream& is, MultiEffectGeode& node)
{
    unsigned int size = is.readSize();
    if (size == 0) return true;
    is >> is.BEGIN_BRACKET;

    osg::ref_ptr<MultiEffectContainer> container = new MultiEffectContainer;
    for (unsigned int i = 0; i < size; ++i)
    {
        osg::ref_ptr<osg::StateSet> ss = is.readObjectOfType<osg::StateSet>();
        if (ss.valid()) container->addPass(ss.get());
    }
    is >> is.END_BRACKET;
    node.setEffect(container.get());
    return true;
}

static bool writeEffect(osgDB::OutputStream& os, const MultiEffectGeode& node)
{
    const MultiEffectContainer* container = node.getEffect();
    unsigned int size = container ? container->getNumPasses() : 0;
    os.writeSize(size); os << os.BEGIN_BRACKET << std::endl;
    for (unsigned int i = 0; i < size; ++i) os << container->getPass(i) << std::endl;
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(MultiEffectGeode,
                        new osgVerse::MultiEffectGeode,
                        osgVerse::MultiEffectGeode,
                        "osg::Object osg::Node osg::Geode osgVerse::MultiEffectGeode")
{
    ADD_USER_SERIALIZER(Effect);
}
