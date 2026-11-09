#undef OBJECT_CAST
#define OBJECT_CAST dynamic_cast

#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <animation/TweenAnimation.h>
using namespace osgVerse;

static bool checkAnimations(const TweenAnimation& anim)
{ return !anim.getAnimationNames().empty(); }

static bool readAnimations(osgDB::InputStream& is, TweenAnimation& anim)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size; ++i)
    {
        std::string name; is.readWrappedString(name);
        osg::ref_ptr<osg::AnimationPath> path;
        int mode = 0, direction = 0; float offset = 0.0f, multiplier = 1.0f;
        is >> is.PROPERTY("Path") >> path;
        is >> is.PROPERTY("Property") >> mode >> offset >> multiplier >> direction;
        if (path.valid())
        {
            anim.addAnimation(name, path.get());
            anim.setProperty(name, offset, multiplier);
            anim.setAnimationMode(name, (TweenAnimation::PlayingMode)mode);
        }
    }
    is >> is.END_BRACKET;
    return true;
}

static bool writeAnimations(osgDB::OutputStream& os, const TweenAnimation& anim)
{
    std::vector<std::string> names = anim.getAnimationNames();
    os.writeSize(names.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < names.size(); ++i)
    {
        float offset = 0.0f, multiplier = 1.0f;
        osg::AnimationPath* path = const_cast<TweenAnimation&>(anim).getAnimation(names[i]);
        TweenAnimation::PlayingMode mode = anim.getProperty(names[i], offset, multiplier);
        os.writeWrappedString(names[i]); os << std::endl;
        os << os.PROPERTY("Path") << path << std::endl;
        os << os.PROPERTY("Property") << (int)mode << offset << multiplier << 0 << std::endl;
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(TweenAnimation,
                        new osgVerse::TweenAnimation,
                        osgVerse::TweenAnimation,
                        "osg::Object osg::Callback osg::NodeCallback osgVerse::TweenAnimation")
{
    ADD_VEC3D_SERIALIZER(PivotPoint, osg::Vec3d());
    ADD_BOOL_SERIALIZER(UseInverseMatrix, false);
    ADD_USER_SERIALIZER(Animations);
}
