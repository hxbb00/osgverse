extern "C"
{
#   include <3rdparty/tinyfiledialogs.h>
}
#define LAY_IMPLEMENTATION
#include "3rdparty/layout.h"

#include "modeling/Utilities.h"
#include "pipeline/Utilities.h"
#include "Utilities.h"
#include <osgGA/EventVisitor>
#include <iostream>
using namespace osgVerse;

/// HeadUpDisplayCanvas ///

namespace
{
    class HeadUpDisplayEventCallback : public osg::NodeCallback
    {
    public:
        HeadUpDisplayEventCallback(HeadUpDisplayCanvas* c, int w, int h)
        :   _canvas(c), _width(w), _height(h) {}

        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            osgGA::EventVisitor* ev = static_cast<osgGA::EventVisitor*>(nv);
            if (!ev) { traverse(node, nv); return; }
            
            osgGA::EventQueue::Events& events = ev->getEvents();
            for (osgGA::EventQueue::Events::iterator itr = events.begin(); itr != events.end(); ++itr)
            {
                osgGA::GUIEventAdapter* ea = dynamic_cast<osgGA::GUIEventAdapter*>(itr->get());
                if (!ea) continue;
                
                float xx = (0.5f + ea->getXnormalized() * 0.5f) * (float)_width;
                float yy = (0.5f + ea->getYnormalized() * 0.5f) * (float)_height;
                if (ea->getEventType() == osgGA::GUIEventAdapter::PUSH)
                {   
                }
                else if (ea->getEventType() == osgGA::GUIEventAdapter::RELEASE)
                {
                }
                else if (ea->getEventType() == osgGA::GUIEventAdapter::MOVE)
                {
                    std::vector<std::string> items = _canvas->getItems(xx, yy);
                    //
                }
                else if (ea->getEventType() == osgGA::GUIEventAdapter::RESIZE)
                {
                    float rX = (float)ea->getWindowWidth() / _width;
                    float rY = (float)ea->getWindowHeight() / _height;
                    if (!osg::equivalent(rX, 1.0f) || !osg::equivalent(rY, 1.0f))
                    {
                        // Refresh all items
                        for (std::map<std::string, unsigned int>::iterator it = _canvas->layoutItems.begin();
                             it != _canvas->layoutItems.end(); ++it)
                        {
                            lay_scalar w = 0, h = 0; lay_get_size_xy(_canvas->layout, it->second, &w, &h);
                            lay_set_size_xy(_canvas->layout, it->second, (lay_scalar)(w * rX), (lay_scalar)(h * rY));
                        }
                        lay_run_context(_canvas->layout); updatePositions();
                        _width = ea->getWindowWidth(); _height = ea->getWindowHeight();
                    }
                }
            }
            traverse(node, nv);
        }

    protected:
        void updatePositions()
        {
            for (std::map<std::string, unsigned int>::iterator it = _canvas->layoutItems.begin();
                 it != _canvas->layoutItems.end(); ++it)
            {
                std::map<std::string, osg::ref_ptr<osgText::Text>>::iterator tIt = _canvas->texts.find(it->first);
                osgText::Text* textObj = (tIt == _canvas->texts.end()) ? NULL : tIt->second.get();
                if (textObj)
                {
                    lay_vec4 r = lay_get_rect(_canvas->layout, it->second);
                    textObj->setPosition(osg::Vec3(r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0.0f));
                }

                std::map<std::string, osg::ref_ptr<osg::Geometry>>::iterator gIt = _canvas->buttons.find(it->first);
                osg::Geometry* btnObj = (gIt == _canvas->buttons.end()) ? NULL : gIt->second.get();
                if (btnObj)
                {
                    lay_vec4 r = lay_get_rect(_canvas->layout, it->second);
                    osg::Vec3Array* va = static_cast<osg::Vec3Array*>(btnObj->getVertexArray());
                    if (va && va->size() == 6)
                    {
                        (*va)[0] = osg::Vec3(r[0], r[1], -0.1f); (*va)[1] = osg::Vec3(r[0] + r[2], r[1], -0.1f);
                        (*va)[2] = osg::Vec3(r[0] + r[2], r[1] + r[3], -0.1f); (*va)[3] = osg::Vec3(r[0], r[1], -0.1f);
                        (*va)[4] = osg::Vec3(r[0] + r[2], r[1] + r[3], -0.1f); (*va)[5] = osg::Vec3(r[0], r[1] + r[3], -0.1f);
                    }
                    btnObj->dirtyBound(); va->dirty();
                }
            }
        }

        HeadUpDisplayCanvas* _canvas;
        int _width, _height;
    };
}

HeadUpDisplayCanvas::HeadUpDisplayCanvas()
{
    layout = new lay_context;
    lay_init_context(layout);
    lay_reserve_items_capacity(layout, 1024);
}

HeadUpDisplayCanvas::~HeadUpDisplayCanvas()
{
    lay_destroy_context(layout); delete layout;
}

bool HeadUpDisplayCanvas::createText(const std::string& name, const std::wstring& text, int size,
                                     int width, int height, const std::string& parent, Direction dir,
                                     unsigned int anchor, const std::string& font)
{
    if (layoutItems.find(name) != layoutItems.end()) return true;
    if (layoutItems.find(parent) == layoutItems.end()) return false;
    unsigned int item = lay_item(layout); layoutItems[name] = item;

    lay_insert(layout, layoutItems[parent], item);
    lay_set_size_xy(layout, item, width, height);
    lay_set_behave(layout, item, (unsigned int)anchor);
    lay_set_contain(layout, item, (unsigned int)dir);
    lay_run_context(layout);

    lay_vec4 rect = lay_get_rect(layout, item);
    osgText::Text* textObj = new osgText::Text;
    textObj->setPosition(osg::Vec3(rect[0] + rect[2] * 0.5f, rect[1] + rect[3] * 0.5f, 0.0f));
    textObj->setAlignment(osgText::Text::CENTER_CENTER);
    textObj->setCharacterSize(size, 1.0f);
    textObj->setText(text.c_str());
    
    osgText::Font* f = fonts[font].get();
    if (!f) { f = osgText::readFontFile(font); fonts[font] = f; }
    if (f) textObj->setFont(f);
    textContainer->addDrawable(textObj);
    texts[name] = textObj; return true;
}

bool HeadUpDisplayCanvas::setAsButton(const ItemID& name, OnClickEvent ev, const osg::Vec3& bkColor,
                                      const osg::Vec3& clickedColor, const osg::Vec3& hoverColor)
{
    if (layoutItems.find(name) == layoutItems.end()) return false;
    unsigned int btnItem = lay_item(layout), parent = layoutItems[name];
    layoutItems["BTN_" + name] = btnItem;

    lay_scalar w = 0, h = 0; lay_get_size_xy(layout, parent, &w, &h);
    lay_insert(layout, parent, btnItem); lay_set_size_xy(layout, btnItem, w, h);
    lay_set_behave(layout, btnItem, (unsigned int)ROW);
    lay_set_contain(layout, btnItem, (unsigned int)CENTER);
    lay_run_context(layout);

    lay_vec4 rect = lay_get_rect(layout, btnItem);
    osg::Vec3Array *va = new osg::Vec3Array(6), *na = new osg::Vec3Array(6); na->assign(6, osg::Z_AXIS);
    (*va)[0] = osg::Vec3(rect[0], rect[1], -0.1f); (*va)[1] = osg::Vec3(rect[0] + rect[2], rect[1], -0.1f);
    (*va)[2] = osg::Vec3(rect[0] + rect[2], rect[1] + rect[3], -0.1f); (*va)[3] = osg::Vec3(rect[0], rect[1], -0.1f);
    (*va)[4] = osg::Vec3(rect[0] + rect[2], rect[1] + rect[3], -0.1f); (*va)[5] = osg::Vec3(rect[0], rect[1] + rect[3], -0.1f);
    
    osg::Geometry* quad = createGeometry(va, na, osg::Vec4(bkColor, 1.0f), new osg::DrawArrays(GL_TRIANGLES, 0, va->size()),
                                         true, true, false);
    textContainer->addDrawable(quad); buttons["BTN_" + name] = quad; return true;
}

std::vector<HeadUpDisplayCanvas::ItemID> HeadUpDisplayCanvas::getItems(float x, float y) const
{
    std::vector<HeadUpDisplayCanvas::ItemID> items;
    for (std::map<ItemID, unsigned int>::const_iterator it = layoutItems.begin();
         it != layoutItems.end(); ++it)
    {
        lay_vec4 rect = lay_get_rect(layout, it->second);
        if (it->first == "root") continue;

        if (rect[0] <= x && rect[1] <= y &&
            x <= (rect[0] + rect[2]) && y <= (rect[1] + rect[3]))
        { items.push_back(it->first); }
    }
    return items;
}

osg::Camera* HeadUpDisplayCanvas::create(int width, int height)
{
    // https://github.com/randrew/layout
    layoutItems.clear(); lay_reset_context(layout);
    unsigned int root = lay_item(layout); layoutItems["root"] = root;
    lay_set_size_xy(layout, root, width, height);
    lay_set_contain(layout, root, LAY_ROW);

    textContainer = new osg::Geode;
    textContainer->setName("Canvas_TextContainer");
    fonts[""] = osgText::readFontFile(MISC_DIR + "LXGWFasmartGothic.ttf");

    canvasCamera = createHUDCamera(NULL, width, height);
    canvasCamera->addChild(textContainer.get());
    canvasCamera->setAllowEventFocus(true);
    canvasCamera->setEventCallback(new HeadUpDisplayEventCallback(this, width, height));
    canvasCamera->setName("Canvas");
    return canvasCamera.get();
}

/// KeyboardCacher ///
KeyboardCacher* KeyboardCacher::instance()
{
    static osg::ref_ptr<KeyboardCacher> s_instance = new KeyboardCacher;
    return s_instance.get();
}

void KeyboardCacher::advance(const osgGA::GUIEventAdapter& ea)
{
    if (ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN)
    {
        int key = ea.getKey(); if (_keyStates[key]) return;
        _keyStates[key] = true;
    }
    else if (ea.getEventType() == osgGA::GUIEventAdapter::KEYUP)
    {
        int key = ea.getKey(); if (!_keyStates[key]) return;
        _keyStates[key] = false;
    }
}

bool KeyboardCacher::isKeyDown(int key) const
{
    std::unordered_map<int, bool>::const_iterator it = _keyStates.find(key);
    if (it == _keyStates.end()) return false; else return it->second;
}

bool KeyboardCacher::anyKeyDown(std::initializer_list<int> keys) const
{
    for (int k : keys) { if (isKeyDown(k)) return true; }
    return false;
}

bool KeyboardCacher::allKeyDown(std::initializer_list<int> keys) const
{
    for (int k : keys) { if (!isKeyDown(k)) return false; }
    return true;
}

/// FileDialog ///
namespace
{
    static const char* obtainIconType(FileDialog::NotifyLevel n)
    {
        switch (n)
        {
        case FileDialog::Warn: return "warning";
        case FileDialog::Error: return "error";
        default: return "info";
        }
    }

    static const char* obtainButtonType(FileDialog::ButtonGroup g)
    {
        switch (g)
        {
        case FileDialog::OkCancel: return "okcancel";
        case FileDialog::YesNo: return "yesno";
        case FileDialog::YesNoCancel: return "yesnocancel";
        default: return "ok";
        }
    }
}

void FileDialog::notify(NotifyLevel n, const std::string& title, const std::string& msg,
                        ButtonGroup g, int defaultBtn)
{ tinyfd_messageBox(title.c_str(), msg.c_str(), obtainButtonType(g), obtainIconType(n), defaultBtn); }

std::string FileDialog::selectFolder(const std::string& title, const std::string& defPath)
{
    char* result = tinyfd_selectFolderDialog(title.c_str(), defPath.c_str());
    return result ? std::string(result) : "";
}

