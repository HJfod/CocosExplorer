#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cocos2d.h>
#include <CCScale9Sprite.h>
#include <imgui.h>
#include <imgui_hook.h>
#include <MinHook.h>
#include <queue>
#include <mutex>
#include <fstream>
#include <sstream>
#include "scene.hpp"

// #define GD_CONSOLE

CCNode* getLastChild(CCNode* par) {
    return reinterpret_cast<CCNode*>(par->getChildren()->objectAtIndex(par->getChildrenCount() - 1));
}

using namespace cocos2d;
using namespace cocos2d::extension;

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

bool operator!=(const ccColor3B& a, const ccColor3B& b) {
    return a.r != b.r || a.b != b.b || a.g != b.g;
}

std::queue<std::function<void()>> threadFunctions;
std::mutex threadFunctionsMutex;
enum edit_t { eNormal, eEdit, } editMode;
CCNode* highlightedNode = nullptr;
CCNode* selectedNode = nullptr;
bool addingNode = false;
CCPoint clickOffset;
CCPoint startPos;
bool snapEnabled = true;
bool snapWindowEnabled = true;
bool snapWindowSidesEnabled = true;
bool snapGridEnabled = true;
bool saveChanges = false;
bool onlyDeleteSelected = false;
constexpr const float strokeSize = 3.0f;
std::vector<int> openLocation;
std::map<std::string, scene_edit> scenes;
std::set<CCNode*> changedNodes;
bool g_showWindow = true;
bool selectionMoved = false;
int mouseBtnDown = -1;
bool modifyingNode = false;
bool resizingNode = false;
bool openAddPopup = false;
bool addPopupOpen = false;
CCNode* addTarget = nullptr;
std::string movedToScene = "";
float g_snapThreshold = 10.0f;

float temp_dist_left = 0.0f;

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

CCNode* getNodeByTreeLocation(CCNode* start, std::vector<int> const& loc) {
    CCNode* res = start;

    for (auto l : loc) {
        if (static_cast<int>(res->getChildrenCount()) <= l)
            return nullptr;

        res = reinterpret_cast<CCNode*>(res->getChildren()->objectAtIndex(l));
    }

    return res;
}

class CCTransitionSceneGetter : public CCTransitionScene {
    public:
        CCScene* getInScene() {
            return m_pInScene;
        }
        CCScene* getOutScene() {
            return m_pOutScene;
        }
};

void sortArray(CCArray* pArray) {
    std::qsort(
        pArray->data->arr,
        pArray->data->num,
        sizeof(pArray->data->arr[0]),
        [](const void* p1, const void* p2) -> int {
            return static_cast<int>(
                reinterpret_cast<CCNode*>(const_cast<void*>(p1))->getPositionX() -
                reinterpret_cast<CCNode*>(const_cast<void*>(p2))->getPositionX()
            );
        }
    );
}

void registerNodeAsModified(CCNode* node) {
    changedNodes.insert(node);
}

void loadSceneChanges(CCScene* scene) {
    if (dynamic_cast<CCTransitionScene*>(scene)) {
        scene = dynamic_cast<CCTransitionSceneGetter*>(scene)->getInScene();
    }

    auto name = getNodeName(reinterpret_cast<CCNode*>(scene->getChildren()->objectAtIndex(0)));

    std::string trees = "";

    if (scenes.count(name)) {
        for (auto edit : scenes[name].nodes) {
            auto tloc = std::vector<int>(edit.tree_location.begin() + 1, edit.tree_location.end());

            auto node = getNodeByTreeLocation(scene, tloc);

            for (auto loc : tloc)
                trees += std::to_string(loc) + ".";
            
            trees += " -> " + std::string(node ? getNodeName(node) : "null") + " ";

            if (!node)
                continue;

            node->setPosition(edit.position);
            node->setRotation(edit.rotation.both);
            node->setRotationX(edit.rotation.x);
            node->setRotationY(edit.rotation.y);
            node->setScale(edit.rotation.both);
            node->setScaleX(edit.rotation.x);
            node->setScaleY(edit.rotation.y);
            node->setSkewX(edit.skew.x);
            node->setSkewY(edit.skew.y);
            node->setVisible(edit.visible);
            node->setContentSize(edit.content_size);
            node->setAnchorPoint(edit.anchorpoint);
            node->setZOrder(edit.z_order);

            auto dnode = dynamic_cast<CCNodeRGBA*>(node);
            if (dnode) {
                dnode->setColor(edit.color);
                dnode->setOpacity(edit.opacity);
            }

            auto lnode = dynamic_cast<CCLabelBMFont*>(node);
            if (lnode) {
                lnode->setString(edit.text);
            }
        }
    }

    movedToScene = name + std::string(" ") + trees;
}

void saveSceneChanges(CCScene* scene) {
    if (dynamic_cast<CCTransitionScene*>(scene)) {
        scene = reinterpret_cast<CCTransitionSceneGetter*>(scene)->getOutScene();
    }

    auto name = getNodeName(reinterpret_cast<CCNode*>(scene->getChildren()->objectAtIndex(0)));

    scene_edit edit;

    edit.rtti_name = name;

    if (scenes.count(name)) {
        for (auto node : scenes[name].nodes)
            edit.nodes.push_back(node);
    }
    
    for (auto node : changedNodes) {
        node_edit n;

        n.tree_location = getNodeLocationInTree(node);
        n.position = node->getPosition();
        n.anchorpoint = node->getAnchorPoint();
        n.skew.x = node->getSkewX();
        n.skew.y = node->getSkewY();
        n.content_size = node->getContentSize();
        n.z_order = node->getZOrder();
        n.scale.both = node->getScale();
        n.scale.x = node->getScaleX();
        n.scale.y = node->getScaleY();
        n.rotation.both = node->getRotation();
        n.rotation.x = node->getRotationX();
        n.rotation.y = node->getRotationY();
        n.visible = node->isVisible();

        auto dnode = dynamic_cast<CCNodeRGBA*>(node);
        if (dnode) {
            n.color = dnode->getColor();
            n.opacity = dnode->getOpacity();
        }

        auto lnode = dynamic_cast<CCLabelBMFont*>(node);
        if (lnode) {
            n.text = lnode->getString();
        }

        edit.nodes.push_back(n);
    }

    scenes[name] = edit;
}

bool filterNode(CCNode* node, bool isContainer) {
    if (!node->isVisible())
        return false;

    if (dynamic_cast<CCLayer*>(node))
        return isContainer;
    if (dynamic_cast<CCMenu*>(node))
        return isContainer;

    return !isContainer;
}

bool stopCheckingChildren(CCNode* node) {
    if (dynamic_cast<CCMenuItem*>(node))
        return true;
    if (dynamic_cast<CCLabelBMFont*>(node))
        return true;
    if (dynamic_cast<CCScale9Sprite*>(node))
        return true;
    if (!node->isVisible())
        return true;

    return false;
}

void getNodesUnderMouse(CCNode* parent, CCArray* res, CCPoint mpos, bool containers = false) {
    CCObject* obj;
    CCARRAY_FOREACH(parent->getChildren(), obj) {
        auto node = reinterpret_cast<CCNode*>(obj);

        if (!node) continue;

        auto pos = node->getPosition();
        auto size = node->getScaledContentSize();
        auto rect = CCRect { pos.x, pos.y, size.width, size.height };

        rect.origin = rect.origin - rect.size / 2;

        auto mposn = node->getParent()->convertToNodeSpace(mpos);

        if (rect.containsPoint(mposn) && filterNode(node, containers))
            res->addObject(node);
        
        if (node->getChildrenCount() && !stopCheckingChildren(node))
            getNodesUnderMouse(node, res, mpos, containers);
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

CCRect getNodeRectInWindowSpace(CCNode* node) {
    auto winSize = CCDirector::sharedDirector()->getWinSize();

    auto pos = node->getParent()->convertToWorldSpace(node->getPosition());
    auto size = node->getScaledContentSize();
    auto rect = CCRect { pos.x, pos.y, size.width, size.height };

    rect.origin = rect.origin - rect.size / 2;

    const auto [winWidth, winHeight] = ImGui::GetMainViewport()->Size;

    rect /= winSize;
    rect *= CCSize { winWidth, winHeight };

    rect.origin.y = winHeight - rect.origin.y;
    rect.origin.y -= rect.size.height;

    return rect;
}

enum highlight {
    hlNormal,
    hlSelected,
    hlAlt,
    hlAltOutline,
    hlAltOutline2,
};

void highlightNode(CCNode* node, highlight sel = hlNormal) {
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

    rect.origin.y = winHeight - rect.origin.y;
    rect.origin.y -= rect.size.height;

    switch (sel) {
        case hlSelected:
            list.AddRect(
                ccpToImVec2(rect.origin),
                ccpToImVec2(rect.origin + rect.size),
                0xff00ff00,
                0.0f, 15, strokeSize
            );
            break;

        case hlAlt:
            list.AddRectFilled(
                ccpToImVec2(rect.origin),
                ccpToImVec2(rect.origin + rect.size),
                0x3300ffff
            );
            break;
        
        case hlAltOutline:
            list.AddRect(
                ccpToImVec2(rect.origin),
                ccpToImVec2(rect.origin + rect.size),
                0xffff00ff,
                0.0f, 15, strokeSize
            );
            break;
        
        case hlAltOutline2:
            list.AddRect(
                ccpToImVec2(rect.origin),
                ccpToImVec2(rect.origin + rect.size),
                0xfff0f0ff,
                0.0f, 15, strokeSize
            );
            break;
        
        case hlNormal: default:
            list.AddRectFilled(
                ccpToImVec2(rect.origin),
                ccpToImVec2(rect.origin + rect.size),
                0x3300ff00
            );
            break;
    }
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

void snapNodeToWindowSides(CCNode* node) {
    if (snapWindowSidesEnabled) {
        auto left = node->getParent()->convertToNodeSpace({
            CCDirector::sharedDirector()->getScreenLeft(), 0
        }).x;
        auto right = node->getParent()->convertToNodeSpace({
            CCDirector::sharedDirector()->getScreenRight(), 0
        }).x;
        auto top = node->getParent()->convertToNodeSpace({
            0, CCDirector::sharedDirector()->getScreenTop()
        }).y;
        auto bottom = node->getParent()->convertToNodeSpace({
            0, CCDirector::sharedDirector()->getScreenBottom()
        }).y;

        auto csize = node->getScaledContentSize() / 2;

        temp_dist_left = node->getPositionX() - csize.width - left;

        if (fabsf(node->getPositionX() - csize.width - left) < g_snapThreshold)
            node->setPositionX(left + csize.width);

        if (fabsf(node->getPositionX() + csize.width - right) < g_snapThreshold)
            node->setPositionX(right - csize.width);

        if (fabsf(node->getPositionY() - csize.height - bottom) < g_snapThreshold)
            node->setPositionY(bottom + csize.height);

        if (fabsf(node->getPositionY() + csize.height - top) < g_snapThreshold)
            node->setPositionY(top - csize.height);
    }
}

void snapNodePosition(CCNode* node) {
    if (!snapEnabled)
        return;
    if (node->getParent()->getChildrenCount() == 1)
        return snapNodeToWindowSides(node);

    auto pos = node->getPosition();

    CCArray* closestXNodes = CCArray::create();
    CCArray* closestYNodes = CCArray::create();
    closestXNodes->retain();
    closestYNodes->retain();

    float closestX, closestY;
    CCObject* obj;
    CCARRAY_FOREACH(node->getParent()->getChildren(), obj) {
        if (obj == node)
            continue;
        
        auto nobj = reinterpret_cast<CCNode*>(obj);

        if (nobj) {
            if (!closestXNodes->count()) {
                closestXNodes->addObject(nobj);
                closestYNodes->addObject(nobj);
                closestX = fabsf(nobj->getPositionX() - node->getPositionX());
                closestY = fabsf(nobj->getPositionY() - node->getPositionY());
                continue;
            }

            auto disx = fabsf(nobj->getPositionX() - node->getPositionX());
            if (disx <= closestX) {
                if (disx < closestX)
                    closestXNodes->removeAllObjects();
                closestXNodes->addObject(nobj);
                closestX = disx;
            }

            auto disy = fabsf(nobj->getPositionY() - node->getPositionY());
            if (disy <= closestY) {
                if (disy < closestY)
                    closestYNodes->removeAllObjects();
                closestYNodes->addObject(nobj);
                closestY = disy;
            }
        }
    }

    CCPoint wx, wy;
    CCPoint gwx, gwy;
    bool mouseX = false, mouseY = false;

    if (snapWindowEnabled) {
        auto wposa = static_cast<CCPoint>(CCDirector::sharedDirector()->getWinSize() / 2);
        auto wpos = node->getParent()->convertToNodeSpace(wposa);

        auto disx = fabsf(wpos.x - node->getPositionX());
        if (disx <= closestX) {
            if (disx < closestX)
                closestXNodes->removeAllObjects();
            closestX = disx;
            wx = wpos;
            gwx = wposa;
            mouseX = true;
        }

        auto disy = fabsf(wpos.y - node->getPositionY());
        if (disy <= closestY) {
            if (disy < closestY)
                closestYNodes->removeAllObjects();
            closestY = disy;
            wy = wpos;
            gwy = wposa;
            mouseY = true;
        }
    }

    ImDrawList& list = *ImGui::GetForegroundDrawList();
    const auto [winWidth, winHeight] = ImGui::GetMainViewport()->Size;

    ImVec2 xpos, ypos;

    CCNode* closestXNode = nullptr, *closestYNode = nullptr;

    if (closestXNodes->count()) {
        closestXNode = reinterpret_cast<CCNode*>(
            closestXNodes->objectAtIndex(0)
        );
    }

    if (closestYNodes->count()) {
        closestYNode = reinterpret_cast<CCNode*>(
            closestYNodes->objectAtIndex(0)
        );
    }

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
    
    snapNodeToWindowSides(node);

    if (closestX < g_snapThreshold) {
        if (closestXNode && !mouseX) {
            node->setPositionX(closestXNode->getPositionX());
            list.AddLine({ xpos.x, 0 }, { xpos.x, winHeight }, 0x44ffff00, strokeSize);
        } else {
            node->setPositionX(wx.x);
            list.AddLine({ xpos.x, 0 }, { xpos.x, winHeight }, 0x44ff00ff, strokeSize);
        }

        CCObject* iobj;
        CCARRAY_FOREACH(closestXNodes, iobj)
            highlightNode(reinterpret_cast<CCNode*>(iobj), hlAltOutline);
    }

    if (closestY < g_snapThreshold) {
        if (closestYNode && !mouseY) {
            node->setPositionY(closestYNode->getPositionY());
            list.AddLine({ 0, ypos.y }, { winWidth, ypos.y }, 0x44ffff00, strokeSize);
        } else {
            node->setPositionY(wy.y);
            list.AddLine({ 0, ypos.y }, { winWidth, ypos.y }, 0x44ff00ff, strokeSize);
        }

        CCObject* iobj;
        CCARRAY_FOREACH(closestYNodes, iobj)
            highlightNode(reinterpret_cast<CCNode*>(iobj), hlAltOutline2);
    }
    
    closestXNodes->release();
    closestYNodes->release();
}

void moveSelectedNode() {
    if (!selectedNode)
        return;
    if (modifyingNode && mouseBtnDown != 0)
        return;
    if (resizingNode || addPopupOpen)
        return;
    
    auto eg = CCDirector::sharedDirector()->getOpenGLView();

    auto pos = getRelativeMousePos();

    auto npos = selectedNode->getParent()->convertToNodeSpace(pos);

    selectedNode->setPosition(npos + clickOffset);

    if (ccpDistance(startPos, selectedNode->getPosition()) > 5.0f) {
        selectionMoved = true;
        registerNodeAsModified(selectedNode);
    }

    snapNodePosition(selectedNode);
}

void onSelectNode() {
    if (!selectedNode)
        return;

    auto pos = getRelativeMousePos();

    auto mpos = selectedNode->getParent()->convertToNodeSpace(pos);
    auto npos = selectedNode->getPosition();

    clickOffset = npos - mpos;
    startPos = selectedNode->getPosition();
}

void selectAddNode() {
    if (!addTarget) return;

    if (ImGui::BeginPopupModal("Add Child")) {
        addPopupOpen = true;

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
            threadFunctions.push([] {
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
                    addTarget->addChild(_child);
                }
            });
            threadFunctionsMutex.unlock();
            addPopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            addPopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
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
        if (hix == openLocation.size() - 1) {
            ImGui::SetNextItemOpen(true);
            ImGui::SetScrollHere();
        }

        if (ImGui::TreeNode(node + 1, "Attributes")) {
            if (ImGui::Button("Delete")) {
                node->removeFromParentAndCleanup(true);
                ImGui::TreePop();
                ImGui::TreePop();
                return;
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Child")) {
                addTarget = node;
                ImGui::OpenPopup("Add Child");
            }
            ImGui::SameLine();
            if (ImGui::Button("Highlight")) {
                highlightNode(node);
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
            if (CCSize { _pos[0], _pos[1] } != pos) {
                registerNodeAsModified(node);
            }
            node->setPosition({ _pos[0], _pos[1] });

            float _scale[3] = { node->getScale(), node->getScaleX(), node->getScaleY() };
            ImGui::DragFloat3("Scale", _scale, 0.025f);
            // amazing
            if (node->getScale() != _scale[0]) {
                registerNodeAsModified(node);
                node->setScale(_scale[0]);
            } else {
                if (CCSize { _scale[1], _scale[2] } != CCSize { node->getScaleX(), node->getScaleY() })
                    registerNodeAsModified(node);

                node->setScaleX(_scale[1]);
                node->setScaleY(_scale[2]);
            }

            float _rot[3] = { node->getRotation(), node->getRotationX(), node->getRotationY() };
            ImGui::DragFloat3("Rotation", _rot);
            if (node->getRotation() != _rot[0]) {
                registerNodeAsModified(node);
                node->setRotation(_rot[0]);
            } else {
                if (CCSize { _rot[1], _rot[2] } != CCSize { node->getRotationX(), node->getRotationY() })
                    registerNodeAsModified(node);

                node->setRotationX(_rot[1]);
                node->setRotationY(_rot[2]);
            }

            float _skew[2] = { node->getSkewX(), node->getSkewY() };
            ImGui::DragFloat2("Skew", _skew);
            if (node->getSkewX() != _skew[0] || node->getSkewY() != _skew[1]) {
                registerNodeAsModified(node);
            }
            node->setSkewX(_skew[0]);
            node->setSkewY(_skew[1]);

            auto anchor = node->getAnchorPoint();
            ImGui::DragFloat2("Anchor Point", &anchor.x, 0.05f, 0.f, 1.f);
            if (node->getAnchorPoint() != anchor) {
                registerNodeAsModified(node);
            }
            node->setAnchorPoint(anchor);

            auto contentSize = node->getContentSize();
            ImGui::DragFloat2("Content Size", &contentSize.width);
            if (contentSize != node->getContentSize()) {
                node->setContentSize(contentSize);
                registerNodeAsModified(node);
            }

            int zOrder = node->getZOrder();
            ImGui::InputInt("Z", &zOrder);
            if (node->getZOrder() != zOrder) {
                node->setZOrder(zOrder);
                registerNodeAsModified(node);
            }
            
            auto visible = node->isVisible();
            ImGui::Checkbox("Visible", &visible);
            if (visible != node->isVisible()) {
                node->setVisible(visible);
                registerNodeAsModified(node);
            }

            if (dynamic_cast<CCRGBAProtocol*>(node) != nullptr) {
                auto rgbaNode = dynamic_cast<CCRGBAProtocol*>(node);
                auto color = rgbaNode->getColor();
                float _color[4] = { color.r / 255.f, color.g / 255.f, color.b / 255.f, rgbaNode->getOpacity() / 255.f };
                ImGui::ColorEdit4("Color", _color);
                auto ncol = ccColor3B {
                    static_cast<GLubyte>(_color[0] * 255),
                    static_cast<GLubyte>(_color[1] * 255),
                    static_cast<GLubyte>(_color[2] * 255)
                };
                auto nopacity = static_cast<GLubyte>(_color[3] * 255);
                if (rgbaNode->getColor() != ncol || rgbaNode->getOpacity() != nopacity)
                    registerNodeAsModified(node);
                rgbaNode->setColor(ncol);
                rgbaNode->setOpacity(nopacity);
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
                    registerNodeAsModified(node);
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
    if (selectedNode || addPopupOpen)
        return;
    if (!CCDirector::sharedDirector()->getTouchDispatcher()->isDispatchEvents())
        return;
    
    auto scene = director->getRunningScene();
    
    auto mpos = getRelativeMousePos();
    
    auto array = CCArray::create();
    getNodesUnderMouse(getLastChild(scene), array, mpos, addingNode);
    auto node = getTopMost(array);
    
    highlightedNode = node;

    highlightNode(node, addingNode ? hlAlt : hlNormal);

    if (addingNode) {
        CCObject* child;
        CCARRAY_FOREACH(node->getChildren(), child)
            highlightNode(reinterpret_cast<CCNode*>(child));
    }
}

bool intersectsModifyControls() {
    if (!selectedNode || !modifyingNode)
        return false;

    auto mpos = getRelativeMousePos();

    auto pos = selectedNode->getParent()->convertToWorldSpace(selectedNode->getPosition());
    auto size = selectedNode->getScaledContentSize();
    pos = pos - size / 2;

    auto mrect = CCRect { pos.x - 7.5f, pos.y + size.height - 7.5f, 15.f, 15.f };

    return mrect.containsPoint(mpos);
}

void showModifyControls() {
    if (!selectedNode || !modifyingNode)
        return;

    auto size = getNodeRectInWindowSpace(selectedNode);
    auto pos = size.origin;
    auto rectfw = convertGlobalPointToWindowSpace({ 15.0f, 0.0f }).x;

    ImDrawList& list = *ImGui::GetForegroundDrawList();

    list.AddRectFilled(
        { pos.x - rectfw / 2, pos.y - rectfw / 2 },
        { pos.x + rectfw / 2, pos.y + rectfw / 2 },
        intersectsModifyControls() ? 0xffffff00 : 0xff0000ff
    );

    if (resizingNode) {
        auto mpos = getRelativeMousePos();

        auto mposn = selectedNode->getParent()->convertToNodeSpace(mpos);
        auto npos = selectedNode->getPosition();
        auto clickOffset2 = npos - mposn;

        clickOffset2.x = fabsf(clickOffset2.x);
        clickOffset2.y = fabsf(clickOffset2.y);

        selectedNode->setContentSize(clickOffset2 * 2);

        registerNodeAsModified(selectedNode);
    }
}

void RenderMain() {
    auto director = CCDirector::sharedDirector();

    if (!selectedNode)
        modifyingNode = false;

    moveSelectedNode();
    highlightNodeUnderMouse(director);
    highlightNode(selectedNode, hlSelected);
    showModifyControls();
    
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
            if (snapEnabled) {
                ImGui::SameLine();
                ImGui::Checkbox("Snap To Window", &snapWindowEnabled);
                ImGui::SameLine();
                ImGui::Checkbox("Snap To Sides", &snapWindowSidesEnabled);
            }
            ImGui::Checkbox("Only Delete Selected", &onlyDeleteSelected);
            ImGui::NewLine();

            ImGui::Checkbox("Save Changes", &saveChanges);
            ImGui::SameLine();
            ImGui::Text("Modified nodes: %d, scenes: %d", changedNodes.size(), scenes.size());

            std::string ss = "";
            for (auto [key, val] : scenes)
                ss += key + " -> " + std::to_string (val.nodes.size()) + "; ";
            ImGui::Text(ss.c_str());
            ImGui::Text(movedToScene.c_str());

            ImGui::Text("%.2f", temp_dist_left);

            ImGui::NewLine();
            ImGui::Separator();
            ImGui::NewLine();
            
            auto curScene = director->getRunningScene();
            generateTree(curScene);
            selectAddNode();
        }
        if (openLocation.size())
            openLocation.clear();
        ImGui::End();
    }

    if (openAddPopup) {
        addTarget = highlightedNode;
        ImGui::OpenPopup("Add Child");
        openAddPopup = false;
    }
    if (!g_showWindow)
        selectAddNode();

    if (editMode == eNormal) {
        highlightedNode = nullptr;
        selectedNode = nullptr;
    }
}

inline void(__thiscall* willSwitchToScene)(CCDirector*, CCScene*);
void __fastcall willSwitchToSceneHook(CCDirector* self, void*, CCScene* nScene) {
    if (saveChanges) {
        saveSceneChanges(self->getRunningScene());
    }

    changedNodes.clear();

    willSwitchToScene(self, nScene);

    if (saveChanges) {
        loadSceneChanges(nScene);
    }
}

inline void(__thiscall* onGLFWMouseCallBack)(CCEGLView*, GLFWwindow*, int, int, int);
void __fastcall onGLFWMouseCallBackHook(CCEGLView* self, void*, GLFWwindow* wnd, int btn, int pressed, int z) {
    if (editMode == eNormal)
        return onGLFWMouseCallBack(self, wnd, btn, pressed, z);
    
    if (!CCDirector::sharedDirector()->getTouchDispatcher()->isDispatchEvents())
        return;

    if (pressed) {
        switch (btn) {
            case 0:
                if (modifyingNode) {
                    resizingNode = intersectsModifyControls();
                } else {
                    selectedNode = highlightedNode;
                }
                break;
            case 1: addingNode = true; break;
            case 2: {
                openLocation = getNodeLocationInTree(highlightedNode);

                g_showWindow = true;
            } break;
        }
        mouseBtnDown = btn;
    } else {
        if (!resizingNode && btn == 0 && modifyingNode && selectedNode == highlightedNode && !selectionMoved) {
            selectedNode = nullptr;
            modifyingNode = false;
        }

        if (!modifyingNode && selectionMoved) {
            selectedNode = nullptr;
        } else {
            modifyingNode = true;
        }

        if (addingNode) {
            openAddPopup = true;
        }

        addingNode = false;
        selectionMoved = false;
        mouseBtnDown = -1;
        resizingNode = false;
    }

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

    registerNodeAsModified(target);

    return true;
}

inline void(__thiscall* dispatchKeyboardMSG)(void* self, int key, bool down);
void __fastcall dispatchKeyboardMSGHook(void* self, void*, int key, bool down) {

    if (ImGui::GetIO().WantCaptureKeyboard)
        return;
    else if (down) {
        switch (key) {
            case 'K':
                g_showWindow ^= 1;
                if (!g_showWindow)
                    CCDirector::sharedDirector()->getTouchDispatcher()->setDispatchEvents(true);
                break;
            
            case 'J':
                editMode = editMode == eEdit ? eNormal :  eEdit;
                break;

            case KEY_Delete:
                if (editMode == eEdit) {
                    if (onlyDeleteSelected && !selectedNode)
                        return;
                    
                    if (onlyDeleteSelected)
                        selectedNode->removeFromParentAndCleanup(true);
                    else
                        highlightedNode->removeFromParentAndCleanup(true);

                    if (selectedNode == highlightedNode) {
                        highlightedNode = nullptr;
                        selectedNode = nullptr;
                    }
                }
                break;
        }
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
    MH_CreateHook(
        GetProcAddress(cocosBase, "?willSwitchToScene@CCDirector@cocos2d@@QAEXPAVCCScene@2@@Z"),
        &willSwitchToSceneHook,
        reinterpret_cast<void**>(&willSwitchToScene)
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
