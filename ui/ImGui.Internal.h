#pragma once
#include <osgGA/GUIEventAdapter>
#include <imgui/imgui.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace
{
    struct ImGuiInputEvent
    {
        enum Type { Key, MousePosition, MouseButtons, MouseWheel, VirtualMouse } type;
        int key, buttonMask; unsigned int modifiers; float x, y, wheel; bool down;
        ImGuiInputEvent() : type(Key), key(0), buttonMask(0), modifiers(0), x(0.0f), y(0.0f),
                            wheel(0.0f), down(false) {}

        static ImGuiInputEvent keyEvent(int keyCode, bool isDown, unsigned int modMask)
        {
            ImGuiInputEvent event = {};
            event.type = Key; event.key = keyCode; event.down = isDown;
            event.modifiers = modMask; return event;
        }

        static ImGuiInputEvent mousePositionEvent(float px, float py)
        {
            ImGuiInputEvent event = {};
            event.type = MousePosition; event.x = px; event.y = py; return event;
        }

        static ImGuiInputEvent mouseButtonsEvent(float px, float py, int mask)
        {
            ImGuiInputEvent event = mousePositionEvent(px, py);
            event.type = MouseButtons; event.buttonMask = mask; return event;
        }

        static ImGuiInputEvent mouseWheelEvent(float amount)
        {
            ImGuiInputEvent event = {};
            event.type = MouseWheel; event.wheel = amount; return event;
        }

        static ImGuiInputEvent virtualMouseEvent(float px, float py, int mask, float amount)
        {
            ImGuiInputEvent event = {};
            event.type = VirtualMouse; event.x = px; event.y = py;
            event.buttonMask = mask; event.wheel = amount; return event;
        }

        static float resolveImGuiWheelAmount(const osgGA::GUIEventAdapter& event)
        {
            osgGA::GUIEventAdapter::ScrollingMotion motion = event.getScrollingMotion();
            if (motion == osgGA::GUIEventAdapter::SCROLL_2D) return event.getScrollingDeltaY();
            if (motion == osgGA::GUIEventAdapter::SCROLL_UP) return 1.0f;
            if (motion == osgGA::GUIEventAdapter::SCROLL_DOWN) return -1.0f;
            return 0.0f;
        }

        static int convertImGuiCharacterKey(int key)
        {
            if (key >= 'a' && key <= 'z') return (int)ImGuiKey_A + (key - 'a');
            if (key >= 'A' && key <= 'Z') return (int)ImGuiKey_A + (key - 'A');
            if (key >= '0' && key <= '9') return (int)ImGuiKey_0 + (key - '0');
            switch (key)
            {
            case ' ': return ImGuiKey_Space; case ',': return ImGuiKey_Comma;
            case '-': return ImGuiKey_Minus; case '.': return ImGuiKey_Period;
            case '/': return ImGuiKey_Slash; case ';': return ImGuiKey_Semicolon;
            case '=': return ImGuiKey_Equal; case '[': return ImGuiKey_LeftBracket;
            case '\\': return ImGuiKey_Backslash; case ']': return ImGuiKey_RightBracket;
            case '`': return ImGuiKey_GraveAccent; case '\'': return ImGuiKey_Apostrophe;
            default: return ImGuiKey_None;
            }
        }

        static int convertImGuiSpecialKey(int key)
        {
            if (key >= osgGA::GUIEventAdapter::KEY_F1 && key <= osgGA::GUIEventAdapter::KEY_F24)
                return (int)ImGuiKey_F1 + (key - osgGA::GUIEventAdapter::KEY_F1);
            switch (key)
            {
            case osgGA::GUIEventAdapter::KEY_Tab: return ImGuiKey_Tab;
            case osgGA::GUIEventAdapter::KEY_Left: return ImGuiKey_LeftArrow;
            case osgGA::GUIEventAdapter::KEY_Right: return ImGuiKey_RightArrow;
            case osgGA::GUIEventAdapter::KEY_Up: return ImGuiKey_UpArrow;
            case osgGA::GUIEventAdapter::KEY_Down: return ImGuiKey_DownArrow;
            case osgGA::GUIEventAdapter::KEY_Page_Up: return ImGuiKey_PageUp;
            case osgGA::GUIEventAdapter::KEY_Page_Down: return ImGuiKey_PageDown;
            case osgGA::GUIEventAdapter::KEY_Home: return ImGuiKey_Home;
            case osgGA::GUIEventAdapter::KEY_End: return ImGuiKey_End;
            case osgGA::GUIEventAdapter::KEY_Delete: return ImGuiKey_Delete;
            case osgGA::GUIEventAdapter::KEY_Insert: return ImGuiKey_Insert;
            case osgGA::GUIEventAdapter::KEY_BackSpace: return ImGuiKey_Backspace;
            case osgGA::GUIEventAdapter::KEY_Return: return ImGuiKey_Enter;
            case osgGA::GUIEventAdapter::KEY_Escape: return ImGuiKey_Escape;
            case osgGA::GUIEventAdapter::KEY_Caps_Lock: return ImGuiKey_CapsLock;
            case osgGA::GUIEventAdapter::KEY_KP_Enter: return ImGuiKey_KeypadEnter;
            default: return -1;
            }
        }

        static void applyImGuiInputEvents(ImGuiIO& io, const std::vector<ImGuiInputEvent>& events)
        {
            for (std::vector<ImGuiInputEvent>::const_iterator it = events.begin(); it != events.end(); ++it)
            {
                const ImGuiInputEvent& event = *it;
                if (event.type == ImGuiInputEvent::Key)
                {
                    const unsigned int mod = event.modifiers;
                    io.AddKeyEvent(ImGuiMod_Ctrl, (mod & osgGA::GUIEventAdapter::MODKEY_CTRL) != 0);
                    io.AddKeyEvent(ImGuiMod_Shift, (mod & osgGA::GUIEventAdapter::MODKEY_SHIFT) != 0);
                    io.AddKeyEvent(ImGuiMod_Alt, (mod & osgGA::GUIEventAdapter::MODKEY_ALT) != 0);
                    io.AddKeyEvent(ImGuiMod_Super, (mod & osgGA::GUIEventAdapter::MODKEY_SUPER) != 0);

                    const int specialKey = convertImGuiSpecialKey(event.key);
                    if (specialKey > 0)
                        io.AddKeyEvent((ImGuiKey)specialKey, event.down);
                    else if (event.key > 0 && event.key < 0xFF)
                    {
                        io.AddKeyEvent((ImGuiKey)convertImGuiCharacterKey(event.key), event.down);
                        if (event.down) io.AddInputCharacter((unsigned short)event.key);
                    }
                    else if (event.key >= 0x100 && event.key < 0xE000 &&
                            (event.key < 0xD800 || event.key > 0xDFFF) && event.down)
                    { io.AddInputCharacter((unsigned int)event.key); }
                }
                else if (event.type == ImGuiInputEvent::MousePosition)
                    io.AddMousePosEvent(event.x, io.DisplaySize.y - event.y);
                else if (event.type == ImGuiInputEvent::MouseButtons)
                {
                    io.AddMousePosEvent(event.x, io.DisplaySize.y - event.y);
                    io.AddMouseButtonEvent(0, (event.buttonMask & osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON) != 0);
                    io.AddMouseButtonEvent(1, (event.buttonMask & osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON) != 0);
                    io.AddMouseButtonEvent(2, (event.buttonMask & osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON) != 0);
                }
                else if (event.type == ImGuiInputEvent::MouseWheel)
                    io.AddMouseWheelEvent(0.0f, event.wheel);
                else if (event.type == ImGuiInputEvent::VirtualMouse)
                {
                    io.AddMousePosEvent(io.DisplaySize.x * event.x, io.DisplaySize.y * event.y);
                    io.AddMouseButtonEvent(0, (event.buttonMask & osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON) != 0);
                    io.AddMouseButtonEvent(1, (event.buttonMask & osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON) != 0);
                    io.AddMouseButtonEvent(2, (event.buttonMask & osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON) != 0);
                    io.AddMouseWheelEvent(0.0f, event.wheel);
                }
            }  // for (events...)
        }
    };

    class ImGuiInputQueue
    {
    public:
        ImGuiInputQueue() : _wantsMouse(false), _wantsKeyboard(false) {}
        bool wantsMouse() const { return _wantsMouse.load(); }
        bool wantsKeyboard() const { return _wantsKeyboard.load(); }

        void push(const ImGuiInputEvent& event)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _events.push_back(event);
        }

        std::vector<ImGuiInputEvent> takeAll()
        {
            std::vector<ImGuiInputEvent> result;
            std::lock_guard<std::mutex> lock(_mutex);
            result.swap(_events); return result;
        }

        void publishCapture(bool mouse, bool keyboard)
        { _wantsMouse.store(mouse); _wantsKeyboard.store(keyboard); }

    private:
        std::vector<ImGuiInputEvent> _events;
        std::atomic<bool> _wantsMouse, _wantsKeyboard;
        std::mutex _mutex;
    };
}
