#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cocos2d.h>
#include <imgui.h>
#include <imgui_hook.h>
#include <MinHook.h>
#include <queue>
#include <mutex>
#include <fstream>
#include <sstream>

#define GD_CONSOLE

using namespace cocos2d;

const char* getNodeName(CCNode* node) {
    if (node == nullptr) return "nullptr";
    const char* name = typeid(*node).name() + 6;
    return name;
}

void clipboardText(const char* text) {
    if (!OpenClipboard(NULL)) return;
    if (!EmptyClipboard()) return;
    auto len = std::strlen(text);
    auto mem = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    memcpy(GlobalLock(mem), text, len + 1);
    GlobalUnlock(mem);
    SetClipboardData(CF_TEXT, mem);
    CloseClipboard();
}

bool operator!=(const CCSize& a, const CCSize& b) { return a.width != b.width || a.height != b.height; }

std::queue<std::function<void()>> threadFunctions;
std::mutex threadFunctionsMutex;
enum edit_t { eNormal, eEdit, } editMode;
CCNode* highlightedNode = nullptr;
CCNode* selectedNode = nullptr;
CCPoint clickOffset;
bool snapEnabled = true;
bool snapWindowEnabled = true;
constexpr const float strokeSize = 3.0f;
std::vector<int> openLocation;

std::vector<int> getNodeLocationInTree(CCNode* node) {
    auto c = node;

    std::vector<int> res;

    while (c->getParent()) {
        auto par = c->getParent();
        res.push_back(par->getChildren()->indexOfObject(c));
        c = par;
    }

    res.push_back(0);

    std::reverse(res.begin(), res.end());

    return res;
}

bool filterNode(CCNode* node) {
    if (dynamic_cast<CCLayer*>(node))
        return false;
    if (dynamic_cast<CCMenu*>(node))
        return false;

    return true;
}

bool stopCheckingChildren(CCNode* node) {
    if (dynamic_cast<CCMenuItem*>(node))
        return true;
    if (dynamic_cast<CCLabelBMFont*>(node))
        return true;

    return false;
}

void getNodesUnderMouse(CCNode* parent, CCArray* res, CCPoint mpos) {
    CCObject* obj;
    CCARRAY_FOREACH(parent->getChildren(), obj) {
        auto node = reinterpret_cast<CCNode*>(obj);

        if (!node) continue;

        auto pos = node->getPosition();
        auto size = node->getScaledContentSize();
        auto rect = CCRect { pos.x, pos.y, size.width, size.height };

        rect.origin = rect.origin - rect.size / 2;

        auto mposn = node->getParent()->convertToNodeSpace(mpos);

        if (rect.containsPoint(mposn) && filterNode(node))
            res->addObject(node);
        
        if (node->getChildrenCount() && !stopCheckingChildren(node))
            getNodesUnderMouse(node, res, mpos);
    }
}

const char* getRectText(CCRect const& rect) {
    return std::string(
        std::to_string(rect.origin.x) + ", " +
        std::to_string(rect.origin.y) + "; " +
        std::to_string(rect.size.width) + " : " +
        std::to_string(rect.size.height)
    ).c_str();
}

CCNode* getTopMost(CCArray* nodes) {
    CCObject* obj;
    CCNode* res = nullptr;
    CCARRAY_FOREACH(nodes, obj) {
        auto node = reinterpret_cast<CCNode*>(obj);

        if (!res || node->getZOrder() >= res->getZOrder())
                res = node;
    }

    return res;
}

CCRect operator/=(CCRect & rect, CCSize const& size) {
    rect.origin.x /= size.width;
    rect.origin.y /= size.height;
    rect.size.width /= size.width;
    rect.size.height /= size.height;

    return rect;
}

CCPoint operator/=(CCPoint & point, CCSize const& size) {
    point.x /= size.width;
    point.y /= size.height;

    return point;
}

HWND glfwGetWin32Window(GLFWwindow* wnd) {
    auto cocosBase = GetModuleHandleA("libcocos2d.dll");

    auto pRet = reinterpret_cast<HWND(__cdecl*)(GLFWwindow*)>(
        reinterpret_cast<uintptr_t>(cocosBase) + 0x112c10
    )(wnd);

    return pRet;
}

CCRect operator*=(CCRect & rect, CCSize const& size) {
    rect.origin.x *= size.width;
    rect.origin.y *= size.height;
    rect.size.width *= size.width;
    rect.size.height *= size.height;

    return rect;
}

ImVec2 ccpToImVec2(CCPoint const& p) {
    return { p.x, p.y };
}

ImVec2 convertGlobalPointToWindowSpace(CCPoint const& p) {
    ImVec2 res;

    auto winSize = CCDirector::sharedDirector()->getWinSize();
    auto eg = CCDirector::sharedDirector()->getOpenGLView();
    auto hWnd = glfwGetWin32Window(eg->getWindow());

    RECT wRect;
    GetClientRect(hWnd, &wRect);
    
    float winWidth = static_cast<float>(wRect.right);
    float winHeight = static_cast<float>(wRect.bottom);

    res.x = p.x / winSize.width * winWidth;
    res.y = p.y / winSize.height * winHeight;

    // flip
    res.y = winHeight - res.y;

    return res;
}

void highlightNode(CCNode* node, bool sel = false) {
    if (!node) return;

    auto eg = CCDirector::sharedDirector()->getOpenGLView();
    auto winSize = CCDirector::sharedDirector()->getWinSize();

    ImDrawList& list = *ImGui::GetForegroundDrawList();

    auto pos = node->getParent()->convertToWorldSpace(node->getPosition());
    auto size = node->getScaledContentSize();
    auto rect = CCRect { pos.x, pos.y, size.width, size.height };

    rect.origin = rect.origin - rect.size / 2;

    const auto [winWidth, winHeight] = ImGui::GetMainViewport()->Size;

    rect /= winSize;
    rect *= CCSize { winWidth, winHeight };

    // flip
    rect.origin.y = winHeight - rect.origin.y;
    rect.origin.y -= rect.size.height;

    if (sel)
        list.AddRect(
            ccpToImVec2(rect.origin),
            ccpToImVec2(rect.origin + rect.size),
            0xff00ff00,
            0.0f, 15, strokeSize
        );
    else
        list.AddRectFilled(
            ccpToImVec2(rect.origin),
            ccpToImVec2(rect.origin + rect.size),
            0x3300ff00
        );
}

CCPoint getRelativeMousePos() {
    auto winSize = CCDirector::sharedDirector()->getWinSize();
    auto winSizePx = CCDirector::sharedDirector()->getOpenGLView()->getViewPortRect();
    auto ratio_w = winSize.width / winSizePx.size.width;
    auto ratio_h = winSize.height / winSizePx.size.height;

    auto mpos = CCDirector::sharedDirector()->getOpenGLView()->getMousePosition();
    // the mouse position is stored from the top-left while cocos
    // coordinates are from the bottom-left
    mpos.y = winSizePx.size.height - mpos.y;

    mpos.x *= ratio_w;  // scale mouse position to be in
    mpos.y *= ratio_h;  // cocos2d coordinate space

    return mpos;
}

void snapNodePosition(CCNode* node) {
    if (node->getParent()->getChildrenCount() == 1)
        return;
    
    if (!snapEnabled)
        return;
    
    float threshold = 10.0f;

    auto pos = node->getPosition();

    CCNode* closestXNode = nullptr, *closestYNode = nullptr;
    float closestX, closestY;
    CCObject* obj;
    CCARRAY_FOREACH(node->getParent()->getChildren(), obj) {
        if (obj == node)
            continue;
        
        auto nobj = reinterpret_cast<CCNode*>(obj);

        if (nobj) {
            if (!closestXNode) {
                closestXNode = nobj;
                closestYNode = nobj;
                closestX = fabsf(nobj->getPositionX() - node->getPositionX());
                closestY = fabsf(nobj->getPositionY() - node->getPositionY());
                continue;
            }

            if (fabsf(nobj->getPositionX() - node->getPositionX()) < closestX) {
                closestXNode = nobj;
                closestX = fabsf(nobj->getPositionX() - node->getPositionX());
            }
            if (fabsf(nobj->getPositionY() - node->getPositionY()) < closestY) {
                closestYNode = nobj;
                closestY = fabsf(nobj->getPositionY() - node->getPositionY());
            }
        }
    }

    CCPoint wx, wy;
    CCPoint gwx, gwy;

    if (snapWindowEnabled) {
        auto wposa = static_cast<CCPoint>(CCDirector::sharedDirector()->getWinSize() / 2);
        auto wpos = node->getParent()->convertToNodeSpace(wposa);

        if (fabsf(wpos.x - node->getPositionX()) <= closestX) {
            closestXNode = nullptr;
            closestX = fabsf(wpos.x - node->getPositionX());
            wx = wpos;
            gwx = wposa;
        }
        if (fabsf(wpos.y - node->getPositionY()) <= closestY) {
            closestYNode = nullptr;
            closestY = fabsf(wpos.y - node->getPositionY());
            wy = wpos;
            gwy = wposa;
        }
    }

    ImDrawList& list = *ImGui::GetForegroundDrawList();
    const auto [winWidth, winHeight] = ImGui::GetMainViewport()->Size;

    ImVec2 xpos, ypos;

    if (closestXNode) {
        xpos = convertGlobalPointToWindowSpace(
            closestXNode->getParent()->convertToWorldSpace(closestXNode->getPosition())
        );
    } else {
        xpos = convertGlobalPointToWindowSpace(gwx);
    }
    if (closestYNode) {
        ypos = convertGlobalPointToWindowSpace(
            closestYNode->getParent()->convertToWorldSpace(closestYNode->getPosition())
        );
    } else {
        ypos = convertGlobalPointToWindowSpace(gwy);
    }
    
    if (closestX < threshold) {
        if (closestXNode) {
            node->setPositionX(closestXNode->getPositionX());
            list.AddLine({ xpos.x, 0 }, { xpos.x, winHeight }, 0x44ffff00, strokeSize);
        } else {
            node->setPositionX(wx.x);
            list.AddLine({ xpos.x, 0 }, { xpos.x, winHeight }, 0x44ff00ff, strokeSize);
        }
    }

    if (closestY < threshold) {
        if (closestYNode) {
            node->setPositionY(closestYNode->getPositionY());
            list.AddLine({ 0, ypos.y }, { winWidth, ypos.y }, 0x44ffff00, strokeSize);
        } else {
            node->setPositionY(wy.y);
            list.AddLine({ 0, ypos.y }, { winWidth, ypos.y }, 0x44ff00ff, strokeSize);
        }
    }
}

void moveSelectedNode() {
    if (!selectedNode)
        return;
    
    auto eg = CCDirector::sharedDirector()->getOpenGLView();

    auto pos = getRelativeMousePos();

    auto npos = selectedNode->getParent()->convertToNodeSpace(pos);

    selectedNode->setPosition(npos + clickOffset);

    snapNodePosition(selectedNode);
}

void onSelectNode() {
    if (!selectedNode)
        return;

    auto pos = getRelativeMousePos();

    auto mpos = selectedNode->getParent()->convertToNodeSpace(pos);
    auto npos = selectedNode->getPosition();

    clickOffset = npos - mpos;
}

void generateTree(CCNode* node, unsigned int i = 0, unsigned int hix = 0u) {
    std::stringstream stream;
    stream << "[" << i << "] " << getNodeName(node);
    if (node->getTag() != -1)
        stream << " (" << node->getTag() << ")";
    const auto childrenCount = node->getChildrenCount();
    if (childrenCount)
        stream << " {" << childrenCount << "}";
    if (openLocation.size()) {
        ImGui::SetNextItemOpen(openLocation[hix] == i);
    }
    if (ImGui::TreeNode(node, stream.str().c_str())) {
        if (ImGui::TreeNode(node + 1, "Attributes")) {
            if (ImGui::Button("Delete")) {
                node->removeFromParentAndCleanup(true);
                ImGui::TreePop();
                ImGui::TreePop();
                return;
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Child")) {
                ImGui::OpenPopup("Add Child");
            }
            ImGui::SameLine();
            if (ImGui::Button("Highlight")) {
                highlightNode(node);
            }

            if (ImGui::BeginPopupModal("Add Child")) {
                static int item = 0;
                ImGui::Combo("Node", &item, "CCNode\0CCLabelBMFont\0CCLabelTTF\0CCSprite\0CCMenuItemSpriteExtra\0");

                static int tag = -1;
                ImGui::InputInt("Tag", &tag);

                static char text[256];
                static char labelFont[256] = "bigFont.fnt";
                if (item == 1) {
                    ImGui::InputText("Text", text, 256);
                    ImGui::InputText("Font", labelFont, 256);
                }
                static int fontSize = 20;
                if (item == 2) {
                    ImGui::InputText("Text", text, 256);
                    ImGui::InputInt("Font Size", &fontSize);
                }
                static bool frame = false;
                if (item == 3 || item == 4) {
                    ImGui::InputText("Texture", text, 256);
                    ImGui::Checkbox("Frame", &frame);
                }

                ImGui::Separator();

                if (ImGui::Button("Add")) {
                    threadFunctionsMutex.lock();
                    threadFunctions.push([node] {
                        CCNode* _child = nullptr;
                        switch (item) {
                        case 0:
                            _child = CCNode::create();
                            break;
                        case 1: {
                            auto child = CCLabelBMFont::create(text, labelFont);
                            _child = child;
                            break;
                        }
                        case 2: {
                            auto child = CCLabelTTF::create(text, "Arial", static_cast<float>(fontSize));
                            _child = child;
                            break;
                        }
                        case 3: {
                            CCSprite* child;
                            if (frame)
                                child = CCSprite::createWithSpriteFrameName(text);
                            else
                                child = CCSprite::create(text);
                            _child = child;
                            break;
                        }
                        case 4: {
                            CCSprite* sprite;
                            if (frame)
                                sprite = CCSprite::createWithSpriteFrameName(text);
                            else
                                sprite = CCSprite::create(text);
                            // _child = CCMenuItemSpriteExtra::create(sprite, sprite, nullptr, nullptr);
                            break;
                        }
                        default:
                            return;
                        }
                        if (_child != nullptr) {
                            _child->setTag(tag);
                            node->addChild(_child);
                        }
                    });
                    threadFunctionsMutex.unlock();

                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::Text("Addr: 0x%p", node);
            ImGui::SameLine();
            if (ImGui::Button("Copy")) {
                std::stringstream stream;
                stream << std::uppercase << std::hex << reinterpret_cast<uintptr_t>(node);
                clipboardText(stream.str().c_str());
            }
            if (node->getUserData()) {
                ImGui::Text("User data: 0x%p", node->getUserData());
            }

            auto pos = node->getPosition();
            float _pos[2] = { pos.x, pos.y };
            ImGui::DragFloat2("Position", _pos);
            node->setPosition({ _pos[0], _pos[1] });

            float _scale[3] = { node->getScale(), node->getScaleX(), node->getScaleY() };
            ImGui::DragFloat3("Scale", _scale, 0.025f);
            // amazing
            if (node->getScale() != _scale[0])
                node->setScale(_scale[0]);
            else {
                node->setScaleX(_scale[1]);
                node->setScaleY(_scale[2]);
            }

            float _rot[3] = { node->getRotation(), node->getRotationX(), node->getRotationY() };
            ImGui::DragFloat3("Rotation", _rot);
            if (node->getRotation() != _rot[0])
                node->setRotation(_rot[0]);
            else {
                node->setRotationX(_rot[1]);
                node->setRotationY(_rot[2]);
            }

            float _skew[2] = { node->getSkewX(), node->getSkewY() };
            ImGui::DragFloat2("Skew", _skew);
            node->setSkewX(_skew[0]);
            node->setSkewY(_skew[1]);

            auto anchor = node->getAnchorPoint();
            ImGui::DragFloat2("Anchor Point", &anchor.x, 0.05f, 0.f, 1.f);
            node->setAnchorPoint(anchor);

            auto contentSize = node->getContentSize();
            ImGui::DragFloat2("Content Size", &contentSize.width);
            if (contentSize != node->getContentSize())
                node->setContentSize(contentSize);

            int zOrder = node->getZOrder();
            ImGui::InputInt("Z", &zOrder);
            if (node->getZOrder() != zOrder)
                node->setZOrder(zOrder);
            
            auto visible = node->isVisible();
            ImGui::Checkbox("Visible", &visible);
            if (visible != node->isVisible())
                node->setVisible(visible);

            if (dynamic_cast<CCRGBAProtocol*>(node) != nullptr) {
                auto rgbaNode = dynamic_cast<CCRGBAProtocol*>(node);
                auto color = rgbaNode->getColor();
                float _color[4] = { color.r / 255.f, color.g / 255.f, color.b / 255.f, rgbaNode->getOpacity() / 255.f };
                ImGui::ColorEdit4("Color", _color);
                rgbaNode->setColor({
                    static_cast<GLubyte>(_color[0] * 255),
                    static_cast<GLubyte>(_color[1] * 255),
                    static_cast<GLubyte>(_color[2] * 255)
                });
                rgbaNode->setOpacity(static_cast<GLubyte>(_color[3] * 255));
            }
            if (dynamic_cast<CCLabelProtocol*>(node) != nullptr) {
                auto labelNode = dynamic_cast<CCLabelProtocol*>(node);
                auto labelStr = labelNode->getString();
                char text[256];
                strcpy_s(text, labelStr);
                ImGui::InputText("Text", text, 256);
                if (strcmp(text, labelStr)) {
                    threadFunctionsMutex.lock();
                    threadFunctions.push([labelNode, text]() {
                        labelNode->setString(text);
                    });
                    threadFunctionsMutex.unlock();
                }
            }

            ImGui::TreePop();
        }
        
        auto children = node->getChildren();
        for (unsigned int i = 0; i < node->getChildrenCount(); ++i) {
            auto child = children->objectAtIndex(i);
            generateTree(dynamic_cast<CCNode*>(child), i, hix + 1);
        }
        ImGui::TreePop();
    }
}

void highlightNodeUnderMouse(CCDirector* director) {
    if (editMode != eEdit)
        return;
    if (selectedNode)
        return;
    if (!CCDirector::sharedDirector()->getTouchDispatcher()->isDispatchEvents())
        return;
    
    auto scene = director->getRunningScene();
    
    auto mpos = getRelativeMousePos();
    
    auto array = CCArray::create();
    getNodesUnderMouse(scene, array, mpos);
    auto node = getTopMost(array);
    
    highlightedNode = node;

    highlightNode(node);
}

bool g_showWindow = true;

void RenderMain() {
    auto director = CCDirector::sharedDirector();
    moveSelectedNode();
    highlightNodeUnderMouse(director);
    highlightNode(selectedNode, true);
    
    if (g_showWindow) {
        auto& style = ImGui::GetStyle();
        style.ColorButtonPosition = ImGuiDir_Left;

        // thx andre
        const bool enableTouch = !ImGui::GetIO().WantCaptureMouse;
        director->getTouchDispatcher()->setDispatchEvents(enableTouch);
        if (ImGui::Begin("CocosDesigner"), nullptr, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar) {
            if (ImGui::RadioButton("Edit Mode", editMode == eEdit)) {
                editMode = editMode == eEdit ? eNormal : eEdit;
                selectedNode = nullptr;
                highlightedNode = nullptr;
            }
            ImGui::Text("%s, %s", getNodeName(highlightedNode), getNodeName(selectedNode));
            ImGui::Checkbox("Snap Position", &snapEnabled);
            ImGui::SameLine();
            ImGui::Checkbox("Snap To Window", &snapWindowEnabled);
            ImGui::NewLine();
            ImGui::Separator();
            ImGui::NewLine();
            auto curScene = director->getRunningScene();
            generateTree(curScene);
        }
        if (openLocation.size())
            openLocation.clear();
        ImGui::End();
    }
}

inline void(__thiscall* onGLFWMouseCallBack)(CCEGLView*, GLFWwindow*, int, int, int);
void __fastcall onGLFWMouseCallBackHook(CCEGLView* self, void*, GLFWwindow* wnd, int btn, int pressed, int z) {
    if (editMode == eNormal)
        return onGLFWMouseCallBack(self, wnd, btn, pressed, z);
    
    if (!CCDirector::sharedDirector()->getTouchDispatcher()->isDispatchEvents())
        return;

    if (pressed)
        switch (btn) {
            case 0: selectedNode = highlightedNode; break;
            case 2: {
                openLocation = getNodeLocationInTree(highlightedNode);

                g_showWindow = true;
            } break;
        }
    else
        selectedNode = nullptr;
    
    onSelectNode();
}

bool (__thiscall* dispatchScrollMSG)(CCMouseDelegate*, float, float);
bool __fastcall dispatchScrollMSGHook(CCMouseDelegate* self, void*, float deltaY, float param_2) {
    if (editMode == eNormal)
        return dispatchScrollMSG(self, deltaY, param_2);
    
    if (!CCDirector::sharedDirector()->getTouchDispatcher()->isDispatchEvents())
        return true;

    auto target = selectedNode ? selectedNode : highlightedNode;

    if (!target) return true;

    auto kb = CCDirector::sharedDirector()->getKeyboardDispatcher();

    if (kb->getShiftKeyPressed())
        target->setRotation(target->getRotation() + (-deltaY / 3.0f));
    else
        target->setScale(target->getScale() * (deltaY / 96.0f + 1.0f));

    return true;
}

inline void(__thiscall* dispatchKeyboardMSG)(void* self, int key, bool down);
void __fastcall dispatchKeyboardMSGHook(void* self, void*, int key, bool down) {

    if (ImGui::GetIO().WantCaptureKeyboard) return;
    else if (down && key == 'K') {
        g_showWindow ^= 1;
        if (!g_showWindow)
            CCDirector::sharedDirector()->getTouchDispatcher()->setDispatchEvents(true);
    }
    if (editMode == eEdit)
        return;
    dispatchKeyboardMSG(self, key, down);
}

inline void(__thiscall* schUpdate)(CCScheduler* self, float dt);
void __fastcall schUpdateHook(CCScheduler* self, void*, float dt) {
    threadFunctionsMutex.lock();
    while (!threadFunctions.empty()) {
        threadFunctions.back()();
        threadFunctions.pop();
    }
    threadFunctionsMutex.unlock();
    return schUpdate(self, dt);
}

DWORD WINAPI my_thread(void* hModule) {
#ifdef GD_CONSOLE
    AllocConsole();
    std::ofstream conout("CONOUT$", std::ios::out);
    std::ifstream conin("CONIN$", std::ios::in);
    std::cout.rdbuf(conout.rdbuf());
    std::cin.rdbuf(conin.rdbuf());  
#endif
    editMode = eNormal;

    auto cocosBase = GetModuleHandleA("libcocos2d.dll");
    MH_CreateHook(
        GetProcAddress(cocosBase, "?dispatchKeyboardMSG@CCKeyboardDispatcher@cocos2d@@QAE_NW4enumKeyCodes@2@_N@Z"),
        &dispatchKeyboardMSGHook,
        reinterpret_cast<void**>(&dispatchKeyboardMSG)
    );
    MH_CreateHook(
        GetProcAddress(cocosBase, "?update@CCScheduler@cocos2d@@UAEXM@Z"),
        &schUpdateHook,
        reinterpret_cast<void**>(&schUpdate)
    );
    MH_CreateHook(
        GetProcAddress(cocosBase, "?onGLFWMouseCallBack@CCEGLView@cocos2d@@IAEXPAUGLFWwindow@@HHH@Z"),
        &onGLFWMouseCallBackHook,
        reinterpret_cast<void**>(&onGLFWMouseCallBack)
    );
    MH_CreateHook(
        GetProcAddress(cocosBase, "?dispatchScrollMSG@CCMouseDispatcher@cocos2d@@QAE_NMM@Z"),
        &dispatchScrollMSGHook,
        reinterpret_cast<void**>(&dispatchScrollMSG)
    );
    MH_EnableHook(MH_ALL_HOOKS);

#ifdef GD_CONSOLE
    std::getline(std::cin, std::string());

    MH_Uninitialize();
    conout.close();
    conin.close();
    FreeConsole();
    ImGuiHook::Unload();
    FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(hModule), 0);
#endif
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID _) {
    if (reason == DLL_PROCESS_ATTACH) {
        CreateThread(0, 0x1000, my_thread, module, 0, 0);
        ImGuiHook::Load(RenderMain);
    }
    return TRUE;
}
