#ifndef MANA_UI_UTILITIES_HPP
#define MANA_UI_UTILITIES_HPP

#include <osg/Transform>
#include <osg/Geometry>
#include <osg/Camera>
#include <osgGA/EventQueue>
#include <osgText/Font>
#include <osgText/Text>
#include <unordered_map>
#include <string>
#include <functional>

namespace osgVerse
{

    /** A simple HUD text and simple UI displayer */
    struct HeadUpDisplayCanvas
    {
        typedef std::string ItemID;
        typedef std::function<void ()> OnClickEvent;
        typedef std::function<void (bool, bool)> OnSelectEvent;
        enum Direction { FREE = 0, ROW = 0x002, COLUMN = 0x003, WRAP = 0x004, JUSTIFY_MIDDLE = 0,
                         JUSTIFY_START = 0x008, JUSTIFY_END = 0x010, JUSTIFY_STRETCH = 0x018 };
        enum Anchor { CENTER = 0, LEFT = 0x020, BOTTOM = 0x040, RIGHT = 0x080, TOP = 0x100,
                      HFILL = 0x0A0, VFILL = 0x140, FILL = 0x1E0 };

        std::map<ItemID, osg::ref_ptr<osgText::Font>> fonts;
        std::map<ItemID, osg::ref_ptr<osgText::Text>> texts;
        std::map<ItemID, osg::ref_ptr<osg::Geometry>> buttons;
        std::map<ItemID, unsigned int> layoutItems;
        osg::ref_ptr<osg::Camera> canvasCamera;
        osg::ref_ptr<osg::Geode> textContainer;
        lay_context* layout;

        HeadUpDisplayCanvas();
        ~HeadUpDisplayCanvas();

        /** Create a text object on HUD canvas */
        bool createText(const ItemID& name, const std::wstring& text, int size, int width, int height,
                        const ItemID& parent = "root", Direction dir = ROW, unsigned int anchor = CENTER,
                        const std::string& font = "");

        /** Create a button on existing text object */
        bool setAsButton(const ItemID& name, OnClickEvent ev, const osg::Vec3& bkColor = osg::Vec3(0.3f, 0.3f, 0.3f),
                         const osg::Vec3& clickedColor = osg::Vec3(0.1f, 0.1f, 0.2f), const osg::Vec3& hoverColor = osg::Vec3(0.2f, 0.2f, 0.3f));
        
        /** Create 2 arrow buttons on existing text object */
        //bool setAsSelector(const ItemID& name, OnSelectEvent ev, const osg::Vec3& bkColor = osg::Vec3(0.3f, 0.3f, 0.3f),
        //                   const osg::Vec3& clickedColor = osg::Vec3(0.1f, 0.1f, 0.2f), const osg::Vec3& hoverColor = osg::Vec3(0.2f, 0.2f, 0.3f));

        /** Get items under current position */
        std::vector<HeadUpDisplayCanvas::ItemID> getItems(float x, float y) const;

        /** Create the HUD camera */
        osg::Camera* create(int width, int height);
    };

    /** Keyboard state buffering manager */
    class KeyboardCacher : public osg::Referenced
    {
    public:
        static KeyboardCacher* instance();
        void advance(const osgGA::GUIEventAdapter& ea);

        bool isKeyDown(int key) const;
        bool anyKeyDown(std::initializer_list<int> keys) const;
        bool allKeyDown(std::initializer_list<int> keys) const;

    protected:
        KeyboardCacher() {}
        std::unordered_map<int, bool> _keyStates;
    };

    /** File and message dialog object */
    class FileDialog : public osg::Referenced
    {
    public:
        enum NotifyLevel { Info, Warn, Error };
        enum ButtonGroup { Ok, OkCancel, YesNo, YesNoCancel };

        static void notify(NotifyLevel n, const std::string& title, const std::string& msg,
                           ButtonGroup group, int defaultBtn = 0);
        static std::string selectFolder(const std::string& title, const std::string& defPath = "");
    };

}

#endif
