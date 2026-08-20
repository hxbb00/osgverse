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
                    std::vector<HeadUpDisplayCanvas::ItemPosition> items = _canvas->getItems(xx, yy, true);
                    for (size_t i = 0; i < items.size(); ++i)
                    {   // Push on buttons
                        HeadUpDisplayCanvas::ButtonInfo& info = _canvas->buttons[items[i].first];
                        if (setButtonColor(info, 1, items[i].second)) _pushed[items[i].first] = items[i].second;
                    }
                }
                else if (ea->getEventType() == osgGA::GUIEventAdapter::RELEASE)
                {
                    for (std::map<std::string, osg::Vec2>::iterator it = _pushed.begin();
                         it != _pushed.end(); ++it)
                    {   // Click on buttons
                        HeadUpDisplayCanvas::ButtonInfo& info = _canvas->buttons[it->first];
                        if (info.selected) info.selected(it->second[0] < 0.2f, it->second[0] > 0.8f);
                        if (info.clicked) info.clicked(); setButtonColor(info, 0, it->second);
                    }
                    _pushed.clear();
                }
                else if (ea->getEventType() == osgGA::GUIEventAdapter::MOVE)
                {
                    std::vector<HeadUpDisplayCanvas::ItemPosition> items = _canvas->getItems(xx, yy, true);
                    std::map<std::string, osg::Vec2> toClearHover; toClearHover.swap(_hovered);
                    for (size_t i = 0; i < items.size(); ++i)
                    {   // Hover begin on buttons
                        HeadUpDisplayCanvas::ButtonInfo& info = _canvas->buttons[items[i].first];
                        if (setButtonColor(info, 2, items[i].second)) _hovered[items[i].first] = items[i].second;
                    }

                    for (std::map<std::string, osg::Vec2>::iterator it = toClearHover.begin();
                         it != toClearHover.end(); ++it)
                    {   // Hover end on buttons
                        HeadUpDisplayCanvas::ButtonInfo& info = _canvas->buttons[it->first];
                        if (_hovered.find(it->first) == _hovered.end()) setButtonColor(info, 0, it->second);
                    }
                }
                else if (ea->getEventType() == osgGA::GUIEventAdapter::RESIZE)
                {
                    float rX = (float)ea->getWindowWidth() / _width;
                    float rY = (float)ea->getWindowHeight() / _height;
                    if (!osg::equivalent(rX, 1.0f) || !osg::equivalent(rY, 1.0f))
                    {   // Refresh all items
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
                lay_vec4 r = lay_get_rect(_canvas->layout, it->second);
                osgText::Text* textObj = (tIt == _canvas->texts.end()) ? NULL : tIt->second.get();
                if (textObj) textObj->setPosition(osg::Vec3(r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0.0f));

                std::map<std::string, HeadUpDisplayCanvas::ButtonInfo>::iterator gIt = _canvas->buttons.find(it->first);
                if (gIt == _canvas->buttons.end()) continue;

                const std::vector<osg::ref_ptr<osg::Geometry>>& geomList = gIt->second.shapes;
                for (size_t i = 0; i < geomList.size(); ++i)
                {
                    osg::Geometry* btnObj = geomList[i].get();
                    osg::Vec3Array* va = static_cast<osg::Vec3Array*>(btnObj->getVertexArray());
                    if (va->size() == 6)
                    {
                        r = lay_get_rect(_canvas->layout, _canvas->layoutItems["BTN_" + it->first]);
                        (*va)[0] = osg::Vec3(r[0], r[1], -0.1f); (*va)[1] = osg::Vec3(r[0] + r[2], r[1], -0.1f);
                        (*va)[2] = osg::Vec3(r[0] + r[2], r[1] + r[3], -0.1f); (*va)[3] = osg::Vec3(r[0], r[1], -0.1f);
                        (*va)[4] = osg::Vec3(r[0] + r[2], r[1] + r[3], -0.1f); (*va)[5] = osg::Vec3(r[0], r[1] + r[3], -0.1f);
                    }
                    else if (va->size() == 3)
                    {
                        if (i == 0)  // left selector
                        {
                            r = lay_get_rect(_canvas->layout, _canvas->layoutItems["BTN0_" + it->first]);
                            (*va)[0] = osg::Vec3(r[0] + r[2], r[1] + r[3], -0.1f); (*va)[1] = osg::Vec3(r[0] + r[2], r[1], -0.1f);
                            (*va)[2] = osg::Vec3(r[0], r[1] + r[3] * 0.5f, -0.1f);
                        }
                        else  // right selector
                        {
                            r = lay_get_rect(_canvas->layout, _canvas->layoutItems["BTN1_" + it->first]);
                            (*va)[0] = osg::Vec3(r[0], r[1] + r[3], -0.1f); (*va)[1] = osg::Vec3(r[0], r[1], -0.1f);
                            (*va)[2] = osg::Vec3(r[0] + r[2], r[1] + r[3] * 0.5f, -0.1f);
                        }
                    }
                    btnObj->dirtyBound(); va->dirty();
                }
            }
        }

        bool setButtonColor(HeadUpDisplayCanvas::ButtonInfo& info, int state, const osg::Vec2& pos)
        {
            const std::vector<osg::ref_ptr<osg::Geometry>>& geomList = info.shapes;
            if (geomList.size() == 2)  // selector
            {
                int id = (pos[0] < 0.2f) ? 0 : (pos[0] > 0.8f ? 1 : -1); if (id < 0) return false;
                osg::Vec4Array* ca = static_cast<osg::Vec4Array*>(geomList[id]->getColorArray());
                if (ca) { ca->dirty(); ca->assign(ca->size(), osg::Vec4(info.colors[state], 1.0f)); }
            }
            else if (geomList.size() == 1)  // button
            {
                osg::Vec4Array* ca = static_cast<osg::Vec4Array*>(geomList[0]->getColorArray());
                if (ca) { ca->dirty(); ca->assign(ca->size(), osg::Vec4(info.colors[state], 1.0f)); }
            }
            return true;
        }

        std::map<std::string, osg::Vec2> _pushed, _hovered;
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
                                     int width, int height, const std::string& parent, ChildLayout ch,
                                     unsigned int anchor, const std::string& font)
{
    if (layoutItems.find(name) != layoutItems.end()) return true;
    if (layoutItems.find(parent) == layoutItems.end()) return false;
    unsigned int item = lay_item(layout); layoutItems[name] = item;

    lay_insert(layout, layoutItems[parent], item);
    lay_set_size_xy(layout, item, width, height);
    lay_set_behave(layout, item, (unsigned int)anchor);
    lay_set_contain(layout, item, (unsigned int)ch);
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
    lay_set_behave(layout, btnItem, (unsigned int)CENTER);
    lay_set_contain(layout, btnItem, (unsigned int)FREE);
    lay_run_context(layout);

    lay_vec4 rect = lay_get_rect(layout, btnItem);
    osg::Vec3Array *va = new osg::Vec3Array(6), *na = new osg::Vec3Array(6); na->assign(6, osg::Z_AXIS);
    (*va)[0] = osg::Vec3(rect[0], rect[1], -0.1f); (*va)[1] = osg::Vec3(rect[0] + rect[2], rect[1], -0.1f);
    (*va)[2] = osg::Vec3(rect[0] + rect[2], rect[1] + rect[3], -0.1f); (*va)[3] = osg::Vec3(rect[0], rect[1], -0.1f);
    (*va)[4] = osg::Vec3(rect[0] + rect[2], rect[1] + rect[3], -0.1f); (*va)[5] = osg::Vec3(rect[0], rect[1] + rect[3], -0.1f);
    
    osg::Geometry* quad = createGeometry(va, na, osg::Vec4(bkColor, 1.0f), new osg::DrawArrays(GL_TRIANGLES, 0, va->size()),
                                         true, true, false);
    buttons[name] = ButtonInfo { {quad}, {bkColor, clickedColor, hoverColor}, ev, NULL };
    textContainer->addDrawable(quad); return true;
}

bool HeadUpDisplayCanvas::setAsSelector(const ItemID& name, OnSelectEvent ev, const osg::Vec3& bkColor,
                                        const osg::Vec3& clickedColor, const osg::Vec3& hoverColor)
{
    if (layoutItems.find(name) == layoutItems.end()) return false;
    unsigned int b0 = lay_item(layout), b1 = lay_item(layout), parent = layoutItems[name];
    layoutItems["BTN0_" + name] = b0; layoutItems["BTN1_" + name] = b1;

    lay_scalar w = 0, h = 0; lay_get_size_xy(layout, parent, &w, &h);
    lay_insert(layout, parent, b0); lay_set_size_xy(layout, b0, lay_scalar(w * 0.2f), h);
    lay_insert(layout, parent, b1); lay_set_size_xy(layout, b1, lay_scalar(w * 0.2f), h);
    lay_set_behave(layout, b0, (unsigned int)LEFT); lay_set_behave(layout, b1, (unsigned int)RIGHT);
    lay_set_contain(layout, b0, (unsigned int)FREE); lay_set_contain(layout, b1, (unsigned int)FREE);
    lay_run_context(layout);

    lay_vec4 r0 = lay_get_rect(layout, b0), r1 = lay_get_rect(layout, b1);
    osg::Vec3Array *va = new osg::Vec3Array(3), *na = new osg::Vec3Array(3); na->assign(3, osg::Z_AXIS);
    (*va)[0] = osg::Vec3(r0[0] + r0[2], r0[1] + r0[3], -0.1f); (*va)[1] = osg::Vec3(r0[0] + r0[2], r0[1], -0.1f);
    (*va)[2] = osg::Vec3(r0[0], r0[1] + r0[3] * 0.5f, -0.1f);
    osg::Geometry* q1 = createGeometry(va, na, osg::Vec4(bkColor, 1.0f), new osg::DrawArrays(GL_TRIANGLES, 0, va->size()),
                                       true, true, false);
    va = new osg::Vec3Array(3);
    (*va)[0] = osg::Vec3(r1[0], r1[1] + r1[3], -0.1f); (*va)[1] = osg::Vec3(r1[0], r1[1], -0.1f);
    (*va)[2] = osg::Vec3(r1[0] + r1[2], r1[1] + r1[3] * 0.5f, -0.1f);
    osg::Geometry* q2 = createGeometry(va, na, osg::Vec4(bkColor, 1.0f), new osg::DrawArrays(GL_TRIANGLES, 0, va->size()),
                                       true, true, false);
    buttons[name] = ButtonInfo { {q1, q2}, {bkColor, clickedColor, hoverColor}, NULL, ev };
    textContainer->addDrawable(q1); textContainer->addDrawable(q2); return true;
}

std::vector<HeadUpDisplayCanvas::ItemPosition> HeadUpDisplayCanvas::getItems(float x, float y, bool onlyButtons) const
{
    std::vector<HeadUpDisplayCanvas::ItemPosition> items;
    for (std::map<ItemID, unsigned int>::const_iterator it = layoutItems.begin();
         it != layoutItems.end(); ++it)
    {
        if (onlyButtons && buttons.find(it->first) == buttons.end()) continue;
        else if (it->first == "root") continue;

        lay_vec4 rect = lay_get_rect(layout, it->second);
        if (rect[0] <= x && rect[1] <= y &&
            x <= (rect[0] + rect[2]) && y <= (rect[1] + rect[3]))
        {
            osg::Vec2 pos((x - (float)rect[0]) / (float)rect[2], (y - (float)rect[1]) / (float)rect[3]);
            items.push_back(ItemPosition(it->first, pos));
        }
    }
    return items;
}

osg::Camera* HeadUpDisplayCanvas::create(int width, int height, ChildLayout globalLayout)
{
    // https://github.com/randrew/layout
    layoutItems.clear(); lay_reset_context(layout);
    unsigned int root = lay_item(layout); layoutItems["root"] = root;
    lay_set_size_xy(layout, root, width, height);
    lay_set_contain(layout, root, globalLayout);

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

