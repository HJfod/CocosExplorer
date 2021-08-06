#ifndef __SCENE_HPP__
#define __SCENE_HPP__

#include <vector>
#include <cocos2d.h>

using namespace cocos2d;

struct vec3f_t {
    float x, y, both;
};

struct node_edit {
    std::vector<int> tree_location;
    CCPoint position;
    CCPoint anchorpoint;
    CCPoint skew;
    CCSize content_size;
    int z_order;
    vec3f_t scale;
    vec3f_t rotation;
    bool visible;
    const char* text;
    ccColor3B color;
    GLubyte opacity;
};

struct scene_edit {
    const char* rtti_name;
    std::vector<node_edit> nodes;
};

#endif
